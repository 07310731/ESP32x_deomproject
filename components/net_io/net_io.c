/**
 * @file net_io.c
 * @brief 收发框架：共用队列 + 两个常驻 consumer 任务 + 上行入口 + 门控
 *
 * 【为什么收发分成两个任务】
 *   把"发"和"收"塞进同一个任务，任务阻塞在 recv 上时就没法立刻 send，上行
 *   延迟被 recv 超时拖着走。拆开后：
 *     上行延迟 ≈ 队列投递 + task 调度   （约 1ms）
 *     下行延迟 ≈ 收包 + 立刻回调        （约 1ms）
 *   两个任务都阻塞在事件上（xQueueReceive / recv），空闲时几乎不占 CPU。
 *
 * 【为什么队列元素按值拷贝】
 *   tx/rx item 自带 data[]，投递即拷贝、消费完自动释放，业务不需要关心
 *   谁在什么时候释放这块内存，也不会因为调用者返回而变成悬垂指针。
 */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "net_io.h"
#include "net_io_internal.h"

#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
#include "tcp_client.h"
#endif
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
#include "tcp_server.h"
#endif
#if CONFIG_SERVICE_UDP_ENABLE
#include "udp.h"
#endif

static const char* TAG = "net_io";

/* ===================== 共用队列与任务 ===================== */
QueueHandle_t s_net_io_tx_queue = NULL;
QueueHandle_t s_net_io_rx_queue = NULL;

static volatile bool s_tx_consumer_stop = false;
static volatile bool s_tx_consumer_alive = false;
static volatile bool s_rx_consumer_stop = false;
static volatile bool s_rx_consumer_alive = false;

/* ===================== 门控 ===================== */
volatile bool s_net_io_ready = false;

/* 业务回调：只允许一个（见 net_io.h 的说明） */
static net_io_recv_cb_t s_recv_cb = NULL;

/* 防止重复建资源 */
static bool s_started = false;

/* ===================== TCP 长连接资源 ===================== */
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
int s_tcp_fd = -1;
SemaphoreHandle_t s_tcp_fd_mutex = NULL;
volatile TickType_t s_last_traffic_tick = 0;

bool s_hb_enabled = false;
uint32_t s_hb_interval_ms = 0;
char s_hb_format[8] = "text";
uint8_t s_hb_payload[64];
uint8_t s_hb_payload_len = 0;
#endif

/* ===================== TCP 服务端资源 ===================== */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
net_io_server_slot_t s_server_clients[CONFIG_TCP_SERVER_MAX_CLIENTS];
volatile bool s_server_running = false;
#endif

/* ===================== UDP 资源 ===================== */
#if CONFIG_SERVICE_UDP_ENABLE
int s_udp_fd = -1;
volatile bool s_udp_running = false;
#endif

/* ===================== 内部 helper ===================== */

