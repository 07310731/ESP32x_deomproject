#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"  /* xQueueCreate / xQueueSend / xQueueReceive（上行队列） */
#include "freertos/semphr.h" /* xSemaphoreCreateMutex（保护 g_fd）                   */

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h" /* esp_timer_get_time()：给 send 打耗时探针 */
#include "esp_netif.h" /* ip_event_got_ip_t / IPSTR，用于打印管理页地址 */
#include "esp_wifi.h"

#include "gpio.h"
#include "uart.h"
#include "webconfig.h"
#include "wifi.h"

#include <stdbool.h>    /* TCP 任务里的 volatile bool s_sta_got_ip 需要 */
#include "tcp_client.h" /* TCP 客户端封装组件                                */

/* ---- 硬件引脚 ---- */
#define RUNNING_LIGHT GPIO_NUM_21 /*!< 状态指示灯（流水灯）所在引脚           */
#define RUNNING_LIGHT_HALF_MS 250 /*!< 电平保持时长：亮/灭各 250ms → 2Hz 闪烁 */

static const char* TAG = "main";

/* 默认配置即"推挽输出 + 无上下拉 + 不启用中断"，正好用来推 LED：
 * 输出模式能驱动；LED 无需外部上拉；闪烁靠任务里 delay，不用中断。 */
static const gpio_init_cfg_t running_light_cfg = GPIO_INIT_CFG_DEFAULT();

static esp_err_t hardware_init(void);
static void running_light_task(void* arg);
static void log_link_diag(void);

/* 适配层（见文件末尾）：把 webconfig 的端口回调用 wifi 组件实现出来 */
static esp_err_t app_ap_start(const webconfig_ap_cfg_t* ap, void* ctx);
static esp_err_t app_sta_apply(const char* ssid, const char* password, void* ctx);
static void wifi_event_forward(void* arg, esp_event_base_t base, int32_t id, void* data);

/**
 * @brief 配网组件配置：热点参数用默认值，两个端口回调把 webconfig 接到 wifi 组件上
 *
 * @note webconfig 自己不认识 esp_wifi，开热点 / 切连接全靠这两个回调；
 *       连接结果则通过 wifi_event_forward() 反向通知给它。
 *
 * @note keep_httpd = true：配网结束后 HTTP 服务器继续运行，设备连上路由器后
 *       仍可用它在路由器网段的 IP 打开管理页（地址见串口日志），
 *       不用重新连热点就能改配置。
 */
static const webconfig_cfg_t webconfig_cfg = {
    .ap = WEBCONFIG_AP_CFG_DEFAULT(),
    .port = {
        .ap_start = &app_ap_start,
        .sta_apply = &app_sta_apply,
    },
    .keep_httpd = true,
    .ctx = NULL,
};

/* ===================== TCP 长连接业务框架 =====================
 * 面向"无心跳 + 快速响应 + 随时上传 + 长连接"的场景。结构：
 *
 *         tcp_conn_task                       服务器
 *      ┌──────────────────┐    recv   ┌──────────────┐
 *      │  连接管理 + 接收   │◄─────────│              │
 *      │                  │    send   │              │
 *      └────────┬─────────┘──────────►└──────────────┘
 *               │ 收到数据立刻回调（零排队、零 sleep）
 *               ▼
 *        tcp_app_on_recv()   ← 【目前原样回传；协议解析写这里】
 *
 *         tcp_tx_task
 *      ┌──────────────────┐    send   ┌──────────────┐
 *      │  只负责上行发送    │──────────►│    服务器      │
 *      └────────▲─────────┘           └──────────────┘
 *               │ 阻塞等队列（空闲不占 CPU）
 *          g_tx_queue
 *               ▲
 *               │ 非阻塞投递，任何任务随时可调
 *          tcp_app_send()      ← 【你的业务代码用这个"上传"】
 *
 * 【为什么拆成两个任务】
 *   把"发"和"收"塞进同一个任务，任务阻塞在 recv 上时就没法立刻 send，
 *   上行延迟被 recv 超时拖着走。拆开后：
 *     上行延迟 ≈ 队列投递 + task 调度   （约 1ms）
 *     下行延迟 ≈ 收包 + 立刻回调        （约 1ms）
 *   两个任务都阻塞在事件上（xQueueReceive / recv），空闲时几乎不占 CPU。
 *
 * 【没有心跳包，靠什么发现断线】
 *   1. 对端主动 close  → recv 返回 0 → ESP_ERR_INVALID_STATE，立刻发现；
 *   2. 发送失败        → send 返回错误，立刻发现；
 *   3. 对端静默失联（拔网线 / 断电）→ 只能靠 TCP keepalive 探针：
 *      连接空闲 keepalive_idle_s 秒后开始探测，每 keepalive_interval_s 秒
 *      发一次，连续 keepalive_count 次收不到应答就判死。按下面的参数，
 *      最坏约 20 + 5*3 = 35 秒能发现。这就是"无应用层心跳"的兜底手段。
 */
