/**
 * @file net_io_tcp_client.c
 * @brief TCP 长连接：挑目标地址 / 挑心跳 → 连接 → 收 → 断线重连
 *
 * 【靠什么发现断线 / 保住长连接】
 *   0. 应用层心跳（CONFIG_TCP_CLIENT_HEARTBEAT_MS，治"空闲自己断开"）：
 *      空闲超过心跳间隔就主动发一帧带 payload 的数据。TCP keepalive 的探针是
 *      "纯 ACK、零字节 payload"，很多路由器/AP 的 NAT 会话表只认"带数据的包"
 *      才刷新老化计时 —— 表项老化、连接被中间设备静默丢弃后，keepalive 反而
 *      把它"发现"成断线。带 payload 的心跳帧能把所有中间设备的会话表都续上。
 *   1. 对端主动 close  → recv 返回 0 → ESP_ERR_INVALID_STATE，立刻发现；
 *   2. 发送失败        → send 返回错误，立刻发现；
 *   3. 对端静默失联    → 心跳发送失败也会 break（窗口压到一个心跳周期）；
 *      最终兜底是 TCP keepalive（最坏约 idle + interval*count 秒）。
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

/* 当前连接目标（运行期可变）：tcp_conn_task 每轮重连前由 net_io_pick_target()
 * 重新挑值——优先用 NVS 里用户填的、查不到再回落 menuconfig 的 WiFi 模式相关
 * 默认（AP-only 用 HOST_AP，其它用 HOST_STA）。 */
static char s_target_host[128] = CONFIG_TCP_CLIENT_HOST_STA;
static uint16_t s_target_port = CONFIG_TCP_CLIENT_PORT;

void net_io_pick_target(void)
{
    /* 优先用 NVS 里用户填的目标 */
    webconfig_dev_cfg_t dev;
    if (webconfig_load_dev_cfg(&dev) && dev.tcp_client_server_ip[0] != '\0' &&
        dev.tcp_client_server_port != 0)
    {
        strlcpy(s_target_host, dev.tcp_client_server_ip, sizeof(s_target_host));
        s_target_port = dev.tcp_client_server_port;
        return;
    }

    /* 回落 menuconfig 的 WiFi 模式相关默认 */
    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);
    const bool ap_only = (mode == WIFI_MODE_AP);
    strlcpy(s_target_host, ap_only ? CONFIG_TCP_CLIENT_HOST_AP : CONFIG_TCP_CLIENT_HOST_STA,
            sizeof(s_target_host));
    s_target_port = CONFIG_TCP_CLIENT_PORT;
}

/**
 * @brief 把 enabled / interval / format / payload 一组解析成运行时快照
 *
 * 调用节奏与 net_io_pick_target() 一致：
 *   - tcp_client_service_fn() 启动时（保证 worker 一上来就用对的值）；
 *   - tcp_conn_task 外层每轮重连开头（网页改了保存后下次连接生效）。
 *
 * 字段不合法时（format 非法 / content 为空 / HEX 奇数位）退回 menuconfig，
 * **不报错**：心跳是辅助功能，坏值不应该阻塞 worker 启动。
 *
 * unit: NVS 里 interval 是"秒"，menuconfig 的 HEARTBEAT_MS 是毫秒；
 *       统一按"运行时毫秒"写到 s_hb_interval_ms。
 */
