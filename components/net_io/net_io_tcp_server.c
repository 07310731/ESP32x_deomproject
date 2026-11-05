/**
 * @file net_io_tcp_server.c
 * @brief TCP 服务端：等网络就绪 → listen → accept 派发 → 每连接一个 recv 任务
 *
 * 槽位只在"handler 任务真的退出"时释放，所以每条连接最终都必须能退出：
 * 对端 close、收发出错、keepalive 探活失败、空闲超时（
 * CONFIG_TCP_SERVER_IDLE_TIMEOUT_MS）。缺了后两道，僵尸连接会把槽位占满。
 */
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"

#include "net_io.h"
#include "net_io_internal.h"
#include "tcp_server.h"
#include "webconfig.h"

static const char* TAG = "net_srv";

#if CONFIG_SERVICE_TCP_SERVER_ENABLE

static volatile bool s_server_stop = false;
static volatile bool s_server_alive = false;

static void tcp_server_client_task(void* arg);

/* fd 和 task 都归零才算空槽：只看 fd 会把"fd 已回收、handler 还没退出"的
 * 槽位误判为空闲，新连接复用后会踩到旧 handler。 */
static int alloc_slot(void)
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

static void clear_slot(int idx)
{
    s_server_clients[idx].fd = -1;
    s_server_clients[idx].task = NULL;
}

/* 每个连接的接收任务：阻塞 recv → 投 RX 队列，由业务回调统一处理 */
static void tcp_server_client_task(void* arg)
{
    const int idx = (int)(intptr_t)arg;
    const int fd = s_server_clients[idx].fd;
    TickType_t last_rx = xTaskGetTickCount(); /* 用于识别"连着但不说话"的连接 */

    ESP_LOGI(TAG, "client[%d] 已接入 (fd=%d)", idx, fd);

    while (s_server_running)
    {
        net_io_rx_item_t item = { 0 };
        size_t got = 0;
        esp_err_t err = tcp_server_recv(fd, item.data, sizeof(item.data), &got);
        item.len = (uint16_t)got;

        if (err == ESP_ERR_INVALID_STATE) /* 对端有序关闭 */
        {
            break;
        }
        if (err != ESP_OK) /* fd 被 server 关掉 / keepalive 判死 */
        {
            ESP_LOGW(TAG, "client[%d] recv 错误: %s", idx, esp_err_to_name(err));
            break;
        }
        if (item.len == 0)
        {
            /* 超时是空闲连接的常态，不能一超时就断；只有累计空闲超阈值才断 */
            if (CONFIG_TCP_SERVER_IDLE_TIMEOUT_MS > 0 &&
                (xTaskGetTickCount() - last_rx) >=
                    pdMS_TO_TICKS(CONFIG_TCP_SERVER_IDLE_TIMEOUT_MS))
            {
                ESP_LOGW(TAG, "client[%d] 空闲 %d ms 无数据，主动断开", idx,
                         CONFIG_TCP_SERVER_IDLE_TIMEOUT_MS);
                break;
            }
            continue;
        }

        last_rx = xTaskGetTickCount();
        ESP_LOGI(TAG, "RX %u 字节 (client[%d])", (unsigned)item.len, idx);
        if (!net_io_rx_post(NET_IO_LINK_TCP_SERVER, fd, 0, 0, item.data, item.len))
        {
            ESP_LOGW(TAG, "client[%d] RX 队列满，本段 %u 字节被丢弃", idx, (unsigned)item.len);
        }
    }

    /* 只动还属于自己的槽位：
     *   task == 自己 → 正常收尾，fd 还是自己的就 close，再释放槽位；
     *   task == NULL → server 关停时已替我们 close 并把 fd 置 -1，不能再 close
     *                  （那个编号可能已分配给别的连接）。 */
    if (s_server_clients[idx].task == xTaskGetCurrentTaskHandle())
    {
        if (s_server_clients[idx].fd == fd)
        {
            (void)tcp_server_close(fd);
        }
        clear_slot(idx);
    }
    ESP_LOGI(TAG, "client[%d] 已关闭", idx);
    vTaskDelete(NULL);
}

/* 主循环。退出重建有三种原因，共用同一套收尾：
 * 停服 / 网络断开 / 本机 IP 变了（绑定的地址失效，需重建）。 */
