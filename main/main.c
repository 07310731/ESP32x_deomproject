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

#include <stdbool.h>         /* TCP 任务里的 volatile bool s_net_ready 需要 */
#include <unistd.h>          /* close()：server 关停时直接关 client fd       */
#include "tcp_client.h"      /* TCP 客户端封装组件                                */
#include "tcp_server.h"      /* TCP 服务端封装组件                                */
#include "udp.h"             /* UDP 通讯封装组件                                   */
#include "service_manager.h" /* 统一管理"现在跑哪个网络服务" */

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

/* ===================== 三个服务共用的业务框架 =====================
 * TCP_CLIENT / TCP_SERVER / UDP 三个 service 共用一对队列 + 一对 consumer task：
 *
 *           g_tx_queue ◄──── tx_queue_post() ◄──── 上行入口
 *                              ▲                  (tcp_app_send / udp_app_send /
 *                              │                   srv_app_send，任一任务可调)
 *                              │
 *                       tx_consumer_task ──── 看 kind 决定发到：
 *                                              - TCP_CLIENT → send(g_fd)
 *                                              - TCP_SERVER → send(item.fd)
 *                                              - UDP        → sendto(g_udp_fd, ip, port)
 *
 *           g_rx_queue ◄──── rx_queue_post() ◄──── 各 service 的 recv 路径
 *                              ▲                  (tcp_conn_task / server_client_task / udp_task)
 *                              │
 *                       rx_consumer_task ──── 调 tcp_app_on_recv(item)
 *                                                  │
 *                                                  ▼
 *                                       tcp_app_handle_frame()
 *                                       ← 【目前原样回传；你的协议解析写这里】
 *
 * 【为什么拆成两个任务】
 *   把"发"和"收"塞进同一个任务，任务阻塞在 recv 上时就没法立刻 send，
 *   上行延迟被 recv 超时拖着走。拆开后：
 *     上行延迟 ≈ 队列投递 + task 调度   （约 1ms）
 *     下行延迟 ≈ 收包 + 立刻回调        （约 1ms）
 *   两个任务都阻塞在事件上（xQueueReceive / recv），空闲时几乎不占 CPU。
 *
 * 【靠什么发现断线 / 保住长连接】
 *   0. 应用层心跳（CONFIG_TCP_CLIENT_HEARTBEAT_MS，治"空闲自己断开"）：
 *      空闲超过心跳间隔就主动发一帧带 payload 的数据。TCP keepalive 的
 *      探针是"纯 ACK、零字节 payload"，很多路由器/AP 的 NAT 会话表只认
 *      "带数据的包"才刷新老化计时 —— 表项老化、连接被中间设备静默丢弃
 *      后，keepalive 反而把它"发现"成断线。带 payload 的心跳帧能把所有
 *      中间设备的会话表都续上；心跳 send 失败也把"连接已死"的发现窗口
 *      从协议栈重传的几十秒压到一个心跳周期内。
 *   1. 对端主动 close  → recv 返回 0 → ESP_ERR_INVALID_STATE，立刻发现；
 *   2. 发送失败        → send 返回错误，立刻发现；
 *   3. 对端静默失联（拔网线 / 断电）→ 只能靠 TCP keepalive 探针：
 *      连接空闲 keepalive_idle_s 秒后开始探测，每 keepalive_interval_s 秒
 *      发一次，连续 keepalive_count 次收不到应答就判死。按下面的参数，
 *      最坏约 20 + 5*3 = 35 秒能发现 —— 这是不发心跳时的兜底手段。
 */
/* ---- TCP 服务器目标（"地址先写死"阶段） ----
 * 【现状】地址是编译期常量，按 WiFi 模式各给一个默认值；
 * 【后续】网页配置做好后，改成从 NVS 读用户填写的地址来覆盖它 —— 只需要
 *         换掉"填值来源"，tcp_conn_task 里的连接逻辑一行都不用动。
 *
 * ⚠ 两种模式的网段不同，默认值不能混用：
 *   - STA 模式：设备挂在你家路由器网段（192.168.1.x），服务器是局域网里的上位机；
 *   - AP  模式：设备自己就是热点 192.168.4.1，接入的客户端由内置 DHCP 从
 *               192.168.4.2 起分配 —— 所以"服务器"必须落在这个网段里，
 *               且建议在上位机侧手工设成固定 IP（DHCP 分配顺序不保证）。
 *
 * 【所有 TCP / UDP 编译期参数（端口、缓冲区、max clients、keepalive...）都
 *   已经搬到 menuconfig → CONFIG_TCP_CLIENT_* / CONFIG_TCP_SERVER_* /
 *   CONFIG_UDP_*；下面不再写 #define，统一引 CONFIG_ 宏。】
 *
 * 切换三个网络服务（TCP_CLIENT / TCP_SERVER / UDP 只能同时跑一个）选
 * 项在 Service Mode 子菜单；service_manager 组件读它的 CONFIG_SERVICE_*_ENABLE
 * 决定哪些模式可用，再按 NVS / menuconfig 默认值挑第一个跑起来的。 */

/* 【三个服务全关 → 下面整段消失】
 * 从这里到 app_main 之前，全部属于"网络服务框架"：队列元素类型、共用队列、
 * consumer 任务的存活标志、三个服务各自的状态变量、以及各种前置声明。
 * menuconfig 里三个服务一个都没勾选时，这些东西没有任何引用者
 * （不编译掉会撞 -Wunused-variable / -Wunused-function），所以整体编译掉。
 * 同一个判据（SERVICE_ANY_ENABLE）也用在 service_manager.c 和 Kconfig 里
 * 隐藏 "Default service mode" / "Service common"。 */
#if CONFIG_SERVICE_ANY_ENABLE

/* 上行队列的元素：一帧待发送数据。 */
/* ===================== 队列元素：tx_item_t / rx_item_t =====================
 * 数据直接放在结构体里（而不是"指针 + malloc"）：队列按值拷贝，
 * 发送完自动释放，不用管内存生命周期，也更省 heap。 */
typedef enum
{
    TX_KIND_TCP_CLIENT = 0, /*!< 发到 g_fd（TCP 长连接）                  */
    TX_KIND_TCP_SERVER = 1, /*!< 发到 item.fd（服务端某 client）          */
    TX_KIND_UDP = 2,        /*!< 发到 item.dst_ip:dst_port                */
} tx_item_kind_t;

typedef struct
{
    tx_item_kind_t kind;               /*!< 目的地类型                                */
    uint16_t len;                      /*!< data 有效字节数（1 ~ CONFIG_RX_MAX_FRAME）*/
    int fd;                            /*!< TX_KIND_TCP_SERVER：目标 client fd        */
    uint32_t dst_ip;                   /*!< TX_KIND_UDP：网络字节序                   */
    uint16_t dst_port;                 /*!< TX_KIND_UDP：主机字节序                   */
    uint8_t data[CONFIG_RX_MAX_FRAME]; /*!< 单帧内容（最多 RX_MAX_FRAME 字节）    */
} tx_item_t;

typedef enum
{
    RX_KIND_TCP_CLIENT = 0, /*!< 来自 g_fd                        */
    RX_KIND_TCP_SERVER = 1, /*!< 来自 item.fd（服务端某 client）  */
    RX_KIND_UDP = 2,        /*!< 来自 item.src_ip:src_port         */
} rx_item_kind_t;

typedef struct
{
    rx_item_kind_t kind;               /*!< 来源类型                                  */
    uint16_t len;                      /*!< data 有效字节数                           */
    int fd;                            /*!< RX_KIND_TCP_SERVER：来源 client fd        */
    uint32_t src_ip;                   /*!< RX_KIND_UDP：网络字节序                   */
    uint16_t src_port;                 /*!< RX_KIND_UDP：主机字节序                   */
    uint8_t data[CONFIG_RX_MAX_FRAME]; /*!< 单段内容（最多 RX_MAX_FRAME 字节）    */
} rx_item_t;

/* ----- TCP 长连接资源（仍只 TCP_CLIENT 用；其它服务有自己的 fd 集合） ----- */
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
static int g_fd = -1;                       /* 当前长连接 fd；-1 = 未连接 */
static SemaphoreHandle_t g_fd_mutex = NULL; /* 保护 g_fd 的读写 + send 的原子性 */
#endif                                      /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */

/* ----- 共用队列与 consumer 任务控制（永远存在；service 全关时也只是空转） ----- */
static QueueHandle_t g_tx_queue = NULL;           /* 上行队列：发送入口 → tx_consumer_task */
static QueueHandle_t g_rx_queue = NULL;           /* 下行队列：recv task → rx_consumer_task */
static volatile bool s_tx_consumer_stop = false;  /* 给 tx_consumer_task 的停服请求（目前永不停） */
static volatile bool s_tx_consumer_alive = false; /* tx_consumer_task 自己活着 */
static volatile bool s_rx_consumer_stop = false;
static volatile bool s_rx_consumer_alive = false;

/* ---- 应用层心跳 ----
 * 心跳帧内容：一帧带 payload 的小数据。内容本身不重要，重要的是"它带
 * payload" —— 只有带数据的包才能刷新路由器/AP 的 NAT 会话表（见文件头
 * 注释第 0 条）。echo 服务器会把它原样回回来，tcp_app_on_recv() 里会跳过
 * 它的回传，避免"心跳 → 回显 → 再回显"无限乒乓。 */
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
#define TCP_APP_HEARTBEAT_PAYLOAD "PING"
#define TCP_APP_HEARTBEAT_LEN (sizeof(TCP_APP_HEARTBEAT_PAYLOAD) - 1) /* 不含 '\0' */

/* 最近一次网络流量的时刻（任何方向的 send/recv 成功都会刷新，含心跳）。
 * tcp_conn_task 在 recv 超时的空窗里检查它，判断"空闲多久了、该不该发心跳"。
 * TickType_t 是 32 位对齐字，Xtensa 上单条指令读写、天然原子，volatile
 * 就够了，不需要再套 g_fd_mutex。 */
