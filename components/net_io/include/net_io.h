/**
 * @file net_io.h
 * @brief 网络收发层对外接口：业务只跟这一组函数打交道
 *
 * 三条上行入口 + 一个下行回调 + 一个启动函数。socket、队列、consumer 任务、
 * 重连、心跳、门控全在组件内部：换服务模式（TCP 客户端 / 服务端 / UDP）
 * 不需要改业务代码。
 *
 * 线程模型：
 *   上行：调用者 → tx 队列 → tx_consumer → socket send
 *   下行：各服务 recv 任务 → rx 队列 → rx_consumer → 业务回调
 *   两个方向独立，发送不会被接收阻塞。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 帧的来源 / 去向，值即内部队列元素的 kind */
typedef enum
{
    NET_IO_LINK_TCP_CLIENT = 0, /*!< TCP 长连接（设备作客户端）   */
    NET_IO_LINK_TCP_SERVER = 1, /*!< TCP 服务端某个已接入的客户端 */
    NET_IO_LINK_UDP        = 2, /*!< UDP 对端                     */
} net_io_link_t;

/** 一段收到的数据（只在回调期间有效） */
typedef struct
{
    net_io_link_t link;  /*!< 从哪个通道来的                                */
    int peer_fd;         /*!< TCP_SERVER：对端 fd，回发时原样传回            */
    uint32_t peer_ip;    /*!< UDP：对端 IP（网络字节序）                     */
    uint16_t peer_port;  /*!< UDP：对端端口（主机字节序）                    */
    const uint8_t* data; /*!< **回调返回后失效**，要留存请自己 memcpy        */
    uint16_t len;        /*!< 字节数                                        */
} net_io_frame_t;

/** 业务侧收数据回调；运行在 rx_consumer 任务（栈 4096），不要做长耗时操作 */
typedef void (*net_io_recv_cb_t)(const net_io_frame_t* frame);

/** 启动收发层：建队列 + 两个常驻 consumer 任务（可重复调用） */
esp_err_t net_io_start(void);

/** 注册收数据回调（启动前后都能调，NULL = 不处理；只支持一个回调） */
void net_io_set_recv_cb(net_io_recv_cb_t cb);

/**
 * @brief 这段数据是不是"自己发出去的心跳被回显回来了"
 *
 * echo 类服务器会把心跳原样回传，业务若也回发就成了无限乒乓。回调里第一件事
 * 调本函数，是心跳就 return。
 */
bool net_io_heartbeat_is_echo(const void* data, size_t len);

/** 重读运行期配置（目标地址、心跳）；网页保存后调用，不等 service 重启 */
void net_io_reload_config(void);

/** 门控总闸：false 时各服务挂起等待（配网热点阶段始终 false） */
void net_io_set_net_ready(bool ready);
bool net_io_is_net_ready(void);

/**
 * @brief 上行入口：TCP 长连接 / 服务端某客户端 / UDP 对端
 *
 * @return true = 已入队（**不代表已发到对端**）；false = 未连接 / 队列满 / 参数非法
 * @note 非阻塞，任意任务可调。
 */
bool net_io_send_tcp_client(const void* data, size_t len);
bool net_io_send_tcp_server(int client_fd, const void* data, size_t len);
bool net_io_send_udp(uint32_t dst_ip, uint16_t dst_port, const void* data, size_t len);

/** 按 frame->link 原路发回（echo 场景的一行封装） */
bool net_io_send_back(const net_io_frame_t* frame, const void* data, size_t len);

#ifdef __cplusplus
}
#endif
