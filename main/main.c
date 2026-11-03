#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "app_wifi.h"        /* WiFi / 配网初始化接口（app_wifi_start）           */
#include "factory_reset.h"   /* BOOT 键长按恢复出厂设置                          */
#include "gpio.h"
#include "net_io.h"          /* 数据收发接口：3 个上行入口 + 1 个下行回调         */
#include "service_manager.h" /* 统一管理"现在跑哪个网络服务"                      */
#include "webconfig.h"

/* ---- 硬件引脚 ---- */
#define RUNNING_LIGHT GPIO_NUM_21 /*!< 状态指示灯（流水灯）所在引脚           */
#define RUNNING_LIGHT_HALF_MS 250 /*!< 电平保持时长：亮/灭各 250ms → 2Hz 闪烁 */

static const char* TAG = "main";

/* ===================== 版本 =====================
 * 由 menuconfig (Application identity) 配置，启动时打日志并灌给 webconfig。
 * 格式建议：vMAJOR.MINOR.PATCH[-PRERELEASE][+BUILD] */
#define FW_VERSION CONFIG_FW_VERSION
#define APP_VERSION CONFIG_APP_VERSION

/* 默认配置即"推挽输出 + 无上下拉 + 不启用中断"，正好用来推 LED：
 * 输出模式能驱动；LED 无需外部上拉；闪烁靠任务里 delay，不用中断。 */
static const gpio_init_cfg_t running_light_cfg = GPIO_INIT_CFG_DEFAULT();

/* BOOT 键长按恢复出厂设置期间置 true：状态灯从 2Hz 改成 8Hz 快闪，
 * 让用户"按够了没有"有个直观反馈（只有一颗灯，只能靠节奏区分）。
 * volatile：写方是 factory_reset 任务，读方是 running_light_task。 */
static volatile bool s_led_fast_blink = false;

/* 长按已经达成、正在擦配置 → 灯停止翻转、保持常亮，直到重启。
 * 与 fast_blink 分开是因为这一步的语义是"结束了，别再闪"。 */
static volatile bool s_led_hold_on = false;

static esp_err_t hardware_init(void);
static void running_light_task(void* arg);

#if CONFIG_FACTORY_RESET_ENABLE
static esp_err_t app_erase_all_settings(void* ctx);
static void app_factory_reset_evt(factory_reset_evt_t evt, void* ctx);
static void app_start_factory_reset(void);
#endif

/* ===================== 业务层看到的东西 =====================
 * 基础设施全部下沉到两个组件，本文件只做调用：
 *
 *   初始化　：app_wifi_start()（WiFi / 配网选路）
 *             net_io_start()   （收发框架：队列 + 两个常驻任务）
 *   数据发送：net_io_send_tcp_client() / net_io_send_tcp_server() / net_io_send_udp()
 *             非阻塞投队列，任意任务可调；返回 true 只代表"已入队"。
 *   数据接收：net_io_set_recv_cb(app_on_frame) 注册回调，收到数据在
 *             rx_consumer 任务里回调本文件的 app_on_frame()。
 *
 * 换协议（TCP 客户端 / 服务端 / UDP）不需要改这里的任何一行：service_manager
 * 切 worker，net_io 内部换通道，业务侧的收发调用保持不变。
 */

/* 业务处理体：收到一帧数据时由 net_io 的 rx_consumer 任务回调（协议解析写这里）。
 * 定义在文件末尾，先声明让 app_main 里的 net_io_set_recv_cb() 能引用。 */
static void app_on_frame(const net_io_frame_t* frame);

