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

#include <stdbool.h>    /* TCP 任务里的 volatile bool s_net_ready 需要 */
#include <unistd.h>     /* close()：server 关停时直接关 client fd       */
#include "tcp_client.h" /* TCP 客户端封装组件                                */
#include "tcp_server.h" /* TCP 服务端封装组件                                */
#include "udp.h"        /* UDP 通讯封装组件                                   */

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
/* ---- TCP 服务器目标（当前写死，后期改由网页配置） ----
 * 【现状】地址是编译期常量，按 WiFi 模式各给一个默认值；
 * 【后续】网页配置做好后，改成从 NVS 读用户填写的地址来覆盖它 —— 只需要
 *         换掉"填值来源"，tcp_conn_task 里的连接逻辑一行都不用动。
 *
 * ⚠ 两种模式的网段不同，默认值不能混用：
 *   - STA 模式：设备挂在你家路由器网段（192.168.1.x），服务器是局域网里的上位机；
 *   - AP  模式：设备自己就是热点 192.168.4.1，接入的客户端由内置 DHCP 从
 *               192.168.4.2 起分配 —— 所以"服务器"必须落在这个网段里，
 *               且建议在上位机侧手工设成固定 IP（DHCP 分配顺序不保证）。
 */
#define TCP_APP_HOST_STA "192.168.1.38" /* STA 模式：局域网里的服务器 */
#define TCP_APP_HOST_AP "192.168.4.1"   /* AP 模式：接入热点的上位机  */
#define TCP_APP_PORT 8888               /* 两种模式共用同一个端口      */

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
 * 循环里的说明，1KB 数据约阻塞数百毫秒，会把整条"接收 → 发送"路径拖慢。
 * 注意：定义必须是 0；之前 commit 写成 1 时 hexdump 实际开着，"发送不及时"
 * 的修复看似生效但其实没生效 —— 这是注释/实现不一致的典型坑。 */
#define TCP_APP_LOG_RX_DUMP 0

/* ---- 上行队列参数 ----
 * 定长队列：每个元素都按"最大帧"预分配，heap 占用 ≈ 深度 × (MAX_FRAME + 4) + 80B。
 * 深度只用来吸收"突发 + 调度抖动"（tcp_tx_task 发 1KB 只要几 ms），
 * 没法解决"产生速度 > WiFi 发送速度"——那种情况再大也会满，得靠上层
 * 限流或丢帧策略。所以按实际需要给，不必盲目加大。 */
#define TCP_APP_TX_QUEUE_LEN 4 /* 队列最多缓存几帧；满了 tcp_app_send() 返回 false */
#define TCP_APP_MAX_FRAME 1024 /* 单帧最大字节数（1KB）；队列元素按这么大分配      */

/* ---- TCP 服务端 ----
 * 与客户端完全独立：监听 socket 与主动连接 socket 不冲突，AP/STA 模式下
 * 都可用 —— 通过 s_net_ready 统一门控（STA 拿 IP 或 AP 启动 = 就绪）。
 *
 * 【绑定地址】只监听本机 IP（tcp_app_get_local_ip：AP=192.168.4.1 /
 *         STA=路由器分配的地址），不绑 0.0.0.0；取不到 IP 时才回退通配。
 * 【IP 变化】s_net_ready 只反映"有没有网"，不反映"IP 是多少"。AP 自定义
 *         网段是在 esp_wifi_start() 之后才下发到 netif 的，配网改网段更是
 *         "不发事件、直接改 netif"（AP netif 没设 IP_MODIFIED 标志）。这类
 *         变化不改 s_net_ready，所以 server 另在 accept 循环里轮询比对 IP，
 *         一旦发现绑定的地址已经不在了就重建 listen —— 否则旧 socket 守着
 *         一个已失效的地址，新地址上的客户端永远连不上，且不会自愈。
 * 【端口】8888：与客户端的 8899 分开，两边可以同时跑。
 * 【最大并发】2：与 webconfig 配网热点默认 max_conn 对齐（配网场景 1~2
 *         个客户端足够）。想改大时同步把 webconfig_ap_cfg_t.max_conn 调大。
 * 【echo 缓冲区】1024：与 TCP_APP_MAX_FRAME 一致，每条消息上限 1KB。 */
#define TCP_SERVER_PORT 8888
#define TCP_SERVER_MAX_CLIENTS 2
#define TCP_SERVER_ECHO_BUF 1024
/* 【空闲断开】连接空闲多久没收到任何数据就主动断开（ms）。
 * 与 tcp_server 内置的 TCP keepalive 分工：
 *   - keepalive 兜"对端已经死了"（断电 / 掉线，内核发探测包发现不通）；
 *   - 这个兜"对端还活着但一直不说话"（进程卡死、客户端逻辑跑飞）。
 * 两者缺一，这种连接就会永远占着槽位（handler 的 recv 一直只是超时而非错误，
 * 循环不会退出），攒满 TCP_SERVER_MAX_CLIENTS 后服务端再也接不进新连接。
 * 阈值必须 >= 业务允许的最大静默间隔：如果上位机正常也会长时间不发数据，
 * 就要调大，否则会误杀正常连接。设为 0 = 关闭这道检查（只留底层 keepalive），
 * 适合"客户端连上后长期静默也算正常"的场景。 */
