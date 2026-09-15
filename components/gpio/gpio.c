#include "gpio.h"
#include <stddef.h>
#include "esp_log.h"

static const char* TAG = "gpio";

/* ISR 服务全局只能安装一次，这里记住有没有装过，避免重复安装刷错误日志 */
static bool s_isr_service_installed = false;

/**
 * @brief  GPIO配置初始化
 *
 * @param  pin 引脚号
 * @param  cfg 配置信息
 * @return  ESP_OK 成功
 * @return  其他 失败
 *
 * @note  初始化GPIO引脚配置，包括模式和上下拉电阻
 */
esp_err_t gpio_config_init(gpio_num_t pin, const gpio_init_cfg_t* cfg)
{
    if (cfg == NULL)
    {
        ESP_LOGE(TAG, "cfg 不能为 NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!GPIO_IS_VALID_GPIO(pin))
    {
        ESP_LOGE(TAG, "引脚号 %d 非法", (int)pin);
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- 引脚模式 ---- */
    gpio_mode_t mode = cfg->mode;

    const bool want_output = (mode == GPIO_MODE_OUTPUT) || (mode == GPIO_MODE_OUTPUT_OD) ||
                             (mode == GPIO_MODE_INPUT_OUTPUT) ||
                             (mode == GPIO_MODE_INPUT_OUTPUT_OD);

    /* ESP32 的 GPIO34~39 只能做输入，配成输出会配不上，提前拦掉 */
    if (want_output && !GPIO_IS_VALID_OUTPUT_GPIO(pin))
    {
        ESP_LOGE(TAG, "引脚 %d 不支持输出(只能做输入)", (int)pin);
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- 上下拉 ----
     * gpio_config 要的是"上拉""下拉"两个独立使能位，
     * 这里把 pull 这一个枚举拆成这两位。
     */
    gpio_pullup_t pull_up = GPIO_PULLUP_DISABLE;
    gpio_pulldown_t pull_down = GPIO_PULLDOWN_DISABLE;

    switch (cfg->pull)
    {
        case GPIO_PULLUP_ONLY:
            pull_up = GPIO_PULLUP_ENABLE;
            break;

        case GPIO_PULLDOWN_ONLY:
            pull_down = GPIO_PULLDOWN_ENABLE;
            break;

        case GPIO_PULLUP_PULLDOWN:
            pull_up = GPIO_PULLUP_ENABLE;
            pull_down = GPIO_PULLDOWN_ENABLE;
            ESP_LOGW(TAG, "引脚 %d 上拉下拉同时打开，两电阻并联分压，通常不是想要的结果", (int)pin);
            break;

        case GPIO_FLOATING:
        default:
            break;
    }

    /* ---- 中断 ----
     * 配了触发方式就必须让输入通道打开。
     * gpio_config 在纯输出模式下会调 gpio_input_disable()，输入缓冲器一关，
     * 引脚上的电平变化送不进中断矩阵，中断永远不会触发，而且不报任何错。
     * 所以纯输出 + 中断时，把模式升成对应的输入输出模式。
     */
    if (cfg->intr_type != GPIO_INTR_DISABLE)
    {
        if (mode == GPIO_MODE_OUTPUT)
        {
            mode = GPIO_MODE_INPUT_OUTPUT;
        }
        else if (mode == GPIO_MODE_OUTPUT_OD)
        {
            mode = GPIO_MODE_INPUT_OUTPUT_OD;
        }

        if (mode != cfg->mode)
        {
            ESP_LOGW(TAG, "引脚 %d 是纯输出模式但配了中断，模式已自动改为 %d 以打开输入通道",
                     (int)pin, (int)mode);
        }
    }

    const gpio_config_t io = {
        .pin_bit_mask = BIT64(pin),
        .mode = mode,
        .pull_up_en = pull_up,
        .pull_down_en = pull_down,
        .intr_type = cfg->intr_type,
    };

    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "引脚 %d 初始化失败: %s", (int)pin, esp_err_to_name(err));
        return err;
    }

    /* ---- 中断处理函数注册 ---- */
    if (cfg->isr != NULL)
    {
        if (cfg->intr_type == GPIO_INTR_DISABLE)
        {
            ESP_LOGW(TAG, "引脚 %d 传了处理函数但 intr_type 是 DISABLE，函数永远不会被调用",
                     (int)pin);
        }
        else
        {
            /* 第一步：安装 ISR 服务，整个程序只需要装一次 */
            if (!s_isr_service_installed)
            {
                err = gpio_install_isr_service(0);
                /* INVALID_STATE = 别处已经装过(比如自己在 app_main 里调过)，不算错 */
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
                {
                    ESP_LOGE(TAG, "安装 GPIO ISR 服务失败: %s", esp_err_to_name(err));
                    return err;
                }
                s_isr_service_installed = true;
            }

            /* 第二步：给这个引脚挂上处理函数，cfg->isr_arg 会原样传给它 */
            err = gpio_isr_handler_add(pin, cfg->isr, cfg->isr_arg);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "注册引脚 %d 的中断处理函数失败: %s", (int)pin, esp_err_to_name(err));
                return err;
            }
        }
    }
    else if (cfg->intr_type != GPIO_INTR_DISABLE)
    {
        ESP_LOGW(TAG, "引脚 %d 开了中断但没给处理函数，中断会触发但没人处理", (int)pin);
    }

    ESP_LOGD(TAG, "引脚 %d 初始化完成: mode=%d pull=%d intr=%d", (int)pin, (int)mode,
             (int)cfg->pull, (int)cfg->intr_type);
    return ESP_OK;
}

/**
 * @brief  设置指定 GPIO 的输出电平（高 / 低）
 *
 * @param  pin   引脚号
 * @param  level 输出电平，非 0 输出高电平，0 输出低电平
 * @return ESP_OK 成功
 * @return ESP_ERR_INVALID_ARG 引脚号非法
 *
 * @note 仅做合法性校验后透传给 gpio_set_level()。
 *       要求引脚已经配置为输出或输入输出模式，否则行为按 ESP-IDF 规则走。
 */
esp_err_t gpio_write(gpio_num_t pin, uint32_t level)
{
    if (!GPIO_IS_VALID_GPIO(pin))
    {
        ESP_LOGE(TAG, "gpio_write: 引脚号 %d 非法", (int)pin);
        return ESP_ERR_INVALID_ARG;
    }

    gpio_set_level(pin, level ? 1 : 0);
    return ESP_OK;
}

/**
 * @brief  读取指定 GPIO 的电平（高 / 低）
 *
 * @param  pin 引脚号
 * @param  level 输出参数，用于返回电平：0=低电平，1=高电平。
 *              不可为 NULL。
 * @return ESP_OK 成功
 * @return ESP_ERR_INVALID_ARG 引脚号非法或 level 为 NULL
 *
 * @note 仅做合法性校验后透传给 gpio_get_level()。
 *       要求引脚已经配置为输入或输入输出模式，否则读到的值无意义。
 */
esp_err_t gpio_read(gpio_num_t pin, uint32_t* level)
{
    if (level == NULL)
    {
        ESP_LOGE(TAG, "gpio_read: level 不能为 NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!GPIO_IS_VALID_GPIO(pin))
    {
        ESP_LOGE(TAG, "gpio_read: 引脚号 %d 非法", (int)pin);
        return ESP_ERR_INVALID_ARG;
    }

    *level = gpio_get_level(pin) ? 1 : 0;
    return ESP_OK;
}