void net_io_pick_heartbeat(void)
{
    /* ---- 1) menuconfig 兜底（默认按"10s + PING 文本"走，与改造前一致）---- */
    bool enabled = (CONFIG_TCP_CLIENT_HEARTBEAT_MS > 0);
    uint32_t interval_ms = (uint32_t)CONFIG_TCP_CLIENT_HEARTBEAT_MS;

    /* format: Kconfig choice，选中 HEX 时 CONFIG_TCP_CLIENT_HEARTBEAT_FORMAT_HEX=1 */
    char format[8] = "text";
#if defined(CONFIG_TCP_CLIENT_HEARTBEAT_FORMAT_HEX)
#if CONFIG_TCP_CLIENT_HEARTBEAT_FORMAT_HEX
    strcpy(format, "hex");
#endif
#endif

    char payload[64] = CONFIG_TCP_CLIENT_HEARTBEAT_PAYLOAD;

    /* ---- 2) NVS 优先：有 dev 配置就把字段覆盖上去 ---- */
    webconfig_dev_cfg_t dev;
    if (webconfig_load_dev_cfg(&dev))
    {
        enabled = dev.tcp_client_hb_enabled;

        /* interval: NVS 存"秒"，0 表示"用 menuconfig 的"（不强制关闭） */
        if (dev.tcp_client_hb_interval > 0)
        {
            interval_ms = (uint32_t)dev.tcp_client_hb_interval * 1000U;
        }

        /* format: 必须是 "text" / "hex" 之一，否则退回 menuconfig */
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

    /* ---- 3) 按 format 把"内容"解码成字节流 ----
     * text: UTF-8 字节直接 memcpy；
     * hex : "AA55" → [0xAA, 0x55]，每两字符解码一字节。 */
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
            bool bad = false;
            for (size_t i = 0; i < plen; i += 2)
            {
                unsigned int byte = 0;
                if (sscanf(payload + i, "%2x", &byte) != 1)
                {
                    bad = true;
                    break;
                }
                buf[i / 2] = (uint8_t)byte;
            }
            if (bad)
            {
                ESP_LOGW(TAG, "心跳 HEX content 含非 HEX 字符，关闭心跳");
                enabled = false;
            }
            else
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
        const size_t cp = (n > sizeof(buf)) ? sizeof(buf) : n;
        memcpy(buf, payload, cp);
        len = (uint8_t)cp;
    }

    /* enabled = true 但 interval == 0 也视为关闭（避免下一秒就被零间隔心跳撑爆队列） */
    if (enabled && interval_ms == 0)
    {
        ESP_LOGW(TAG, "心跳 enabled 但 interval=0，关闭心跳");
        enabled = false;
    }

    /* ---- 4) 提交到运行时快照（下一次 pick 之前稳定） ---- */
    s_hb_enabled = enabled;
    s_hb_interval_ms = interval_ms;
    strlcpy(s_hb_format, format, sizeof(s_hb_format));
    if (len > 0)
    {
        memcpy(s_hb_payload, buf, len);
    }
    s_hb_payload_len = len;

    ESP_LOGI(TAG, "心跳配置: enabled=%d interval=%u ms format=%s len=%u",
             (int)s_hb_enabled, (unsigned)s_hb_interval_ms, s_hb_format,
             (unsigned)s_hb_payload_len);
}

