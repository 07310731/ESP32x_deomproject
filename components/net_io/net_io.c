/**
 * @file net_io.c
 * @brief 收发框架：共用队列 + tx/rx 两个常驻任务 + 上行入口 + 门控
 *
 * 收发分成两个任务：send 不会被 recv 的阻塞/超时拖住，上行延迟只取决于
 * 队列投递 + 调度（约 1ms）。两个任务都阻塞在队列上，空闲不占 CPU。
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

/* ===================== 共用队列 / 门控 ===================== */
QueueHandle_t s_net_io_tx_queue = NULL;
QueueHandle_t s_net_io_rx_queue = NULL;
volatile bool s_net_io_ready = false;

static net_io_recv_cb_t s_recv_cb = NULL;
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

/* 收发共用的入队实现：参数非法 / 队列满都返回 false，不阻塞 */
static bool post(QueueHandle_t q, net_io_link_t kind, int fd, uint32_t peer_ip,
                 uint16_t peer_port, const void* data, size_t len)
{
    if (q == NULL || data == NULL || len == 0 || len > CONFIG_RX_MAX_FRAME)
    {
        return false;
    }
    net_io_item_t item = {
        .kind = kind,
        .len = (uint16_t)len,
        .fd = fd,
        .peer_ip = peer_ip,
        .peer_port = peer_port,
    };
    memcpy(item.data, data, len);
    return xQueueSend(q, &item, 0) == pdTRUE;
}

bool net_io_tx_post(net_io_link_t kind, int fd, uint32_t peer_ip, uint16_t peer_port,
                    const void* data, size_t len)
{
    return post(s_net_io_tx_queue, kind, fd, peer_ip, peer_port, data, len);
}

bool net_io_rx_post(net_io_link_t kind, int fd, uint32_t peer_ip, uint16_t peer_port,
                    const void* data, size_t len)
{
    return post(s_net_io_rx_queue, kind, fd, peer_ip, peer_port, data, len);
}

#if CONFIG_SERVICE_TCP_SERVER_ENABLE || CONFIG_SERVICE_UDP_ENABLE
uint32_t net_io_local_ip(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK)
    {
        return 0;
    }

    /* ifkey 是 esp_netif_create_default_wifi_*() 写死的外部键，IDF 没公开宏，
     * 只能硬编码。每次调用是一次跨线程往返，但只 200ms 一次，可不缓存句柄。 */
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
#endif

/* ===================== 上行入口 ===================== */

/* 三入口共用的入队 + 满队列告警 */
static bool submit(net_io_link_t kind, int fd, uint32_t peer_ip, uint16_t peer_port,
                   const void* data, size_t len)
{
    if (net_io_tx_post(kind, fd, peer_ip, peer_port, data, len))
    {
        return true;
    }
    ESP_LOGW(TAG, "上行队列满，本帧 %u 字节被丢弃", (unsigned)len);
    return false;
}

bool net_io_send_tcp_client(const void* data, size_t len)
{
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
    return submit(NET_IO_LINK_TCP_CLIENT, -1, 0, 0, data, len);
#else
    (void)data;
    (void)len;
    return false;
#endif
}