static void tcp_server_task(void* arg)
{
    (void)arg;
    s_server_alive = true;

    for (int i = 0; i < CONFIG_TCP_SERVER_MAX_CLIENTS; i++)
    {
        clear_slot(i);
    }

    while (!s_server_stop)
    {
        while (!net_io_is_net_ready() && !s_server_stop)
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        if (s_server_stop)
        {
            break;
        }

        /* 端口：NVS 优先（每次重建重读，网页改了下次生效）→ menuconfig 兜底。
         * 拿不到本机 IP 时 bind 0.0.0.0，保证 server 不会因"IP 未就绪"起不来。 */
        tcp_server_cfg_t cfg = TCP_SERVER_CFG_DEFAULT();
        webconfig_dev_cfg_t dev;
        cfg.port = (webconfig_load_dev_cfg(&dev) && dev.tcp_server_port != 0)
                       ? dev.tcp_server_port
                       : CONFIG_TCP_SERVER_PORT;
        cfg.bind_addr = net_io_local_ip();
        if (cfg.bind_addr == 0)
        {
            ESP_LOGW(TAG, "未取到本机 IP，暂时监听所有地址（0.0.0.0）");
        }

        int listen_fd = -1;
        esp_err_t err = tcp_server_listen(&cfg, &listen_fd);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "启动失败（端口 %u），2s 后重试", (unsigned)cfg.port);
            net_io_breakable_delay_ms(2000, &s_server_stop);
            continue;
        }

        s_server_running = true;
        webconfig_set_comm_state(true);
        ESP_LOGI(TAG, "已监听 %u 端口（bind=%u）", (unsigned)cfg.port, (unsigned)cfg.bind_addr);

        bool ip_changed = false;
        while (net_io_is_net_ready() && !s_server_stop)
        {
            int cfd = -1;
            err = tcp_server_accept(listen_fd, &cfd, cfg.accept_poll_ms);
            if (err == ESP_ERR_TIMEOUT)
            {
                /* 没人连时顺手看 IP 变没变：门控只知道"有没有网"，不知道 IP 变化，
                 * 而 listen socket 绑的是旧地址，不重建就等不到新地址的客户端。
                 * now_ip == 0 是 netif 暂时无地址，交给"网络断开"那条路径处理。 */
                const uint32_t now_ip = net_io_local_ip();
                if (now_ip != 0 && now_ip != cfg.bind_addr)
                {
                    ip_changed = true;
                    break;
                }
                continue;
            }
            if (err != ESP_OK)
            {
                break; /* listen_fd 坏了，退出重建 */
            }

            const int slot = alloc_slot();
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
            /* 栈 4096：ESP_LOG 的 vprintf 峰值 1KB+，2048 会撞栈溢出 */
            if (xTaskCreate(tcp_server_client_task, task_name, 4096, (void*)(intptr_t)slot, 5,
                            &s_server_clients[slot].task) != pdPASS)
            {
                ESP_LOGE(TAG, "创建 client handler 失败，关掉连接");
                (void)tcp_server_close(cfd);
                clear_slot(slot);
                continue;
            }
            ESP_LOGI(TAG, "新连接接入槽位 %d（当前 %d/%d）", slot, slot + 1,
                     CONFIG_TCP_SERVER_MAX_CLIENTS);
        }

        /* 收尾顺序很重要：先关 listen + client fd + 等 handler，最后才判停服。
         * 若一见 s_server_stop 就 break，socket 全不会 close —— fd 泄漏，且下次
         * 切回 TCP_SERVER 时 bind 会因端口仍被自己占着而失败。 */
        if (s_server_stop)
        {
            ESP_LOGW(TAG, "service_manager 要求停止");
        }
        else if (ip_changed)
        {
            ESP_LOGW(TAG, "本机 IP 已变化，重建（改绑新地址）");
        }
        else
        {
            ESP_LOGW(TAG, "网络断开，停止");
        }
        s_server_running = false;
        webconfig_set_comm_state(false);

        (void)tcp_server_close(listen_fd);

        /* 关掉所有 client fd：handler 的 recv/send 会立刻报错退出。用 plain close
         * 而非封装函数（关停时 fd 状态不定，避免刷警告）。fd 置 -1 是给 handler
         * 的信号"已被 server 回收，别再 close"；task 字段保留，alloc_slot 不会
         * 把还没腾空的槽位分给新连接。 */
        for (int i = 0; i < CONFIG_TCP_SERVER_MAX_CLIENTS; i++)
        {
            if (s_server_clients[i].fd >= 0)
            {
                close(s_server_clients[i].fd);
                s_server_clients[i].fd = -1;
            }
        }

        /* 等 handler 自己退出，最多 6s（比 SO_SNDTIMEO 略长）。仍不退的【不强清】：
         * 强清会让新连接复用槽位，而旧 handler 手里的 fd 编号可能已重新分配。 */
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

        if (s_server_stop)
        {
            for (int i = 0; i < CONFIG_TCP_SERVER_MAX_CLIENTS; i++)
            {
                if (s_server_clients[i].task != NULL)
                {
                    ESP_LOGE(TAG, "停服：槽位 %d 的 handler 仍未退出，保持占用", i);
                }
            }
            break;
        }
    }
    s_server_alive = false;

    /* 清共用队列：残留 item（尤其 TX 里的旧 fd）不该被下一个 service 处理。
     * 本任务是自杀式退出，清理必须放在 vTaskDelete 之前。 */
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

void tcp_server_service_fn(void)
{
    s_server_stop = false;
    s_server_alive = true;
    tcp_server_task(NULL); /* 结尾自己清队列 + vTaskDelete */
}

void tcp_server_request_stop(void)
{
    s_server_stop = true;
}

bool tcp_server_is_running(void)
{
    return s_server_alive;
}

#else /* !CONFIG_SERVICE_TCP_SERVER_ENABLE */

void tcp_server_service_fn(void) {}
void tcp_server_request_stop(void) {}
bool tcp_server_is_running(void) { return false; }

#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */
