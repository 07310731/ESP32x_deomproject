/*
 * udp.c —— BSD socket 之上的最小可用 UDP 封装
 *
 * 与 tcp_server 的设计对照（刻意展示两者复杂度）：
 *   - 没有 listen / accept / 派生 handler / 槽位管理 / 僵尸连接 —— UDP
 *     没"连接"概念，整个组件约 tcp_server 一半代码；
 *   - open 配好超时后，调用方拿到 fd 可以直接 sendto / recvfrom；
 *   - recvfrom 每次都带对端地址（src_ip / src_port），没有"对端句柄"概念，
 *     也不需要管理。
 *
 * 【函数名为什么是 udp_socket_*】
 *   lwIP 的 netconn/raw API 用 udp_* 前缀（udp_new / udp_bind / udp_sendto
 *   / udp_recvfrom ...），这些符号会出现在 liblwip.a 里；用户组件若定义
 *   同名函数会撞"multiple definition"。加 socket_ 中缀同时：
 *     - 避开冲突
 *     - 表意："这是 BSD socket 层封装，不是 lwIP netconn 层"
 *
 * 设计要点（详见 udp.h 注释）：
 *   - SO_REUSEADDR：与 tcp_server 一致，关停重起能立刻绑回同一端口；
 *   - SO_RCVTIMEO / SO_SNDTIMEO 在 open 时配好，调用方无需再 setsockopt；
 *   - bind 与 tcp_server 完全相同的"网络字节序 + bind_addr==0 通配"约定；
 *   - UDP 没有 keepalive —— 这是协议决定的，没有"半开连接"这件事。
 */

#include <string.h> /* memset                                                  */
#include <errno.h>  /* errno / EAGAIN / EWOULDBLOCK / EBADF 等                 */
#include <unistd.h> /* close()                                                 */

#include <sys/socket.h>  /* socket() bind() sendto() recvfrom() setsockopt()    */
#include <netinet/in.h>  /* IPPROTO_UDP / struct sockaddr_in / INADDR_ANY       */
#include <arpa/inet.h>   /* htons()                                             */

#include "lwip/ip4_addr.h" /* ip4_addr_t / ip4addr_ntoa_r()：格式化地址         */

#include "esp_err.h" /* esp_err_t / ESP_OK / ESP_FAIL / ESP_ERR_*              */
#include "esp_log.h" /* ESP_LOGE / ESP_LOGI / ESP_LOGW 日志宏                    */

#include "udp.h" /* 本组件自己的类型与函数声明                                  */

/* 日志标签：每条日志会带 "udp" 前缀 */
static const char* TAG = "udp";

/* ---- 默认值（cfg 字段 <= 0 时使用） ---- */
#define UDP_RECV_TIMEOUT_DEFAULT_MS 1000 /* recvfrom 轮询间隔（ms）             */
#define UDP_SEND_TIMEOUT_DEFAULT_MS 5000 /* sendto 超时（ms）                   */

/* ---- "<= 0 用默认"的统一翻译（int 字段通用；与 tcp_server 一致） ---- */
static int resolve_int(int v, int dflt)
{
    return v > 0 ? v : dflt;
}

/**
 * @brief 把 IPv4 + port 打包成 sockaddr_in（host 序 port → net 序）
 */
static void fill_sockaddr_in(struct sockaddr_in* addr, uint32_t ipv4_net, uint16_t port_host)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port_host);
    addr->sin_addr.s_addr = (in_addr_t)ipv4_net;
}