#define TCP_SERVER_IDLE_TIMEOUT_MS 30000

/* ---- UDP 通讯 ----
 * 设计要点：
 *   - 单 socket + 单任务 + echo：与 tcp_server 的 echo 风格对齐，先有"通路"，
 *     用例（命令 / 状态 / 发现）后面再加；
 *   - UDP 不区分 server / client：同一个 socket 既能 sendto（主动）也能
 *     recvfrom（被动），无连接、无槽位、无僵尸连接问题；
 *   - AP/STA 通用：复用 s_net_ready 门控 + tcp_app_get_local_ip() 取 bind IP，
 *     与 tcp_server_task 同一套路（IP 变了 close+open 重建）。
 *
 * 【端口】7777：与 TCP 服务端 8888 / TCP 客户端 8899 都错开，三者可同时跑。
 * 【缓冲区】1500：略小于典型 MTU；超过此值的数据报会被 IP 层分片甚至丢弃。
 *   如果上位机需要更大的帧，把宏改大即可（lwIP 限制 UDP 单帧 <= 65507）。 */
#define UDP_PORT 7777
#define UDP_RX_BUF_SIZE 1500

/* 上行队列的元素：一帧待发送数据。
 * 数据直接内嵌在结构体里（而不是"指针 + malloc"）：队列按值拷贝，
 * 发送完自动释放，不用管内存生命周期，也更省 heap。 */
typedef struct
{
    uint16_t len;                    /* 本帧有效字节数             */
    uint8_t data[TCP_APP_MAX_FRAME]; /* 本帧内容（最多 1024 字节） */
} tcp_tx_item_t;

static int g_fd = -1;                       /* 当前长连接 fd；-1 表示未连接     */
static SemaphoreHandle_t g_fd_mutex = NULL; /* 保护 g_fd 的读写 + send 的原子性 */
static QueueHandle_t g_tx_queue = NULL;     /* 上行队列：tcp_app_send → tcp_tx_task */

/* ---- TCP 服务端状态 ----
 * 每个槽位持有一个 client handler 任务的句柄和它负责的 fd。两个字段要一起看
 * 才能判断槽位状态（只看 fd 会误判）：
 *   fd >= 0  且 task != NULL → 占用中，连接活着
 *   fd == -1 且 task != NULL → fd 已被 server 关停回收，但 handler 还没退出来，
 *                              等它自己清完才可复用
 *   fd == -1 且 task == NULL → 真正的空槽，可以接受新连接
 *
 * 【谁在写】只有两个：tcp_server_task（分配槽位 / 关停时回收 fd / 建任务失败
 * 回滚）和 handler 自己（退出前清槽）。没有锁 —— 靠"所有权"而不是互斥保证正确：
 * handler 退出清理前先确认槽位仍然属于自己（task == 当前任务句柄），否则一律
 * 不动。这样晚退出的旧 handler 既不会 close 掉新连接的 fd（fd 编号可能已经被
 * 重新分配出去），也不会把新连接的槽位记录抹掉。
 *
 * 【buf 为什么在结构体里】
 * echo 缓冲区必须"每个 handler 任务独立一份"，否则两个并发客户端在同一
 * recv→send 窗口里会互相覆盖。C 的 static 局部变量是程序级共享（不是
 * 任务级），放函数里会把所有 handler 共享到同一份内存。
 * 直接放进槽位结构体最干净：BSS 区按槽位静态分配、零运行时分配、零共享，
 * 代价是 N × 1KB 的 BSS（TCP_SERVER_MAX_CLIENTS=2 → 2KB，可接受）。 */
typedef struct
{
    int fd;                           /*!< 该 handler 持有的 socket fd；-1 = 已被回收/未分配 */
    TaskHandle_t task;                /*!< client handler 任务句柄；NULL = 该槽位无人使用   */
    uint8_t buf[TCP_SERVER_ECHO_BUF]; /*!< handler 专属 echo 缓冲区         */
} tcp_server_slot_t;

static tcp_server_slot_t s_server_clients[TCP_SERVER_MAX_CLIENTS];

/* "server 是否在运行"标志：true = 接受新连接；false = 拒收 + 通知已连的
 * 客户端任务退出（每个 handler 在循环里查这个标志 + 自己的 fd 是否还活着）。
 * 由 tcp_server_task 写，client handler 读。 */
static volatile bool s_server_running = false;

/* "UDP 是否在运行"标志：true = socket 已就绪、能正常收发；false = 已关 socket。
 * 当前项目里 udp_task 自己读写（不像 s_server_running 要给 handler 看），
 * 加上 volatile 仅出于风格一致 —— 显式说明"运行状态可被外部观测"。 */
static volatile bool s_udp_running = false;

/* ---- 当前连接目标（运行期可变） ----
 * 现在是"地址先写死"阶段的实现：tcp_conn_task 每轮重连前按 WiFi 模式挑
 * STA / AP 的默认值填进来。后期网页配置做好后，只需换掉这里的填值来源
 * （先查 NVS 里用户填的地址、查不到再回落默认值），连接逻辑不用动。 */
