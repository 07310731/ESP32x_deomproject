#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 恢复出厂设置（BOOT 键长按）的运行参数
     *
     * @note 组件只做"什么时候算长按够了"这一件事，"擦什么、怎么擦"由调用方
     *       通过 factory_reset_erase_fn 注入——与项目里 webconfig 用回调注入
     *       无线能力是同一套思路，组件本身不认识 NVS 里的任何业务字段。
     */
    typedef struct
    {
        gpio_num_t pin;      /*!< 按键引脚。ESP32 开发板的 BOOT 键是 GPIO0 */
        bool active_low;     /*!< true = 按下时引脚为低电平（BOOT 键带外部上拉，是这种） */
        uint32_t hold_ms;    /*!< 按住多少毫秒算"长按"，达到后触发擦除 + 重启 */
        uint32_t poll_ms;    /*!< 轮询周期，0 用默认 50ms；越大越省 CPU、但触发更迟钝 */
    } factory_reset_cfg_t;

/**
 * @brief 默认参数：GPIO0（BOOT 键）、按下为低、长按 5 秒、50ms 轮询一次
 *
 * @note 引脚 / 时长在 menuconfig 的 "Factory reset (BOOT button)" 里可改，
 *       应用层拿到宏之后覆盖这两个字段即可。
 */
#define FACTORY_RESET_CFG_DEFAULT()                                                                \
    ((factory_reset_cfg_t){                                                                        \
        .pin = GPIO_NUM_0,                                                                         \
        .active_low = true,                                                                        \
        .hold_ms = 5000,                                                                            \
        .poll_ms = 50,                                                                              \
    })

    /**
     * @brief 长按过程中的事件（供应用层做 LED / 日志提示）
     */
    typedef enum
    {
        FACTORY_RESET_EVT_HOLD_BEGIN = 0, /*!< 刚开始按住，还没到触发时长 */
        FACTORY_RESET_EVT_HOLD_ABORT,     /*!< 没按够就松开了，本次取消 */
        FACTORY_RESET_EVT_TRIGGER,        /*!< 按够了，马上擦除并重启 */
    } factory_reset_evt_t;

    /**
     * @brief 擦除所有设置的实现（由应用层提供）
     *
     * @param ctx 注册时传入的上下文
     *
     * @return
     *     - ESP_OK    全部擦除成功
     *     - 其它      有字段没擦掉；组件仍会重启（否则用户连重试的机会都没有），
     *                 只把错误码打在日志里
     *
     * @note 运行在 factory_reset 自己的任务里（不是中断上下文），可以调 NVS。
     */
    typedef esp_err_t (*factory_reset_erase_fn)(void* ctx);

    /**
     * @brief 长按事件回调（可选，传 NULL 表示不关心）
     *
     * @note 同样运行在组件任务里，但**不要在里面做长耗时操作**：调用完
     *       TRIGGER 之后组件只等 300ms 就重启了。
     */
    typedef void (*factory_reset_evt_fn)(factory_reset_evt_t evt, void* ctx);

    /**
     * @brief 启动 BOOT 键监听任务
     *
     * @param cfg    运行参数，不可为 NULL；poll_ms 传 0 用默认值
     * @param erase  擦除实现，不可为 NULL
     * @param on_evt 事件回调，可为 NULL
     * @param ctx    传给上述两个回调的上下文，可为 NULL
     *
     * @return
     *     - ESP_OK               任务已启动
     *     - ESP_ERR_INVALID_ARG  参数非法
     *     - ESP_ERR_INVALID_STATE 已经启动过（重复调用）
     *     - ESP_ERR_NO_MEM       任务创建失败
     *     - 其它                 引脚配置失败（gpio_config_init 的错误码）
     *
     * @note 任务栈 4096、优先级 3（低于所有业务任务）：轮询本身几乎不占 CPU，
     *       只在按住时每秒读 20 次引脚。
     *
     * @note 这个功能是"救砖"用的：网页进不去、WiFi 连不上时唯一能改回来的手段，
     *       所以不依赖网络、不依赖 webconfig 是否初始化成功，只要求 NVS 可写。
     */
    esp_err_t factory_reset_start(const factory_reset_cfg_t* cfg,
                                  factory_reset_erase_fn erase,
                                  factory_reset_evt_fn on_evt,
                                  void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_RESET_H */