#define TCP_APP_HOST "192.168.1.35" /* 服务器地址（局域网自定义 TCP 服务） */
#define TCP_APP_PORT 8899

#define TCP_APP_RECV_TIMEOUT_MS                                                                    \
    1000 /* recv 单次阻塞上限：只是"隔多久醒来查一次连接                        \
            状态"，不影响响应延迟——有数据会立刻返回 */
#define TCP_APP_RECONNECT_MS 2000 /* 连接断开后多久重连 */

/* ---- 无心跳场景的断线探测参数（TCP keepalive） ---- */
#define TCP_APP_KEEPALIVE_IDLE_S 20 /* 连接空闲多久后开始探活（秒） */
#define TCP_APP_KEEPALIVE_INTVL_S 5 /* 探针重发间隔（秒）           */
#define TCP_APP_KEEPALIVE_CNT 3     /* 连续几次无响应判定断开       */

/* 【延迟探针阈值】单帧 send 循环耗时超过它就打警告（单位微秒）。
 * 正常值 <1ms：只是把数据拷进 lwIP 发送缓冲，物理发出去是驱动的事。
 * 超过 20ms 说明卡在协议栈/射频侧（发送缓冲满、等 ACK 或窗口、重传），
 * 与 tcp_tx_task 的调度无关。 */
#define TCP_APP_SEND_SLOW_US (20LL * 1000)

/* 【接收帧内容调试打印】默认 0=关闭。
 * 打开后每收到一段数据就 hexdump + ASCII 打印原始内容（排查协议内容时
 * 很有用），但它是同步阻塞写 UART 的 —— 详细代价见 tcp_conn_task 接收
 * 循环里的说明，1KB 数据约阻塞数百毫秒，会把整条"接收 → 发送"路径拖慢。 */
#define TCP_APP_LOG_RX_DUMP 1

/* ---- 上行队列参数 ----
 * 定长队列：每个元素都按"最大帧"预分配，heap 占用 ≈ 深度 × (MAX_FRAME + 4) + 80B。
 * 深度只用来吸收"突发 + 调度抖动"（tcp_tx_task 发 1KB 只要几 ms），
 * 没法解决"产生速度 > WiFi 发送速度"——那种情况再大也会满，得靠上层
 * 限流或丢帧策略。所以按实际需要给，不必盲目加大。 */
#define TCP_APP_TX_QUEUE_LEN 4 /* 队列最多缓存几帧；满了 tcp_app_send() 返回 false */
#define TCP_APP_MAX_FRAME 1024 /* 单帧最大字节数（1KB）；队列元素按这么大分配      */

/* 上行队列的元素：一帧待发送数据。
 * 数据直接内嵌在结构体里（而不是"指针 + malloc"）：队列按值拷贝，
 * 发送完自动释放，不用管内存生命周期，也更省 heap。 */
typedef struct
{
    uint16_t len;                    /* 本帧有效字节数              */
    uint8_t data[TCP_APP_MAX_FRAME]; /* 本帧内容（最多 1024 字节） */
} tcp_tx_item_t;

static int g_fd = -1;                       /* 当前长连接 fd；-1 表示未连接     */
static SemaphoreHandle_t g_fd_mutex = NULL; /* 保护 g_fd 的读写 + send 的原子性 */
static QueueHandle_t g_tx_queue = NULL;     /* 上行队列：tcp_app_send → tcp_tx_task */

