/**
 * @file net_io_internal.h
 * @brief net_io 内部共享：队列元素、共用状态、跨文件 helper（业务不要 include）
 *
 * 三个服务的实现分散在 net_io_tcp_client.c / net_io_tcp_server.c / net_io_udp.c，
 * 但共用同一对队列和门控标志，这些符号必须跨编译单元可见。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "net_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 队列元素 =====================
 * 数据直接放在结构体里（不是"指针 + malloc"）：队列按值拷贝，发送完自动
 * 释放，不用管内存生命周期，也更省 heap。收发共用一种元素。 */
typedef struct
{
    net_io_link_t kind;                /*!< 通道类型                          */
    uint16_t len;                      /*!< data 有效字节数（1 ~ MAX_FRAME）  */
    int fd;                            /*!< TCP_SERVER：对端 client fd        */
    uint32_t peer_ip;                  /*!< UDP：网络字节序                   */
    uint16_t peer_port;                /*!< UDP：主机字节序                   */
    uint8_t data[CONFIG_RX_MAX_FRAME]; /*!< 单帧内容                          */
} net_io_item_t;

/* 收发队列元素类型相同，保留两个别名让调用点语义清晰 */
typedef net_io_item_t net_io_tx_item_t;
typedef net_io_item_t net_io_rx_item_t;

/* ===================== 共用队列 / 门控 ===================== */
extern QueueHandle_t s_net_io_tx_queue;
extern QueueHandle_t s_net_io_rx_queue;
extern volatile bool s_net_io_ready;

/* ===================== TCP 长连接资源 ===================== */
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
extern int s_tcp_fd;                     /*!< 当前长连接 fd；-1 = 未连接 */
extern SemaphoreHandle_t s_tcp_fd_mutex; /*!< 保护 fd 读写 + send 原子性  */
extern volatile TickType_t s_last_traffic_tick;

/* 心跳运行时快照（pick 之后稳定，发送与 echo 防乒乓共用） */
extern bool s_hb_enabled;
extern uint32_t s_hb_interval_ms;
extern char s_hb_format[8];
extern uint8_t s_hb_payload[64];
extern uint8_t s_hb_payload_len;
#endif

/* ===================== TCP 服务端资源 ===================== */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
typedef struct
{
    int fd;            /*!< socket fd；-1 = 已被回收/未分配 */
    TaskHandle_t task; /*!< handler 任务句柄；NULL = 槽位空闲 */
} net_io_server_slot_t;

extern net_io_server_slot_t s_server_clients[CONFIG_TCP_SERVER_MAX_CLIENTS];
extern volatile bool s_server_running;
#endif

/* ===================== UDP 资源 ===================== */
#if CONFIG_SERVICE_UDP_ENABLE
extern int s_udp_fd;
extern volatile bool s_udp_running;
#endif

/* ===================== 内部 helper ===================== */

/** 构造并投递上行帧；true = 已入队 */
bool net_io_tx_post(net_io_link_t kind, int fd, uint32_t peer_ip, uint16_t peer_port,
                    const void* data, size_t len);

/** 构造并投递下行帧（各服务 recv 任务调用，业务不可见） */
bool net_io_rx_post(net_io_link_t kind, int fd, uint32_t peer_ip, uint16_t peer_port,
                    const void* data, size_t len);

/** 可中断延时：stop 置位后最坏 50ms 返回 */
void net_io_breakable_delay_ms(int ms, volatile bool* stop);

#if CONFIG_SERVICE_TCP_SERVER_ENABLE || CONFIG_SERVICE_UDP_ENABLE
/** 当前 WiFi 模式下的本机 IPv4（网络字节序）；取不到返回 0 */
uint32_t net_io_local_ip(void);
#endif

#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
void net_io_pick_target(void);
void net_io_pick_heartbeat(void);

/* service_manager 的 ops 表符号（定义在 net_io_tcp_client.c） */
void tcp_client_service_fn(void);
void tcp_client_request_stop(void);
bool tcp_client_is_running(void);
#endif

#if CONFIG_SERVICE_TCP_SERVER_ENABLE
void tcp_server_service_fn(void);
void tcp_server_request_stop(void);
bool tcp_server_is_running(void);
#endif

#if CONFIG_SERVICE_UDP_ENABLE
void udp_service_fn(void);
void udp_request_stop(void);
bool udp_is_running(void);
#endif

#ifdef __cplusplus
}
#endif