static volatile TickType_t s_last_traffic_tick = 0;
#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */

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
 * 【为什么槽位里没有 buf 了】
 * 旧设计：每个槽位 BSS 里挂一份 echo buf（N × 1KB 常驻）。
 * 新设计：handler 改用"栈上 rx_item_t + srv_app_send 投队列"——
 * 栈上 item 本身就是 handler 私有的 recv 缓冲，函数返回才释放，
 * 与 task 一一对应，不存在多 handler 互踩；BSS 也省下来了。 */
/* TCP Server 用的状态与 typedef：仅在 menuconfig 启用 TCP_SERVER 时编译 */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
typedef struct
{
    int fd;            /*!< 该 handler 持有的 socket fd；-1 = 已被回收/未分配 */
    TaskHandle_t task; /*!< client handler 任务句柄；NULL = 该槽位无人使用   */
} tcp_server_slot_t;

static tcp_server_slot_t s_server_clients[CONFIG_TCP_SERVER_MAX_CLIENTS];

/* "server 是否在运行"标志：true = 接受新连接；false = 拒收 + 通知已连的
 * 客户端任务退出（每个 handler 在循环里查这个标志 + 自己的 fd 是否还活着）。
 * 由 tcp_server_task 写，client handler 读。 */
static volatile bool s_server_running = false;
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */

#if CONFIG_SERVICE_UDP_ENABLE
/* UDP socket fd：udp_task 在 open 时填、close 时清。
 * tx_consumer_task 收到 TX_KIND_UDP 时从这里取 fd 当 sendto 的第一参数。
 * UDP socket 在 udp_task 自己手里、不需要互斥锁。 */
static int g_udp_fd = -1;
#endif /* CONFIG_SERVICE_UDP_ENABLE */

/* "UDP 是否在运行"标志：true = socket 已就绪、能正常收发；false = 已关 socket。
 * 【目前只写不读】—— 不像 s_server_running 需要给 handler 任务看，UDP 没有
 * "别的任务要判断它是否在跑"的场景，所以眼下没有任何读者。保留它是为了与
 * s_server_running 对称，并给将来的状态查询 / 看门狗留一个现成的观测点；
 * 若嫌它是死变量，删掉此声明和 udp_task 里的两处赋值即可，无其它引用。 */
/* UDP 用的运行标志：同上，只有 UDP 模式启用时才编译 */
#if CONFIG_SERVICE_UDP_ENABLE
static volatile bool s_udp_running = false;
#endif /* CONFIG_SERVICE_UDP_ENABLE */

/* ---- 当前连接目标（运行期可变） ----
 * 现在是"地址先写死"阶段的实现：tcp_conn_task 每轮重连前按 WiFi 模式挑
 * STA / AP 的默认值填进来。后期网页配置做好后，只需换掉这里的填值来源
 * （先查 NVS 里用户填的地址、查不到再回落默认值），连接逻辑不用动。 */
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
static char s_target_host[128] = CONFIG_TCP_CLIENT_HOST_STA;
static uint16_t s_target_port = CONFIG_TCP_CLIENT_PORT;
#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */

/* 由 wifi_event_forward() 双向维护：
 *   IP_EVENT_STA_GOT_IP         → 置 true （"STA 已联网，可走 TCP 业务"）
 *   WIFI_EVENT_AP_START         → 置 true （"AP 已起来，热点内网络可用"）
 *   WIFI_EVENT_STA_DISCONNECTED → 置 false（"STA 已掉线，TCP 业务暂停"）
 *
 * 【为什么 AP 模式也算"就绪"】
 * 纯 AP 模式下设备是热点、拿不到 STA 侧 IP，但热点内部本身就是一个可用的
 * 局域网：设备（192.168.4.1）与接入的客户端（192.168.4.x）在同一网段、
 * 二层互通，所以 TCP client 照样能工作 —— 连的服务器就是挂在同一个热点下
 * 的上位机（地址见 CONFIG_TCP_CLIENT_HOST_AP）。区别只是"服务器在哪里"：STA 模式下
 * 在路由器网段，AP 模式下在热点网段。
 *
 * tcp_conn_task 看到 false 就挂起等待，看到 true 就推进；这样不管 WiFi
 * 是首次连得慢、还是中途断开又恢复，TCP 任务都会跟着网状态进出，不会
 * 因为启动时还没联网就退出后"起不来"。
 *
 * volatile 修饰：它会被事件回调改写（可能跑在中断上下文），编译器不能
 * 把值缓存到寄存器，每次必须重新从内存读。 */
static volatile bool s_net_ready = false;

/* TCP 客户端任务的 forward 声明：只有 TCP_CLIENT 启用时才声明 */
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
/* TCP 客户端 forward 声明 */
static void tcp_conn_task(void* arg);  /* 连接管理 + 接收 */
static void tcp_app_pick_target(void); /* 按模式挑目标地址 */
#endif                                 /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */

/* 共用收发 consumer 任务的 forward 声明。
 * 【不能塞进上面那个 #if】app_main 里无条件创建这两个 consumer 任务，
 * 声明一旦被 TCP_CLIENT 的 #if 包住，三个 service 全关时 app_main 就会报
 * "tx_consumer_task / rx_consumer_task undeclared"。
 * （tx_queue_post / rx_queue_post 不需要 forward 声明：定义在唯一调用点之前。） */
static void tx_consumer_task(void* arg);
static void rx_consumer_task(void* arg);
static void tcp_app_on_recv(const rx_item_t* item); /* 收数据的业务回调（统一） */
/* 本机 IPv4 查询：只有 TCP Server / UDP 会用到（见下文定义处的说明），
 * 两个服务都关掉时连声明一起省掉 —— 否则 static 声明无定义也会报
 * -Wunused-function（declared 'static' but never defined）。 */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE || CONFIG_SERVICE_UDP_ENABLE
static uint32_t tcp_app_get_local_ip(void); /* 按模式取本机 IPv4 */
#endif
/* TCP Server / UDP 任务的 forward 声明：只有对应模式启用时声明，避免引用未定义符号 */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
static void tcp_server_task(void* arg);        /* TCP 服务端主任务 */
static void tcp_server_client_task(void* arg); /* 服务端每个连接的回环任务 */
#endif                                         /* CONFIG_SERVICE_TCP_SERVER_ENABLE */
#if CONFIG_SERVICE_UDP_ENABLE
static void udp_task(void* arg); /* UDP 通讯任务（echo） */
#endif                           /* CONFIG_SERVICE_UDP_ENABLE */

/* ===================== service 运行期状态变量 =====================
 * 每个 service 用一对 volatile bool 表示"s_tcp_client_stop 请求停服"和
 * "s_tcp_client_alive 自己还活着"；manager 任务靠它们判断停服是否完成、
 * 以及是否异常退出。真正的 service_manager 接口实现在文件末尾
 * （"service_manager 接口实现" 段）。 */

/* TCP 客户端 service 父子结构：service_fn 创 conn 子任务，等它退出 */
static volatile bool s_tcp_client_stop = false;
static volatile bool s_tcp_conn_stop = false;    /* 给 conn 子任务的停服请求 */
static volatile bool s_tcp_conn_alive = false;   /* tcp_conn_task 自己活着 */
static volatile bool s_tcp_client_alive = false; /* service_fn 自己活着 */

/* TCP 服务端 / UDP 的 stop/alive 标志：同上，按 service 是否启用决定是否编译 */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
/* TCP 服务端：单任务，service_fn 等价于 tcp_server_task 自己 */
static volatile bool s_server_stop = false;
static volatile bool s_server_alive = false;
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */
#if CONFIG_SERVICE_UDP_ENABLE
/* UDP：单任务 */
static volatile bool s_udp_stop = false;
static volatile bool s_udp_alive = false;
#endif /* CONFIG_SERVICE_UDP_ENABLE */

#endif /* CONFIG_SERVICE_ANY_ENABLE —— 服务框架的类型 / 状态 / 声明到此为止 */

void app_main(void)
{
    if (hardware_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "硬件初始化失败，停在 app_main");
        return;
    }

    xTaskCreate(running_light_task, "running_light_task", 2048, NULL, 10, NULL);

    /* ---- 收发资源 / consumer 任务：只有存在服务时才建 ----
     * 队列和两个常驻 consumer 任务都属于"服务框架"，一个服务都没勾选时
     * 它们既没人用、也没有存在的意义，连代码都不编译（见文件上方
     * CONFIG_SERVICE_ANY_ENABLE 的说明）。此时设备只做"配网 + 网页 + 流水灯"。 */
#if CONFIG_SERVICE_ANY_ENABLE

    /* ---- 共用收发资源（三个 service 都用）----
     * g_tx_queue / g_rx_queue + 两个 consumer task 在任何 service 启动前建好；
     * 切到不同 service 时它们保留（service 切换时复用），consumer task 不停。
     * TCP_CLIENT 的 g_fd_mutex 也在这里建好（互斥保护 g_fd + send 的原子性）。 */
    g_tx_queue = xQueueCreate(CONFIG_TX_QUEUE_LEN, sizeof(tx_item_t));
    g_rx_queue = xQueueCreate(CONFIG_RX_QUEUE_LEN, sizeof(rx_item_t));
    configASSERT(g_tx_queue != NULL && g_rx_queue != NULL);
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
    g_fd_mutex = xSemaphoreCreateMutex();
    configASSERT(g_fd_mutex != NULL);
#endif
    /* 起两个常驻 consumer task：阻塞在队列上、空闲几乎不占 CPU。
     * 栈 4096：consumer 里只有 ESP_LOG（峰值 1KB+），4096 与 rx_consumer 那边栈调用对齐。 */
    BaseType_t ok_tx = xTaskCreate(tx_consumer_task, "tx_consumer", 4096, NULL, 5, NULL);
    BaseType_t ok_rx = xTaskCreate(rx_consumer_task, "rx_consumer", 4096, NULL, 5, NULL);
    configASSERT(ok_tx == pdPASS && ok_rx == pdPASS);
#endif /* CONFIG_SERVICE_ANY_ENABLE */

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

