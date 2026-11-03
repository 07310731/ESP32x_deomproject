/**
 * @file net_io_udp.c
 * @brief UDP 通讯任务：等门控 → open → recvfrom 循环 → 投 RX 队列
 *
 * 与 TCP 服务端的关键区别：UDP 没有"连接"概念，所以**没有 handler 任务、
 * 没有槽位、没有 fd 泄漏 / 缓冲区互踩等问题**。关停时直接 close：等待中的
 * recvfrom 立刻返错退出，干净利落。
 *
 * 退出原因（决定收尾日志措辞、以及要不要退避）：
 *   0 = 网络断开（门控变 false）；
 *   1 = 本机 IP 变了（网络仍可用，改绑新地址即可，无需退避）；
 *   2 = recvfrom 出错（socket 可能已坏，退避 2s 再重建，避免空转）；
 *   3 = service_manager 要求停止（直接走完收尾，不再重建）。
 */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "net_io.h"
#include "net_io_internal.h"
#include "udp.h"
#include "webconfig.h"

static const char* TAG = "net_udp";

#if CONFIG_SERVICE_UDP_ENABLE

static volatile bool s_udp_stop = false;
static volatile bool s_udp_alive = false;

static void udp_task(void* arg)
{
    (void)arg;
    s_udp_alive = true;

    while (!s_udp_stop)
    {
        /* 1) 等网络就绪（同时尊重停服标志） */
        while (!net_io_is_net_ready() && !s_udp_stop)
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
        {
            /* 端口：NVS 优先（每次新 worker 启动时重读一次） → menuconfig 兜底 */
            webconfig_dev_cfg_t dev;
            cfg.port = (webconfig_load_dev_cfg(&dev) && dev.udp_port != 0) ? dev.udp_port
                                                                          : CONFIG_UDP_PORT;
        }
        cfg.bind_addr = net_io_local_ip();
        cfg.broadcast = false; /* echo 场景不需要发广播 */
        if (cfg.bind_addr == 0)
        {
            ESP_LOGW(TAG, "未取到本机 IP，暂时监听 0.0.0.0");
        }

        /* 3) open —— 失败一律退避 2s 后重试（端口占用 / bind 失败等） */
        int fd = -1;
        esp_err_t err = udp_socket_open(&cfg, &fd);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "启动失败（端口 %u），2s 后重试", (unsigned)cfg.port);
            net_io_breakable_delay_ms(2000, &s_udp_stop);
            continue;
        }
        s_udp_fd = fd; /* tx_consumer 通过它 sendto */

        s_udp_running = true;
        webconfig_set_comm_state(true); /* bind 成功 → 页头"状态"在线 */
        ESP_LOGI(TAG, "已绑定 %u 端口", (unsigned)cfg.port);

        /* 4) recvfrom 循环 */
        int stop_reason = 0;
        while (net_io_is_net_ready() && !s_udp_stop)
        {
            net_io_rx_item_t item = { 0 };
            size_t got = 0;
            uint32_t src_ip = 0;
            uint16_t src_port = 0;
            err = udp_socket_recvfrom(fd, item.data, sizeof(item.data), &src_ip, &src_port, &got);
            item.len = (uint16_t)got;
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "recvfrom 错误: %s", esp_err_to_name(err));
                stop_reason = 2;
                break;
            }

            if (item.len == 0)
            {
                /* 超时——顺手看一眼本机 IP 是否变了。门控只知道"有没有网"，
                 * 不知道 IP 是多少；AP 自定义网段是在 esp_wifi_start() 之后
                 * 才下发到 netif 的，正好落在这个窗口里。 */
                const uint32_t now_ip = net_io_local_ip();
                if (now_ip != 0 && now_ip != cfg.bind_addr)
                {
                    stop_reason = 1;
                    break;
                }
                continue;
            }

            esp_ip4_addr_t peer = { .addr = src_ip };
            ESP_LOGI(TAG, "RX %u 字节 (from " IPSTR ":%u)", (unsigned)item.len, IP2STR(&peer),
                     (unsigned)src_port);

            /* 数据报打满缓冲区 = 超长部分被内核直接丢弃、收端无从感知，
             * 只能靠这条日志提示（调到 CONFIG_RX_MAX_FRAME 以上）。 */
            if (item.len == sizeof(item.data))
            {
                ESP_LOGW(TAG, "数据报已打满 %u 字节缓冲区，超长部分被静默丢弃（调大 RX_MAX_FRAME）",
                         (unsigned)sizeof(item.data));
            }

            /* 投给共用 rx 队列：业务回调 → net_io_send_udp() → tx_consumer 用
             * s_udp_fd 发回对端。不在本任务里直 sendto 是为了和 TCP 两个服务
             * 走同一条数据通路。 */
            if (!net_io_rx_post(NET_IO_LINK_UDP, fd, src_ip, src_port, item.data, item.len))
            {
                ESP_LOGW(TAG, "RX 队列满，本段 %u 字节被丢弃", (unsigned)item.len);
            }
        }

        /* 从 recvfrom 循环出来了：若是被要求停服，补上 stop_reason —— 单靠
         * s_udp_stop 退出时 stop_reason 仍是 0，不补这一句会被下面的 else
         * 误报成"网络断开"。 */
        if (s_udp_stop)
        {
            stop_reason = 3;
        }

        /* 5) 关停收尾 —— 四种退出原因共用同一套，区别只在日志措辞与是否退避。
         * 与 TCP 服务端不同的关键点：**没有等 handler 退出这一步**。 */
        s_udp_running = false;
        webconfig_set_comm_state(false); /* socket 关闭 → 离线 */
        s_udp_fd = -1;                   /* 让 tx_consumer 看到 socket 没了 */
        (void)udp_socket_close(fd);

        if (stop_reason == 1)
        {
            /* 网络还在：回第 1 步会立刻通过门控，无需退避 */
            ESP_LOGW(TAG, "本机 IP 已变化，重建 socket（改绑新地址）");
        }
        else if (stop_reason == 2)
        {
            /* 不是网络问题，立刻重建多半还会失败；退避一下避免空转刷日志 */
            ESP_LOGW(TAG, "socket 异常，2s 后退避重建");
            net_io_breakable_delay_ms(2000, &s_udp_stop);
        }
        else if (stop_reason == 3)
        {
            ESP_LOGW(TAG, "service_manager 要求停止");
            break;
        }
        else
        {
            ESP_LOGW(TAG, "网络断开，停止");
        }
    }
    s_udp_alive = false;

    /* 清空共用队列：理由同 TCP 服务端。UDP 残留 item 因为 s_udp_fd 已被
     * close + 置 -1 兜住（"socket 未就绪"分支），但清掉仍更干净。 */
    if (s_net_io_tx_queue != NULL)
    {
        (void)xQueueReset(s_net_io_tx_queue);
    }
    if (s_net_io_rx_queue != NULL)
    {
        (void)xQueueReset(s_net_io_rx_queue);
    }

    vTaskDelete(NULL);
}

void udp_service_fn(void)
{
    s_udp_stop = false;
    s_udp_alive = true;
    udp_task(NULL); /* 清理已下沉到 task 内部 vTaskDelete 之前 */
}

void udp_request_stop(void)
{
    s_udp_stop = true;
}

bool udp_is_running(void)
{
    return s_udp_alive;
}

#else /* !CONFIG_SERVICE_UDP_ENABLE */

void udp_service_fn(void)
{
}

void udp_request_stop(void)
{
}

bool udp_is_running(void)
{
    return false;
}

#endif /* CONFIG_SERVICE_UDP_ENABLE */