void app_main(void)
{
    /* 启动横幅：先报版本，方便从串口日志里定位"是不是新固件" */
    ESP_LOGI(TAG, "底层 APP 版本: %s", FW_VERSION);
    ESP_LOGI(TAG, "App 版本:       %s", APP_VERSION);
    webconfig_set_fw_version(FW_VERSION);
    webconfig_set_app_version(APP_VERSION);

    /* ---- 1) 硬件：指示灯引脚 ---- */
    if (hardware_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "硬件初始化失败，停在 app_main");
        return;
    }
    xTaskCreate(running_light_task, "running_light_task", 2048, NULL, 10, NULL);

    /* ---- 2) 网络收发层：队列 + 两个常驻 consumer 任务 ----
     * 必须排在 WiFi 之前：门控事件（AP_START / 拿到 IP）随时可能在
     * app_wifi_start() 内部同步触发，队列得先存在，那一下才不会丢数据。
     *
     * 队列、consumer 任务、重连 / 心跳 / 门控全在组件内部，业务只通过
     * net_io.h 的 3 个上行入口 + 1 个下行回调打交道。三个服务全关时也照样
     * 启动：任务阻塞在队列上不占 CPU，"要不要发"由 net_io_send_* 内部按
     * 编译开关返回 false 决定 —— 上层调用点不必套 #if。 */
    net_io_set_recv_cb(app_on_frame);
    ESP_ERROR_CHECK(net_io_start());

    /* ---- 3) WiFi / 配网：NVS 有配置就直连，没有就开配网热点 ----
     * 内部在连上网络时给 net_io 开门（net_io_set_net_ready），掉线 /
     * AP 客户端全走光时关门。业务侧不必管这个时序。 */
    if (app_wifi_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi / 配网初始化失败，停在 app_main");
        return;
    }

    /* ---- 启动 service_manager ----
     * init 阶段：
     *   - 读 NVS 看上次选了什么模式；
     *   - 把 /api/service-mode 挂到 webconfig 的 HTTP 服务器上（等它起来）；
     *   - 设好 pending，等待 manager 任务起来后真正拉起第一个 service。
     * start 阶段：创建 manager 任务，它按 100ms 一轮检查 pending/active 差异。
     *
     * 【三个服务全关时这两个是空壳】见 service_manager.c 末尾：不建 manager
     * 任务、不挂 /api/service-mode、不碰 NVS —— 所以这里不必再套 #if。 */
    ESP_ERROR_CHECK(service_manager_init());
    service_manager_start();

#if CONFIG_FACTORY_RESET_ENABLE
    /* 放在最后：擦除要走 NVS（前面 webconfig 已初始化好），而且它是"救砖"
     * 手段，不该跟正常启动流程抢资源。排在最末也保证长按 BOOT 时整机已经
     * 稳定跑起来了。 */
    app_start_factory_reset();
#endif /* CONFIG_FACTORY_RESET_ENABLE */
}

/**
 * @brief 硬件初始化：只负责引脚（网络归 app_wifi_start，收发归 net_io_start）
 */
static esp_err_t hardware_init(void)
{
    ESP_ERROR_CHECK(gpio_config_init(RUNNING_LIGHT, &running_light_cfg));
    return ESP_OK;
}

/**
 * @brief 状态指示灯（流水灯）：周期翻转 GPIO 电平，供肉眼确认"程序还活着"
 *
 * @note 刻意只做一件事——翻转引脚。不碰外设、不写日志：它要在系统刚启动、
 *       WiFi / TCP 都还没就绪时就工作，越简单越不会出问题。
 *
 * @note 周期 = RUNNING_LIGHT_HALF_MS × 2。用 pdMS_TO_TICKS() 换算成 tick：
 *       FreeRTOS tick 默认 100Hz（1 tick = 10ms），若直接写 250 会被当成
 *       250 个 tick（2.5 秒），所以必须除一次。
 *
 * @note BOOT 键长按期间改走 60ms 半周期（约 8Hz 快闪），作为"恢复出厂设置
 *       倒计时"的反馈——板上只有这一颗灯，只能靠节奏区分。
 *
 * @note 栈 2048 字节、优先级 10（见 app_main）：它只调用 gpio_write 和
 *       vTaskDelay，几乎不占 CPU，栈给的很宽裕。
 */
