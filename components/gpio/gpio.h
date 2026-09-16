#ifndef _GPIO_H_
#define _GPIO_H_

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 引脚初始化参数
     *
     * @note 三个字段都会生效，不要只写自己关心的那一项。
     *       推荐先用 GPIO_INIT_CFG_DEFAULT() 整体初始化，再覆盖需要的字段，
     *       否则被省略的字段会被编译器补 0，而 0 恰好是 GPIO_MODE_DISABLE，
     *       不是想要的输出模式。
     */
    typedef struct
    {
        gpio_mode_t mode;      /*!< 引脚模式：输入 / 输出 / 开漏 / 输入输出 */
        gpio_pull_mode_t pull; /*!< 上下拉：GPIO_PULLUP_ONLY / GPIO_PULLDOWN_ONLY / GPIO_FLOATING */
        gpio_int_type_t intr_type; /*!< 中断触发方式：GPIO_INTR_DISABLE 表示不启用中断 */
        gpio_isr_t isr;            /*!< 中断处理函数，NULL 表示不注册 */
        void* isr_arg;             /*!< 传给中断处理函数的参数，处理函数里用 arg 收到 */
    } gpio_init_cfg_t;

/**
 * @brief 默认配置：推挽输出、无上下拉、不启用中断、不注册处理函数
 */
#define GPIO_INIT_CFG_DEFAULT()                                                                    \
    ((gpio_init_cfg_t){                                                                            \
        .mode = GPIO_MODE_OUTPUT,                                                                  \
        .pull = GPIO_FLOATING,                                                                     \
        .intr_type = GPIO_INTR_DISABLE,                                                            \
        .isr = NULL,                                                                               \
        .isr_arg = NULL,                                                                           \
    })

    /**
     * @brief 初始化一个引脚：配置引脚模式、上下拉、中断触发方式
     *
     * @param pin 引脚号
     * @param cfg 配置参数，不可为 NULL
     *
     * @note 该函数会整体覆盖引脚原有的配置（gpio_config 的行为）。
     *
     * @note 若请求的是纯输出模式(GPIO_MODE_OUTPUT / GPIO_MODE_OUTPUT_OD)同时又启用了中断，
     *       本函数会自动把模式升级成对应的输入输出模式并打印警告。
     *       原因是纯输出模式下输入通道被关闭，引脚上的电平变化无法送进中断矩阵，
     *       中断永远不会触发。
     *
     * @note cfg->isr 不为 NULL 且启用了中断时，本函数会自动完成
     *       gpio_install_isr_service() + gpio_isr_handler_add() 两步注册。
     *       ISR 服务全局只安装一次，多个中断引脚各自传各自的处理函数即可。
     *       处理函数运行在中断上下文：只能调用带 _isr / IRAM_SAFE 后缀的 API，
     *       不能用 printf / vTaskDelay 等会碰 flash 或阻塞的函数。
     *
     * @return
     *     - ESP_OK               成功
     *     - ESP_ERR_INVALID_ARG  引脚号非法、引脚不支持输出、cfg 为 NULL
     *     - 其它                 gpio_config 返回的错误码
     */
    esp_err_t gpio_config_init(gpio_num_t pin, const gpio_init_cfg_t* cfg);

    /**
     * @brief 设置指定 GPIO 的输出电平（高 / 低）
     *
     * @param pin   引脚号
     * @param level 输出电平，非 0 输出高电平，0 输出低电平
     *
     * @return
     *     - ESP_OK               成功
     *     - ESP_ERR_INVALID_ARG  引脚号非法
     */
    esp_err_t gpio_write(gpio_num_t pin, uint32_t level);

    /**
     * @brief 读取指定 GPIO 的电平（高 / 低）
     *
     * @param pin   引脚号
     * @param level 输出参数，返回电平：0=低电平，1=高电平。不可为 NULL
     *
     * @return
     *     - ESP_OK               成功
     *     - ESP_ERR_INVALID_ARG  引脚号非法或 level 为 NULL
     */
    esp_err_t gpio_read(gpio_num_t pin, uint32_t* level);

#ifdef __cplusplus
}
#endif

#endif