/* forward declaration：helper 定义在 running_light_task 之后（webconfig 适配层之前） */
static void app_fill_wifi_cfg_from_menuconfig(wifi_init_cfg_t* wifi_cfg);

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
    const bool has_saved = webconfig_load_sta_cfg(&saved);

    /* 用 menuconfig 默认值先把 wifi_cfg 填好（模式 / AP 参数 / STA IP 策略），
     * 再按"NVS 优先 / menuconfig 优先"的策略覆盖 STA ssid+密码。
     *
     * 优先级：
     *   1. NVS 有保存 → 用 NVS 的凭证（web 配网留下的运行时覆盖优先）
     *   2. 没保存但 menuconfig 给出了 STA SSID → 用 menuconfig 烧死进去的凭证
     *   3. 都没有 + 模式包含 STA → 走 web 配网
     *   4. 模式 = AP-only + 没保存 → wifi_cfg 里的 AP 参数已经齐全，
     *                                 直接 wifi_config_init() 当热点开
     *
     * 无论哪一支都跑 app_fill_wifi_cfg_from_menuconfig()，所以 wifi_cfg 里
     * 的 mode / AP / STA-IP 策略永远不会丢。 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    app_fill_wifi_cfg_from_menuconfig(&wifi_cfg);

    if (has_saved)
    {
        ESP_LOGI(TAG, "使用已保存的 WiFi 配置连接: %s", saved.ssid);

        strlcpy(wifi_cfg.sta_ssid, saved.ssid, sizeof(wifi_cfg.sta_ssid));
        strlcpy(wifi_cfg.sta_password, saved.password, sizeof(wifi_cfg.sta_password));
        err = wifi_config_init(&wifi_cfg);

        /* 不开热点，但把管理页挂上去：连上路由器后用它拿到的 IP 访问 */
        if (err == ESP_OK)
        {
            err = webconfig_httpd_start(&webconfig_cfg);
        }
    }
    else if (wifi_cfg.mode == WIFI_MODE_AP)
    {
        /* 模式 = AP-only 且没保存凭证：跳过 web 配网，直接当热点开。
         * 【绕过 webconfig_start() 是有意为之】web 配网是为"未知 STA 凭证"准备的；
         * 用户已经在 menuconfig 里决定只跑 AP，根本不需要 配网 UI 这一步。
         * 管理页仍正常挂上（webconfig_httpd_start 在热点上提供），
         * 后续若有需要可在管理页加一个"切回 STA 配网"的入口。 */
        ESP_LOGI(TAG, "模式 = AP-only，按 menuconfig 直接开机为热点");
        err = wifi_config_init(&wifi_cfg);

        if (err == ESP_OK)
        {
            err = webconfig_httpd_start(&webconfig_cfg);
        }
    }
#if defined(CONFIG_WIFI_STA_SSID) && defined(CONFIG_WIFI_STA_PASSWORD)
    /* 这一支编译期由 CONFIG_WIFI_STA_SSID 决定是否存在（depends on WIFI_HAS_STA）。
     * menuconfig 模式 = AP-only 时这两个宏都未定义、整段被预处理器吃掉，
     * 下面的 else 会自然挂到上面的 `wifi_cfg.mode == WIFI_MODE_AP` 那条上；
     * 不会留下"if / else if / (空) / else"这种 C 语言里非法的悬空结构。 */
    else if (wifi_cfg.mode == WIFI_MODE_STA && CONFIG_WIFI_STA_SSID[0] != '\0')
    {
        ESP_LOGI(TAG, "用 menuconfig 里的 STA 凭证连接: %s", CONFIG_WIFI_STA_SSID);

        strlcpy(wifi_cfg.sta_ssid, CONFIG_WIFI_STA_SSID, sizeof(wifi_cfg.sta_ssid));
        strlcpy(wifi_cfg.sta_password, CONFIG_WIFI_STA_PASSWORD, sizeof(wifi_cfg.sta_password));
        err = wifi_config_init(&wifi_cfg);

        if (err == ESP_OK)
        {
            err = webconfig_httpd_start(&webconfig_cfg);
        }
    }
#endif
    else
    {
        ESP_LOGI(TAG, "没有可用的 STA 凭证，启动配网热点");
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

/**
 * @brief  把 menuconfig 里的 WiFi 选项填进 wifi_cfg（mode / AP 参数 / STA IP 策略）
 *
 * @note   STA 的 ssid / 密码不在这里填 —— 那部分由调用方根据"NVS 优先 vs menuconfig
 *         优先"的策略来覆盖，helper 只负责"业务无关的、和 NVS 无关的"那些字段。
 *
 * @note   AP 信道目前固定为 8（与 webconfig 组件配网热点保持一致，避免用户切模式时
 *           因为信道变化而被踢下线）；将来若需要可在 Kconfig 加一个 WiFi AP channel 选项。
 */
static void app_fill_wifi_cfg_from_menuconfig(wifi_init_cfg_t* wifi_cfg)
{
    /* 模式 */
#if defined(CONFIG_WIFI_MODE_AP)
    wifi_cfg->mode = WIFI_MODE_AP;
#else
    wifi_cfg->mode = WIFI_MODE_STA;
#endif

    /* AP 参数：仅在模式包含 AP 时才下发（wifi 组件内部 want_ap=false 时会忽略） */
#if defined(CONFIG_WIFI_HAS_AP)
    strlcpy(wifi_cfg->ap_ssid, CONFIG_WIFI_AP_SSID, sizeof(wifi_cfg->ap_ssid));
    strlcpy(wifi_cfg->ap_password, CONFIG_WIFI_AP_PASSWORD, sizeof(wifi_cfg->ap_password));
    wifi_cfg->ap_channel = 8;
    wifi_cfg->ap_max_conn = CONFIG_WIFI_AP_MAX_CONN;
    /* AP 网段：netmask 固定 /24（与 ESP-IDF 默认一致），gateway 跟 IP 同地址，
     * IP 走 CONFIG_WIFI_AP_IP。wifi 组件在校验里要求三个字段同时给，所以这里也填上。 */
    wifi_cfg->ap_use_static_ip = true;
    strlcpy(wifi_cfg->ap_ip, CONFIG_WIFI_AP_IP, sizeof(wifi_cfg->ap_ip));
    strlcpy(wifi_cfg->ap_netmask, "255.255.255.0", sizeof(wifi_cfg->ap_netmask));
    strlcpy(wifi_cfg->ap_gateway, CONFIG_WIFI_AP_IP, sizeof(wifi_cfg->ap_gateway));
#endif

    /* STA 的 IP 策略：menuconfig 只有 STA / AP 两种模式，
     * AP 模式下 CONFIG_WIFI_HAS_STA 未定义、整段不参与编译；
     * 所以走到这里必然是 STA-only，直接按 menuconfig 的 DHCP 选择填。 */
#if defined(CONFIG_WIFI_HAS_STA)
#   if CONFIG_WIFI_STA_USE_DHCP
        wifi_cfg->sta_use_static_ip = false;
#   else
        wifi_cfg->sta_use_static_ip = true;
        strlcpy(wifi_cfg->sta_ip, CONFIG_WIFI_STA_STATIC_IP, sizeof(wifi_cfg->sta_ip));
        strlcpy(wifi_cfg->sta_netmask, CONFIG_WIFI_STA_STATIC_NETMASK,
                sizeof(wifi_cfg->sta_netmask));
        strlcpy(wifi_cfg->sta_gateway, CONFIG_WIFI_STA_STATIC_GATEWAY,
                sizeof(wifi_cfg->sta_gateway));
        /* DNS 留空 → wifi 组件视为"不设置"，跳过 esp_netif_set_dns_info()。 */
#       if defined(CONFIG_WIFI_STA_DNS_MAIN)
        strlcpy(wifi_cfg->sta_dns_main, CONFIG_WIFI_STA_DNS_MAIN,
                sizeof(wifi_cfg->sta_dns_main));
#       endif
#       if defined(CONFIG_WIFI_STA_DNS_BACKUP)
        strlcpy(wifi_cfg->sta_dns_backup, CONFIG_WIFI_STA_DNS_BACKUP,
                sizeof(wifi_cfg->sta_dns_backup));
#       endif
#   endif
#endif
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

    /* 切回 STA 模式连接：wifi_update_config() 会断开当前 AP、用新凭证重连。
     * 模式 / IP 策略沿用 menuconfig，只把用户在网页上提交的 ssid / 密码覆盖上去
     * —— 这样 web 配网不会把"DHCP vs 静态 IP"这条设定也连带清掉。 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    app_fill_wifi_cfg_from_menuconfig(&wifi_cfg);
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
#if CONFIG_SERVICE_ANY_ENABLE
            /* STA 掉线 → STA 侧 IP 已无效：清掉就绪标志，让 tcp_conn_task
             * 暂停等 WiFi 重新连上。esp_wifi 默认会自己重连，GOT_IP 事件
             * 一回来又会置回 true。
             *
             * 注意：这里按"纯 STA"语义处理。将来若改成 APSTA 共存，STA 掉线
             * 时 AP 侧可能还在、连接也可能还活着，就不能无条件清这个标志了。 */
            s_net_ready = false;
#endif /* CONFIG_SERVICE_ANY_ENABLE：无服务时这个标志不存在 */
        }
        else if (id == WIFI_EVENT_AP_START)
        {
#if CONFIG_SERVICE_ANY_ENABLE
            /* AP 模式：设备自己是热点（192.168.4.1），拿不到 STA 侧 IP，但热点
             * 内部本身就是一个可用的局域网 —— 设备与接入的客户端同网段、二层
             * 互通，所以 TCP client 照样能工作，连的服务器就是"挂在同一个热点
             * 下的上位机"（地址由 tcp_app_pick_target() 按模式挑选）。
             *
             * 这一支就是"AP 模式下也能用 TCP client"的入口：不开这个门，
             * tcp_conn_task 会永远卡在等就绪的循环里，一次 connect 都不发。 */
            s_net_ready = true;
            ESP_LOGI(TAG, "AP 已启动：热点内网络可用，放行 TCP 业务");
#endif /* CONFIG_SERVICE_ANY_ENABLE：无服务时不需要"放行 TCP" */
        }
        return;
    }

    /* 拿到 IP 说明设备已经挂在路由器网段上：管理页就在这个地址，
     * 打出来省得去路由器后台翻（DHCP 分配的地址不固定） */
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
#if CONFIG_SERVICE_ANY_ENABLE
        /* 给 TCP 长连接任务开门：让 tcp_conn_task 看到 s_net_ready 后往下走 */
        s_net_ready = true;