static void running_light_task(void* arg)
{
    (void)arg;

    uint32_t level = 0; /* 当前电平：0=灭，1=亮 */

    while (1)
    {
        /* 恢复出厂设置已触发（正在擦 NVS、300ms 后重启）：常亮，别再闪 */
        if (s_led_hold_on)
        {
            (void)gpio_write(RUNNING_LIGHT, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* 每轮重算：s_led_fast_blink 由 factory_reset 任务随时翻转 */
        const TickType_t half =
            pdMS_TO_TICKS(s_led_fast_blink ? 60 : RUNNING_LIGHT_HALF_MS);

        level ^= 1; /* 翻转电平，省掉"写 1 睡一次、写 0 睡一次"的两段重复 */
        /* 引脚模式在 hardware_init() 里已校验过，这里失败也无补救动作，忽略返回值 */
        (void)gpio_write(RUNNING_LIGHT, level);
        vTaskDelay(half);
    }
}

#if CONFIG_FACTORY_RESET_ENABLE
/**
 * @brief 擦除所有保存的设置（BOOT 键长按 → 恢复出厂设置的真正动作）
 *
 * @note "擦哪些"交给 webconfig_erase_all_settings() 统一决定：网页上的
 *       "恢复出厂设置"按钮走的是同一个函数，两个入口不会出现"清的范围不一样"。
 *       这里只补一条总结日志（按键路径没有 HTTP 响应，全靠串口提示）。
 *
 * @note 擦完不重启——重启由 factory_reset 组件统一做，这里只管擦。
 *
 * @return 第一个失败的错误码；全成功返回 ESP_OK。
 */
static esp_err_t app_erase_all_settings(void* ctx)
{
    (void)ctx;

    const esp_err_t err = webconfig_erase_all_settings();
    if (err == ESP_OK)
    {
        ESP_LOGW(TAG, "已清除所有设置（设备名 / 网络 / 通讯 / 服务模式）");
    }
    else
    {
        /* 逐项错误已由 webconfig 打出来，这里只标记整体结果 */
        ESP_LOGE(TAG, "清除设置不完整: %s（仍将重启）", esp_err_to_name(err));
    }

    return err;
}

/**
 * @brief BOOT 键长按事件：只做提示（灯 / 日志），不碰配置
 *
 * @note 真正的擦除在 app_erase_all_settings()，由组件在长按达成时调用。
 */
static void app_factory_reset_evt(factory_reset_evt_t evt, void* ctx)
{
    (void)ctx;

    switch (evt)
    {
    case FACTORY_RESET_EVT_HOLD_BEGIN:
        s_led_fast_blink = true;
        ESP_LOGW(TAG, "BOOT 键按住中（状态灯快闪）：保持 %d 秒后恢复出厂设置，松手可取消",
                 CONFIG_FACTORY_RESET_HOLD_MS / 1000);
        break;

    case FACTORY_RESET_EVT_HOLD_ABORT:
        s_led_fast_blink = false;
        break;

    case FACTORY_RESET_EVT_TRIGGER:
        /* 常亮，直到 300ms 后重启：明确区别于"还在倒计时"的快闪 */
        s_led_fast_blink = false;
        s_led_hold_on = true;
        break;

    default:
        break;
    }
}

/**
 * @brief 启动 BOOT 键长按监听（失败只警告：不影响其余功能）
 */
static void app_start_factory_reset(void)
{
    factory_reset_cfg_t fr = FACTORY_RESET_CFG_DEFAULT();
    fr.pin = (gpio_num_t)CONFIG_FACTORY_RESET_GPIO;
    fr.hold_ms = CONFIG_FACTORY_RESET_HOLD_MS;

    const esp_err_t err =
        factory_reset_start(&fr, app_erase_all_settings, app_factory_reset_evt, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "BOOT 键恢复出厂设置未启动: %s（其余功能不受影响）", esp_err_to_name(err));
    }
}
#endif /* CONFIG_FACTORY_RESET_ENABLE */

/* ===================== 业务处理体（日常开发主要改这里）=====================
 * 数据由 net_io 的 rx_consumer 任务回调进来，本函数就运行在那个任务上下文
 * （栈 4096）：不要在这里做长耗时操作，也不能阻塞等待同一个任务。
 *
 * 【发送接口一览】（全部非阻塞，返回 true 只代表已入队，不代表已到对端）
 *   net_io_send_tcp_client(data, len)                              TCP 长连接
 *   net_io_send_tcp_server(frame->peer_fd, data, len)              回给服务端某客户端
 *   net_io_send_udp(frame->peer_ip, frame->peer_port, data, len)   UDP 对端
 *   net_io_send_back(frame, data, len)                             按来源原路发回
 *
 * 【数据从哪来】frame->link 标明这一帧来自哪个通道：
 *   NET_IO_LINK_TCP_CLIENT / NET_IO_LINK_TCP_SERVER / NET_IO_LINK_UDP
 * 协议解析不需要关心当前是哪种服务模式 —— service_manager 已经切好了。
 *
 * 【frame->data 的生命周期】指向 rx 任务栈上的缓冲，**本函数返回后失效**；
 * 需要留存请自己 memcpy，不要保存这个指针。
 */
static void app_on_frame(const net_io_frame_t* frame)
{
    if (frame == NULL || frame->len == 0)
    {
        return;
    }

    /* 心跳回包直接丢掉：echo 服务器会把心跳原样发回，若再回传就无限乒乓 */
    if (net_io_heartbeat_is_echo(frame->data, frame->len))
    {
        return;
    }

    /* ---- 当前实现：原路回传（echo）----
     * 接真实业务时把这里换成你的协议解析，例如：
     *   if (frame->data[0] == 0xA5) { 解析指令...; }
     * 需要回数据时调用上面任意一个 net_io_send_* 即可。 */
    if (!net_io_send_back(frame, frame->data, frame->len))
    {
        ESP_LOGW(TAG, "回传失败（上行队列满），本段 %u 字节被丢弃", (unsigned)frame->len);
    }
}