static void tcp_conn_task(void* arg)
{
    (void)arg;
    s_tcp_conn_alive = true;

    /* 1) 启动阶段：无限等网络就绪（不因暂时无网退出任务）
     * 用可中断延时而不是裸 vTaskDelay：停服标志一到，最坏 50ms 就退出。 */
    ESP_LOGI(TAG, "长连接任务启动，等待网络就绪（无限等）...");
    while (!net_io_is_net_ready() && !s_tcp_conn_stop)
    {
        net_io_breakable_delay_ms(1000, &s_tcp_conn_stop);
    }
    if (s_tcp_conn_stop)
    {
        /* 早退路径也必须自己清 alive 并自删：否则 s_tcp_conn_alive 永远停在
         * true，service_fn 等不到它退出；而且 task 函数"裸 return"在 FreeRTOS
         * 里是未定义行为（栈不回收）。 */
        s_tcp_conn_alive = false;
        vTaskDelete(NULL);
        return;
    }

    while (!s_tcp_conn_stop)
    {
        /* 2) 网络不可用 → 挂起等恢复（避免无意义的 connect） */
        if (!net_io_is_net_ready())
        {
            ESP_LOGW(TAG, "网络已断开，暂停业务，等恢复...");
            while (!net_io_is_net_ready() && !s_tcp_conn_stop)
            {
                net_io_breakable_delay_ms(1000, &s_tcp_conn_stop);
            }
            if (s_tcp_conn_stop)
            {
                break;
            }
            ESP_LOGI(TAG, "网络已恢复，重连服务器");
        }

        /* 3) 每轮重连都重新挑目标 + 心跳：地址/心跳都是运行期变量，这样
         *    "换了地址/改了心跳下一轮就生效"，网页改完不需要重启。 */
        net_io_pick_target();
        net_io_pick_heartbeat();

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
        /* connect 硬超时：非阻塞 connect + poll，最坏这么久返回。必须明显小于
         * CONFIG_SERVICE_SWITCH_TIMEOUT_MS —— 否则切模式时本任务可能正卡在
         * connect 里，等它返回时 manager 的等待已经超时。 */
        cfg.connect_timeout_ms = CONFIG_TCP_CLIENT_CONNECT_TIMEOUT_MS;
        /* recv 阻塞上限：只是"隔多久醒来看一眼连接状态"，不影响响应延迟 */
        cfg.recv_timeout_ms = CONFIG_TCP_CLIENT_RECV_TIMEOUT_MS;

        ESP_LOGI(TAG, "正在连接 %s:%d", s_target_host, (int)s_target_port);

        /* 4) 建立长连接 */
        int fd = -1;
        esp_err_t err = tcp_client_open(&cfg, &fd);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "连接失败，%d ms 后重试", CONFIG_TCP_CLIENT_RECONNECT_MS);
            net_io_breakable_delay_ms(CONFIG_TCP_CLIENT_RECONNECT_MS, &s_tcp_conn_stop);
            continue;
        }
        s_last_traffic_tick = xTaskGetTickCount(); /* 心跳计时从连接建立这一刻起算 */
        ESP_LOGI(TAG, "长连接已建立 (fd=%d)", fd);

        /* 5) 发布 fd：tx_consumer 从此刻起可以发送上行数据 */
        xSemaphoreTake(s_tcp_fd_mutex, portMAX_DELAY);
        s_tcp_fd = fd;
        xSemaphoreGive(s_tcp_fd_mutex);
        webconfig_set_comm_state(true); /* 连上 → 页头"状态"在线 */

        /* 6) 纯接收循环：没有"轮次"、不主动断开，收到就交给上层。
         *
         * 【必须是 while (!s_tcp_conn_stop) 而不是 while (1)】
         * recv 有超时，超时返回 got=0 后 continue 会回到这里重新判条件 ——
         * 于是停服请求最坏 1 秒内一定能被看到。若写成 while(1)，长连接一切
         * 正常时这个循环永远不从内部退出，切换服务模式就会卡死在这儿。 */
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
            if (item.len == 0)
            {
                /* ---- 空闲心跳 ----
                 * recv 超时 = 这 1 秒没有任何下行。检查距上次流量是否已超过
                 * 心跳间隔，超过了就主动发一帧（带 payload 才能刷新 NAT 会话表）。
                 * 差值用 int32_t 强转再比较：tick 是 32 位无符号、会回绕。 */
                if (s_hb_enabled && s_hb_interval_ms > 0 && s_hb_payload_len > 0 &&
                    (int32_t)(xTaskGetTickCount() - s_last_traffic_tick) >=
                        (int32_t)pdMS_TO_TICKS(s_hb_interval_ms))
                {
                    xSemaphoreTake(s_tcp_fd_mutex, portMAX_DELAY);
                    const int hb_fd = s_tcp_fd;
                    size_t hb_n = 0;
                    const esp_err_t hb_err =
                        (hb_fd >= 0) ? tcp_client_send(hb_fd, (const char*)s_hb_payload,
                                                       s_hb_payload_len, &hb_n)
                                     : ESP_ERR_INVALID_STATE;
                    xSemaphoreGive(s_tcp_fd_mutex);

                    if (hb_err == ESP_OK && hb_n == s_hb_payload_len)
                    {
                        s_last_traffic_tick = xTaskGetTickCount();
                        ESP_LOGI(TAG, "心跳 %u 字节 (fd=%d)", (unsigned)hb_n, hb_fd);
                    }
                    else
                    {
                        /* 心跳都发不出去 = 连接已经死了（不只是"没数据"）。
                         * 这正是"假连接"的快速发现路径。 */
                        ESP_LOGW(TAG, "心跳发送失败 (err=%s) —— 判定连接已死，准备重连",
                                 esp_err_to_name(hb_err));
                        break;
                    }
                }
                continue;
            }

            /* 有数据 → 先打一行"收到多少字节"，再立刻投 RX 队列。
             *
             * 【整条接收路径上唯一的耗时点就是 hexdump 开关】
             * 日志是同步阻塞写 UART 的。hexdump 每 16 字节占一行，1KB 要打
             * 100 多行、约 6000 字符 → 阻塞数百毫秒，而这段时间全在投队列
             * **之前**，表现就是"收到了却压着不发"。
             * 只适合排查协议时临时打开（CONFIG_TCP_CLIENT_LOG_RX_DUMP=1）。
             * 这行 `RX` 保留 —— 它和随后 `TX` 行的时间戳之差就是设备内部耗时。 */
            ESP_LOGI(TAG, "RX %u 字节", (unsigned)item.len);
