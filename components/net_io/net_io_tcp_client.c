/**
 * @file net_io_tcp_client.c
 * @brief TCP 长连接：挑目标 / 挑心跳 → 连接 → 收 → 断线重连
 *
 * 断线发现手段（按快慢）：
 *   1. 对端 close → recv 返回 0；
 *   2. 发送失败 → send 报错；
 *   3. 应用层心跳发不出去 → 判定"假连接"（这是主要手段，窗口 = 一个心跳周期）；
 *   4. 兜底：TCP keepalive（最坏 idle + interval*count 秒）。
 * 心跳带 payload 是为了刷新中间设备的 NAT 会话表——纯 ACK 的 keepalive 探针
 * 有些设备不认，会话老化后连接会被静默丢弃。
 */
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "net_io.h"
#include "net_io_internal.h"
#include "tcp_client.h"
#include "webconfig.h"

static const char* TAG = "net_tcp";

#if CONFIG_SERVICE_TCP_CLIENT_ENABLE

static volatile bool s_tcp_client_stop = false;
static volatile bool s_tcp_conn_stop = false;
static volatile bool s_tcp_conn_alive = false;
static volatile bool s_tcp_client_alive = false;

/* 运行期可变：每轮重连前重挑（NVS 优先，回落 menuconfig 的 AP/STA 默认） */
static char s_target_host[128] = CONFIG_TCP_CLIENT_HOST_STA;
static uint16_t s_target_port = CONFIG_TCP_CLIENT_PORT;

void net_io_pick_target(void)
{
    webconfig_dev_cfg_t dev;
    if (webconfig_load_dev_cfg(&dev) && dev.tcp_client_server_ip[0] != '\0' &&
        dev.tcp_client_server_port != 0)
    {
        strlcpy(s_target_host, dev.tcp_client_server_ip, sizeof(s_target_host));
        s_target_port = dev.tcp_client_server_port;
        return;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);
    strlcpy(s_target_host,
            (mode == WIFI_MODE_AP) ? CONFIG_TCP_CLIENT_HOST_AP : CONFIG_TCP_CLIENT_HOST_STA,
            sizeof(s_target_host));
    s_target_port = CONFIG_TCP_CLIENT_PORT;
}

/**
 * @brief 解析 enabled / interval / format / payload 为运行时快照
 *
 * 调用点：service_fn 启动时 + 每轮重连前（网页改完下一轮生效）。
 * 字段非法时退回 menuconfig 或关闭心跳，**不报错**——心跳是辅助功能，
 * 坏值不该阻塞 worker 启动。NVS 的 interval 是秒，这里统一换算成毫秒。
 */
