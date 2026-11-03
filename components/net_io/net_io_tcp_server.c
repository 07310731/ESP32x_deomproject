/**
 * @file net_io_tcp_server.c
 * @brief TCP 服务端：监听 → accept 派发 → 每个连接一个 recv 任务 → 投 RX 队列
 *
 * 【连接槽位的退役条件】槽位只有"handler 任务真的退出"才会被释放，所以必须
 * 保证每条连接最终都会退出。路径有三条：对端正常关闭（recv 返回 0）、收发出错、
 * 以及两道"连接还在、但已经不该留着"的兜底：
 *   - TCP keepalive（tcp_server 在 accept 时自动配）：对端断电 / 掉线造成的
 *     半开连接，靠内核探测发现并让 recv 报错；
 *   - CONFIG_TCP_SERVER_IDLE_TIMEOUT_MS 空闲断开：对端还活着、但一直不说话。
 * 缺了这两道，僵尸连接会把槽位永久占满，服务端从此接不进新客户端。
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

/* 找一个真正的空槽：fd 和 task 都归零才算 —— 只看 fd 会把"fd 已回收、handler
 * 还没退出来"的槽位误判为空闲，新连接复用后会踩到旧 handler。 */
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

/* 把槽位标成真正的空槽。只在确定"既没有活着的 handler、也没有待回收的 fd"
 * 时调用：server 任务启动初始化、xTaskCreate 失败回滚，以及 handler 自己
 * 确认所有权后退出。 */
static void clear_slot(int idx)
{
    s_server_clients[idx].fd = -1;
    s_server_clients[idx].task = NULL;
}

/**
 * @brief 服务端每个连接的接收任务：阻塞 recv → 投 RX 队列
 *
 * @param arg 槽位下标（intptr_t）
 *
 * @note recv 到数据后不直接 send 回去，而是投到 rx 队列：rx_consumer 任务调
 *       业务回调，业务再 net_io_send_tcp_server() 回投 tx 队列发送。链路长了
 *       一步，但换来"三个服务共用同一条数据通路"，且 handler 不再需要常驻 buf。
 *
 * @note handler 栈 4096：栈上 rx_item_t ≈ 1KB + ESP_LOG 路径峰值 ~1KB +
 *       函数调用栈帧 200B，余量足够。
 */
