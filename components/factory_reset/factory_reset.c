/**
 * @file factory_reset.c
 * @brief BOOT 键长按 → 清除所有设置 → 重启
 *
 * 硬件背景：这块板只有 BOOT 键（GPIO0），没有独立的复位/恢复键。BOOT 键
 * 在运行期间只是一个普通输入，所以可以拿来做"长按恢复出厂设置"：
 *
 *   按住 → 状态灯快闪（由应用层在 HOLD_BEGIN 事件里决定）→ 按够时长
 *        → 调应用层注入的擦除函数 → 等 300ms 让日志刷出去 → esp_restart()
 *
 * 为什么用轮询而不是中断：按键是机械触点，抖动 + 长按判定都要做；用一个
 * 50ms 周期的任务比在 ISR 里算时间简单得多，代价几乎为零（每轮只读一次
 * 引脚寄存器）。
 *
 * 为什么时长用 esp_timer_get_time() 而不是累加计数：vTaskDelay 的实际
 * 唤醒时间会被 tick 调度拉长，按次数算会让"5 秒"变成 6~7 秒；
 * 时间戳跟调度无关，用户掐表就是 5 秒。
 */

#include "factory_reset.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gpio.h"

static const char* TAG = "factory_rst";

#define FR_TASK_STACK 4096
#define FR_TASK_PRIO 3              /* 低于所有业务任务：只在被按住时才有点活 */
#define FR_REBOOT_DELAY_MS 300      /* 擦完等一下，让串口把日志吐完再重启 */
#define FR_DEFAULT_POLL_MS 50

typedef struct
{
    factory_reset_cfg_t cfg;
    factory_reset_erase_fn erase;
    factory_reset_evt_fn on_evt;
    void* ctx;
    bool started;
} fr_state_t;

static fr_state_t s_fr;

/**
 * @brief 读一次引脚，判断当前是否处于"按下"状态
 *
 * @note 读失败按"没按下"处理：宁可漏一次触发，也不要因为一次总线错误
 *       把用户正在用的配置擦掉。
 */
static bool fr_is_pressed(const factory_reset_cfg_t* cfg)
{
    uint32_t level = 1;
    if (gpio_read(cfg->pin, &level) != ESP_OK)
    {
        return false;
    }
    return cfg->active_low ? (level == 0) : (level != 0);
}

static void fr_notify(factory_reset_evt_t evt)
{
    if (s_fr.on_evt != NULL)
    {
        s_fr.on_evt(evt, s_fr.ctx);
    }
}

static void fr_task(void* arg)
{
    (void)arg;

    const factory_reset_cfg_t* cfg = &s_fr.cfg;

    bool holding = false;
    bool triggered = false;
    int64_t press_us = 0;

    ESP_LOGI(TAG, "BOOT 键监听已启动：GPIO%d（%s），长按 %u ms 恢复出厂设置",
             (int)cfg->pin, cfg->active_low ? "按下为低" : "按下为高",
             (unsigned)cfg->hold_ms);

    while (1)
    {
        if (fr_is_pressed(cfg))
        {
            if (!holding)
            {
                holding = true;
                triggered = false;
                press_us = esp_timer_get_time();
                ESP_LOGW(TAG, "BOOT 键按下：保持 %u ms 将清除所有设置并重启",
                         (unsigned)cfg->hold_ms);
                fr_notify(FACTORY_RESET_EVT_HOLD_BEGIN);
            }
            else if (!triggered &&
                     (esp_timer_get_time() - press_us) >= (int64_t)cfg->hold_ms * 1000)
            {
                triggered = true; /* 防重入：擦除 + 重启期间别再触发一次 */

                ESP_LOGE(TAG, "长按 %u ms 达成，开始清除所有设置", (unsigned)cfg->hold_ms);
                fr_notify(FACTORY_RESET_EVT_TRIGGER);

                if (s_fr.erase != NULL)
                {
                    const esp_err_t err = s_fr.erase(s_fr.ctx);
                    if (err != ESP_OK)
                    {
                        /* 没擦干净也照样重启：用户手里只有这一个键，
                         * 停在原地只会让他以为按键坏了。错误码留在日志里。 */
                        ESP_LOGE(TAG, "清除设置不完整（%s），仍继续重启", esp_err_to_name(err));
                    }
                    else
                    {
                        ESP_LOGW(TAG, "所有设置已清除");
                    }
                }

                vTaskDelay(pdMS_TO_TICKS(FR_REBOOT_DELAY_MS));
                ESP_LOGW(TAG, "重启，恢复为出厂状态…");
                esp_restart();
            }
        }
        else if (holding)
        {
            /* 没按够就松开：本次取消，下次按住重新计时 */
            holding = false;
            triggered = false;
            ESP_LOGI(TAG, "BOOT 键已松开，取消恢复出厂设置");
            fr_notify(FACTORY_RESET_EVT_HOLD_ABORT);
        }

        vTaskDelay(pdMS_TO_TICKS(cfg->poll_ms));
    }
}

esp_err_t factory_reset_start(const factory_reset_cfg_t* cfg,
                              factory_reset_erase_fn erase,
                              factory_reset_evt_fn on_evt,
                              void* ctx)
{
    if (cfg == NULL || erase == NULL)
    {
        ESP_LOGE(TAG, "cfg / erase 不能为 NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->pin < 0 || cfg->hold_ms == 0)
    {
        ESP_LOGE(TAG, "参数非法：pin=%d hold_ms=%u", (int)cfg->pin, (unsigned)cfg->hold_ms);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_fr.started)
    {
        ESP_LOGW(TAG, "已经启动过，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    /* 输入 + 上/下拉：BOOT 键（GPIO0）板上有外部上拉，这里再开内部上拉是为了
     * 兼容没有外部上拉的裸模块；active_low=false 时反过来用下拉。 */
    gpio_init_cfg_t io = GPIO_INIT_CFG_DEFAULT();
    io.mode = GPIO_MODE_INPUT;
    io.pull = cfg->active_low ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY;

    const esp_err_t err = gpio_config_init(cfg->pin, &io);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO%d 配置失败: %s", (int)cfg->pin, esp_err_to_name(err));
        return err;
    }

    s_fr.cfg = *cfg;
    if (s_fr.cfg.poll_ms == 0)
    {
        s_fr.cfg.poll_ms = FR_DEFAULT_POLL_MS;
    }
    s_fr.erase = erase;
    s_fr.on_evt = on_evt;
    s_fr.ctx = ctx;

    if (xTaskCreate(fr_task, "factory_reset", FR_TASK_STACK, NULL, FR_TASK_PRIO, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "任务创建失败（栈 %d）", FR_TASK_STACK);
        return ESP_ERR_NO_MEM;
    }

    s_fr.started = true;
    return ESP_OK;
}