#endif /* CONFIG_SERVICE_ANY_ENABLE */
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
 *   tcp_conn_task     : 等 IP → open → 循环 recv → 投 RX 队列 → 出错 close 重连
 *   tx_consumer_task  : 阻塞等 g_tx_queue → 按 item.kind 派发 send
 * 收发共用同一个 fd，用 g_fd_mutex 保证"send 期间不会被 close"。
 */

/* ===================== 网络服务实现（无服务时整段消失）=====================
 * 从这里一直到文件末尾，全部是"网络服务"的实现：
 *   - 三个共用 helper（breakable_delay_ms / tx_queue_post / rx_queue_post）
 *   - 共用上行入口（tcp_app_send / udp_app_send / srv_app_send）
 *   - 业务处理体 tcp_app_handle_frame / tcp_app_on_recv
 *   - 常驻的 tx_consumer_task / rx_consumer_task
 *   - TCP 客户端 / TCP 服务端 / UDP 三个服务的完整实现
 *   - 文件末尾给 service_manager 用的一组 *_service_fn / *_request_stop / *_is_running
 * menuconfig 里三个服务一个都没勾选时，这些没有任何调用者，整体编译掉
 * （判据 SERVICE_ANY_ENABLE 与 Kconfig 隐藏菜单、service_manager.c 编译掉
 * 整个模式管理用的是同一个）。 */
#if CONFIG_SERVICE_ANY_ENABLE

/**
 * @brief "可被打断的 vTaskDelay"：每 50ms 醒来查一次 stop 标志，
 *        标志位被 service_fn 置位就立刻返回
 *
 * @param ms     最长等待毫秒数（> 0）
 * @param stop   指向停服标志位（volatile bool*），调用方应传自己 service 的标志
 *
 * @note 返回值无意义 —— 调用方只关心"等到的时间够不够 ms"，自己再判断。
 *       标志位为 true 时函数提前返回，调用方应当退出外层循环。
 */