void net_io_pick_heartbeat(void)
{
    /* 1) menuconfig 兜底 */
    bool enabled = (CONFIG_TCP_CLIENT_HEARTBEAT_MS > 0);
    uint32_t interval_ms = (uint32_t)CONFIG_TCP_CLIENT_HEARTBEAT_MS;

    char format[8] = "text";
#if defined(CONFIG_TCP_CLIENT_HEARTBEAT_FORMAT_HEX) && CONFIG_TCP_CLIENT_HEARTBEAT_FORMAT_HEX
    strcpy(format, "hex");
#endif

    char payload[64] = CONFIG_TCP_CLIENT_HEARTBEAT_PAYLOAD;

    /* 2) NVS 优先 */
    webconfig_dev_cfg_t dev;
    if (webconfig_load_dev_cfg(&dev))
    {
        enabled = dev.tcp_client_hb_enabled;

        if (dev.tcp_client_hb_interval > 0) /* 0 = 沿用 menuconfig */
        {
            interval_ms = (uint32_t)dev.tcp_client_hb_interval * 1000U;
        }

        if (dev.tcp_client_hb_format[0] != '\0')
        {
            if (strcmp(dev.tcp_client_hb_format, "text") == 0 ||
                strcmp(dev.tcp_client_hb_format, "hex") == 0)
            {
                strlcpy(format, dev.tcp_client_hb_format, sizeof(format));
            }
            else
            {
                ESP_LOGW(TAG, "心跳 format='%s' 非法，退回 menuconfig 兜底",
                         dev.tcp_client_hb_format);
            }
        }

        if (dev.tcp_client_hb_content[0] != '\0')
        {
            strlcpy(payload, dev.tcp_client_hb_content, sizeof(payload));
        }
    }

    /* 3) 解码成字节流：text 直接拷，hex 每两字符一字节 */
    uint8_t buf[64];
    uint8_t len = 0;

    if (strcmp(format, "hex") == 0)
    {
        const size_t plen = strnlen(payload, sizeof(payload));
        if (plen == 0 || (plen % 2) != 0)
        {
            ESP_LOGW(TAG, "心跳 HEX content 长度非法（%u，奇数或为空），关闭心跳", (unsigned)plen);
            enabled = false;
        }
        else
        {
            for (size_t i = 0; i < plen; i += 2)
            {
                unsigned int byte = 0;
                if (sscanf(payload + i, "%2x", &byte) != 1)
                {
                    ESP_LOGW(TAG, "心跳 HEX content 含非 HEX 字符，关闭心跳");
                    enabled = false;
                    break;
                }
                buf[i / 2] = (uint8_t)byte;
            }
            if (enabled)
            {
                len = (uint8_t)(plen / 2);
            }
        }
    }
    else
    {
        const size_t n = strnlen(payload, sizeof(payload));
        if (n > sizeof(buf))
        {
            ESP_LOGW(TAG, "心跳文本 content 超过 %u 字节，截断", (unsigned)sizeof(buf));
        }
        len = (uint8_t)((n > sizeof(buf)) ? sizeof(buf) : n);
        memcpy(buf, payload, len);
    }

    /* enabled 但 interval=0 视为关闭，否则会零间隔刷屏把队列撑爆 */
    if (enabled && interval_ms == 0)
    {
        ESP_LOGW(TAG, "心跳 enabled 但 interval=0，关闭心跳");
        enabled = false;
    }

    s_hb_enabled = enabled;
    s_hb_interval_ms = interval_ms;
    strlcpy(s_hb_format, format, sizeof(s_hb_format));
    if (len > 0)
    {
        memcpy(s_hb_payload, buf, len);
    }
    s_hb_payload_len = len;

    ESP_LOGI(TAG, "心跳配置: enabled=%d interval=%u ms format=%s len=%u", (int)s_hb_enabled,
             (unsigned)s_hb_interval_ms, s_hb_format, (unsigned)s_hb_payload_len);
}

/* 等网络就绪；返回 false = 期间收到停服请求 */
static bool wait_net_ready(void)
{
    if (net_io_is_net_ready())
    {
        return true;
    }
    ESP_LOGW(TAG, "网络不可用，暂停业务，等恢复...");
    while (!net_io_is_net_ready() && !s_tcp_conn_stop)
    {
        net_io_breakable_delay_ms(1000, &s_tcp_conn_stop);
    }
    if (s_tcp_conn_stop)
    {
        return false;
    }
    ESP_LOGI(TAG, "网络已恢复");
    return true;
}

/* 空闲到点就发一帧心跳；返回 false = 发送失败，连接已死 */
static bool maybe_send_heartbeat(void)
{
    if (!s_hb_enabled || s_hb_interval_ms == 0 || s_hb_payload_len == 0)
    {
        return true;
    }
    /* tick 是 32 位无符号会回绕，差值强转 int32_t 再比 */
    if ((int32_t)(xTaskGetTickCount() - s_last_traffic_tick) <
        (int32_t)pdMS_TO_TICKS(s_hb_interval_ms))
    {
        return true;
    }

    xSemaphoreTake(s_tcp_fd_mutex, portMAX_DELAY);
    const int fd = s_tcp_fd;
    size_t n = 0;
    const esp_err_t err = (fd >= 0) ? tcp_client_send(fd, (const char*)s_hb_payload,
                                                      s_hb_payload_len, &n)
                                    : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_tcp_fd_mutex);

    if (err == ESP_OK && n == s_hb_payload_len)
    {
        s_last_traffic_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "心跳 %u 字节 (fd=%d)", (unsigned)n, fd);
        return true;
    }
    ESP_LOGW(TAG, "心跳发送失败 (err=%s) —— 判定连接已死，准备重连", esp_err_to_name(err));
    return false;
}

