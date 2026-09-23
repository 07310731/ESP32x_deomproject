/*
 * udp.h —— BSD socket 之上的最小可用 UDP 封装
 *
 * 设计要点：
 *   - 与 tcp_server 风格一致：暴露真实 lwIP socket fd（不 opaque handle），
 *     调用方可直接 select / fcntl / setsockopt；
 *   - UDP **不区分 server / client**：同一个 fd 既能 sendto（主动）也能
 *     recvfrom（被动），每条报文都带对端地址；
 *   - open 时自动配 SO_RCVTIMEO（默认 1s，调用方可改）：让 recvfrom 能
 *     周期性唤醒，调用方能检查 "网络/任务该退出了吗"，避免永远阻塞；
 *   - IPv4 only：默认 bind 通配 0.0.0.0，也可由调用方指定只监听本机某个 IP。
 *
 * 不做：
 *   - 多 socket 管理 / 组播 / 收发解耦 / 应用层协议 —— 由调用方决定。
 *
 * 【为什么函数名是 udp_socket_* 而不是 udp_*】
 *   lwIP 把自己的 netconn/raw API 用 udp_* 前缀命名（udp_new / udp_bind /
 *   udp_send / udp_sendto / udp_recv / udp_recvfrom ...），这些符号会出
 *   现在 liblwip.a 里；用户组件若定义同名函数会撞"multiple definition"。
 *   加 socket_ 中缀避开冲突，同时表明"这是 BSD socket 层的封装，不是
 *   lwIP netconn 层"。
 */

#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UDP 打开配置（字段 <= 0 / false 时使用本组件默认值）
 */
typedef struct
{
    uint16_t port;        /*!< bind 端口（**必填**）；填 0 = 非法，直接拒绝    */
    uint32_t bind_addr;   /*!< 绑定的本机 IPv4（**网络字节序**）；
                                0 = INADDR_ANY（监听本机所有 IP）。想只监听某个
                                netif 时由调用方先取到本机 IP（如 esp_netif_get_ip_info()）
                                再填进来                                          */
    int recv_timeout_ms;  /*!< SO_RCVTIMEO（ms），<= 0 走默认（1s）            */
    int send_timeout_ms;  /*!< SO_SNDTIMEO（ms），<= 0 走默认（5s）            */
    bool broadcast;       /*!< 是否打开 SO_BROADCAST（允许 sendto 发广播）。
                                仅在"主动发广播"时需要；接收对端广播不需要。     */
} udp_cfg_t;

/**
 * @brief 用宏一键得到一份"全部走默认"的配置；调用方再覆盖需要的字段
 *
 * @code
 *     udp_cfg_t cfg = UDP_CFG_DEFAULT();
 *     cfg.port = 7777;
 *     cfg.bind_addr = my_local_ip; // 本机 IPv4（网络字节序）；0 = 通配
 *     udp_socket_open(&cfg, &fd);
 * @endcode
 */
#define UDP_CFG_DEFAULT() ((udp_cfg_t){                      \
    .port = 0,                                               \
    .bind_addr = 0,                                          \
    .recv_timeout_ms = 0,                                    \
    .send_timeout_ms = 0,                                    \
    .broadcast = false,                                      \
})

/**
 * @brief 建 socket + bind；与 tcp_server_listen 风格一致
 *
 * @param[in]  cfg      配置（port 必填；其它字段 <= 0 / false 走默认）
 * @param[out] sock_fd  成功时写入有效 socket fd；失败时写入 -1
 *
 * @return ESP_OK               成功
 * @return ESP_ERR_INVALID_ARG  cfg 或 sock_fd 为空 / port 为 0
 * @return ESP_FAIL             socket / bind 失败（具体原因看 errno 日志）
 *
 * @note 失败时不持有任何资源；调用方拿到的 sock_fd 一定是 -1
 */
esp_err_t udp_socket_open(const udp_cfg_t* cfg, int* sock_fd);

/**
 * @brief 向指定地址发送一帧 UDP 数据报
 *
 * @param fd         socket fd（由 udp_socket_open 返回）
 * @param data       待发送数据
 * @param len        数据长度
 * @param dest_ip    目标 IPv4（**网络字节序**，与 esp_ip4_addr_t.addr 同布局）
 * @param dest_port  目标端口（**主机字节序**，内部统一 htons）
 * @param[out] sent  实际发送字节数（NULL 时忽略）。
 *                   UDP 下"全部发送或失败" —— sent == len 即成功
 *
 * @return ESP_OK
 * @return ESP_ERR_INVALID_ARG  参数非法（fd<0 / data 为空 / len=0 /
 *                              dest_ip=0 / dest_port=0）
 * @return ESP_ERR_TIMEOUT      sendto 阻塞超时（EAGAIN / EWOULDBLOCK / ETIMEDOUT）
 * @return ESP_FAIL             其它错误（具体看 errno 日志）
 *
 * @note dest_ip == 0 不合法（INADDR_ANY 不可作为 sendto 目标，系统返 EINVAL）。
 *       dest_port == 0 也不合法（系统会随机选端口，但语义混乱，直接挡掉）。
 *       如果调用方真要发广播，把 dest_ip 填 `0xFFFFFFFF`（INADDR_BROADCAST），
 *       并在 cfg 里 broadcast=true。
 */
esp_err_t udp_socket_sendto(int fd, const void* data, size_t len,
                            uint32_t dest_ip, uint16_t dest_port, size_t* sent);

/**
 * @brief 接收一帧 UDP 数据报（带对端地址）
 *
 * @param fd            socket fd
 * @param buf           接收缓冲区（建议 >= 1500，避免收不到大于 MTU 的报文）
 * @param max_len       缓冲区大小
 * @param[out] src_ip   对端 IPv4（**网络字节序**）；NULL 时忽略
 * @param[out] src_port 对端端口（**主机字节序**，内部 ntohs）；NULL 时忽略
 * @param[out] received 实际接收字节数（NULL 时忽略）。
 *                      超时没数据时为 0
 *
 * @return ESP_OK               收到数据（received > 0）或超时没数据（received == 0）
 * @return ESP_ERR_INVALID_ARG  参数非法
 * @return ESP_FAIL             其它错误（具体看 errno 日志）
 *
 * @note UDP recvfrom 几乎不返回 0 字节（对端发空报文没意义）；本函数把所有
 *       "返回 0"或超时都当作"无数据"处理（ESP_OK + received=0），让调用方
 *       继续循环。
 */
esp_err_t udp_socket_recvfrom(int fd, void* buf, size_t max_len,
                              uint32_t* src_ip, uint16_t* src_port, size_t* received);

/**
 * @brief 关 socket（与 tcp_server_close 风格一致）
 *
 * @param fd socket fd（由 udp_socket_open 返回）；传 < 0 时直接返回 ESP_OK
 *
 * @return ESP_OK    成功（或 fd 无效）
 * @return ESP_FAIL  close 失败（具体看 errno 日志）
 */
esp_err_t udp_socket_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* UDP_H */