static void tcp_server_client_task(void* arg)
{
    const int idx = (int)(intptr_t)arg;
    const int fd = s_server_clients[idx].fd;
    /* 最后一次收到数据的时刻，用来识别"连接还在、对端却不再说话"的连接。
     * keepalive 只能发现"对端已经死了"，发现不了"对端活着但一直不发数据"——
     * 后者同样会让本任务永远占着槽位。 */
    TickType_t last_rx = xTaskGetTickCount();

    ESP_LOGI(TAG, "client[%d] 已接入 (fd=%d)", idx, fd);

    while (s_server_running)
    {
        net_io_rx_item_t item = { 0 };
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
            ESP_LOGW(TAG, "client[%d] recv 错误: %s", idx, esp_err_to_name(err));
            break;
        }
        if (item.len == 0)
        {
            /* 本轮 recv 超时。"超时"是空闲连接的常态而非故障，所以不能一超时
             * 就断开；只有累计空闲超过阈值才判定为死连接。 */
            if (CONFIG_TCP_SERVER_IDLE_TIMEOUT_MS > 0 &&
                (xTaskGetTickCount() - last_rx) >= pdMS_TO_TICKS(CONFIG_TCP_SERVER_IDLE_TIMEOUT_MS))
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
    ESP_LOGI(TAG, "client[%d] 已关闭", idx);
    vTaskDelete(NULL);
}

/**
 * @brief TCP 服务端主循环：等网络就绪 → listen → accept 派发 → 关停收尾
 *
 * 退出重建的两种原因（都走同一套收尾）：
 *   - 网络断开（门控变 false）；
 *   - 本机 IP 变了（网络还在，但绑定的地址已失效，比如 AP 自定义网段
 *     在 esp_wifi_start() 之后才下发）。
 */
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
        /* 1) 等网络就绪（同时尊重停服标志，避免"切换模式后这一支一直卡住"） */
        while (!net_io_is_net_ready() && !s_server_stop)
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        if (s_server_stop)
        {
            break;
        }

        /* 2) 取本机 IPv4 作为 bind 地址；拿不到时回退通配 0.0.0.0，
         *    保证 server 不会因为"IP 还没就绪"直接起不来。 */
        tcp_server_cfg_t cfg = TCP_SERVER_CFG_DEFAULT();
        {
            /* 端口：NVS 优先（每次新 worker 启动时重读一次，切换 / 重新配网后
             * 下次启动就生效） → menuconfig 兜底 */
            webconfig_dev_cfg_t dev;
            cfg.port = (webconfig_load_dev_cfg(&dev) && dev.tcp_server_port != 0)
                           ? dev.tcp_server_port
                           : CONFIG_TCP_SERVER_PORT;
        }
        cfg.bind_addr = net_io_local_ip();
        if (cfg.bind_addr == 0)
        {
            ESP_LOGW(TAG, "未取到本机 IP，暂时监听所有地址（0.0.0.0）");
        }

        /* 3) listen —— 端口被占用等失败一律退避 2s 后重试。
         *    地址后面变了也没关系：accept 循环会检测到并回到步骤 2 重建。 */
        int listen_fd = -1;
        esp_err_t err = tcp_server_listen(&cfg, &listen_fd);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "启动失败（端口 %u），2s 后重试", (unsigned)cfg.port);
            net_io_breakable_delay_ms(2000, &s_server_stop);
            continue;
        }

        s_server_running = true;
        webconfig_set_comm_state(true); /* listen 成功 → 页头"状态"在线 */
        ESP_LOGI(TAG, "已监听 %u 端口（bind=%u）", (unsigned)cfg.port, (unsigned)cfg.bind_addr);

        /* 4) accept 循环 */
        bool ip_changed = false; /* true = 退出原因是"本机 IP 变了"，网络仍然可用 */
        while (net_io_is_net_ready() && !s_server_stop)
        {
            int cfd = -1;
            err = tcp_server_accept(listen_fd, &cfd, cfg.accept_poll_ms);
            if (err == ESP_ERR_TIMEOUT)
            {
                /* 这轮没人连。顺手看一眼本机 IP 是否变了 —— 门控只知道"有没有
                 * 网"，对 IP 变化一无所知，而 listen socket 绑的是旧地址，不重建
                 * 就永远等不到新地址上的客户端。now_ip == 0 表示 netif 暂时没
                 * 地址，那种情况由"网络断开"那条路径处理，这里不重建。 */
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
                break; /* listen_fd 可能坏了，退出重建 */
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
            /* 栈 4096：ESP_LOG 的 vprintf 路径峰值能到 1KB+，加上 lwIP socket
             * 调用帧 + 业务局部变量，2048 会撞栈溢出。 */
            BaseType_t ok = xTaskCreate(tcp_server_client_task, task_name, 4096,
                                        (void*)(intptr_t)slot, 5, &s_server_clients[slot].task);
            if (ok != pdPASS)
            {
                ESP_LOGE(TAG, "创建 client handler 失败，关掉连接");
                (void)tcp_server_close(cfd);
                clear_slot(slot);
                continue;
            }
            ESP_LOGI(TAG, "新连接接入槽位 %d（当前 %d/%d）", slot, slot + 1,
                     CONFIG_TCP_SERVER_MAX_CLIENTS);
        }

        /* 5) 关停收尾 —— 三种退出原因共用同一套清理。
         *
         * 【顺序很重要】必须先做完"关 listen + 关 client fd + 等 handler"，
         * 最后才判 s_server_stop 决定走还是留。若一见 s_server_stop 就 break，
         * listen socket 和所有 client socket 都不会被 close —— fd 泄漏之外，
         * 下次再切回 TCP_SERVER 时 bind 会因为端口仍被自己占着而失败。 */
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
        webconfig_set_comm_state(false); /* 监听关闭（重建/停服）→ 离线 */

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

        /* 等 handler 自己退出，最多 6s（刻意比 SO_SNDTIMEO 长一点点）。仍然
         * 没退出的【不强清】：强清会把槽位标成空闲，新连接随即复用它，而旧
         * handler 手里的 fd 编号可能已被新连接重新分配，那就真会误伤。保持
         * "占用"状态让新连接避开这个槽位，是更安全的降级方向。 */
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

    /* 清空共用队列：残留的 RX/TX item 不应该被下一个 service 当成"上一个服务
     * 的数据"处理（尤其 TX 残留里的 item.fd 已是 close 后的旧编号）。
     * worker 自杀不返回，所以清理必须放在 vTaskDelete 之前。 */
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
    tcp_server_task(NULL); /* 函数结尾会自己清队列 + vTaskDelete(NULL) */
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

void tcp_server_service_fn(void)
{
}

void tcp_server_request_stop(void)
{
}

bool tcp_server_is_running(void)
{
    return false;
}

#endif /* CONFIG_SERVICE_TCP_SERVER_ENABLE */