static void tcp_conn_task(void* arg)
{
    (void)arg;
    s_tcp_conn_alive = true;

    /* 启动阶段无限等网络：不因暂时无网退出任务 */
    ESP_LOGI(TAG, "长连接任务启动");
    if (!wait_net_ready())
    {
        /* 早退也必须清 alive 并自删：否则 service_fn 等不到它退出 */
        s_tcp_conn_alive = false;
        vTaskDelete(NULL);
        return;
    }

    while (!s_tcp_conn_stop)
    {
        if (!wait_net_ready())
        {
            break;
        }

        /* 每轮重连都重挑目标 + 心跳：网页改完不需重启即可生效 */
        net_io_pick_target();
        net_io_pick_heartbeat();

        tcp_client_cfg_t cfg = TCP_CLIENT_CFG_DEFAULT();
        strlcpy(cfg.host, s_target_host, sizeof(cfg.host));
        cfg.port = s_target_port;
        cfg.nodelay = true;   /* 关 Nagle：小包立刻发 */
        cfg.keepalive = true; /* 无心跳时靠它兜底探测静默失联 */
        cfg.keepalive_idle_s = CONFIG_TCP_CLIENT_KEEPALIVE_IDLE_S;
        cfg.keepalive_interval_s = CONFIG_TCP_CLIENT_KEEPALIVE_INTVL_S;
        cfg.keepalive_count = CONFIG_TCP_CLIENT_KEEPALIVE_CNT;
        /* connect 硬超时必须明显小于 CONFIG_SERVICE_SWITCH_TIMEOUT_MS，
         * 否则切模式时 manager 会等超时 */
        cfg.connect_timeout_ms = CONFIG_TCP_CLIENT_CONNECT_TIMEOUT_MS;
        cfg.recv_timeout_ms = CONFIG_TCP_CLIENT_RECV_TIMEOUT_MS;

        ESP_LOGI(TAG, "正在连接 %s:%d", s_target_host, (int)s_target_port);

        int fd = -1;
        esp_err_t err = tcp_client_open(&cfg, &fd);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "连接失败，%d ms 后重试", CONFIG_TCP_CLIENT_RECONNECT_MS);
            net_io_breakable_delay_ms(CONFIG_TCP_CLIENT_RECONNECT_MS, &s_tcp_conn_stop);
            continue;
        }
        s_last_traffic_tick = xTaskGetTickCount(); /* 心跳从连接建立起算 */
        ESP_LOGI(TAG, "长连接已建立 (fd=%d)", fd);

        /* 发布 fd：tx_consumer 从此刻起可以发送 */
        xSemaphoreTake(s_tcp_fd_mutex, portMAX_DELAY);
        s_tcp_fd = fd;
        xSemaphoreGive(s_tcp_fd_mutex);
        webconfig_set_comm_state(true);

        /* 接收循环必须带 stop 条件：recv 超时后回到这里重判，停服最坏 1 秒可见。
         * 写成 while(1) 会让长连接正常时永远退不出去，切模式就卡死。 */
        while (!s_tcp_conn_stop)
        {
            net_io_rx_item_t item = { 0 };
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
            if (item.len == 0) /* recv 超时 = 这 1 秒无下行 */
            {
                if (!maybe_send_heartbeat())
                {
                    break;
                }
                continue;
            }

            /* 注意 hexdump 是同步阻塞写 UART，1KB 要几百毫秒且发生在投队列之前，
             * 只在排查协议时临时打开（CONFIG_TCP_CLIENT_LOG_RX_DUMP）。 */
            ESP_LOGI(TAG, "RX %u 字节", (unsigned)item.len);
#if CONFIG_TCP_CLIENT_LOG_RX_DUMP
            ESP_LOG_BUFFER_HEXDUMP(TAG, item.data, item.len, ESP_LOG_INFO);
            ESP_LOG_BUFFER_CHAR(TAG, item.data, item.len);
#endif

            /* 有真实下行流量 → 心跳计时重算（起点是"收到数据的时刻"） */
            s_last_traffic_tick = xTaskGetTickCount();
            if (!net_io_rx_post(NET_IO_LINK_TCP_CLIENT, -1, 0, 0, item.data, item.len))
            {
                ESP_LOGW(TAG, "RX 队列满，本段 %u 字节被丢弃", (unsigned)item.len);
            }
        }

        /* 先撤 fd 挡住 tx_consumer，再 close */
        xSemaphoreTake(s_tcp_fd_mutex, portMAX_DELAY);
        s_tcp_fd = -1;
        xSemaphoreGive(s_tcp_fd_mutex);
        webconfig_set_comm_state(false);
        tcp_client_close(fd);
        ESP_LOGI(TAG, "长连接已关闭 (fd=%d)", fd);

        if (s_tcp_conn_stop)
        {
            break;
        }
        ESP_LOGI(TAG, "%d ms 后重连", CONFIG_TCP_CLIENT_RECONNECT_MS);
        net_io_breakable_delay_ms(CONFIG_TCP_CLIENT_RECONNECT_MS, &s_tcp_conn_stop);
    }

    s_tcp_conn_alive = false;
    vTaskDelete(NULL);
}

/* service_manager 的入口：拉起 conn 子任务 → 等停服 → 等子任务退出 → 清队列 */
void tcp_client_service_fn(void)
{
    s_tcp_client_stop = false;
    s_tcp_conn_stop = false;
    s_tcp_client_alive = true;

    /* 启动时先挑一次心跳，避免首次 pick 之前心跳被吞掉；重连循环里还会再挑 */
    net_io_pick_heartbeat();

    if (xTaskCreate(tcp_conn_task, "tcp_conn", 4096, NULL, 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "创建 conn 子任务失败");
        s_tcp_conn_stop = true;
        s_tcp_client_alive = false;
        vTaskDelete(NULL);
        return;
    }

    while (!s_tcp_client_stop)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_tcp_conn_stop = true;

    /* 超时了也继续等，不能"放弃后自删"：manager 看到 is_running 仍为 true,
     * 会按自己的逻辑继续等，两边对状态的认知始终一致。 */
    int waited = 0;
    while (s_tcp_conn_alive)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
        if (waited >= CONFIG_SERVICE_SWITCH_TIMEOUT_MS)
        {
            ESP_LOGE(TAG, "conn %d ms 仍未退出（alive=%d），继续等待（检查是否卡在阻塞调用里）",
                     waited, (int)s_tcp_conn_alive);
            waited = 0;
        }
    }

    /* 清空共用队列：本服务残留的 item 不该被下一个 service 当成自己的数据 */
    if (s_net_io_tx_queue != NULL)
    {
        (void)xQueueReset(s_net_io_tx_queue);
    }
    if (s_net_io_rx_queue != NULL)
    {
        (void)xQueueReset(s_net_io_rx_queue);
    }

    s_tcp_client_alive = false;
    vTaskDelete(NULL);
}

void tcp_client_request_stop(void)
{
    s_tcp_client_stop = true;
}

bool tcp_client_is_running(void)
{
    return s_tcp_client_alive;
}

#else /* !CONFIG_SERVICE_TCP_CLIENT_ENABLE：service_manager 的 ops 表仍需要这三个符号 */

void tcp_client_service_fn(void) {}
void tcp_client_request_stop(void) {}
bool tcp_client_is_running(void) { return false; }

#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */
