#ifndef _TCP_SERVER_H_
#define _TCP_SERVER_H_

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief TCP 服务端初始化参数
     *
     * @note 字段都有对应的默认值，推荐先用 TCP_SERVER_CFG_DEFAULT() 整体初始化，
     *       再覆盖需要的字段（最常见的是 .port）。被省略的字段（<= 0）会自动
     *       翻译成"合理默认值"。
     */
    typedef struct
    {
        uint16_t port;       /*!< 监听端口，1 ~ 65535，不可为 0                   */
        int listen_backlog;  /*!< 内核 listen 队列长度（已接受但未被 accept 取出
                                 的连接数上限），<= 0 用默认 4                    */
        int accept_poll_ms;  /*!< accept() 单次 select 等待毫秒数；用于周期性
                                 醒来检查 "server 是否还要继续"。 <= 0 用默认 200。
                                 @note listen() 不读这个字段：它由调用方在 accept
                                 循环里传给 tcp_server_accept()。放在 cfg 里只是
                                 为了让它跟 port / backlog 一起被
                                 TCP_SERVER_CFG_DEFAULT() 带出默认值。 */

        /*!< 绑定的本地 IPv4，**网络字节序**（直接填 esp_ip4_addr_t.addr）；
             0 = 监听所有本机 IP（等价 0.0.0.0）。想只监听某一个接口/地址时，
             由调用方先取到本机 IP（如 esp_netif_get_ip_info()）再填进来。 */
        uint32_t bind_addr;
    } tcp_server_cfg_t;

    /**
     * @brief 默认配置：port=0（必须由调用方填）、listen_backlog=4、accept_poll_ms=200
     *
     * @note listen() 之前**必须**自己填 .port；其它字段用 0 就走默认值。
     */
#define TCP_SERVER_CFG_DEFAULT()                                                                     \
    ((tcp_server_cfg_t){                                                                             \
        .port = 0,                                                                                   \
        .listen_backlog = 4,                                                                         \
        .accept_poll_ms = 200,                                                                       \
        .bind_addr = 0,                                                                              \
    })

    /**
     * @brief  建监听 socket：socket(AF_INET) → setsockopt(SO_REUSEADDR) →
     *         bind(bind_addr:port) → listen(backlog)
     *
     * @param  cfg       连接参数，不可为 NULL，.port 必须 > 0
     * @param  listen_fd 输出参数：成功时写入 fd（>= 0），失败时写入 -1，不可为 NULL
     *
     * @note .bind_addr == 0 时绑定通配地址 0.0.0.0（监听本机所有 IP）；
     *       非 0 时只绑定该地址 —— 地址必须已经在本机网卡上配置好，否则 bind
     *       会失败（EADDRNOTAVAIL），此时应等拿到 IP 之后再调用。
     *
     * @note 失败时 *listen_fd 一定是 -1；调用方可以直接判断，不读返回值也安全。
     *
     * @return
     *     - ESP_OK               成功
     *     - ESP_ERR_INVALID_ARG  cfg / listen_fd 为 NULL 或 port 为 0
     *     - ESP_FAIL             socket / bind / listen 失败（errno 见日志）
     */
    esp_err_t tcp_server_listen(const tcp_server_cfg_t* cfg, int* listen_fd);

    /**
     * @brief  等一个新连接：内部用 select() 短轮询唤醒，再 accept()
     *
     * @param  listen_fd tcp_server_listen() 返回的监听 fd（>= 0）
     * @param  client_fd 输出参数：成功时写入新连接的 fd（>= 0），失败时写入 -1，
     *                    不可为 NULL
     * @param  poll_ms   select 单次等待毫秒数，<= 0 用默认 200。
     *                   常规用法：把本轮 listen 用的那份 cfg 的
     *                   .accept_poll_ms 直接传进来，这样 "cfg 里配了就生效"；
     *                   传 0 则用默认值。
     *
     * @note  接受成功后会自动给新 fd 配置：
     *        - SO_RCVTIMEO / SO_SNDTIMEO = 5s（与 tcp_client 的默认对齐）；
     *        - TCP keepalive：空闲 10s 开始探测、间隔 3s、连续 3 次无响应判定
     *          连接死亡。用来回收"对端断电/掉线但没发 FIN"的半开连接 —— 没有它，
     *          这类连接的 recv 会永远只是超时（不是错误），调用方的循环退不出来，
     *          连接槽也就永远不释放。
     *        所以调用方可以直接用 tcp_server_send / tcp_server_recv /
     *        tcp_server_close，无需自己 setsockopt。
     *
     * @note  poll_ms 同时也是调用方"最长多久被打断一次"的上限：超时返回
     *        ESP_ERR_TIMEOUT 后调用方有机会重新检查自己的退出条件。
     *
     * @return
     *     - ESP_OK               成功（新连接已接受，*client_fd >= 0）
     *     - ESP_ERR_INVALID_ARG  listen_fd < 0 或 client_fd 为 NULL
     *     - ESP_ERR_TIMEOUT      select 超时，无新连接 —— 调用方继续循环
     *     - ESP_FAIL             select / accept 失败（errno 见日志）
     */
    esp_err_t tcp_server_accept(int listen_fd, int* client_fd, int poll_ms);

    /**
     * @brief  发送数据：与 tcp_client_send 语义一致，但用于服务端接受的连接
     *
     * @return
     *     - ESP_OK               调用成功，*written == len
     *     - ESP_ERR_INVALID_ARG  fd < 0、data 为 NULL 或 len 为 0
     *     - ESP_ERR_TIMEOUT      超时，*written 写入已发送字节数
     *     - ESP_FAIL             send 失败或对端关闭，*written 写入已发送字节数
     */
    esp_err_t tcp_server_send(int fd, const void* data, size_t len, size_t* written);

    /**
     * @brief  接收数据：与 tcp_client_recv 语义一致，但用于服务端接受的连接
     *
     * @return
     *     - ESP_OK                  调用成功（读到 0 字节视为超时，*read = 0）
     *     - ESP_ERR_INVALID_ARG     fd < 0、buf 为 NULL 或 max_len 为 0
     *     - ESP_ERR_INVALID_STATE   对端关闭连接（应 tcp_server_close）
     *     - ESP_FAIL                recv 失败（errno 见日志）
     */
    esp_err_t tcp_server_recv(int fd, void* buf, size_t max_len, size_t* read);

    /**
     * @brief  关闭 socket（listen / client 都用同一个）；fd < 0 直接返回 ESP_OK
     *
     * @return
     *     - ESP_OK  总是成功（失败仅打 WARN，不挡调用方）
     */
    esp_err_t tcp_server_close(int fd);

#ifdef __cplusplus
}
#endif

#endif