static char s_target_host[128] = TCP_APP_HOST_STA;
static uint16_t s_target_port = TCP_APP_PORT;

/* 由 wifi_event_forward() 双向维护：
 *   IP_EVENT_STA_GOT_IP         → 置 true （"STA 已联网，可走 TCP 业务"）
 *   WIFI_EVENT_AP_START         → 置 true （"AP 已起来，热点内网络可用"）
 *   WIFI_EVENT_STA_DISCONNECTED → 置 false（"STA 已掉线，TCP 业务暂停"）
 *
 * 【为什么 AP 模式也算"就绪"】
 * 纯 AP 模式下设备是热点、拿不到 STA 侧 IP，但热点内部本身就是一个可用的
 * 局域网：设备（192.168.4.1）与接入的客户端（192.168.4.x）在同一网段、
 * 二层互通，所以 TCP client 照样能工作 —— 连的服务器就是挂在同一个热点下
 * 的上位机（地址见 TCP_APP_HOST_AP）。区别只是"服务器在哪里"：STA 模式下
 * 在路由器网段，AP 模式下在热点网段。
 *
 * tcp_conn_task 看到 false 就挂起等待，看到 true 就推进；这样不管 WiFi
 * 是首次连得慢、还是中途断开又恢复，TCP 任务都会跟着网状态进出，不会
 * 因为启动时还没联网就退出后"起不来"。
 *
 * volatile 修饰：它会被事件回调改写（可能跑在中断上下文），编译器不能
 * 把值缓存到寄存器，每次必须重新从内存读。 */
static volatile bool s_net_ready = false;

static void tcp_conn_task(void* arg);                         /* 连接管理 + 接收  */
static void tcp_tx_task(void* arg);                           /* 上行发送         */
static void tcp_app_on_recv(const uint8_t* data, size_t len); /* 收数据的业务回调 */
static void tcp_app_pick_target(void);                        /* 按模式挑目标地址 */
static uint32_t tcp_app_get_local_ip(void);                   /* 按模式取本机 IPv4 */
static void tcp_server_task(void* arg);                       /* TCP 服务端主任务 */
static void tcp_server_client_task(void* arg);                /* 服务端每个连接的回环任务 */
static void udp_task(void* arg);                              /* UDP 通讯任务（echo） */

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
    /* TCP 服务端与客户端独立任务：与 tcp_conn_task 同优先级（5），共用同一份
     * s_net_ready 门控；栈给大点（select + 路径处理）。 */
    xTaskCreate(tcp_server_task, "tcp_server", 3072, NULL, 5, NULL);
    /* UDP 通讯任务：与 tcp_server_task 同优先级（5）、同栈大小；
     * 共享 s_net_ready 门控。单 socket 单任务、UDP 无连接，所以没有
     * "派生 handler / 槽位管理"这些额外开销。 */
    xTaskCreate(udp_task, "udp", 3072, NULL, 5, NULL);
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

    /* ---- 先建事件循环并注册事件转发，再启动 WiFi ----
     * 【为什么必须放在最前】配网路径下 webconfig_start() 内部会一路走到
     * esp_wifi_start()，WIFI_EVENT_AP_START 在它返回前就已经触发。注册若放在
     * WiFi 启动之后，AP 模式会错过 AP_START，s_net_ready 永远是 false，
     * tcp_conn_task 就卡死在等待循环里、一次 connect 都不发 —— 这就是
     * "AP 模式下 TCP 没有连接动作"的根因。
     * 事件循环提前创建对 wifi_config_init() 是安全的：它内部有
     * "已存在（ESP_ERR_INVALID_STATE）不算错"的守卫，会直接复用。 */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    /* 注册事件转发（只影响"连不上自动回配网"和 TCP 就绪开门，失败不拦着正常功能） */
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
            /* STA 掉线 → STA 侧 IP 已无效：清掉就绪标志，让 tcp_conn_task
             * 暂停等 WiFi 重新连上。esp_wifi 默认会自己重连，GOT_IP 事件
             * 一回来又会置回 true。
             *
             * 注意：这里按"纯 STA"语义处理。将来若改成 APSTA 共存，STA 掉线
             * 时 AP 侧可能还在、连接也可能还活着，就不能无条件清这个标志了。 */
            s_net_ready = false;
        }
        else if (id == WIFI_EVENT_AP_START)
        {
            /* AP 模式：设备自己是热点（192.168.4.1），拿不到 STA 侧 IP，但热点
             * 内部本身就是一个可用的局域网 —— 设备与接入的客户端同网段、二层
             * 互通，所以 TCP client 照样能工作，连的服务器就是"挂在同一个热点
             * 下的上位机"（地址由 tcp_app_pick_target() 按模式挑选）。
             *
             * 这一支就是"AP 模式下也能用 TCP client"的入口：不开这个门，
             * tcp_conn_task 会永远卡在等就绪的循环里，一次 connect 都不发。 */
            s_net_ready = true;
            ESP_LOGI(TAG, "AP 已启动：热点内网络可用，放行 TCP 业务");
        }
        return;
    }

    /* 拿到 IP 说明设备已经挂在路由器网段上：管理页就在这个地址，
     * 打出来省得去路由器后台翻（DHCP 分配的地址不固定） */
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
        /* 给 TCP 长连接任务开门：让 tcp_conn_task 看到 s_net_ready 后往下走 */
        s_net_ready = true;
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