void net_io_breakable_delay_ms(int ms, volatile bool* stop)
{
    while (ms > 0 && !*stop)
    {
        const int step = (ms < 50) ? ms : 50;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
}

bool net_io_tx_post(net_io_link_t kind, int fd, uint32_t dst_ip, uint16_t dst_port,
                    const void* data, size_t len)
{
    if (s_net_io_tx_queue == NULL || data == NULL || len == 0 || len > CONFIG_RX_MAX_FRAME)
    {
        return false;
    }
    net_io_tx_item_t item = { 0 };
    item.kind = kind;
    item.fd = fd;
    item.dst_ip = dst_ip;
    item.dst_port = dst_port;
    item.len = (uint16_t)len;
    memcpy(item.data, data, len);
    /* 0 超时：队列满不等待，立刻返回 false（由调用方决定怎么处理） */
    return xQueueSend(s_net_io_tx_queue, &item, 0) == pdTRUE;
}

bool net_io_rx_post(net_io_link_t kind, int fd, uint32_t src_ip, uint16_t src_port,
                    const void* data, size_t len)
{
    if (s_net_io_rx_queue == NULL || data == NULL || len == 0 || len > CONFIG_RX_MAX_FRAME)
    {
        return false;
    }
    net_io_rx_item_t item = { 0 };
    item.kind = kind;
    item.fd = fd;
    item.src_ip = src_ip;
    item.src_port = src_port;
    item.len = (uint16_t)len;
    memcpy(item.data, data, len);
    return xQueueSend(s_net_io_rx_queue, &item, 0) == pdTRUE;
}

#if CONFIG_SERVICE_TCP_SERVER_ENABLE || CONFIG_SERVICE_UDP_ENABLE
uint32_t net_io_local_ip(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK)
    {
        return 0;
    }

    /* ifkey 是 esp_netif_create_default_wifi_sta() / _ap() 固定写死的外部键，
     * wifi 组件正是用这两个函数建 netif 的。字面量定义在 IDF 的
     * esp_netif_defaults.h 里（.if_key 字段），没有公开宏，只能硬编码。
     *
     * 这里每调一次 = 一次跨线程往返（esp_netif_get_handle_from_ifkey 会把请求
     * IPC 到 lwIP 线程）。当前调用频率是 200ms 一次（server accept 循环拿它做
     * IP 变化检测），开销可忽略，故不缓存句柄。 */
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

/* ===================== 上行入口 ===================== */

bool net_io_send_tcp_client(const void* data, size_t len)
{
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
    if (!net_io_tx_post(NET_IO_LINK_TCP_CLIENT, -1, 0, 0, data, len))
    {
        ESP_LOGW(TAG, "上行队列满，本帧 %u 字节被丢弃", (unsigned)len);
        return false;
    }
    return true;
#else
    (void)data;
    (void)len;
    return false;
#endif
}

bool net_io_send_tcp_server(int client_fd, const void* data, size_t len)
{
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
    if (client_fd < 0)
    {
        return false;
    }
    if (!net_io_tx_post(NET_IO_LINK_TCP_SERVER, client_fd, 0, 0, data, len))
    {
        ESP_LOGW(TAG, "上行队列满，本帧 %u 字节被丢弃", (unsigned)len);
        return false;
    }
    return true;
#else
    (void)client_fd;
    (void)data;
    (void)len;
    return false;
#endif
}

bool net_io_send_udp(uint32_t dst_ip, uint16_t dst_port, const void* data, size_t len)
{
#if CONFIG_SERVICE_UDP_ENABLE
    if (dst_ip == 0 || dst_port == 0)
    {
        return false;
    }
    if (!net_io_tx_post(NET_IO_LINK_UDP, -1, dst_ip, dst_port, data, len))
    {
        ESP_LOGW(TAG, "上行队列满，本帧 %u 字节被丢弃", (unsigned)len);
        return false;
    }
    return true;
#else
    (void)dst_ip;
    (void)dst_port;
    (void)data;
    (void)len;
    return false;
#endif
}

bool net_io_send_back(const net_io_frame_t* frame, const void* data, size_t len)
{
    if (frame == NULL)
    {
        return false;
    }
    switch (frame->link)
    {
        case NET_IO_LINK_TCP_CLIENT:
            return net_io_send_tcp_client(data, len);
        case NET_IO_LINK_TCP_SERVER:
            return net_io_send_tcp_server(frame->peer_fd, data, len);
        case NET_IO_LINK_UDP:
            return net_io_send_udp(frame->peer_ip, frame->peer_port, data, len);
        default:
            return false;
    }
}

/* ===================== 门控 / 回调 ===================== */

void net_io_set_net_ready(bool ready)
{
    /* 不在这里打日志：调用方（WiFi 适配层）的日志带触发原因和客户端数，
     * 更有排查价值；打两条会互相干扰时序对比。 */
    s_net_io_ready = ready;
}

void net_io_reload_config(void)
{
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
    net_io_pick_target();
    net_io_pick_heartbeat();
#endif
}

bool net_io_heartbeat_is_echo(const void* data, size_t len)
{
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
    if (!s_hb_enabled || s_hb_payload_len == 0 || data == NULL ||
        len != (size_t)s_hb_payload_len)
    {
        return false;
    }
    return memcmp(data, s_hb_payload, s_hb_payload_len) == 0;
#else
    (void)data;
    (void)len;
    return false;
#endif
}

bool net_io_is_net_ready(void)
{
    return s_net_io_ready;
}

void net_io_set_recv_cb(net_io_recv_cb_t cb)
{
    s_recv_cb = cb;
}

/* ===================== consumer 任务 ===================== */

/**
 * @brief 上行发送消费任务（常驻）：阻塞等队列 → 按 kind 派发 send
 *
 * @note 100ms 轮询停服标志，避免"队列一直空着时看不见停服请求"。
 */
static void tx_consumer_task(void* arg)
{
    (void)arg;
    s_tx_consumer_alive = true;
    net_io_tx_item_t item;

    while (!s_tx_consumer_stop)
    {
        if (xQueueReceive(s_net_io_tx_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            continue;
        }

        switch (item.kind)
        {
            case NET_IO_LINK_TCP_CLIENT:
            {
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
                /* 用互斥锁保护 fd + send：避免 send 期间被 conn_task close */
                xSemaphoreTake(s_tcp_fd_mutex, portMAX_DELAY);
                const int fd = s_tcp_fd;
                if (fd < 0)
                {
                    ESP_LOGW(TAG, "暂无连接，丢弃上行帧（%u 字节）", (unsigned)item.len);
                    xSemaphoreGive(s_tcp_fd_mutex);
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
                    {
                        break;
                    }
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
                xSemaphoreGive(s_tcp_fd_mutex);
#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */
                break;
            }
            case NET_IO_LINK_TCP_SERVER:
            {
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
                size_t sent = 0;
                while (sent < item.len)
                {
                    size_t n = 0;
                    esp_err_t err = tcp_server_send(item.fd, item.data + sent, item.len - sent, &n);
                    if (err != ESP_OK || n == 0)
                    {
                        break;
                    }
                    sent += n;
                }
                if (sent < item.len)
                {
                    ESP_LOGW(TAG, "服务端发送失败：%u/%u 字节 (fd=%d)", (unsigned)sent,
                             (unsigned)item.len, item.fd);
                }
                else
                {
                    ESP_LOGI(TAG, "TX %u 字节 (srv fd=%d)", (unsigned)sent, item.fd);
                }
#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */
                break;
            }
            case NET_IO_LINK_UDP:
            {
#if CONFIG_SERVICE_UDP_ENABLE
                const int udp_fd = s_udp_fd;
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
 * @brief 下行接收消费任务（常驻）：阻塞等队列 → 转成 net_io_frame_t 回调业务
 *
 * @note 回调在**本任务**上下文执行，data 指向本任务栈上的 item.data，
 *       回调返回后即失效（见 net_io.h 的说明）。
 */
static void rx_consumer_task(void* arg)
{
    (void)arg;
    s_rx_consumer_alive = true;
    net_io_rx_item_t item;

    while (!s_rx_consumer_stop)
    {
        if (xQueueReceive(s_net_io_rx_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            continue;
        }

        const net_io_recv_cb_t cb = s_recv_cb;
        if (cb == NULL)
        {
            /* 业务还没注册回调：丢弃并提示一次，避免刷屏 */
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                ESP_LOGW(TAG, "收到 %u 字节但没有注册接收回调（net_io_set_recv_cb），已丢弃",
                         (unsigned)item.len);
            }
            continue;
        }

        const net_io_frame_t frame = {
            .link = item.kind,
            .peer_fd = item.fd,
            .peer_ip = item.src_ip,
            .peer_port = item.src_port,
            .data = item.data,
            .len = item.len,
        };
        cb(&frame);
    }
    s_rx_consumer_alive = false;
    vTaskDelete(NULL);
}

/* ===================== 启动 ===================== */

esp_err_t net_io_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    s_net_io_tx_queue = xQueueCreate(CONFIG_TX_QUEUE_LEN, sizeof(net_io_tx_item_t));
    s_net_io_rx_queue = xQueueCreate(CONFIG_RX_QUEUE_LEN, sizeof(net_io_rx_item_t));
    if (s_net_io_tx_queue == NULL || s_net_io_rx_queue == NULL)
    {
        ESP_LOGE(TAG, "队列创建失败（tx=%p rx=%p）", (void*)s_net_io_tx_queue,
                 (void*)s_net_io_rx_queue);
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
    s_tcp_fd_mutex = xSemaphoreCreateMutex();
    if (s_tcp_fd_mutex == NULL)
    {
        ESP_LOGE(TAG, "fd 互斥锁创建失败");
        return ESP_ERR_NO_MEM;
    }
#endif

    /* 栈 4096：consumer 里有 ESP_LOG（峰值 1KB+），4096 与各 recv 任务对齐 */
    if (xTaskCreate(tx_consumer_task, "net_tx", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(rx_consumer_task, "net_rx", 4096, NULL, 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "consumer 任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "收发框架已启动（tx 队列 %d / rx 队列 %d，单帧上限 %d 字节）",
             CONFIG_TX_QUEUE_LEN, CONFIG_RX_QUEUE_LEN, CONFIG_RX_MAX_FRAME);
    return ESP_OK;
}
