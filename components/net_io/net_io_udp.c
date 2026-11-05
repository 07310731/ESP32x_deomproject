/**
 * @file net_io_udp.c
 * @brief UDP：等门控 → open → recvfrom 循环 → 投 RX 队列
 *
 * 与 TCP 服务端的区别：UDP 没有连接概念，没有 handler、没有槽位，关停直接
 * close 即可（等待中的 recvfrom 立刻返错退出）。
 *
 * 退出原因决定收尾措辞与是否退避：
 *   0 网络断开 | 1 IP 变了（免退避） | 2 recvfrom 出错（退避 2s） | 3 停服
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

enum
{
    STOP_NET_DOWN = 0,
    STOP_IP_CHANGED,
    STOP_SOCK_ERR,
    STOP_REQUESTED,
};

static volatile bool s_udp_stop = false;
static volatile bool s_udp_alive = false;

static void udp_task(void* arg)
{
    (void)arg;
    s_udp_alive = true;

    while (!s_udp_stop)
    {
        while (!net_io_is_net_ready() && !s_udp_stop)
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        if (s_udp_stop)
        {
            break;
        }

        /* 端口：NVS 优先（每次重建重读）→ menuconfig 兜底。
         * 拿不到本机 IP 时 bind 0.0.0.0，保证不会因"IP 未就绪"起不来。 */
        udp_cfg_t cfg = UDP_CFG_DEFAULT();
        webconfig_dev_cfg_t dev;
        cfg.port = (webconfig_load_dev_cfg(&dev) && dev.udp_port != 0) ? dev.udp_port
                                                                      : CONFIG_UDP_PORT;
        cfg.bind_addr = net_io_local_ip();
        cfg.broadcast = false; /* echo 场景不需要广播 */
        if (cfg.bind_addr == 0)
        {
            ESP_LOGW(TAG, "未取到本机 IP，暂时监听 0.0.0.0");
        }

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
        webconfig_set_comm_state(true);
        ESP_LOGI(TAG, "已绑定 %u 端口", (unsigned)cfg.port);

        int stop_reason = STOP_NET_DOWN;
        while (net_io_is_net_ready() && !s_udp_stop)
        {
            net_io_rx_item_t item = { 0 };
            size_t got = 0;
            uint32_t src_ip = 0;
            uint16_t src_port = 0;
            err = udp_socket_recvfrom(fd, item.data, sizeof(item.data), &src_ip, &src_port,
                                      &got);
            item.len = (uint16_t)got;
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "recvfrom 错误: %s", esp_err_to_name(err));
                stop_reason = STOP_SOCK_ERR;
                break;
            }

            if (item.len == 0)
            {
                /* 超时——顺手看 IP 变没变：门控只知道"有没有网"，不知道 IP。
                 * AP 自定义网段是 esp_wifi_start() 之后才下发到 netif 的。 */
                const uint32_t now_ip = net_io_local_ip();
                if (now_ip != 0 && now_ip != cfg.bind_addr)
                {
                    stop_reason = STOP_IP_CHANGED;
                    break;
                }
                continue;
            }

            const esp_ip4_addr_t peer = { .addr = src_ip };
            ESP_LOGI(TAG, "RX %u 字节 (from " IPSTR ":%u)", (unsigned)item.len, IP2STR(&peer),
                     (unsigned)src_port);

            /* 打满缓冲区 = 超长部分被内核静默丢弃，收端无从感知，只能提示调大上限 */
            if (item.len == sizeof(item.data))
            {
                ESP_LOGW(TAG, "数据报打满 %u 字节缓冲，超长部分被丢弃（调大 RX_MAX_FRAME）",
                         (unsigned)sizeof(item.data));
            }

            if (!net_io_rx_post(NET_IO_LINK_UDP, fd, src_ip, src_port, item.data, item.len))
            {
                ESP_LOGW(TAG, "RX 队列满，本段 %u 字节被丢弃", (unsigned)item.len);
            }
        }

        /* 单靠 s_udp_stop 退出时 stop_reason 还是 0，会误报成"网络断开" */
        if (s_udp_stop)
        {
            stop_reason = STOP_REQUESTED;
        }

        /* 收尾：UDP 没有 handler 要等，直接关 */
        s_udp_running = false;
        webconfig_set_comm_state(false);
        s_udp_fd = -1; /* 让 tx_consumer 看到 socket 没了 */
        (void)udp_socket_close(fd);

        if (stop_reason == STOP_IP_CHANGED)
        {
            ESP_LOGW(TAG, "本机 IP 已变化，重建 socket（改绑新地址）");
        }
        else if (stop_reason == STOP_SOCK_ERR)
        {
            /* 不是网络问题，立刻重建多半还会失败，退避避免空转刷日志 */
            ESP_LOGW(TAG, "socket 异常，2s 后退避重建");
            net_io_breakable_delay_ms(2000, &s_udp_stop);
        }
        else if (stop_reason == STOP_REQUESTED)
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

    /* 清共用队列（理由同 TCP 服务端），清理必须放在 vTaskDelete 之前 */
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
    udp_task(NULL);
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

void udp_service_fn(void) {}
void udp_request_stop(void) {}
bool udp_is_running(void) { return false; }

#endif /* CONFIG_SERVICE_UDP_ENABLE */