esp_err_t udp_socket_open(const udp_cfg_t* cfg, int* sock_fd)
{
    /* ============ 0. 参数校验 ============ */
    if (!cfg || !sock_fd)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->port == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* 防御性：先置 -1，失败直接 return 时调用方拿到的也是无效值 */
    *sock_fd = -1;

    /* ============ 1. 建 socket ============ */
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
    {
        ESP_LOGE(TAG, "socket 失败: errno=%d", errno);
        return ESP_FAIL;
    }

    /* ============ 2. SO_REUSEADDR ============
     * 没这条的话，前一次 socket 还在 TIME_WAIT（UDP 没握手，实际概率为 0；
     * 这里是防御性设置）时重启 udp，bind 同一个端口会失败。 */
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* ============ 3. SO_BROADCAST（可选） ============
     * 仅"主动发广播"需要；接收对端的广播报文不需要此选项。 */
    if (cfg->broadcast)
    {
        (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    }

    /* ============ 4. 收/发超时 ============
     * recv 超时给短一些（默认 1s），让 recvfrom 能周期性唤醒、让调用方能
     * 检查网络状态 / IP 是否变了 —— 否则会卡死。send 超时给 5s 与 tcp_server
     * 对齐（UDP sendto 局域网几乎不会阻塞，更多是保险）。 */
    int rcvto_ms = resolve_int(cfg->recv_timeout_ms, UDP_RECV_TIMEOUT_DEFAULT_MS);
    struct timeval rcvto = {
        .tv_sec = rcvto_ms / 1000,
        .tv_usec = (rcvto_ms % 1000) * 1000,
    };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

    int sndto_ms = resolve_int(cfg->send_timeout_ms, UDP_SEND_TIMEOUT_DEFAULT_MS);
    struct timeval sndto = {
        .tv_sec = sndto_ms / 1000,
        .tv_usec = (sndto_ms % 1000) * 1000,
    };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));

    /* ============ 5. bind ============
     * bind_addr == 0 → 通配 0.0.0.0（本机所有 IP）；
     * bind_addr != 0 → 只绑该地址（调用方传入本机 IP，如 STA 拿到的地址）。
     * 非 0 时该地址必须已经配置在某个网卡上，否则内核返回 EADDRNOTAVAIL。
     * 与 tcp_server_listen 完全相同的约定 —— bind_addr 是网络字节序。 */
    struct sockaddr_in addr;
    fill_sockaddr_in(&addr, cfg->bind_addr, cfg->port);

    /* 日志用点分十进制：bind_addr 是网络字节序，与 lwIP ip4_addr_t.addr 同布局 */
    char bind_str[16];
    ip4_addr_t bind_ip;
    bind_ip.addr = cfg->bind_addr;
    (void)ip4addr_ntoa_r(&bind_ip, bind_str, sizeof(bind_str));

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        ESP_LOGE(TAG, "bind %s:%u 失败: errno=%d", bind_str, (unsigned)cfg->port, errno);
        close(fd);
        return ESP_FAIL;
    }

    *sock_fd = fd;
    ESP_LOGI(TAG, "监听 %s:%u (fd=%d, rcvto=%dms, sndto=%dms, bcast=%d)",
             bind_str, (unsigned)cfg->port, fd, rcvto_ms, sndto_ms, (int)cfg->broadcast);
    return ESP_OK;
}

esp_err_t udp_socket_sendto(int fd, const void* data, size_t len,
                            uint32_t dest_ip, uint16_t dest_port, size_t* sent)
{
    if (fd < 0 || !data || len == 0 || dest_ip == 0 || dest_port == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (sent)
    {
        *sent = 0;
    }

    struct sockaddr_in addr;
    fill_sockaddr_in(&addr, dest_ip, dest_port);

    ssize_t n = sendto(fd, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    if (n > 0)
    {
        if (sent)
        {
            *sent = (size_t)n;
        }
        return ESP_OK;
    }
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
        {
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGE(TAG, "sendto 失败: errno=%d", errno);
        return ESP_FAIL;
    }
    /* sendto 返回 0 几乎不可能（UDP 不会短写），但写下来防一手 */
    return ESP_FAIL;
}

esp_err_t udp_socket_recvfrom(int fd, void* buf, size_t max_len,
                              uint32_t* src_ip, uint16_t* src_port, size_t* received)
{
    if (fd < 0 || !buf || max_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (received)
    {
        *received = 0;
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    ssize_t n = recvfrom(fd, buf, max_len, 0, (struct sockaddr*)&addr, &addr_len);
    if (n > 0)
    {
        if (src_ip)
        {
            *src_ip = (uint32_t)addr.sin_addr.s_addr;
        }
        if (src_port)
        {
            *src_port = ntohs(addr.sin_port);
        }
        if (received)
        {
            *received = (size_t)n;
        }
        return ESP_OK;
    }
    if (n == 0)
    {
        /* UDP recvfrom 几乎不返回 0 字节（对端发空报文没意义）；
         * 按"无数据"处理让调用方继续循环。 */
        return ESP_OK;
    }

    /* n < 0 */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
    {
        /* 超时没数据：与 tcp_server_recv 一致返回 ESP_OK + received=0 */
        return ESP_OK;
    }
    if (errno == EINTR)
    {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "recvfrom 失败: errno=%d", errno);
    return ESP_FAIL;
}

esp_err_t udp_socket_close(int fd)
{
    if (fd < 0)
    {
        return ESP_OK;
    }
    if (close(fd) < 0)
    {
        ESP_LOGW(TAG, "close fd=%d 失败: errno=%d", fd, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}