/* ===================== TCP 服务端实现 =====================
 * 设计：
 *   - tcp_server_task 管监听 + 接受连接 + 派生 client handler 任务；
 *   - 每个连接一个独立任务做 recv/send（先 echo），退出时清理槽位；
 *   - 全部由 s_net_ready 门控：网络断开时把监听 + 所有 client fd 关掉，
 *     handler 在下一次 recv 失败时自我清理。
 *   - 与客户端完全独立：不共用 fd、不共用队列、不共用 mutex。
 *
 * 【连接槽位的退役条件】槽位只有"handler 任务真的退出"才会被释放，所以必须
 * 保证每条连接最终都会退出。路径有三条：对端正常关闭（recv 返回 0）、收发出错、
 * 以及下面这两道"连接还在、但已经不该留着"的兜底 ——
 *   - TCP keepalive（tcp_server 在 accept 时自动配）：对端断电 / 掉线造成的
 *     半开连接，靠内核探测发现并让 recv 报错；
 *   - TCP_SERVER_IDLE_TIMEOUT_MS 空闲断开：对端还活着、但一直不说话。
 * 缺了这两道，僵尸连接会把两个槽位永久占满，服务端从此接不进新客户端。
 */

/* 找一个真正的空槽：fd 和 task 都归零才算 —— 只看 fd 会把"fd 已回收、handler
 * 还没退出来"的槽位误判为空闲，新连接复用后会踩到旧 handler。 */
static int tcp_server_alloc_slot(void)
{
    for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++)
    {
        if (s_server_clients[i].fd < 0 && s_server_clients[i].task == NULL)
        {
            return i;
        }
    }
    return -1;
}

/* 把槽位标成真正的空槽。只在确定"既没有活着的 handler、也没有待回收的 fd"
 * 时调用：server 任务启动初始化、xTaskCreate 失败回滚，以及 handler 自己
 * 确认所有权后退出。 */
static void tcp_server_clear_slot(int idx)
{
    s_server_clients[idx].fd = -1;
    s_server_clients[idx].task = NULL;
}

/**
 * @brief 服务端每个连接的回环任务：阻塞 recv → 立即 send 回原 socket
 *
 * @param arg 槽位下标（intptr_t）
 *
 * @note 与客户端的 tcp_app_on_recv 无关：这里是"自己 socket 直接回"，
 *       不走 g_tx_queue，因为 echo 的对端固定（就是这条连接的对端）。
 */
static void tcp_server_client_task(void* arg)
{
    const int idx = (int)(intptr_t)arg;
    const int fd = s_server_clients[idx].fd;
    /* buffer 用槽位自带的那一份：编译期就保证了"每个 handler 任务独立"，
     * 见 tcp_server_slot_t 的 buf 字段注释。 */
    uint8_t* const buf = s_server_clients[idx].buf;
    /* 最后一次收到数据的时刻，用来识别"连接还在、对端却不再说话"的连接。
     * keepalive 只能发现"对端已经死了"，发现不了"对端活着但一直不发数据"——
     * 后者同样会让本任务永远占着槽位。 */
    TickType_t last_rx = xTaskGetTickCount();

    ESP_LOGI(TAG, "server client[%d] 已接入 (fd=%d)", idx, fd);

    while (s_server_running)
    {
        size_t got = 0;
        esp_err_t err = tcp_server_recv(fd, buf, sizeof(s_server_clients[0].buf), &got);
        if (err == ESP_ERR_INVALID_STATE)
        {
            /* 对端有序关闭（recv 返回 0） */
            break;
        }
        if (err != ESP_OK)
        {
            /* fd 被 server 关掉 / 出错（含 keepalive 探活失败）—— 一律退出 */
            ESP_LOGW(TAG, "server client[%d] recv 错误: %s", idx, esp_err_to_name(err));
            break;
        }
        if (got == 0)
        {
            /* 本轮 recv 超时（5s 内没数据）。"超时"是空闲连接的常态而非故障，
             * 所以不能一超时就断开；只有累计空闲超过阈值才判定为死连接。
             * 检查放在这个分支里：收到数据的路径下面会刷新 last_rx，没必要
             * 每轮都算一次。 */
            /* 前置 `> 0` 是为了让"宏设为 0 = 关闭这道检查"成立：否则
             * pdMS_TO_TICKS(0) == 0，条件恒真，第一次超时就会把连接踢掉 ——
             * 恰好和"禁用"相反。常量分支会被编译器直接优化掉。 */
            if (TCP_SERVER_IDLE_TIMEOUT_MS > 0 &&
                (xTaskGetTickCount() - last_rx) >= pdMS_TO_TICKS(TCP_SERVER_IDLE_TIMEOUT_MS))
            {
                ESP_LOGW(TAG, "server client[%d] 空闲 %d ms 无数据，主动断开", idx,
                         TCP_SERVER_IDLE_TIMEOUT_MS);
                break;
            }
            continue;
        }

        last_rx = xTaskGetTickCount();

        ESP_LOGI(TAG, "server RX %u 字节 (client[%d])", (unsigned)got, idx);

        /* echo：循环补发到发完为止（与 tcp_tx_task 的 send 循环一个套路） */
        size_t sent = 0;
        while (sent < got)
        {
            size_t n = 0;
            err = tcp_server_send(fd, buf + sent, got - sent, &n);
            if (err != ESP_OK || n == 0)
            {
                ESP_LOGW(TAG, "server client[%d] send 失败: %s", idx, esp_err_to_name(err));
                goto exit;
            }
            sent += n;
        }
    }

exit:
    /* 清理必须"只动还属于自己的槽位"，否则会误伤新连接：
     *   task == 自己 → 正常收尾：fd 字段还是自己就顺手 close，再释放槽位；
     *   task == NULL → server 关停时已经替我们 close 过 fd 并把它置成 -1 了，
     *                  不能再 close —— 那个编号可能已经分配给别的连接了。
     * （task 指向别人的情况在当前分配规则下不会出现：alloc_slot 要求 fd<0 且
     *   task==NULL 才复用槽位。这里一并挡住只是为了防未来的改动。） */
    if (s_server_clients[idx].task == xTaskGetCurrentTaskHandle())
    {
        if (s_server_clients[idx].fd == fd)
        {
            (void)tcp_server_close(fd);
        }
        s_server_clients[idx].fd = -1;
        s_server_clients[idx].task = NULL;
    }
    ESP_LOGI(TAG, "server client[%d] 已关闭", idx);
    vTaskDelete(NULL);
}