#if CONFIG_TCP_CLIENT_LOG_RX_DUMP
            ESP_LOG_BUFFER_HEXDUMP(TAG, item.data, item.len, ESP_LOG_INFO);
            ESP_LOG_BUFFER_CHAR(TAG, item.data, item.len);
#endif

            /* 有数据（含心跳回包）= 连接上有真实下行流量，心跳计时重新起算。
             * 放在投队列之前：计时的"起点"应该是"收到数据的时刻"。 */
            s_last_traffic_tick = xTaskGetTickCount();
            if (!net_io_rx_post(NET_IO_LINK_TCP_CLIENT, -1, 0, 0, item.data, item.len))
            {
                ESP_LOGW(TAG, "RX 队列满，本段 %u 字节被丢弃", (unsigned)item.len);
            }
        }

        /* 7) 连接坏了 / 被要求停服：先撤销 fd（挡住 tx_consumer），再 close */
        xSemaphoreTake(s_tcp_fd_mutex, portMAX_DELAY);
        s_tcp_fd = -1;
        xSemaphoreGive(s_tcp_fd_mutex);
        webconfig_set_comm_state(false); /* 断开 → 离线 */
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

/**
 * @brief TCP 客户端 service_fn：拉起 conn 子任务；manager 调 request_stop 时
 *        把它踢停、等它自我删除，再清共用队列残留，最后退出自己
 */
void tcp_client_service_fn(void)
{
    s_tcp_client_stop = false;
    s_tcp_conn_stop = false;
    s_tcp_client_alive = true;

    /* 启动时挑一次心跳：worker 一上来就用对的值，避免首次 pick 之前
     * s_hb_interval_ms 还是 0 时本应发的心跳被吞掉。重连循环里还会再挑一次。 */
    net_io_pick_heartbeat();

    BaseType_t ok_c = xTaskCreate(tcp_conn_task, "tcp_conn", 4096, NULL, 5, NULL);
    if (ok_c != pdPASS)
    {
        ESP_LOGE(TAG, "创建 conn 子任务失败 (%d)", (int)ok_c);
        s_tcp_conn_stop = true;
        s_tcp_client_alive = false;
        vTaskDelete(NULL);
        return;
    }

    /* 等 manager 调 request_stop */
    while (!s_tcp_client_stop)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_tcp_conn_stop = true;

    /* 等 conn 子任务退出。
     * 【超时了也必须继续等，不能"放弃后自删"】manager 那边看到 is_running 仍为
     * true，会按自己的逻辑继续等/重试，两边对状态的认识始终一致。 */
    const int step_ms = 50;
    const int warn_every_ms = CONFIG_SERVICE_SWITCH_TIMEOUT_MS;
    int waited = 0;
    while (s_tcp_conn_alive)
    {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
        if (waited >= warn_every_ms)
        {
            ESP_LOGE(TAG, "conn %d ms 仍未退出（alive=%d），继续等待（检查是否卡在阻塞调用里）",
                     waited, (int)s_tcp_conn_alive);
            waited = 0;
        }
    }

    /* 清空共用队列：本服务退出后残留的 item 不应该被下一个 service 当成
     * "本服务的数据"处理。xQueueReset 会丢掉未消费的项。 */
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

#else /* !CONFIG_SERVICE_TCP_CLIENT_ENABLE */

void tcp_client_service_fn(void)
{
}

void tcp_client_request_stop(void)
{
}

bool tcp_client_is_running(void)
{
    return false;
}

#endif /* CONFIG_SERVICE_TCP_CLIENT_ENABLE */