static void breakable_delay_ms(int ms, volatile bool* stop)
{
    while (ms > 0 && !*stop)
    {
        const int step = (ms < 50) ? ms : 50;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
}

/**
 * @brief 【业务上传入口】把一帧数据投到上行队列，立刻返回
 *
 * 非阻塞、线程安全：任何任务里都能调；真正的 send() 由 tx_consumer_task 去做，
 * 所以调用者不会被网络阻塞。适合"随时都可能要上传数据"的场景。
 *
 * @param data 待上传数据，不可为 NULL
 * @param len  字节数，1 ~ CONFIG_RX_MAX_FRAME
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
/* ===================== 共用收发 API + consumer 任务 =====================
 * 三个 service（TCP_CLIENT / TCP_SERVER / UDP）共用同一对队列：
 * 上行入口（tcp_app_send / udp_app_send / srv_app_send）经 tx_queue_post 投
 * g_tx_queue，由 tx_consumer_task 取出后按 item.kind 派发；下行由各 recv task
 * 经 rx_queue_post 投 g_rx_queue，由 rx_consumer_task 取出后调
 * tcp_app_on_recv()。
 *
 * consumer 任务本体（tx_consumer_task / rx_consumer_task）不嵌 #if —— 服务全关
 * 时它们仍然存在但永远空转（阻塞在队列上，0 CPU 占用）；而"上行入口"
 * （tcp_app_send 等）以及上面这两个队列 helper 只在至少启用一个 service 时才有
 * 调用者，各自用 #if 包住。 */

/**
 * @brief 内部 helper：构造 tx_item_t 投 g_tx_queue
 *
 * 三个上行入口（tcp_app_send / udp_app_send / srv_app_send）共用本函数。
 * fd / dst_ip / dst_port 的语义按 kind 决定：
 *   - TX_KIND_TCP_CLIENT：fd=-1（用全局 g_fd），dst_ip/port 忽略；
 *   - TX_KIND_TCP_SERVER：fd=目标 client fd，dst_ip/port 忽略；
 *   - TX_KIND_UDP：fd=-1，dst_ip/port=目标地址。
 */
static bool tx_queue_post(tx_item_kind_t kind, int fd, uint32_t dst_ip, uint16_t dst_port,
                          const void* data, size_t len)
{
    if (g_tx_queue == NULL || data == NULL || len == 0 || len > CONFIG_RX_MAX_FRAME)
    {
        return false;
    }
    tx_item_t item = { 0 };
    item.kind = kind;
    item.fd = fd;
    item.dst_ip = dst_ip;
    item.dst_port = dst_port;
    item.len = (uint16_t)len;
    memcpy(item.data, data, len);
    /* 0 超时：队列满不等待，立刻返回 false（由调用方决定怎么处理） */
    return xQueueSend(g_tx_queue, &item, 0) == pdTRUE;
}

/**
 * @brief 内部 helper：构造 rx_item_t 投 g_rx_queue
 *
 * 调用方是各服务的 recv task（tcp_conn_task / tcp_server_client_task /
 * udp_task），不是业务代码 —— 业务代码只看到 tcp_app_on_recv()。
 * fd / src_ip / src_port 的语义按 kind 决定：
 *   - RX_KIND_TCP_CLIENT：fd=-1，src_ip/port 忽略；
 *   - RX_KIND_TCP_SERVER：fd=来源 client fd，src_ip/port 忽略；
 *   - RX_KIND_UDP：fd=UDP socket fd，src_ip/port=对端地址。
 */
static bool rx_queue_post(rx_item_kind_t kind, int fd, uint32_t src_ip, uint16_t src_port,
                          const void* data, size_t len)
{
    if (g_rx_queue == NULL || data == NULL || len == 0 || len > CONFIG_RX_MAX_FRAME)
    {
        return false;
    }
    rx_item_t item = { 0 };
    item.kind = kind;
    item.fd = fd;
    item.src_ip = src_ip;
    item.src_port = src_port;
    item.len = (uint16_t)len;
    memcpy(item.data, data, len);
    return xQueueSend(g_rx_queue, &item, 0) == pdTRUE;
}

#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
/**
 * @brief TCP 客户端上行入口：构造 TX_KIND_TCP_CLIENT 投 g_tx_queue
 *
 * @note 非阻塞，队列满时丢弃并打 warning。调用方可以是任意任务。
 */
bool tcp_app_send(const void* data, size_t len)
{
    if (!tx_queue_post(TX_KIND_TCP_CLIENT, -1, 0, 0, data, len))
    {
        ESP_LOGW(TAG, "上行队列满，本帧 %u 字节被丢弃", (unsigned)len);
        return false;
    }
    return true;
}
#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */

#if CONFIG_SERVICE_UDP_ENABLE
/**
 * @brief UDP 上行入口：构造 TX_KIND_UDP 投 g_tx_queue
 *
 * @param dst_ip 网络字节序的 IPv4
 * @param dst_port 主机字节序端口
 */
bool udp_app_send(uint32_t dst_ip, uint16_t dst_port, const void* data, size_t len)
{
    if (dst_ip == 0 || dst_port == 0)
    {
        return false;
    }
    if (!tx_queue_post(TX_KIND_UDP, -1, dst_ip, dst_port, data, len))
    {
        ESP_LOGW(TAG, "上行队列满，本帧 %u 字节被丢弃", (unsigned)len);
        return false;
    }
    return true;
}
#endif /* CONFIG_SERVICE_UDP_ENABLE */

#if CONFIG_SERVICE_TCP_SERVER_ENABLE
/**
 * @brief TCP 服务端回送入口：构造 TX_KIND_TCP_SERVER 投 g_tx_queue
 *
 * @param client_fd 要回送的目标 client fd（来自 tcp_server_accept）
 */
bool srv_app_send(int client_fd, const void* data, size_t len)
{
    if (client_fd < 0)
    {
        return false;
    }
    if (!tx_queue_post(TX_KIND_TCP_SERVER, client_fd, 0, 0, data, len))
    {
        ESP_LOGW(TAG, "上行队列满，本帧 %u 字节被丢弃", (unsigned)len);
        return false;
    }
    return true;
}
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */

/**
 * @brief 【业务处理体】处理一段已收到的数据 —— 当前实现是"原样回传（echo）"
 *
 * 不管是 TCP_CLIENT / TCP_SERVER / UDP 三种 RX_KIND 都走这里：
 * echo 场景下"原路返回"对应的协议就是按"来源种类"反向投递：
 *   - RX_KIND_TCP_CLIENT → tcp_app_send()
 *   - RX_KIND_TCP_SERVER → srv_app_send(item.fd)
 *   - RX_KIND_UDP        → udp_app_send(item.src_ip, item.src_port)
 *
 * 接真实业务时，把函数体换成协议解析。item->kind 给业务代码做分发用。
 */
static void tcp_app_handle_frame(const rx_item_t* item)
{
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
    /* 心跳回包不回显：echo 服务器会把心跳原样发回来，若也走回传，
     * 就变成"心跳 → 回显 → 回显 → ..."无限乒乓。
     * 心跳宏只在 TCP_CLIENT 启用时定义，所以这段也必须跟着 #if 走。 */
    if (item->kind == RX_KIND_TCP_CLIENT && item->len == TCP_APP_HEARTBEAT_LEN &&
        memcmp(item->data, TCP_APP_HEARTBEAT_PAYLOAD, TCP_APP_HEARTBEAT_LEN) == 0)
    {
        return;
    }
#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */

    bool ok = false;
    /* 【每个 case 都要单独包 #if】三个上行入口（tcp_app_send / srv_app_send /
     * udp_app_send）各自只在"对应的 service 启用"时才有定义；某个 service 关掉时
     * 仍保留它的 case，会撞上 "implicit declaration" —— 本工程是 -std=gnu23，
     * 这直接是 error，不是 warning。 */
    switch (item->kind)
    {
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
        case RX_KIND_TCP_CLIENT:
            ok = tcp_app_send(item->data, item->len);
            break;
#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
        case RX_KIND_TCP_SERVER:
            ok = srv_app_send(item->fd, item->data, item->len);
            break;
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */
#if CONFIG_SERVICE_UDP_ENABLE
        case RX_KIND_UDP:
            ok = udp_app_send(item->src_ip, item->src_port, item->data, item->len);
            break;
#endif /* CONFIG_SERVICE_UDP_ENABLE */
        default:
            return;
    }
    if (!ok)
    {
        ESP_LOGW(TAG, "回传失败：上行队列满，本段 %u 字节被丢弃", (unsigned)item->len);
    }
}

/**
 * @brief 【收数据入口】rx_consumer_task 取出 item 后调本函数
 *
 * 业务代码只需关心 tcp_app_handle_frame()；本函数目前只做转发，
 * 留作将来加 hook（统计 / 鉴权 / 多路分发）的钩子。
 */
static void tcp_app_on_recv(const rx_item_t* item)
{
    if (item == NULL || item->len == 0)
    {
        return;
    }
    tcp_app_handle_frame(item);
}

/**
 * @brief 上行发送消费任务（全局常驻）：阻塞等 g_tx_queue → 按 item.kind 派发 send
 *
 * service 全开/全关都跑着（永远空闲等队列）；100ms 轮询停服标志避免死卡。
 */
static void tx_consumer_task(void* arg)
{
    (void)arg;
    s_tx_consumer_alive = true;
    tx_item_t item;

    while (!s_tx_consumer_stop)
    {
        if (xQueueReceive(g_tx_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            continue;
        }

        switch (item.kind)
        {
            case TX_KIND_TCP_CLIENT:
            {
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
                /* 用互斥锁保护 g_fd + send：避免 send 期间被 conn_task close */
                xSemaphoreTake(g_fd_mutex, portMAX_DELAY);
                const int fd = g_fd;
                if (fd < 0)
                {
                    ESP_LOGW(TAG, "暂无连接，丢弃上行帧（%u 字节）", (unsigned)item.len);
                    xSemaphoreGive(g_fd_mutex);
                    break;
                }
                /* 循环补发（TCP 是字节流，send 可能只写一部分） */
                size_t sent = 0;
                esp_err_t err = ESP_OK;
                const int64_t t0_us = esp_timer_get_time();
                while (sent < item.len)
                {
                    size_t n = 0;
                    err = tcp_client_send(fd, item.data + sent, item.len - sent, &n);
                    if (err != ESP_OK)
                        break;
                    if (n == 0)
                    {
                        err = ESP_FAIL;
                        break;
                    }
                    sent += n;
                }
                if (sent < item.len)
                {
                    ESP_LOGW(TAG, "上行失败：%u/%u 字节已写出 (err=%d)", (unsigned)sent,
                             (unsigned)item.len, (int)err);
                }
                else
                {
                    s_last_traffic_tick = xTaskGetTickCount();
                    ESP_LOGI(TAG, "TX %u 字节", (unsigned)sent);
                }
                const int64_t cost_us = esp_timer_get_time() - t0_us;
                if (cost_us > CONFIG_TCP_CLIENT_SEND_SLOW_US)
                {
                    ESP_LOGW(TAG, "send() 耗时 %lld ms —— 延迟出在协议栈/射频侧",
                             (long long)(cost_us / 1000));
                }
                xSemaphoreGive(g_fd_mutex);
#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */
                break;
            }
            case TX_KIND_TCP_SERVER:
            {
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
                /* 客户端 echo：循环补发 */
                size_t sent = 0;
                while (sent < item.len)
                {
                    size_t n = 0;
                    esp_err_t err = tcp_server_send(item.fd, item.data + sent, item.len - sent, &n);
                    if (err != ESP_OK || n == 0)
                        break;
                    sent += n;
                }
                if (sent < item.len)
                {
                    ESP_LOGW(TAG, "服务端 echo 失败：%u/%u 字节 (fd=%d)", (unsigned)sent,
                             (unsigned)item.len, item.fd);
                }
                else
                {
                    ESP_LOGI(TAG, "TX %u 字节 (srv fd=%d)", (unsigned)sent, item.fd);
                }
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */
                break;
            }
            case TX_KIND_UDP:
            {
#if CONFIG_SERVICE_UDP_ENABLE
                const int udp_fd = g_udp_fd;
                if (udp_fd < 0)
                {
                    ESP_LOGW(TAG, "UDP socket 未就绪，丢弃上行帧（%u 字节）", (unsigned)item.len);
                    break;
                }
                size_t sent = 0;
                esp_err_t err = udp_socket_sendto(udp_fd, item.data, item.len, item.dst_ip,
                                                  item.dst_port, &sent);
                if (err != ESP_OK || sent != item.len)
                {
                    ESP_LOGW(TAG, "udp 上行失败：%u/%u 字节 (err=%d)", (unsigned)sent,
                             (unsigned)item.len, (int)err);
                }
                else
                {
                    esp_ip4_addr_t peer = { .addr = item.dst_ip };
                    ESP_LOGI(TAG, "TX %u 字节 (udp to " IPSTR ":%u)", (unsigned)sent, IP2STR(&peer),
                             (unsigned)item.dst_port);
                }
#endif /* CONFIG_SERVICE_UDP_ENABLE */
                break;
            }
            default:
                break;
        }
    }
    s_tx_consumer_alive = false;
    vTaskDelete(NULL);
}

/**
 * @brief 下行接收消费任务（全局常驻）：阻塞等 g_rx_queue → 调 tcp_app_on_recv()
 *
 * 把三个服务的"收到一段数据"都统一到这一个 task 处理；业务逻辑只需关心
 * tcp_app_handle_frame()（由 tcp_app_on_recv() 调用）。
 */
static void rx_consumer_task(void* arg)
{
    (void)arg;
    s_rx_consumer_alive = true;
    rx_item_t item;

    while (!s_rx_consumer_stop)
    {
        if (xQueueReceive(g_rx_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            continue;
        }
        tcp_app_on_recv(&item);
    }
    s_rx_consumer_alive = false;
    vTaskDelete(NULL);
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
 *   - CONFIG_TCP_SERVER_IDLE_TIMEOUT_MS 空闲断开：对端还活着、但一直不说话。
 * 缺了这两道，僵尸连接会把两个槽位永久占满，服务端从此接不进新客户端。
 */

/* 找一个真正的空槽：fd 和 task 都归零才算 —— 只看 fd 会把"fd 已回收、handler
 * 还没退出来"的槽位误判为空闲，新连接复用后会踩到旧 handler。 */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
static int tcp_server_alloc_slot(void)
{
    for (int i = 0; i < CONFIG_TCP_SERVER_MAX_CLIENTS; i++)
    {
        if (s_server_clients[i].fd < 0 && s_server_clients[i].task == NULL)
        {
            return i;
        }
    }
    return -1;
}
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */

/* 把槽位标成真正的空槽。只在确定"既没有活着的 handler、也没有待回收的 fd"
 * 时调用：server 任务启动初始化、xTaskCreate 失败回滚，以及 handler 自己
 * 确认所有权后退出。 */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
static void tcp_server_clear_slot(int idx)
{
    s_server_clients[idx].fd = -1;
    s_server_clients[idx].task = NULL;
}
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */

/**
 * @brief 服务端每个连接的回环任务：阻塞 recv → 投 RX 队列
 *
 * @param arg 槽位下标（intptr_t）
 *
 * @note 【统一队列后路径变了】旧版 recv 到 buf 后直 send 回 fd；
 *       现在改成"投到 rx 队列"——
 *       rx_consumer_task 取出后调 tcp_app_handle_frame() → srv_app_send()
 *       → tx_consumer_task 看到 kind=TX_KIND_TCP_SERVER → 用 item.fd 发回。
 *       链路变长但不再需要"每个 handler 挂一份 BSS buf"，
 *       栈上 rx_item_t 本身就是 handler 私有的临时缓冲。
 *
 * 【handler 栈】4096：栈上 rx_item_t ≈ 1KB + ESP_LOG 路径峰值 ~1KB +
 *                    函数调用栈帧 200B，4096 余量足够。
 */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
static void tcp_server_client_task(void* arg)
{
    const int idx = (int)(intptr_t)arg;
    const int fd = s_server_clients[idx].fd;
    /* 最后一次收到数据的时刻，用来识别"连接还在、对端却不再说话"的连接。
     * keepalive 只能发现"对端已经死了"，发现不了"对端活着但一直不发数据"——
     * 后者同样会让本任务永远占着槽位。 */
    TickType_t last_rx = xTaskGetTickCount();

    ESP_LOGI(TAG, "server client[%d] 已接入 (fd=%d)", idx, fd);

    while (s_server_running)
    {
        /* 栈上 rx_item_t：handler 私有的 recv 缓冲，函数退出才释放，
         * 与 task 一一对应，不存在多 handler 互踩。 */
        rx_item_t item = { 0 };
        size_t got = 0;
        esp_err_t err = tcp_server_recv(fd, item.data, sizeof(item.data), &got);
        item.len = (uint16_t)got;
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
        if (item.len == 0)
        {
            /* 本轮 recv 超时（5s 内没数据）。"超时"是空闲连接的常态而非故障，
             * 所以不能一超时就断开；只有累计空闲超过阈值才判定为死连接。
             * 检查放在这个分支里：收到数据的路径下面会刷新 last_rx，没必要
             * 每轮都算一次。 */
            if (CONFIG_TCP_SERVER_IDLE_TIMEOUT_MS > 0 &&
                (xTaskGetTickCount() - last_rx) >= pdMS_TO_TICKS(CONFIG_TCP_SERVER_IDLE_TIMEOUT_MS))
            {
                ESP_LOGW(TAG, "server client[%d] 空闲 %d ms 无数据，主动断开", idx,
                         CONFIG_TCP_SERVER_IDLE_TIMEOUT_MS);
                break;
            }
            continue;
        }

        last_rx = xTaskGetTickCount();

        ESP_LOGI(TAG, "server RX %u 字节 (client[%d])", (unsigned)item.len, idx);

        /* 投到共用 rx 队列 —— rx_consumer_task 取出后调 tcp_app_on_recv()
         * → tcp_app_handle_frame() → srv_app_send() → tx_consumer_task 发回 */
        item.kind = RX_KIND_TCP_SERVER;
        item.fd = fd;
        if (!rx_queue_post(RX_KIND_TCP_SERVER, fd, 0, 0, item.data, item.len))
        {
            ESP_LOGW(TAG, "server client[%d] RX 队列满，本段 %u 字节被丢弃", idx,
                     (unsigned)item.len);
        }
    }

    /* 清理必须"只动还属于自己的槽位"，否则会误伤新连接：
     *   task == 自己 → 正常收尾：fd 字段还是自己就顺手 close，再释放槽位；
     *   task == NULL → server 关停时已经替我们 close 过 fd 并把它置成 -1 了，
     *                  不能再 close —— 那个编号可能已经分配给别的连接了。 */
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
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */

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
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
static void tcp_server_task(void* arg)
{
    (void)arg;
    s_server_alive = true;

    for (int i = 0; i < CONFIG_TCP_SERVER_MAX_CLIENTS; i++)
    {
        tcp_server_clear_slot(i);
    }

    while (!s_server_stop)
    {
        /* 1) 等网络就绪（同时尊重停服标志，避免"切换模式后这一支一直卡住"） */
        while (!s_net_ready && !s_server_stop)
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        if (s_server_stop)
        {
            break;
        }

        /* 2) 取本机 IPv4 作为 bind 地址（AP=192.168.4.1 / STA=路由器分配的
         *    地址）；拿不到时回退通配 0.0.0.0，保证 server 不会因为"IP 还没
         *    就绪"直接起不来。 */
        tcp_server_cfg_t cfg = TCP_SERVER_CFG_DEFAULT();
        cfg.port = CONFIG_TCP_SERVER_PORT;
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
            breakable_delay_ms(2000, &s_server_stop);
            continue;
        }

        s_server_running = true;

        /* 4) accept 循环 */
        bool ip_changed = false; /* true = 退出原因是"本机 IP 变了"，网络仍然可用 */
        while (s_net_ready && !s_server_stop)
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
                ESP_LOGW(TAG, "已达最大并发 %u，拒绝新连接",
                         (unsigned)CONFIG_TCP_SERVER_MAX_CLIENTS);
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

        /* 5) 关停收尾 —— 三种退出原因共用同一套清理，区别只在日志措辞和
         *    "清理完后是重建还是退出"：
         *   - 网络断开 (ip_changed=false)          → 清理后回外层 while 重建
         *   - 本机 IP 变了 (ip_changed=true)        → 清理后回外层 while 重建
         *   - service_manager 要求停止 (s_server_stop=true) → 清理后 break 退出
         *
         * 【顺序很重要】必须先做完"关 listen + 关 client fd + 等 handler"，
         * 最后才判 s_server_stop 决定走还是留。若像早先那样一见 s_server_stop
         * 就 break，listen socket 和所有 client socket 都不会被 close ——
         * fd 泄漏之外，下次再切回 TCP_SERVER 时 bind 8888 会因为端口仍被
         * 自己占着而失败。 */
        if (s_server_stop)
        {
            ESP_LOGW(TAG, "service_manager 要求停止 TCP 服务端");
        }
        else if (ip_changed)
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
        for (int i = 0; i < CONFIG_TCP_SERVER_MAX_CLIENTS; i++)
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
         * 这里最多等 6s（60 × 100ms）—— 刻意比 SO_SNDTIMEO 长一点点，好让
         * "卡在 send 上"这种最慢的 handler 也能自行退出。仍然没退出的【不强清】：
         * 强清会把槽位标成空闲，新连接随即复用它，而旧 handler 手里的 fd 编号
         * 可能已被这个新连接重新分配，那就真会误伤。保持"占用"状态让新连接
         * 避开这个槽位，是更安全的降级方向（最坏情况只是并发数临时少一个）。 */
        for (int waited = 0; waited < 60; waited++)
        {
            bool all_gone = true;
            for (int i = 0; i < CONFIG_TCP_SERVER_MAX_CLIENTS; i++)
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

        /* 停服：上面 listen / client fd 都关干净了，handler 也等过 6s，
         * 这时候退出整个任务才安全（端口已释放，新 service 能 bind 上）。
         *
         * 万一 6s 后仍有 handler 没退（卡在没有超时的调用里），这里【仍然退出】
         * 而不是无限等 —— 否则用户永远切不了模式。残留 handler 会把槽位保持
         * 占用（新连接接不进来），但不会影响端口释放，属可接受的降级；明确
         * 打日志把这种情况暴露出来，便于排查。 */
        if (s_server_stop)
        {
            for (int i = 0; i < CONFIG_TCP_SERVER_MAX_CLIENTS; i++)
            {
                if (s_server_clients[i].task != NULL)
                {
                    ESP_LOGE(TAG,
                             "停服：槽位 %d 的 client handler 仍未退出，该槽位保持"
                             "占用（不影响端口释放，但此槽位暂时接不了新连接）",
                             i);
                }
            }
            break;
        }
    }
    s_server_alive = false;

    /* 清空共用队列：本 service 退出后残留的 RX/TX item 不应该被下一个 service 的
     * rx_consumer_task 当成"上一个 service 的回声"处理（特别是 TX_KIND_TCP_SERVER
     * 残留里的 item.fd 已是 close 后的旧编号，tx_consumer 拿去 send 会刷一批失败日志）。
     * 与 tcp_client 的清理点对齐：tcp_client 在 service_fn 内清；tcp_server / udp
     * 因为 worker 自杀不返回，清理必须放在 worker 内部、vTaskDelete 之前。 */
    if (g_tx_queue != NULL)
    {
        (void)xQueueReset(g_tx_queue);
    }
    if (g_rx_queue != NULL)
    {
        (void)xQueueReset(g_rx_queue);
    }

    vTaskDelete(NULL);
}
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */

/**
 * @brief 按当前 WiFi 模式挑一个写死的默认目标地址（"地址先写死"阶段的临时手段）
 *
 * @note 逻辑刻意做得简单，不查任何配置：
 *       - 纯 AP 模式 → 连热点内的上位机（CONFIG_TCP_CLIENT_HOST_AP）；
 *       - 其它模式   → 连路由器网段的服务器（CONFIG_TCP_CLIENT_HOST_STA）；
 *         APSTA 也走这一支：既然还挂着路由器，服务器就该在路由器那边。
 *
 * @note 【后期扩展点】网页配置做好后，本函数改成"先查 NVS 里用户填的地址，
 *       查不到才回落到这里的默认值"—— 调用点（tcp_conn_task）一行都不用改。
 */
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
static void tcp_app_pick_target(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);

    const bool ap_only = (mode == WIFI_MODE_AP);
    strlcpy(s_target_host, ap_only ? CONFIG_TCP_CLIENT_HOST_AP : CONFIG_TCP_CLIENT_HOST_STA,
            sizeof(s_target_host));
    s_target_port = CONFIG_TCP_CLIENT_PORT;
}
#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */

/* 本函数被两个服务用：tcp_server_task（取 bind 地址 + 检测 IP 变化）和
 * udp_task（同用途）。两个服务都关掉时它整个编译掉 —— 否则 static 函数
 * 无人引用，会触发 -Wunused-function（本工程开着 -Wall，会多一条 warning）。 */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE || CONFIG_SERVICE_UDP_ENABLE
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
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE || CONFIG_SERVICE_UDP_ENABLE */

/**
 * @brief UDP 通讯任务：等网络就绪 → open → 循环 recvfrom → 收到就回发（echo）
 *
 * 路径：
 *   1) 启动时无限等 s_net_ready（与 tcp_server_task 共享门控）；
 *   2) 取本机 IPv4 作为 bind 地址（同 tcp_server_task 的处理）；
 *   3) udp_socket_open：绑 CONFIG_UDP_PORT；
 *   4) recvfrom 循环（1s 超时），收到数据就 sendto 回发（echo）；
 *   5) 关停触发条件（三种，共用同一套收尾）：
 *      - s_net_ready=false：网络断开；
 *      - 本机 IP 变了：网络仍在但绑的地址失效（AP 自定义网段场景）；
 *      - recvfrom 出错：socket 本身可能已坏（退避 2s 再重建）；
 *      收尾：s_udp_running=false → close socket → 回第 2 步重建。
 *
 * @note 与 tcp_server_task 的关键区别：UDP 没有"连接"概念，所以**没有
 *   handler 任务、没有槽位、没有 fd 泄漏 / 缓冲区互踩等问题**。
 *   关停时直接 close：等待中的 recvfrom 立刻返错退出，干净利落。
 */
#if CONFIG_SERVICE_UDP_ENABLE
static void udp_task(void* arg)
{
    (void)arg;
    s_udp_alive = true;

    while (!s_udp_stop)
    {
        /* 1) 等网络就绪（同时尊重停服标志） */
        while (!s_net_ready && !s_udp_stop)
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        if (s_udp_stop)
        {
            break;
        }

        /* 2) 取本机 IP 作为 bind；拿不到时回退通配 0.0.0.0，保证 UDP 不会
         *    因为"IP 还没就绪"直接起不来。 */
        udp_cfg_t cfg = UDP_CFG_DEFAULT();
        cfg.port = CONFIG_UDP_PORT;
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
            breakable_delay_ms(2000, &s_udp_stop);
            continue;
        }
        g_udp_fd = fd; /* tx_consumer_task 通过它 sendto */

        s_udp_running = true;

        /* 4) recvfrom 循环（1s 超时） */
        /* 退出原因（决定收尾日志措辞、以及要不要退避）：
         *   0 = 网络断开（s_net_ready 变 false）；
         *   1 = 本机 IP 变了（网络仍可用，改绑新地址即可，无需退避）；
         *   2 = recvfrom 出错（socket 可能已坏，退避 2s 再重建，避免空转）；
         *   3 = service_manager 要求停止（直接走完收尾，不再重建）。 */
        int stop_reason = 0;
        while (s_net_ready && !s_udp_stop)
        {
            /* 栈上 rx_item_t：与 server handler 同样的设计 —— 任务私有 recv 缓冲，
             * 退出即释放；BSS 不再被每服务各占一份。 */
            rx_item_t item = { 0 };
            size_t got = 0;
            uint32_t src_ip = 0;
            uint16_t src_port = 0;
            err = udp_socket_recvfrom(fd, item.data, sizeof(item.data), &src_ip, &src_port, &got);
            item.len = (uint16_t)got;
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "udp recvfrom 错误: %s", esp_err_to_name(err));
                stop_reason = 2;
                break;
            }

            if (item.len == 0)
            {
                /* 超时（1s 内没数据）—— 顺手看一眼本机 IP 是否变了。
                 * 与 tcp_server_task 同思路：s_net_ready 只知道"有没有网"，
                 * 不知道 IP 是多少；AP 自定义网段是在 esp_wifi_start() 之后
                 * 才下发到 netif 的，正好落在这个窗口里。 */
                const uint32_t now_ip = tcp_app_get_local_ip();
                if (now_ip != 0 && now_ip != cfg.bind_addr)
                {
                    stop_reason = 1;
                    break;
                }
                continue;
            }

            esp_ip4_addr_t peer = { .addr = src_ip };
            ESP_LOGI(TAG, "udp RX %u 字节 (from " IPSTR ":%u)", (unsigned)item.len, IP2STR(&peer),
                     (unsigned)src_port);

            /* 数据报打满缓冲区 = 超长部分被内核直接丢弃、收端无从感知，
             * 只能靠这条日志提示（调到 CONFIG_RX_MAX_FRAME 以上）。 */
            if (item.len == sizeof(item.data))
            {
                ESP_LOGW(TAG, "数据报已打满 %u 字节缓冲区，超长部分被静默丢弃（调大 RX_MAX_FRAME）",
                         (unsigned)sizeof(item.data));
            }

            /* echo —— 投给共用 rx 队列（→ rx_consumer_task → udp_app_send → tx_consumer_task
             * 通过 g_udp_fd 发回对端）。不在本任务里直 sendto 是为了和 TCP 那两个
             * 服务走同一条数据通路。 */
            if (!rx_queue_post(RX_KIND_UDP, fd, src_ip, src_port, item.data, item.len))
            {
                ESP_LOGW(TAG, "RX 队列满，本段 %u 字节被丢弃", (unsigned)item.len);
            }
        }

        /* 从 recvfrom 循环出来了：若退出原因是"被要求停服"，补上 stop_reason。
         * 循环条件是 (s_net_ready && !s_udp_stop)，单靠 s_udp_stop 退出时
         * stop_reason 仍是默认 0 —— 不补这一句就会被下面的 else 误报成
         * "网络断开，停止 UDP 通讯"。 */
        if (s_udp_stop)
        {
            stop_reason = 3;
        }

        /* 5) 关停收尾 —— 四种退出原因共用同一套，区别只在日志措辞与是否退避。
         * 与 tcp_server_task 不同的关键点：**没有等 handler 退出这一步**——
         * UDP 无连接，close(fd) 后正在阻塞的 recvfrom 立刻返错退出，唯一
         * 任务就是 udp_task 自己，关停是同步的、确定的。 */
        s_udp_running = false;
        g_udp_fd = -1; /* 让 tx_consumer_task 看到 socket 没了 */
        (void)udp_socket_close(fd);

        if (stop_reason == 1)
        {
            /* 网络还在：回第 1 步会立刻通过 s_net_ready，无需退避 */
            ESP_LOGW(TAG, "本机 IP 已变化，重建 UDP socket（改绑新地址）");
        }
        else if (stop_reason == 2)
        {
            /* 不是网络问题，立刻重建多半还会失败；退避一下避免空转刷日志 */
            ESP_LOGW(TAG, "UDP socket 异常，2s 后退避重建");
            breakable_delay_ms(2000, &s_udp_stop);
        }
        else if (stop_reason == 3)
        {
            /* service_manager 要求停止：直接退出，不再回到外层 while 重建 */
            ESP_LOGW(TAG, "service_manager 要求停止 UDP");
            break;
        }
        else
        {
            ESP_LOGW(TAG, "网络断开，停止 UDP 通讯");
        }
    }
    /* 函数退出 → service_fn 看到的 s_udp_alive 是 true 还是 false？ */
    s_udp_alive = false;

    /* 清空共用队列：理由同 tcp_server_task。UDP 残留 item 因为 g_udp_fd 已被
     * 前面的 close + 置 -1 流程兜住，处理时会被安全丢弃（"UDP socket 未就绪"
     * 分支），但清掉仍然更干净，避免 rx_consumer 多跑一轮无效分发。 */
    if (g_tx_queue != NULL)
    {
        (void)xQueueReset(g_tx_queue);
    }
    if (g_rx_queue != NULL)
    {
        (void)xQueueReset(g_rx_queue);
    }

    vTaskDelete(NULL);
}
#endif /* CONFIG_SERVICE_UDP_ENABLE */

/**
 * @brief 连接管理与接收任务：等网络就绪 → open → 循环 recv → 断开重连
 *
 * 接收路径：recv 一返回就投到共用 g_rx_queue，由 rx_consumer_task 取出后
 * 调 tcp_app_on_recv()。本任务里没有 sleep、没有轮次控制。
 * 【注】早先有 CONFIG_TCP_CLIENT_RX_QUEUE_LEN=0 的"直通模式"——已废除：
 *       三个服务现在都统一走队列（架构统一、业务延迟在帧延迟级，可忽略）。
 *
 * "网络就绪"由 s_net_ready 表示，两条路都会放行：
 *   - STA 模式：IP_EVENT_STA_GOT_IP（挂上路由器拿到 IP）；
 *   - AP  模式：WIFI_EVENT_AP_START（热点起来，热点内可互通）。
 */
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
static void tcp_conn_task(void* arg)
{
    (void)arg;
    s_tcp_conn_alive = true;

    /* 1) 启动阶段：无限等网络就绪（不因暂时无网退出任务）
     * 用 breakable_delay_ms 而不是裸 vTaskDelay：停服标志一到，最坏 50ms
     * 就退出，而不是等满 1 秒。 */
    ESP_LOGI(TAG, "TCP 长连接任务启动，等待网络就绪（STA 拿 IP 或 AP 启动，无限等）...");
    while (!s_net_ready && !s_tcp_conn_stop)
    {
        breakable_delay_ms(1000, &s_tcp_conn_stop);
    }
    if (s_tcp_conn_stop)
    {
        /* 注意：这里必须自己清 alive 并自删 —— 早退路径如果不写这两个，
         * s_tcp_conn_alive 会永远停在 true，service_fn 等不到它退出；
         * 而且 task 函数"裸 return"在 FreeRTOS 里是未定义行为（栈不回收）。 */
        s_tcp_conn_alive = false;
        vTaskDelete(NULL);
        return; /* 到不了；写出来避免"控制流到达函数结尾"的警告 */
    }

    while (!s_tcp_conn_stop)
    {
        /* 2) 网络不可用 → 挂起等恢复（避免无意义的 connect） */
        if (!s_net_ready)
        {
            ESP_LOGW(TAG, "网络已断开，暂停 TCP 业务，等 WiFi 恢复...");
            while (!s_net_ready && !s_tcp_conn_stop)
            {
                breakable_delay_ms(1000, &s_tcp_conn_stop);
            }
            if (s_tcp_conn_stop)
            {
                break;
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
        cfg.keepalive_idle_s = CONFIG_TCP_CLIENT_KEEPALIVE_IDLE_S;
        cfg.keepalive_interval_s = CONFIG_TCP_CLIENT_KEEPALIVE_INTVL_S;
        cfg.keepalive_count = CONFIG_TCP_CLIENT_KEEPALIVE_CNT;
        /* connect 硬超时：非阻塞 connect + poll，最坏这么久返回。
         * 这个值必须明显小于 CONFIG_SERVICE_SWITCH_TIMEOUT_MS —— 否则切模式时
         * tcp_conn_task 可能正卡在 connect 里、等它返回时 manager 的等待已经超时。 */
        cfg.connect_timeout_ms = CONFIG_TCP_CLIENT_CONNECT_TIMEOUT_MS;
        /* recv 阻塞上限：只是"隔多久醒来看一眼连接状态"，不影响响应延迟 */
        cfg.recv_timeout_ms = CONFIG_TCP_CLIENT_RECV_TIMEOUT_MS;

        ESP_LOGI(TAG, "正在连接 %s:%d", s_target_host, s_target_port);

        /* 4) 建立长连接 */
        int fd = -1;
        esp_err_t err = tcp_client_open(&cfg, &fd);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "连接失败，%d ms 后重试", CONFIG_TCP_CLIENT_RECONNECT_MS);
            breakable_delay_ms(CONFIG_TCP_CLIENT_RECONNECT_MS, &s_tcp_conn_stop);
            continue;
        }
        /* 心跳计时从"连接建立"这一刻重新起算 */
        s_last_traffic_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "长连接已建立 (fd=%d)", fd);

        /* 5) 发布 fd：tx_consumer_task 从此刻起可以发送上行数据 */
        xSemaphoreTake(g_fd_mutex, portMAX_DELAY);
        g_fd = fd;
        xSemaphoreGive(g_fd_mutex);

        /* 6) 纯接收循环：没有"轮次"、不主动断开，收到就交给上层。
         *
         * 【必须是 while (!s_tcp_conn_stop) 而不是 while (1)】
         * recv 有 CONFIG_TCP_CLIENT_RECV_TIMEOUT_MS（默认 1s）的超时，超时
         * 返回 got=0 后 continue 会回到这里重新判条件 —— 于是停服请求最坏
         * 1 秒内一定能被看到。若写成 while(1)，长连接一切正常时这个循环
         * 永远不从内部退出，切换服务模式就会卡死在这儿（s_tcp_conn_stop
         * 置了也没人看），日志表现为"tcp_client 在 N ms 内未退出"。 */
        while (!s_tcp_conn_stop)
        {
            /* 栈上 rx_item_t：本任务私有的 recv 缓冲，
             * 不再依赖文件级 s_rx_buf（已删）—— 栈上 item 自带 buffer，
             * 函数退出即释放，不存在多任务互踩。 */
            rx_item_t item = { 0 };
            size_t got = 0;
            err = tcp_client_recv(fd, item.data, sizeof(item.data), &got);
            item.len = (uint16_t)got;

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
            if (item.len == 0)
            {
#if CONFIG_TCP_CLIENT_HEARTBEAT_MS > 0
                /* ---- 空闲心跳 ----
                 * recv 超时 = 这 1 秒没有任何下行。检查距上次流量是否已超过
                 * 心跳间隔，超过了就主动发一帧（见文件头注释第 0 条：带 payload
                 * 才能刷新 NAT 会话表，治"空闲一段时间自己断开"）。
                 * 差值用 int32_t 强转再比较：tick 是 32 位无符号、会回绕，
                 * "now - last" 的有符号结果对回绕天然正确。 */
                if ((int32_t)(xTaskGetTickCount() - s_last_traffic_tick) >=
                    (int32_t)pdMS_TO_TICKS(CONFIG_TCP_CLIENT_HEARTBEAT_MS))
                {
                    xSemaphoreTake(g_fd_mutex, portMAX_DELAY);
                    const int hb_fd = g_fd;
                    size_t hb_n = 0;
                    const esp_err_t hb_err = (hb_fd >= 0)
                                                 ? tcp_client_send(hb_fd, TCP_APP_HEARTBEAT_PAYLOAD,
                                                                   TCP_APP_HEARTBEAT_LEN, &hb_n)
                                                 : ESP_ERR_INVALID_STATE;
                    xSemaphoreGive(g_fd_mutex);

                    if (hb_err == ESP_OK && hb_n == TCP_APP_HEARTBEAT_LEN)
                    {
                        s_last_traffic_tick = xTaskGetTickCount();
                        ESP_LOGI(TAG, "心跳 (fd=%d)", hb_fd);
                    }
                    else
                    {
                        /* 心跳都发不出去 = 连接已经死了（不只是"没数据"）。
                         * 这正是"假连接"的快速发现路径：send 失败立刻 break
                         * 走第 6 步关连接重连，而不用等协议栈重传放弃的几十秒。 */
                        ESP_LOGW(TAG, "心跳发送失败 (err=%s) —— 判定连接已死，准备重连",
                                 esp_err_to_name(hb_err));
                        break;
                    }
                }
#endif /* CONFIG_TCP_CLIENT_HEARTBEAT_MS */
                /* 没到心跳点 / 心跳已发：继续等下一轮 recv。
                 * （对端静默失联的兜底仍是 TCP keepalive） */
                continue;
            }

            /* 有数据 → 先打印"本次收到多少字节"，再立刻投到共用 RX 队列。
             *
             * 【整条接收路径上唯一的耗时点就在下面那个 hexdump 开关】
             * 日志是同步阻塞写 UART 的（本项目未开异步日志，115200 波特率
             * ≈ 每字符 87µs）。hexdump + ASCII 每 16 字节占一行，1KB 数据
             * 要打 100 多行、约 6000 字符 → 阻塞数百毫秒。这段时间全部发生
             * 在 rx_queue_post() **之前**，等价于"收到了却压着不发"，
             * 表现就是"发送不及时、但收发日志一切正常"；而 send() 本身仍在
             * 毫秒级返回，所以 send() 耗时探针永远不报警。
             *
             * 结论：它只适合在排查协议内容时临时打开（CONFIG_TCP_CLIENT_LOG_RX_DUMP=1），
             * 平时必须关掉。这行 `RX` 保留 —— 它和随后 `TX` 行的时间戳之差，
             * 就是"设备内部耗时"，是判断延迟在不在本机的第一手证据。 */
            ESP_LOGI(TAG, "RX %u 字节", (unsigned)item.len);
#if CONFIG_TCP_CLIENT_LOG_RX_DUMP
            ESP_LOG_BUFFER_HEXDUMP(TAG, item.data, item.len, ESP_LOG_INFO); /* 十六进制 */
            ESP_LOG_BUFFER_CHAR(TAG, item.data, item.len);                  /* ASCII    */
#endif

            /* 有数据（含心跳回包）= 连接上有真实下行流量，心跳计时器重新起算。
             * 放在 rx_queue_post 之前：即使业务回调耗时，计时的"起点"也应该是
             * "收到数据的时刻"而不是"消费完成的时刻"。 */
            s_last_traffic_tick = xTaskGetTickCount();
            item.kind = RX_KIND_TCP_CLIENT;
            if (!rx_queue_post(RX_KIND_TCP_CLIENT, -1, 0, 0, item.data, item.len))
            {
                ESP_LOGW(TAG, "RX 队列满，本段 %u 字节被丢弃", (unsigned)item.len);
            }
        }

        /* 7) 连接坏了 / 被要求停服：先撤销 fd（挡住 tx_consumer_task），再 close */
        xSemaphoreTake(g_fd_mutex, portMAX_DELAY);
        g_fd = -1;
        xSemaphoreGive(g_fd_mutex);
        tcp_client_close(fd);
        ESP_LOGI(TAG, "长连接已关闭 (fd=%d)", fd);

        /* 停服请求导致的退出：不再退避重连，直接出外层循环 */
        if (s_tcp_conn_stop)
        {
            break;
        }

        ESP_LOGI(TAG, "%d ms 后重连", CONFIG_TCP_CLIENT_RECONNECT_MS);
        breakable_delay_ms(CONFIG_TCP_CLIENT_RECONNECT_MS, &s_tcp_conn_stop);
    }
    s_tcp_conn_alive = false;
    vTaskDelete(NULL);
}
#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */

/* ===================== service_manager 接口实现 ===================== */

/**
 * @brief TCP 客户端 service_fn：拉起 conn 子任务；manager 调 request_stop 时
 *        把它踢停、等它自我删除，再清共用队列残留，再退出自己
 *
 * @note 【统一队列后简化】tx / rx 子任务被全局的 tx_consumer_task /
 *       rx_consumer_task 取代，service_fn 不再需要管它们；只在退出前
 *       清空 g_tx_queue / g_rx_queue（避免本服务的残留 item 被
 *       rx_consumer_task 当成"上一个服务的数据"处理掉）。
 */
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
void tcp_client_service_fn(void)
{
    s_tcp_client_stop = false;
    s_tcp_conn_stop = false;
    s_tcp_client_alive = true;

    /* 现在只剩 conn 一个子任务（栈 4096/优先级 5）：管连接 + 接收 + 投 RX 队列。
     * tx/rx 已经在 app_main 里建好、长期跑。 */
    BaseType_t ok_c = xTaskCreate(tcp_conn_task, "tcp_conn", 4096, NULL, 5, NULL);
    if (ok_c != pdPASS)
    {
        ESP_LOGE(TAG, "创建 tcp_conn 子任务失败 (%d)", (int)ok_c);
        s_tcp_conn_stop = true;
        s_tcp_client_alive = false;
        vTaskDelete(NULL);
        return;
    }

    /* 等 manager 调 request_stop → s_tcp_client_stop = true */
    while (!s_tcp_client_stop)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* 给 conn 子任务发停服请求 */
    s_tcp_conn_stop = true;

    /* 等 conn 子任务退出。
     *
     * 【超时了也必须继续等，不能"放弃后自删"】
     * manager 那边看到 is_running 仍为 true，会按自己的逻辑继续等/重试，
     * 两边对状态的认识始终一致。 */
    const int step_ms = 50;
    const int warn_every_ms = CONFIG_SERVICE_SWITCH_TIMEOUT_MS;
    int waited = 0;
    while (s_tcp_conn_alive)
    {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
        if (waited >= warn_every_ms)
        {
            ESP_LOGE(TAG, "tcp_conn %d ms 仍未退出（alive=%d），继续等待（检查是否卡在阻塞调用里）",
                     waited, (int)s_tcp_conn_alive);
            waited = 0;
        }
    }

    /* 清空共用队列：本服务退出后残留的 RX/TX item 不应该被下一个 service 的
     * rx_consumer_task 当成"本服务的回声"处理。xQueueReset 会丢掉未消费的项。 */
    if (g_tx_queue != NULL)
    {
        (void)xQueueReset(g_tx_queue);
    }
    if (g_rx_queue != NULL)
    {
        (void)xQueueReset(g_rx_queue);
    }

    s_tcp_client_alive = false;
    vTaskDelete(NULL);
}
#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */

void tcp_client_request_stop(void)
{
    s_tcp_client_stop = true;
}
bool tcp_client_is_running(void)
{
    return s_tcp_client_alive;
}

/* TCP server / UDP 的 service_fn + 配套 request_stop / is_running：同上，按 service
 * 是否启用决定是否编译 */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
/* TCP server service_fn：直接执行 tcp_server_task 的主体。
 * 之所以不直接传 tcp_server_task 给 ops，是因为 service_manager 的 worker
 * 类型是 void(*)(void)，而 task 函数签名是 void(*)(void*) —— 套一层壳
 * 只是为了让类型对齐，也方便以后挂 hook。 */
void tcp_server_service_fn(void)
{
    s_server_stop = false;
    s_server_alive = true;
    tcp_server_task(NULL); /* 函数结尾会自己清队列 + vTaskDelete(NULL) */
    /* 【注意】这里本来该补清队列，但 tcp_server_task 自杀不返回，
     * 清理已下沉到 task 内部 vTaskDelete 之前（见上面 tcp_server_task 结尾）。 */
}

void tcp_server_request_stop(void)
{
    s_server_stop = true;
}
bool tcp_server_is_running(void)
{
    return s_server_alive;
}
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */

#if CONFIG_SERVICE_UDP_ENABLE
/* UDP service_fn：同本体 */
void udp_service_fn(void)
{
    s_udp_stop = false;
    s_udp_alive = true;
    udp_task(NULL);
    /* 【注意】清理已下沉到 udp_task 内部 vTaskDelete 之前（同 tcp_server_task）。 */
}

void udp_request_stop(void)
{
    s_udp_stop = true;
}
bool udp_is_running(void)
{
    return s_udp_alive;
}
#endif /* CONFIG_SERVICE_UDP_ENABLE */

#endif /* CONFIG_SERVICE_ANY_ENABLE —— 网络服务实现到此为止 */