/**
 * @brief TCP 服务端主循环：等网络就绪 → listen → accept 派发 → 关停收尾
 *
 * 路径：
 *   1) 启动时无限等 s_net_ready（与 tcp_conn_task 共用同一个门控）；
 *   2) 取本机 IPv4 作为 bind 地址（tcp_app_get_local_ip），listen 成功 →
 *      s_server_running = true；
 *   3) accept 循环：select 200ms 超时唤醒（既能及时接连接，又能检查
 *      s_net_ready / IP 是否变了），新连接派生 client handler 任务；
 *   4) 退出重建的两种原因（都走同一套收尾）：
 *      - s_net_ready=false（网络断开）；
 *      - 本机 IP 变了（网络还在，但绑定的地址已失效，比如 AP 自定义网段
 *        在 esp_wifi_start() 之后才下发）。
 *      收尾：s_server_running=false → close listen → close 所有 client fd
 *      并把槽位的 fd 字段置 -1（"已替你回收"的信号）→ 等 handler 自己退出
 *      并清槽位（最多 1s，超时的不强清）→ 回第 1 步重建。
 */
static void tcp_server_task(void* arg)
{
    (void)arg;

    for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++)
    {
        tcp_server_clear_slot(i);
    }

    while (1)
    {
        /* 1) 等网络就绪 */
        while (!s_net_ready)
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        /* 2) 取本机 IPv4 作为 bind 地址（AP=192.168.4.1 / STA=路由器分配的
         *    地址）；拿不到时回退通配 0.0.0.0，保证 server 不会因为"IP 还没
         *    就绪"直接起不来。 */
        tcp_server_cfg_t cfg = TCP_SERVER_CFG_DEFAULT();
        cfg.port = TCP_SERVER_PORT;
        cfg.bind_addr = tcp_app_get_local_ip();
        if (cfg.bind_addr == 0)
        {
            ESP_LOGW(TAG, "未取到本机 IP，暂时监听所有地址（0.0.0.0）");
        }

        /* 3) listen —— 只绑本机 IP，端口被占用等失败一律退避 2s 后重试。
         *    地址后面变了也没关系：accept 循环会检测到并回到步骤 2 重建。 */
        int listen_fd = -1;
        esp_err_t err = tcp_server_listen(&cfg, &listen_fd);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "TCP 服务端启动失败，2s 后重试");
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        s_server_running = true;

        /* 4) accept 循环 */
        bool ip_changed = false; /* true = 退出原因是"本机 IP 变了"，网络仍然可用 */
        while (s_net_ready)
        {
            int cfd = -1;
            err = tcp_server_accept(listen_fd, &cfd, cfg.accept_poll_ms);
            if (err == ESP_ERR_TIMEOUT)
            {
                /* 这轮没人连（200ms 超时）。顺手看一眼本机 IP 是否变了 ——
                 * s_net_ready 只知道"有没有网"，对 IP 变化一无所知，而
                 * listen socket 绑的是旧地址，不重建就永远等不到新地址上的
                 * 客户端（典型的：AP 自定义网段是在 esp_wifi_start() 之后
                 * 才下发到 netif 的，正好落在这个窗口里）。
                 * now_ip == 0 表示 netif 暂时没地址，那种情况 s_net_ready
                 * 会变 false，交给"网络断开"那条路径处理，这里不重建。 */
                const uint32_t now_ip = tcp_app_get_local_ip();
                if (now_ip != 0 && now_ip != cfg.bind_addr)
                {
                    ip_changed = true;
                    break;
                }
                continue; /* 继续看 s_net_ready */
            }
            if (err != ESP_OK)
            {
                break; /* listen_fd 可能坏了，退出重建 */
            }

            const int slot = tcp_server_alloc_slot();
            if (slot < 0)
            {
                ESP_LOGW(TAG, "已达最大并发 %u，拒绝新连接", (unsigned)TCP_SERVER_MAX_CLIENTS);
                (void)tcp_server_close(cfd);
                continue;
            }

            s_server_clients[slot].fd = cfd;
            char task_name[16];
            snprintf(task_name, sizeof(task_name), "svr_c%d", slot);
            /* 任务栈 4096：echo 缓冲区已挪到 static 不占栈，但 ESP_LOG 的 vprintf
             * 路径峰值能到 1KB+，加上 lwIP socket 调用帧 + 业务局部变量，2048
             * 会撞栈溢出（"stack overflow in task svr_cN"），留足余量。 */
            BaseType_t ok = xTaskCreate(tcp_server_client_task, task_name, 4096,
                                        (void*)(intptr_t)slot, 5, &s_server_clients[slot].task);
            if (ok != pdPASS)
            {
                ESP_LOGE(TAG, "创建 client handler 失败，关掉连接");
                (void)tcp_server_close(cfd);
                tcp_server_clear_slot(slot);
                continue;
            }
        }

        /* 5) 关停收尾 —— 网络断开和"IP 变了"共用同一套，区别只在日志措辞。
         * IP 变了只是换个地址重建，不用等 s_net_ready，所以回第 1 步会立刻通过。 */
        if (ip_changed)
        {
            ESP_LOGW(TAG, "本机 IP 已变化，重建 TCP 服务端（改绑新地址）");
        }
        else
        {
            ESP_LOGW(TAG, "网络断开，停止 TCP 服务端");
        }
        s_server_running = false;

        /* 关监听 —— 下一次 select 在 accept 路径返回 -1，handler 不受影响 */
        (void)tcp_server_close(listen_fd);

        /* 关所有 client fd —— handler 的 recv/send 会立刻返回错误并退出。
         * 用 plain close 而非 tcp_server_close，因为关停时 fd 状态不定、
         * 警告日志会刷屏。
         * 关完立刻把 fd 字段置成 -1：这是给 handler 的信号 —— "fd 已由 server
         * 回收，你退出时别再 close 一次"。但 task 字段保留，让 alloc_slot()
         * 知道这个槽位还没腾出来，不会分给新连接。 */
        for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++)
        {
            if (s_server_clients[i].fd >= 0)
            {
                close(s_server_clients[i].fd);
                s_server_clients[i].fd = -1;
            }
        }

        /* 等 handler 自己退出：它会在 exit 分支里看到 fd 已被回收，于是只清槽位、
         * 不再 close。正常几十毫秒内完成（fd 失效后 recv/send 立刻返回），
         * 卡在 send 上最多 SO_SNDTIMEO = 5s。
         * 这里最多等 1s；仍然没退出的【不强清】—— 强清会把槽位标成空闲，新连接
         * 随即复用它，而旧 handler 手里的 fd 编号可能已被这个新连接重新分配，
         * 那就真会误伤。保持"占用"状态让新连接避开这个槽位，是更安全的降级
         * 方向（最坏情况只是并发数临时少一个）。 */
        for (int waited = 0; waited < 10; waited++)
        {
            bool all_gone = true;
            for (int i = 0; i < TCP_SERVER_MAX_CLIENTS; i++)
            {
                if (s_server_clients[i].task != NULL)
                {
                    all_gone = false;
                    break;
                }
            }
            if (all_gone)
            {
                break;
            }
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
}

