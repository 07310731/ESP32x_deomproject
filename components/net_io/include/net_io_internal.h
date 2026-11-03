/**
 * @file net_io_internal.h
 * @brief net_io 组件内部共享：队列元素、共用状态、内部 helper
 *
 * 【为什么这些要放在头文件里】
 * 三个服务的实现分散在 net_io_tcp_client.c / net_io_tcp_server.c / net_io_udp.c，
 * 但它们共用同一对队列、同一个门控标志、同一个"按配置挑目标地址"逻辑，
 * 这些符号必须跨编译单元可见。
 *
 * 【不是对外 API】业务代码只 include net_io.h。本头文件里的类型带 `_item_t`
 * 后缀、都是队列内部结构，将来改字段不应该影响业务。
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
 * 数据直接放在结构体里（而不是"指针 + malloc"）：队列按值拷贝，发送完自动
 * 释放，不用管内存生命周期，也更省 heap。 */

/** 上行队列元素：一帧待发送数据 */
typedef struct
{
    net_io_link_t kind;                /*!< 目的地类型                                */
    uint16_t len;                      /*!< data 有效字节数（1 ~ CONFIG_RX_MAX_FRAME）*/
    int fd;                            /*!< TCP_SERVER：目标 client fd                */
    uint32_t dst_ip;                   /*!< UDP：网络字节序                           */
    uint16_t dst_port;                 /*!< UDP：主机字节序                           */
    uint8_t data[CONFIG_RX_MAX_FRAME]; /*!< 单帧内容                                  */
} net_io_tx_item_t;

/** 下行队列元素：一帧收到的数据 */
typedef struct
{
    net_io_link_t kind;                /*!< 来源类型                                  */
    uint16_t len;                      /*!< data 有效字节数                           */
    int fd;                            /*!< TCP_SERVER：来源 client fd                */
    uint32_t src_ip;                   /*!< UDP：网络字节序                           */
    uint16_t src_port;                 /*!< UDP：主机字节序                           */
    uint8_t data[CONFIG_RX_MAX_FRAME]; /*!< 单段内容                                  */
} net_io_rx_item_t;

/* ===================== 共用队列 ===================== */
extern QueueHandle_t s_net_io_tx_queue;
extern QueueHandle_t s_net_io_rx_queue;

/* ===================== 门控 ===================== */
extern volatile bool s_net_io_ready;

/* ===================== TCP 长连接资源 ===================== */
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
extern int s_tcp_fd;                        /*!< 当前长连接 fd；-1 = 未连接 */
extern SemaphoreHandle_t s_tcp_fd_mutex;    /*!< 保护 fd 读写 + send 原子性  */
extern volatile TickType_t s_last_traffic_tick;

/* 心跳运行时快照（pick 之后稳定，供发送与 echo 防乒乓共用） */
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
    int fd;            /*!< 该 handler 持有的 socket fd；-1 = 已被回收/未分配 */
    TaskHandle_t task; /*!< client handler 任务句柄；NULL = 该槽位无人使用   */
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
/**
 * @brief 构造并投递上行帧（三个上行入口共用）
 *
 * @return true = 已入队；false = 队列未建 / 参数非法 / 队列满
 */
bool net_io_tx_post(net_io_link_t kind, int fd, uint32_t dst_ip, uint16_t dst_port,
                    const void* data, size_t len);

/**
 * @brief 构造并投递下行帧（各服务 recv 任务调用，业务不可见）
 */
bool net_io_rx_post(net_io_link_t kind, int fd, uint32_t src_ip, uint16_t src_port,
                    const void* data, size_t len);

/**
 * @brief 可中断延时：停服标志一到，最坏 50ms 返回
 */
void net_io_breakable_delay_ms(int ms, volatile bool* stop);

#if CONFIG_SERVICE_TCP_SERVER_ENABLE || CONFIG_SERVICE_UDP_ENABLE
/**
 * @brief 取当前 WiFi 模式下的本机 IPv4（网络字节序）；取不到返回 0
 */
uint32_t net_io_local_ip(void);
#endif

#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
/**
 * @brief 按 NVS → menuconfig 的顺序挑目标地址
 */
void net_io_pick_target(void);

/**
 * @brief 按 NVS → menuconfig 的顺序挑心跳配置
 */
void net_io_pick_heartbeat(void);

/**
 * @brief service_manager 需要的三个符号（定义在 net_io_tcp_client.c）
 */
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