/* 由 wifi_event_forward() 双向维护：
 *   IP_EVENT_STA_GOT_IP        → 置 true （"STA 已联网，可走 TCP 业务"）
 *   WIFI_EVENT_STA_DISCONNECTED → 置 false（"STA 已掉线，TCP 业务暂停"）
 *
 * tcp_conn_task 看到 false 就挂起等待，看到 true 就推进；这样不管 WiFi
 * 是首次连得慢、还是中途断开又恢复，TCP 任务都会跟着网状态进出，不会
 * 因为启动时还没联网就退出后"起不来"。
 *
 * volatile 修饰：它会被事件回调改写（可能跑在中断上下文），编译器不能
 * 把值缓存到寄存器，每次必须重新从内存读。 */
static volatile bool s_sta_got_ip = false;

static void tcp_conn_task(void* arg);                         /* 连接管理 + 接收  */
static void tcp_tx_task(void* arg);                           /* 上行发送         */
static void tcp_app_on_recv(const uint8_t* data, size_t len); /* 收数据的业务回调 */

void app_main(void)
{
    if (hardware_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "硬件初始化失败，停在 app_main");
        return;
    }

    xTaskCreate(running_light_task, "running_light_task", 2048, NULL, 10, NULL);

    /* ---- 启动 TCP 长连接框架 ----
     * 1) 先建好互斥锁和上行队列，后面两个任务才敢用它们；
     * 2) 连接/接收任务栈给大些（getaddrinfo 和协议解析都吃栈）；
     * 3) 发送任务只做 memcpy + send，栈可以小一些。
     * 两个任务内部自己等 IP，app_main 这边不需要同步。 */
    g_fd_mutex = xSemaphoreCreateMutex();
    g_tx_queue = xQueueCreate(TCP_APP_TX_QUEUE_LEN, sizeof(tcp_tx_item_t));
    configASSERT(g_fd_mutex != NULL && g_tx_queue != NULL);

    /* 优先级：tcp_tx(6) 高于 tcp_conn(5)。
     * 两者同优先级时，若服务器持续发数据，tcp_conn 会因为"总有数据可收"
     * 一直不阻塞，只能等同优先级时间片轮转（100Hz → 10ms 一片）才轮到
     * tcp_tx，上行延迟就变成随机十几毫秒。调高 tcp_tx 后，投队列一完成
     * 它立刻抢占发送，延迟降到 task 切换级别（约 1ms）。 */
    xTaskCreate(tcp_conn_task, "tcp_conn", 4096, NULL, 5, NULL);
    xTaskCreate(tcp_tx_task, "tcp_tx", 2560, NULL, 6, NULL);
}

/**
 * @brief 一次性把 LED 和 WiFi 都配好；WiFi 优先用配网组件存在 NVS 里的凭证，
 *        没配过网就开热点，等用户连上热点后通过 http://192.168.4.1/ 提交。
 *
 * @note 两条路都会把管理页面准备好，区别只是设备待在哪个网络里：
 *       - 有凭证：STA 直连路由器，页面挂在设备拿到的 IP 上（地址见串口日志）；
 *       - 没凭证：开配网热点，页面挂在 192.168.4.1 上。
 *
 * @note 最后把 WiFi 的连接结果转发给配网组件，它才能判断"新凭证是不是填错了"
 *       （连续失败会自动重开配网热点）。
 */