/**
 * @brief 按当前 WiFi 模式挑一个写死的默认目标地址（"地址先写死"阶段的临时手段）
 *
 * @note 逻辑刻意做得简单，不查任何配置：
 *       - 纯 AP 模式 → 连热点内的上位机（TCP_APP_HOST_AP）；
 *       - 其它模式   → 连路由器网段的服务器（TCP_APP_HOST_STA）；
 *         APSTA 也走这一支：既然还挂着路由器，服务器就该在路由器那边。
 *
 * @note 【后期扩展点】网页配置做好后，本函数改成"先查 NVS 里用户填的地址，
 *       查不到才回落到这里的默认值"—— 调用点（tcp_conn_task）一行都不用改。
 */
static void tcp_app_pick_target(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);

    const bool ap_only = (mode == WIFI_MODE_AP);
    strlcpy(s_target_host, ap_only ? TCP_APP_HOST_AP : TCP_APP_HOST_STA, sizeof(s_target_host));
    s_target_port = TCP_APP_PORT;
}

/**
 * @brief 取"当前 WiFi 模式下的本机 IPv4"（网络字节序）；取不到返回 0
 *
 * @note AP 模式取热点 netif（默认 192.168.4.1），其它模式取 STA netif
 *       （路由器 DHCP 分配或静态配置的地址）。
 *
 * @note 返回 0 表示"本机还没拿到 IP"（netif 未建 / 尚未配置），调用方应回退
 *       到通配监听，而不是直接失败 —— 否则 server 会因为"地址暂时没有"完全
 *       起不来。
 *
 * @note ifkey 是 esp_netif_create_default_wifi_sta() / _ap() 固定写死的外部
 *       键，wifi 组件正是用这两个函数建 netif 的（见 wifi.c）。这两个字面量
 *       定义在 ESP-IDF 的 esp_netif_defaults.h 里（inherent config 结构体的
 *       .if_key 字段，STA / AP 分别是 "WIFI_STA_DEF" / "WIFI_AP_DEF"），是
 *       IDF 内部约定、没有公开宏，所以只能在这里硬编码。
 *
 * @note 【esp_netif_get_handle_from_ifkey() 是干嘛的】按 if_key 字符串在
 *       esp_netif 的全局 netif 链表里线性查找（逐个 strcmp），返回对应句柄，
 *       找不到返回 NULL。之所以要"按名字反查"，是因为 netif 是 wifi 组件在
 *       自己内部建的、句柄存在它的 s_sta_netif / s_ap_netif 里没对外暴露，
 *       而下面的 esp_netif_get_ip_info() 又必须拿到 esp_netif_t* 才能用。
 *
 * @note 【这一步比看上去要重】public 版不是直接读链表，而是把请求 IPC 到
 *       lwIP(tcpip) 线程执行、再同步等回来（只有调用方本来就在 lwIP 线程里
 *       时才走本地捷径）。对比之下，后面的 esp_netif_get_ip_info() 是纯内存
 *       读、零 IPC。所以本函数每调一次 = 一次跨线程往返。
 *       当前调用频率是 200ms 一次（tcp_server_task 的 accept 循环拿它做 IP
 *       变化检测），开销可以忽略，因此不做句柄缓存；真要省，可以把 netif
 *       句柄缓存下来（netif 在 wifi 组件里"只增不减"、运行期稳定），代价是
 *       得处理 WiFi 重新初始化时缓存失效的情况。
 */
static uint32_t tcp_app_get_local_ip(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK)
    {
        return 0;
    }

    const char* ifkey = (mode == WIFI_MODE_AP) ? "WIFI_AP_DEF" : "WIFI_STA_DEF";
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (netif == NULL)
    {
        return 0;
    }

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK)
    {
        return 0;
    }
    return ip_info.ip.addr; /* 网络字节序；0 = 尚未配置 */
}

/**
 * @brief UDP 通讯任务：等网络就绪 → open → 循环 recvfrom → 收到就回发（echo）
 *
 * 路径：
 *   1) 启动时无限等 s_net_ready（与 tcp_server_task 共享门控）；
 *   2) 取本机 IPv4 作为 bind 地址（同 tcp_server_task 的处理）；
 *   3) udp_open：绑 UDP_PORT；
 *   4) recvfrom 循环（1s 超时），收到数据就 sendto 回发（echo）；
 *   5) 关停触发条件：
 *      - s_net_ready=false：网络断开；
 *      - 本机 IP 变了：网络仍在但绑的地址失效（AP 自定义网段场景）；
 *      收尾：s_udp_running=false → close socket → 回第 2 步重建。
 *
 * @note 与 tcp_server_task 的关键区别：UDP 没有"连接"概念，所以**没有
 *   handler 任务、没有槽位、没有 fd 泄漏 / 缓冲区互踩等问题**。
 *   关停时直接 close：等待中的 recvfrom 立刻返错退出，干净利落。
 */