bool net_io_send_tcp_server(int client_fd, const void* data, size_t len)
{
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
    return client_fd >= 0 && submit(NET_IO_LINK_TCP_SERVER, client_fd, 0, 0, data, len);
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
    return dst_ip != 0 && dst_port != 0 &&
           submit(NET_IO_LINK_UDP, -1, dst_ip, dst_port, data, len);
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
    /* 不打日志：调用方（WiFi 适配层）的日志带触发原因和客户端数，更有价值 */
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
    return s_hb_enabled && s_hb_payload_len > 0 && data != NULL &&
           len == (size_t)s_hb_payload_len &&
           memcmp(data, s_hb_payload, s_hb_payload_len) == 0;
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

/* 循环写直到写完或出错（TCP 是字节流，单次 send 可能只写一部分） */
typedef esp_err_t (*send_fn_t)(int fd, const void* data, size_t len, size_t* sent);

static size_t send_all(send_fn_t fn, int fd, const uint8_t* data, size_t len,
                       esp_err_t* out_err)
{
    size_t sent = 0;
    while (sent < len)
    {
        size_t n = 0;
        const esp_err_t err = fn(fd, data + sent, len - sent, &n);
        if (err != ESP_OK || n == 0)
        {
            *out_err = (err != ESP_OK) ? err : ESP_FAIL;
            break;
        }
        sent += n;
    }
    return sent;
}

/* 上行发送消费任务（常驻）：阻塞等队列 → 按 kind 派发 */
static void tx_consumer_task(void* arg)
{
    (void)arg;
    net_io_tx_item_t item;

    while (1)
    {
        if (xQueueReceive(s_net_io_tx_queue, &item, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (item.kind)
        {
            case NET_IO_LINK_TCP_CLIENT:
            {
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
                /* 锁住 fd：避免 send 期间被 conn_task close */
                xSemaphoreTake(s_tcp_fd_mutex, portMAX_DELAY);
                const int fd = s_tcp_fd;
                if (fd < 0)
                {
                    ESP_LOGW(TAG, "暂无连接，丢弃上行帧（%u 字节）", (unsigned)item.len);
                    xSemaphoreGive(s_tcp_fd_mutex);
                    break;
                }
                esp_err_t err = ESP_OK;
                const int64_t t0_us = esp_timer_get_time();
                const size_t sent = send_all(tcp_client_send, fd, item.data, item.len, &err);
                const int64_t cost_us = esp_timer_get_time() - t0_us;

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
                if (cost_us > CONFIG_TCP_CLIENT_SEND_SLOW_US)
                {
                    ESP_LOGW(TAG, "send() 耗时 %lld ms —— 延迟出在协议栈/射频侧",
                             (long long)(cost_us / 1000));
                }
                xSemaphoreGive(s_tcp_fd_mutex);
#endif
                break;
            }
            case NET_IO_LINK_TCP_SERVER:
            {
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
                esp_err_t err = ESP_OK;
                const size_t sent = send_all(tcp_server_send, item.fd, item.data, item.len, &err);
                if (sent < item.len)
                {
                    ESP_LOGW(TAG, "服务端发送失败：%u/%u 字节 (fd=%d)", (unsigned)sent,
                             (unsigned)item.len, item.fd);
                }
                else
                {
                    ESP_LOGI(TAG, "TX %u 字节 (srv fd=%d)", (unsigned)sent, item.fd);
                }
#endif
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
                const esp_err_t err = udp_socket_sendto(udp_fd, item.data, item.len, item.peer_ip,
                                                        item.peer_port, &sent);
                if (err != ESP_OK || sent != item.len)
                {
                    ESP_LOGW(TAG, "udp 上行失败：%u/%u 字节 (err=%d)", (unsigned)sent,
                             (unsigned)item.len, (int)err);
                }
                else
                {
                    const esp_ip4_addr_t peer = { .addr = item.peer_ip };
                    ESP_LOGI(TAG, "TX %u 字节 (udp to " IPSTR ":%u)", (unsigned)sent,
                             IP2STR(&peer), (unsigned)item.peer_port);
                }
#endif
                break;
            }
            default:
                break;
        }
    }
}

/* 下行消费任务（常驻）：阻塞等队列 → 转成 net_io_frame_t 回调业务。
 * 回调在本任务上下文执行，data 指向本任务栈，返回后失效。 */
static void rx_consumer_task(void* arg)
{
    (void)arg;
    net_io_rx_item_t item;

    while (1)
    {
        if (xQueueReceive(s_net_io_rx_queue, &item, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        const net_io_recv_cb_t cb = s_recv_cb;
        if (cb == NULL)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                ESP_LOGW(TAG, "收到 %u 字节但没注册接收回调（net_io_set_recv_cb），已丢弃",
                         (unsigned)item.len);
            }
            continue;
        }

        const net_io_frame_t frame = {
            .link = item.kind,
            .peer_fd = item.fd,
            .peer_ip = item.peer_ip,
            .peer_port = item.peer_port,
            .data = item.data,
            .len = item.len,
        };
        cb(&frame);
    }
}

/* ===================== 启动 ===================== */

esp_err_t net_io_start(void)
{
    if (s_started)
    {
        return ESP_OK;
    }

    s_net_io_tx_queue = xQueueCreate(CONFIG_TX_QUEUE_LEN, sizeof(net_io_item_t));
    s_net_io_rx_queue = xQueueCreate(CONFIG_RX_QUEUE_LEN, sizeof(net_io_item_t));
    if (s_net_io_tx_queue == NULL || s_net_io_rx_queue == NULL)
    {
        ESP_LOGE(TAG, "队列创建失败");
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

    /* 栈 4096：consumer 里有 ESP_LOG（峰值 1KB+） */
    if (xTaskCreate(tx_consumer_task, "net_tx", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(rx_consumer_task, "net_rx", 4096, NULL, 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "consumer 任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "收发框架已启动（tx %d / rx %d，单帧上限 %d 字节）", CONFIG_TX_QUEUE_LEN,
             CONFIG_RX_QUEUE_LEN, CONFIG_RX_MAX_FRAME);
    return ESP_OK;
}