static esp_err_t hardware_init(void)
{
    ESP_ERROR_CHECK(gpio_config_init(RUNNING_LIGHT, &running_light_cfg));

    esp_err_t err;
    webconfig_sta_cfg_t saved;

    if (webconfig_load_sta_cfg(&saved))
    {
        ESP_LOGI(TAG, "使用已保存的 WiFi 配置连接: %s", saved.ssid);

        wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
        strlcpy(wifi_cfg.sta_ssid, saved.ssid, sizeof(wifi_cfg.sta_ssid));
        strlcpy(wifi_cfg.sta_password, saved.password, sizeof(wifi_cfg.sta_password));
        err = wifi_config_init(&wifi_cfg);

        /* 不开热点，但把管理页挂上去：连上路由器后用它拿到的 IP 访问 */
        if (err == ESP_OK)
        {
            err = webconfig_httpd_start(&webconfig_cfg);
        }
    }
    else
    {
        ESP_LOGI(TAG, "没有已保存的 WiFi 配置，启动配网热点");
        err = webconfig_start(&webconfig_cfg);
    }

    if (err != ESP_OK)
    {
        return err;
    }

    /* 关掉 WiFi 省电（modem sleep），换取低延迟。
     * 默认的 WIFI_PS_MIN_MODEM 下，射频空闲时周期性休眠、只按 DTIM 周期
     * 醒来；数据到了要等它醒，延迟在几十~几百 ms 之间随机跳动，表现就是
     * "响应忽快忽慢"。本场景要快速响应，关掉；代价是功耗升高。
     *
     * 【务必看返回值】本函数在"配网路径"下被调用时，驱动还停在 AP-only 模式
     * （STA 接口尚未启用），这一行有失败的可能；而两个启动路径唯一的区别就
     * 在这里。失败会被记成警告，STA 连上后 wifi_event_forward() 还会再下发
     * 一次并打印实际生效值。 */
    const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK)
    {
        ESP_LOGW(TAG, "初始化时关闭 WiFi 省电失败: %s（STA 连上后会再试一次）",
                 esp_err_to_name(ps_err));
    }

    /* 注册事件转发（只影响"连不上自动回配网"和打印管理页地址，失败不拦着正常功能） */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_forward,
                                              NULL, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "注册 WiFi 事件转发失败，密码填错时不会自动重开配网热点: %s",
                 esp_err_to_name(err));
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_forward,
                                              NULL, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "注册 IP 事件转发失败，不会打印管理页地址: %s", esp_err_to_name(err));
    }

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
 * @note 栈 2048 字节、优先级 10（见 app_main）：它只调用 gpio_write 和
 *       vTaskDelay，几乎不占 CPU，栈给的很宽裕。
 */
static void running_light_task(void* arg)
{
    (void)arg;

    const TickType_t half = pdMS_TO_TICKS(RUNNING_LIGHT_HALF_MS);
    uint32_t level = 0; /* 当前电平：0=灭，1=亮 */

    while (1)
    {
        level ^= 1; /* 翻转电平，省掉"写 1 睡一次、写 0 睡一次"的两段重复 */
        /* 引脚模式在 hardware_init() 里已校验过，这里失败也无补救动作，忽略返回值 */
        (void)gpio_write(RUNNING_LIGHT, level);
        vTaskDelay(half);
    }
}

/* ===================== webconfig 适配层 =====================
 * webconfig 组件只认两个回调，不知道底下是哪套无线栈：
 *   - ap_start  : 开配网热点；
 *   - sta_apply : 关掉热点、用用户在网页上填的凭证去连接。
 * 这里用 wifi 组件把它们实现出来；反向的"连上/断开"则由 wifi_event_forward()
 * 转成 webconfig_notify_sta_*()，供配网组件做连接失败自愈。
 */

