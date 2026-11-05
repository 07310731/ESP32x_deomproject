/*
 * udp.c —— BSD socket 之上的最小 UDP 封装
 *
 * 比 tcp_server 简单得多：没有 listen / accept / handler / 槽位 —— UDP 没有
 * "连接"概念，也就没有槽位错乱、fd 泄漏、僵尸连接这些问题。
 *
 * 【函数名为什么带 socket_】lwIP 的 netconn/raw API 用 udp_* 前缀
 * （udp_new / udp_bind / udp_sendto ...），会出现在 liblwip.a 里，同名会撞
 * multiple definition。加 socket_ 中缀既避冲突，也表明"这是 BSD socket 层"。
 *
 * 约定：SO_REUSEADDR 便于关停重起；收发超时在 open 时配好；bind_addr 是
 * 网络字节序，0 表示 0.0.0.0。UDP 没有 keepalive —— 协议决定的。
 */
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "lwip/ip4_addr.h"

#include "esp_err.h"
#include "esp_log.h"

#include "udp.h"

static const char* TAG = "udp";

#define UDP_RECV_TIMEOUT_DEFAULT_MS 1000
#define UDP_SEND_TIMEOUT_DEFAULT_MS 5000

static int resolve_int(int v, int dflt)
{
    return v > 0 ? v : dflt;
}

/* IPv4 + 主机序端口 → sockaddr_in */
static void fill_sockaddr_in(struct sockaddr_in* addr, uint32_t ipv4_net, uint16_t port_host)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port_host);
    addr->sin_addr.s_addr = (in_addr_t)ipv4_net;
}

esp_err_t udp_socket_open(const udp_cfg_t* cfg, int* sock_fd)
{
    if (!cfg || !sock_fd || cfg->port == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *sock_fd = -1;

    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
    {
        ESP_LOGE(TAG, "socket 失败: errno=%d", errno);
        return ESP_FAIL;
    }

    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (cfg->broadcast)
    {
        (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    }

    /* recv 超时给短（默认 1s）：让 recvfrom 周期性回到调用方检查网络 / IP 变化，
     * 否则会卡死。send 给 5s 与 tcp_server 对齐（UDP 局域网几乎不会阻塞）。 */
    const int rcvto_ms = resolve_int(cfg->recv_timeout_ms, UDP_RECV_TIMEOUT_DEFAULT_MS);
    struct timeval rcvto = { .tv_sec = rcvto_ms / 1000, .tv_usec = (rcvto_ms % 1000) * 1000 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

    const int sndto_ms = resolve_int(cfg->send_timeout_ms, UDP_SEND_TIMEOUT_DEFAULT_MS);
    struct timeval sndto = { .tv_sec = sndto_ms / 1000, .tv_usec = (sndto_ms % 1000) * 1000 };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));

    struct sockaddr_in addr;
    fill_sockaddr_in(&addr, cfg->bind_addr, cfg->port);

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
    ESP_LOGI(TAG, "监听 %s:%u (fd=%d, rcvto=%dms, sndto=%dms, bcast=%d)", bind_str,
             (unsigned)cfg->port, fd, rcvto_ms, sndto_ms, (int)cfg->broadcast);
    return ESP_OK;
}

esp_err_t udp_socket_sendto(int fd, const void* data, size_t len, uint32_t dest_ip,
                            uint16_t dest_port, size_t* sent)
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

    const ssize_t n = sendto(fd, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
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
    return ESP_FAIL; /* sendto 返回 0 几乎不可能（UDP 不短写），防一手 */
}

esp_err_t udp_socket_recvfrom(int fd, void* buf, size_t max_len, uint32_t* src_ip,
                              uint16_t* src_port, size_t* received)
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
    const ssize_t n = recvfrom(fd, buf, max_len, 0, (struct sockaddr*)&addr, &addr_len);

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
        return ESP_OK; /* 空报文：按"无数据"处理 */
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT || errno == EINTR)
    {
        return ESP_OK; /* 超时没数据，与 tcp_server_recv 一致 */
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