static void udp_task(void* arg)
{
    (void)arg;

    /* echo 缓冲区挪到 BSS（不在栈上） —— 1500 字节放局部变量太重，会撞
     * 3072 字节栈（ESP_LOG 的 vprintf 路径峰值 1KB+ 也会吃栈）。函数局部
     * static 与全局 static 等价，都是 .bss 区分配。 */
    static uint8_t s_udp_rx_buf[UDP_RX_BUF_SIZE];

    while (1)
    {
        /* 1) 等网络就绪 */
        while (!s_net_ready)
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }

        /* 2) 取本机 IP 作为 bind；拿不到时回退通配 0.0.0.0，保证 UDP 不会
         *    因为"IP 还没就绪"直接起不来。 */
        udp_cfg_t cfg = UDP_CFG_DEFAULT();
        cfg.port = UDP_PORT;
        cfg.bind_addr = tcp_app_get_local_ip();
        cfg.broadcast = false; /* echo 场景不需要发广播 */
        if (cfg.bind_addr == 0)
        {
            ESP_LOGW(TAG, "未取到本机 IP，UDP 暂时监听 0.0.0.0");
        }

        /* 3) open —— 失败一律退避 2s 后重试（端口占用 / bind 失败等） */
        int fd = -1;
        esp_err_t err = udp_socket_open(&cfg, &fd);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "UDP 启动失败，2s 后重试");
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        s_udp_running = true;

        /* 4) recvfrom 循环（1s 超时） */
        bool ip_changed = false; /* true = 退出原因是"本机 IP 变了"，网络仍然可用 */
        while (s_net_ready)
        {
            uint32_t src_ip = 0;
            uint16_t src_port = 0;
            size_t got = 0;
            err = udp_socket_recvfrom(fd, s_udp_rx_buf, sizeof(s_udp_rx_buf),
                               &src_ip, &src_port, &got);
            if (err != ESP_OK)
            {
                /* fd 可能坏了：退出重建 */
                ESP_LOGE(TAG, "udp recvfrom 错误: %s", esp_err_to_name(err));
                break;
            }

            /* 超时（1s 内没数据）—— 顺手看一眼本机 IP 是否变了。
             * 与 tcp_server_task 同思路：s_net_ready 只知道"有没有网"，
             * 不知道 IP 是多少；AP 自定义网段是在 esp_wifi_start() 之后
             * 才下发到 netif 的，正好落在这个窗口里。
             * now_ip == 0 表示 netif 暂时没地址，那种情况 s_net_ready
             * 会变 false，交给"网络断开"那条路径处理。 */
            if (got == 0)
            {
                const uint32_t now_ip = tcp_app_get_local_ip();
                if (now_ip != 0 && now_ip != cfg.bind_addr)
                {
                    ip_changed = true;
                    break;
                }
                continue; /* 继续看 s_net_ready */
            }

            ESP_LOGI(TAG, "udp RX %u 字节 (from %08x:%u)",
                     (unsigned)got, (unsigned)src_ip, (unsigned)src_port);

            /* echo —— 原样回给对端（UDP 没有"连接"概念，回包地址必须由调用方
             * 自己记：刚才是从 src_ip/src_port 收的，现在回给同地址）。 */
            size_t sent = 0;
            err = udp_socket_sendto(fd, s_udp_rx_buf, got, src_ip, src_port, &sent);
            if (err != ESP_OK || sent != got)
            {
                /* 一次 echo 失败不代表 fd 失效（对端可能中途走丢）——
                 * 仅警告、继续循环。                                       */
                ESP_LOGW(TAG, "udp echo 失败: %s", esp_err_to_name(err));
                continue;
            }
            ESP_LOGI(TAG, "udp TX echo %u 字节 (to %08x:%u)",
                     (unsigned)sent, (unsigned)src_ip, (unsigned)src_port);
        }

        /* 5) 关停收尾 —— 网络断开和"IP 变了"共用同一套，区别只在日志措辞。
         * 与 tcp_server_task 不同的关键点：**没有等 handler 退出这一步**——
         * UDP 无连接，close(fd) 后正在阻塞的 recvfrom 立刻返错退出，唯一
         * 任务就是 udp_task 自己，关停是同步的、确定的。 */
        s_udp_running = false;
        if (ip_changed)
        {
            ESP_LOGW(TAG, "本机 IP 已变化，重建 UDP socket（改绑新地址）");
        }
        else
        {
            ESP_LOGW(TAG, "网络断开，停止 UDP 通讯");
        }
        (void)udp_socket_close(fd);
    }
}

/**
 * @brief 连接管理与接收任务：等网络就绪 → open → 循环 recv → 断开重连
 *
 * 接收路径零额外延迟：recv 一返回就交给 tcp_app_on_recv()，
 * 中间没有队列、没有 sleep、没有轮次控制。
 *
 * "网络就绪"由 s_net_ready 表示，两条路都会放行：
 *   - STA 模式：IP_EVENT_STA_GOT_IP（挂上路由器拿到 IP）；
 *   - AP  模式：WIFI_EVENT_AP_START（热点起来，热点内可互通）。
 */
static void tcp_conn_task(void* arg)
{
    (void)arg;

    /* 1) 启动阶段：无限等网络就绪（不因暂时无网退出任务） */
    ESP_LOGI(TAG, "TCP 长连接任务启动，等待网络就绪（STA 拿 IP 或 AP 启动，无限等）...");
    while (!s_net_ready)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    while (1)
    {
        /* 2) 网络不可用 → 挂起等恢复（避免无意义的 connect） */
        if (!s_net_ready)
        {
            ESP_LOGW(TAG, "网络已断开，暂停 TCP 业务，等 WiFi 恢复...");
            while (!s_net_ready)
            {
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
            ESP_LOGI(TAG, "网络已恢复，重连服务器");
        }

        /* 3) 每轮重连都重新构建 cfg，并在此时重新挑一次目标地址。
         *    放在循环里的原因：地址是运行期变量，这样"换了地址下一轮就生效"，
         *    后期网页配置改完地址后不需要重启、也不需要额外的唤醒机制。 */
        tcp_app_pick_target();

        tcp_client_cfg_t cfg = TCP_CLIENT_CFG_DEFAULT();
        strlcpy(cfg.host, s_target_host, sizeof(cfg.host));
        cfg.port = s_target_port;
        /* 关 Nagle：小包立刻发、低延迟，对"随时上行小数据"很关键 */
        cfg.nodelay = true;
        /* 无应用层心跳 → 靠 TCP keepalive 兜底探测对端静默失联 */
        cfg.keepalive = true;
        cfg.keepalive_idle_s = TCP_APP_KEEPALIVE_IDLE_S;
        cfg.keepalive_interval_s = TCP_APP_KEEPALIVE_INTVL_S;
        cfg.keepalive_count = TCP_APP_KEEPALIVE_CNT;
        /* recv 阻塞上限：只是"隔多久醒来看一眼连接状态"，不影响响应延迟 */
        cfg.recv_timeout_ms = TCP_APP_RECV_TIMEOUT_MS;

        ESP_LOGI(TAG, "正在连接 %s:%d", s_target_host, s_target_port);

        /* 4) 建立长连接 */
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