static esp_err_t app_ap_start(const webconfig_ap_cfg_t* ap, void* ctx)
{
    (void)ctx;

    /* wifi_config_init() 可重复调用：会复用已初始化的全局资源
     * （netif / 事件循环 / 驱动 / 回调），只重新下发模式与参数，
     * 所以"连接失败自动重开热点"时再调一次也是安全的。 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    wifi_cfg.mode = WIFI_MODE_AP;
    strlcpy(wifi_cfg.ap_ssid, ap->ssid, sizeof(wifi_cfg.ap_ssid));
    strlcpy(wifi_cfg.ap_password, ap->password, sizeof(wifi_cfg.ap_password));
    wifi_cfg.ap_channel = ap->channel;
    wifi_cfg.ap_max_conn = ap->max_conn;

    return wifi_config_init(&wifi_cfg);
}

static esp_err_t app_sta_apply(const char* ssid, const char* password, void* ctx)
{
    (void)ctx;

    /* 切回 STA 模式连接：wifi_update_config() 会断开当前 AP、用新凭证重连 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    strlcpy(wifi_cfg.sta_ssid, ssid, sizeof(wifi_cfg.sta_ssid));
    strlcpy(wifi_cfg.sta_password, password, sizeof(wifi_cfg.sta_password));

    return wifi_update_config(&wifi_cfg);
}

static void wifi_event_forward(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;

    if (base == WIFI_EVENT)
    {
        if (id == WIFI_EVENT_STA_CONNECTED)
        {
            webconfig_notify_sta_connected();

            /* 【低延迟的关键一步，也是"配网路径变慢"的修复点】
             * hardware_init() 里那次 esp_wifi_set_ps(WIFI_PS_NONE) 是在 WiFi 刚
             * 起来时调的。两条启动路径在那一刻的驱动状态不同：
             *   - 重启直连（有凭证）：那一刻模式已经是 STA，设置直接落在 STA 上；
             *   - 首次配网：那一刻模式还是 AP-only（STA 接口未启用），
             *     httpd 回调里 wifi_update_config() 才把模式切成 STA 并新建
             *     STA 接口 —— 这个"后出生"的 STA 接口不保证继承之前那次设置，
             *     可能退回默认的 WIFI_PS_MIN_MODEM（modem sleep）。
             * modem sleep 一旦生效，收包要等 DTIM 周期才醒：服务器发来的数据晚
             * 几十~几百毫秒才被收到，应答/上行跟着一起晚，表现就是"收发不及时
             * 但代码日志一切正常"。所以这里每次连上都幂等地再下发一次，
             * 并把驱动里实际生效的值打印出来（0=NONE 才是我们要的）。 */
            /* 先读"连上前"的实际值：这是判断根因的关键证据 ——
             * 配网路径下若这里是 1（MIN_MODEM），就说明 hardware_init() 那次
             * 设置没落到这个"后建出来"的 STA 接口上，问题实锤。 */
            wifi_ps_type_t ps_before = WIFI_PS_MIN_MODEM;
            (void)esp_wifi_get_ps(&ps_before);

            const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
            if (ps_err != ESP_OK)
            {
                ESP_LOGW(TAG, "关闭 WiFi 省电失败: %s", esp_err_to_name(ps_err));
            }
            /* 连上后再读一次实际生效值：0=NONE（我们要的），1=MIN_MODEM。
             * 若这里还是 1，说明这次下发没生效，得继续查驱动状态。 */
            wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
            (void)esp_wifi_get_ps(&ps);
            ESP_LOGI(TAG,
                     "WiFi 省电模式: 连上前 PS=%d → 下发后 PS=%d（0=NONE 低延迟；1=MIN_MODEM 会有 "
                     "DTIM 级延迟）",
                     (int)ps_before, (int)ps);
        }
        else if (id == WIFI_EVENT_STA_DISCONNECTED)
        {
            webconfig_notify_sta_disconnected();
            /* STA 掉线 → IP 已无效：把 s_sta_got_ip 清掉，
             * 让 tcp_demo_task 暂停等 WiFi 重新连上。
             * esp_wifi 默认会自己重连，GOT_IP 事件一回来又会
             * 把 s_sta_got_ip 置回 true。 */
            s_sta_got_ip = false;
        }
        return;
    }

    /* 拿到 IP 说明设备已经挂在路由器网段上：管理页就在这个地址，
     * 打出来省得去路由器后台翻（DHCP 分配的地址不固定） */
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
        /* 给 TCP 长连接任务开门：让 tcp_conn_task 看到 s_sta_got_ip 后往下走 */
        s_sta_got_ip = true;
        ESP_LOGI(TAG, "管理页面: http://" IPSTR "/", IP2STR(&e->ip_info.ip));
        log_link_diag(); /* 一行链路诊断，便于对比"配网路径"和"重启直连"的差异 */
    }
}

/**
 * @brief 打印一行链路诊断（模式 / 省电 / RSSI / 信道 / 协议）
 *
 * @note 只为排查"两条启动路径行为不一致"这类问题，正常运行时也可以留着
 *       （每拿到一次 IP 才打印一行）。看三个重点值：
 *       - mode：1=STA，3=APSTA。配网后若还是 3，说明热点没关掉，射频要在
 *         AP 与 STA 之间分时，收发必然变慢；
 *       - PS  ：1=MIN_MODEM 表示 modem sleep 还开着 → 收包要等 DTIM 周期；
 *       - RSSI：低于 -75 dBm 时重传明显增多，延迟抖动变大。
 */
