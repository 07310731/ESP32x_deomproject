/**
 * @file net_io.h
 * @brief 网络收发层对外接口：业务只跟这一组函数打交道
 *
 * 【这个组件解决什么问题】
 * 改造前 main.c 把三件事混在一起：业务处理、socket 收发、WiFi 适配。上层业务
 * 想发一帧数据，得先知道"现在是 TCP 客户端还是服务端还是 UDP"、得拿到 fd、
 * 得自己算目的地址。换一个服务模式，调用点的代码全都要跟着改。
 *
 * 现在 socket / 队列 / consumer 任务 / 重连 / 心跳 / 门控全在组件内部，
 * 对外只留：
 *   - 3 个上行入口：net_io_send_tcp_client() / net_io_send_tcp_server() /
 *     net_io_send_udp()，都是"投队列立刻返回"，任意任务可调、不会被网络阻塞；
 *   - 1 个下行回调：net_io_set_recv_cb()，收到数据时由本组件在 rx_consumer
 *     任务里回调，业务只在这里解析协议；
 *   - 1 个启动：net_io_start()。
 *
 * 【线程模型】上行：调用者 → 队列 → tx_consumer 任务 → socket send。
 *              下行：各服务 recv 任务 → 队列 → rx_consumer 任务 → 回调。
 *              两个方向各自独立，发送不会被接收阻塞。
 *
 * 【依赖】本组件只依赖 tcp_client / tcp_server / udp 三个 socket 封装组件，
 *         不依赖任何业务，也不认识 webconfig 之外的应用概念。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 帧的来源 / 去向
 *
 * @note 值直接对应内部队列元素的 kind，切换服务模式时不会错位。
 */
typedef enum
{
    NET_IO_LINK_TCP_CLIENT = 0, /*!< TCP 长连接（设备作客户端）           */
    NET_IO_LINK_TCP_SERVER = 1, /*!< TCP 服务端某个已接入的客户端         */
    NET_IO_LINK_UDP        = 2, /*!< UDP 对端                             */
} net_io_link_t;

/**
 * @brief 一段收到的数据（回调参数，只在回调期间有效）
 *
 * @note data 指向 rx_consumer 任务栈上的接收缓冲，**回调返回后失效**。
 *       需要留存请自行 memcpy，不要保存指针。
 */
typedef struct
{
    net_io_link_t link;      /*!< 从哪个通道来的                                  */
    int peer_fd;             /*!< NET_IO_LINK_TCP_SERVER：对端 fd，回发时原样传回   */
    uint32_t peer_ip;        /*!< NET_IO_LINK_UDP：对端 IP（网络字节序）           */
    uint16_t peer_port;      /*!< NET_IO_LINK_UDP：对端端口（主机字节序）          */
    const uint8_t* data;     /*!< 数据首地址，len 字节，回调返回后失效              */
    uint16_t len;            /*!< 字节数                                           */
} net_io_frame_t;

/**
 * @brief 业务侧收数据回调
 *
 * @note 运行在 rx_consumer 任务上下文（栈 4096）：不要在里面做长耗时操作，
 *       也不要调用会阻塞等这个任务的动作（例如再往 rx 队列塞数据后等它处理）。
 */
typedef void (*net_io_recv_cb_t)(const net_io_frame_t* frame);

/**
 * @brief 启动收发层：建队列 + 起两个常驻 consumer 任务
 *
 * @note 与 service 是否启用无关：三个服务全关时这两个任务也建（阻塞在队列上、
 *       不占 CPU），这样上层调用发送入口的行为一致——不会出现"某些配置下
 *       net_io_send_* 直接返回 false"。
 *
 * @return
 *     - ESP_OK   启动完成（重复调用只返回 ESP_OK，不会重复建资源）
 *     - 其它     队列 / 任务创建失败
 */
esp_err_t net_io_start(void);

/**
 * @brief 注册业务收数据回调（启动前后都可以调，NULL = 不处理）
 *
 * @note 只允许一个回调：本组件定位是"把数据送到业务"，多路分发应由业务层
 *       在回调里自己做。替换回调是原子的（下一次回调生效）。
 */
void net_io_set_recv_cb(net_io_recv_cb_t cb);

/**
 * @brief 判断一段收到的数据是不是"自己发出去的心跳被回显回来了"
 *
 * @note 只对 TCP 长连接有意义（心跳是那条链路上的概念）。echo 类服务器会把
 *       心跳原样回传，业务若也照原样回发，就变成"心跳 → 回显 → 再回显"的
 *       无限乒乓。业务在回调里第一件事调本函数，是心跳就 return。
 *
 * @return true = 是心跳回包（丢弃）；false = 正常业务数据
 */
bool net_io_heartbeat_is_echo(const void* data, size_t len);

/**
 * @brief 重新读取运行期配置（目标地址、心跳参数）
 *
 * @note 网页保存配置后由应用层调用，让"同协议小改"（只改心跳 / 目标地址，
 *       不触发 service 重启）也能立即生效。协议本身切换由 service_manager
 *       负责，新 worker 启动时会自己再读一次，这里多调一次没有副作用。
 */
void net_io_reload_config(void);

/**
 * @brief 网络是否可用（门控总闸）
 *
 * @note 由 WiFi 适配层按"STA 拿到 IP / 业务 AP 有客户端接入"来置位；
 *       三个服务的收发任务都看它：false 时挂起等待，不空跑、不反复打
 *       重连日志。配网热点阶段始终 false（那里只提供配置页）。
 */
void net_io_set_net_ready(bool ready);
bool net_io_is_net_ready(void);

/**
 * @brief 上行入口：发到 TCP 长连接（TCP_CLIENT 模式）
 *
 * @return true = 已入队（不代表已发到对端）；false = 未连接 / 队列满 / 参数非法
 *
 * @note 非阻塞，调用者可以是任意任务（含中断外的业务任务）。
 */
bool net_io_send_tcp_client(const void* data, size_t len);

/**
 * @brief 上行入口：发回 TCP 服务端的某个客户端
 *
 * @param client_fd 目标 fd，取自 net_io_frame_t.peer_fd（原样传回即可）
 */
bool net_io_send_tcp_server(int client_fd, const void* data, size_t len);

/**
 * @brief 上行入口：发到 UDP 对端
 *
 * @param dst_ip   网络字节序的 IPv4
 * @param dst_port 主机字节序端口
 */
bool net_io_send_udp(uint32_t dst_ip, uint16_t dst_port, const void* data, size_t len);

/**
 * @brief 把一帧数据"按来源原路发回"（echo 等场景的一行便捷封装）
 *
 * @note 等价于按 frame->link 分发到上面三个入口之一。业务要做真正的协议处理，
 *       就在回调里自己判断，不必用这个。
 */
bool net_io_send_back(const net_io_frame_t* frame, const void* data, size_t len);

#ifdef __cplusplus
}
#endif