static void log_link_diag(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);

    wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
    (void)esp_wifi_get_ps(&ps);

    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        ESP_LOGI(TAG, "链路诊断: mode=%d PS=%d RSSI=%d 信道=%d 11b/g/n=%d/%d/%d", (int)mode,
                 (int)ps, ap.rssi, ap.primary, ap.phy_11b, ap.phy_11g, ap.phy_11n);
    }
    else
    {
        ESP_LOGI(TAG, "链路诊断: mode=%d PS=%d（此刻读不到 AP 信息）", (int)mode, (int)ps);
    }
}

/* ===================== TCP 长连接实现 =====================
 * 两个任务各管一件事，互不阻塞：
 *   tcp_conn_task : 等 IP → open → 循环 recv → 出错 close 重连
 *   tcp_tx_task   : 阻塞等 g_tx_queue → send
 * 收发共用同一个 fd，用 g_fd_mutex 保证"send 期间不会被 close"。
 */

/**
 * @brief 【业务上传入口】把一帧数据投到上行队列，立刻返回
 *
 * 非阻塞、线程安全：任何任务里都能调；真正的 send() 由 tcp_tx_task 去做，
 * 所以调用者不会被网络阻塞。适合"随时都可能要上传数据"的场景。
 *
 * @param data 待上传数据，不可为 NULL
 * @param len  字节数，1 ~ TCP_APP_MAX_FRAME
 *
 * @return
 *   - true ：已进入发送队列（注意：不代表此刻已发到对端）
 *   - false：队列满 / 未初始化 / 参数非法（本帧被丢弃）
 *
 * @note 队列满意味着"业务产生速度 > 网络发送速度"。上层要么重试、
 *       要么丢弃、要么落盘，由你的业务决定。
 *
 * @note 如果"上传入口"要给别的组件（如 sensors / cmd 任务）调用，
 *       把这个函数原型挪到一个共享头文件里、include 过去即可。
 */
bool tcp_app_send(const void* data, size_t len)
{
    if (g_tx_queue == NULL || data == NULL || len == 0 || len > TCP_APP_MAX_FRAME)
    {
        return false;
    }

    tcp_tx_item_t item = { 0 };
    item.len = (uint16_t)len;
    memcpy(item.data, data, len);

    /* 0 超时：队列满不等待，立刻返回 false（由调用方决定怎么处理） */
    return xQueueSend(g_tx_queue, &item, 0) == pdTRUE;
}

/**
 * @brief 【收数据入口】当前实现是"原样回传（echo）"
 *
 * 由 tcp_conn_task 在读到数据后直接调用（在 TCP 任务上下文，不是中断）。
 *
 * @note 现在把收到的每个字节原样投回上行队列，由 tcp_tx_task 发回服务器，
 *       方便先验证"收 → 发"整条链路通不通。
 *
 *       接真实业务时，把函数体换成你的协议解析 —— 注意 TCP 是"字节流"，
 *       一次 recv 可能拿到半包、也可能粘了几条消息，需要自己维护
 *       接收缓冲区 / 状态机来做分帧。
 *
 * @warning 不要在这里做耗时阻塞的事（长 sleep、等锁、写 Flash、发 HTTP
 *          请求），否则会挡住 tcp_conn_task 收下一条数据。重活请投到别的任务。
 */
static void tcp_app_on_recv(const uint8_t* data, size_t len)
{
    /* 回传：把收到的这段原样投到上行队列（非阻塞），投完立刻返回继续收。 */
    if (!tcp_app_send(data, len))
    {
        ESP_LOGW(TAG, "回传失败：上行队列满，本段 %u 字节被丢弃", (unsigned)len);
    }
}

/**
 * @brief 上行发送任务：从队列取数据 → send
 *
 * 阻塞在 xQueueReceive，队列空时完全不占 CPU；
 * 一旦 tcp_app_send() 投递数据就立刻被唤醒发送。
 */
static void tcp_tx_task(void* arg)
{
    (void)arg;
    tcp_tx_item_t item;

    while (1)
    {
        /* portMAX_DELAY：空队列永远阻塞，直到有人投递 */
        if (xQueueReceive(g_tx_queue, &item, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        /* 用互斥锁包住"读 fd + send"：
         * 防止 send 期间连接任务刚好把 fd close 掉（连接任务 close 前
         * 也会先取这把锁）。只在这一小段持有锁，不会挡住连接任务太久。 */
        xSemaphoreTake(g_fd_mutex, portMAX_DELAY);

        const int fd = g_fd;
        if (fd >= 0)
        {
            /* 【必须循环补发】TCP 是字节流，一次 send 只保证写进去"一部分"：
             * lwIP 发送缓冲区装不下时，send 返回已写入的字节数（小于剩余
             * 长度）而不是报错。旧代码只 send 一次、也不看 written 够不够，
             * 剩下的字节就被静默丢掉了 —— 表现就是"回传的内容偶尔缺一截"，
             * 缺多少取决于当时缓冲区余量，看着很"随机"。
             * 所以这里按"写到写不动为止"循环补发。 */
            size_t sent = 0;
            esp_err_t err = ESP_OK;
            const int64_t send_t0_us = esp_timer_get_time(); /* 【延迟探针】见下方说明 */
            while (sent < item.len)
            {
                size_t n = 0;
                err = tcp_client_send(fd, item.data + sent, item.len - sent, &n);
                if (err != ESP_OK)
                {
                    break; /* 真错误（超时 / EPIPE 等） */
                }
                if (n == 0)
                {
                    err = ESP_FAIL; /* 没写进去又没报错：防死循环 */
                    break;
                }
                sent += n;
            }

            if (sent < item.len)
            {
                ESP_LOGW(TAG, "上行失败：%u/%u 字节已写出，剩余丢弃 (err=%d)", (unsigned)sent,
                         (unsigned)item.len, (int)err);
                /* 不在这里 close：交给 tcp_conn_task 的 recv 去发现并重连，
                 * 避免两个任务同时 close 同一个 fd。 */
            }
            else
            {
                ESP_LOGI(TAG, "TX %u 字节", (unsigned)sent);
            }

            /* 【延迟探针】send 循环本身的耗时：
             * 正常 <1ms（只是把数据拷进 lwIP 发送缓冲，真正发出去是驱动的活）；
             * 若打出几十~几百 ms，说明卡在协议栈/射频侧（发送缓冲满、等 ACK 或
             * 窗口、重传、modem sleep 错过收包窗口），要去查 WiFi 省电模式、
             * RSSI、信道，而不是改这里。只在异常时打印，正常帧不产生额外日志。 */
            const int64_t send_cost_us = esp_timer_get_time() - send_t0_us;
            if (send_cost_us > TCP_APP_SEND_SLOW_US)
            {
                ESP_LOGW(TAG, "send() 耗时 %lld ms —— 延迟出在协议栈/射频侧，不是本任务调度",
                         (long long)(send_cost_us / 1000));
            }
        }
        else
        {
            ESP_LOGW(TAG, "暂无连接，丢弃上行帧（%u 字节）", (unsigned)item.len);
        }

        xSemaphoreGive(g_fd_mutex);
    }
}

/**
 * @brief 连接管理与接收任务：等 IP → open → 循环 recv → 断开重连
 *
 * 接收路径零额外延迟：recv 一返回就交给 tcp_app_on_recv()，
 * 中间没有队列、没有 sleep、没有轮次控制。
 */
static void tcp_conn_task(void* arg)
{
    (void)arg;

    /* ---- 连接参数 ---- */
    tcp_client_cfg_t cfg = TCP_CLIENT_CFG_DEFAULT();
    strlcpy(cfg.host, TCP_APP_HOST, sizeof(cfg.host));
    cfg.port = TCP_APP_PORT;
    /* 关 Nagle：小包立刻发、低延迟，对"随时上行小数据"很关键 */
    cfg.nodelay = true;
    /* 无应用层心跳 → 靠 TCP keepalive 兜底探测对端静默失联 */
    cfg.keepalive = true;
    cfg.keepalive_idle_s = TCP_APP_KEEPALIVE_IDLE_S;
    cfg.keepalive_interval_s = TCP_APP_KEEPALIVE_INTVL_S;
    cfg.keepalive_count = TCP_APP_KEEPALIVE_CNT;
    /* recv 阻塞上限：只是"隔多久醒来看一眼连接状态"，不影响响应延迟 */
    cfg.recv_timeout_ms = TCP_APP_RECV_TIMEOUT_MS;

    /* 1) 启动阶段：无限等 IP（不因暂时无网退出任务） */
    ESP_LOGI(TAG, "TCP 长连接任务启动，等待 STA 拿到 IP（无限等）...");
    while (!s_sta_got_ip)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    ESP_LOGI(TAG, "IP 就绪，开始连接 %s:%d", TCP_APP_HOST, TCP_APP_PORT);

    while (1)
    {
        /* 2) WiFi 掉线 → 挂起等恢复（避免无意义的 connect） */
        if (!s_sta_got_ip)
        {
            ESP_LOGW(TAG, "STA 已掉线，暂停 TCP 业务，等 WiFi 恢复...");
            while (!s_sta_got_ip)
            {
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
            ESP_LOGI(TAG, "STA 已恢复，重连服务器");
        }

        /* 3) 建立长连接 */
        int fd = -1;
        esp_err_t err = tcp_client_open(&cfg, &fd);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "连接失败，%d ms 后重试", TCP_APP_RECONNECT_MS);
            vTaskDelay(TCP_APP_RECONNECT_MS / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "长连接已建立 (fd=%d)", fd);

        /* 4) 发布 fd：tcp_tx_task 从此刻起可以发送上行数据 */
        xSemaphoreTake(g_fd_mutex, portMAX_DELAY);
        g_fd = fd;
        xSemaphoreGive(g_fd_mutex);

        /* 5) 纯接收循环：没有"轮次"、不主动断开，收到就交给上层 */
        while (1)
        {
            uint8_t buf[TCP_APP_MAX_FRAME];
            size_t got = 0;
            err = tcp_client_recv(fd, buf, sizeof(buf), &got);

            if (err == ESP_ERR_INVALID_STATE)
            {
                ESP_LOGW(TAG, "对端关闭连接");
                break;
            }
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "recv 失败");
                break;
            }
            if (got == 0)
            {
                /* 超时没数据：正常空窗（对端静默失联由 keepalive 兜底），继续等 */
                continue;
            }

            /* 有数据 → 先打印"本次收到多少字节"，再立刻交给业务。
             *
             * 【整条接收路径上唯一的耗时点就在下面那个 hexdump 开关】
             * 日志是同步阻塞写 UART 的（本项目未开异步日志，115200 波特率
             * ≈ 每字符 87µs）。hexdump + ASCII 每 16 字节占一行，1KB 数据
             * 要打 100 多行、约 6000 字符 → 阻塞数百毫秒。这段时间全部发生
             * 在 tcp_app_on_recv() 投队列**之前**，等价于"收到了却压着不发"，
             * 表现就是"发送不及时、但收发日志一切正常"；而 send() 本身仍在
             * 毫秒级返回，所以 send() 耗时探针永远不报警。
             *
             * 结论：它只适合在排查协议内容时临时打开（TCP_APP_LOG_RX_DUMP=1），
             * 平时必须关掉。这行 `RX` 保留 —— 它和随后 `TX` 行的时间戳之差，
             * 就是"设备内部耗时"，是判断延迟在不在本机的第一手证据。 */
            ESP_LOGI(TAG, "RX %u 字节", (unsigned)got);
#if TCP_APP_LOG_RX_DUMP
            ESP_LOG_BUFFER_HEXDUMP(TAG, buf, got, ESP_LOG_INFO); /* 十六进制 */
            ESP_LOG_BUFFER_CHAR(TAG, buf, got);                  /* ASCII    */
#endif

            /* 有数据 → 立刻交给业务，最短延迟 */
            tcp_app_on_recv(buf, got);
        }

        /* 6) 连接坏了：先撤销 fd（挡住 tcp_tx_task），再 close */
        xSemaphoreTake(g_fd_mutex, portMAX_DELAY);
        g_fd = -1;
        xSemaphoreGive(g_fd_mutex);
        tcp_client_close(fd);

        ESP_LOGI(TAG, "%d ms 后重连", TCP_APP_RECONNECT_MS);
        vTaskDelay(TCP_APP_RECONNECT_MS / portTICK_PERIOD_MS);
    }
}
