/* service_manager.h —— "当前跑哪一个网络服务"的统一管理
 *
 * 设计要点：
 *   - 任意时刻只允许 TCP_CLIENT / TCP_SERVER / UDP 三者之一在跑，
 *     由本组件独占做启/停，其它代码不能越过它直接 xTaskCreate；
 *   - "选哪个"由三层决定（按优先级）：
 *       1) 运行时通过 service_manager_request_switch() 切的（web 等）
 *       2) 启动期 NVS 里记住的"上次用户选的"
 *       3) 都拿不到时回退到 menuconfig 的 CONFIG_SERVICE_DEFAULT_MODE
 *   - 切换流程：先信号让旧 worker 退出 → 等它真的 vTaskDelete 掉（带超时）→
 *     再启动新 worker，旧 fd / 任务都释放干净才接新的，杜绝端口占用冲突。
 *
 * 【和 tcp_app_send / tcp_app_on_recv 的关系】
 *   三个 service 共用同一对 g_tx_queue / g_rx_queue + 一对 consumer 任务
 *   （tx_consumer_task / rx_consumer_task，常驻、不随 service 切换销毁）：
 *     - tcp_app_send()        → TX_KIND_TCP_CLIENT → tx_consumer_task 走 send(g_fd)
 *     - srv_app_send(fd)      → TX_KIND_TCP_SERVER → tx_consumer_task 走 send(fd)
 *     - udp_app_send(ip,port) → TX_KIND_UDP        → tx_consumer_task 走 sendto(g_udp_fd)
 *   下行：各 service 的 recv task 投 RX_KIND_* 到 g_rx_queue，rx_consumer_task
 *   统一调 tcp_app_on_recv()。
 *   因为 service_manager 让任意时刻只有一个 service 在跑，资源不会冲突，
 *   这是这个设计能成立的关键前提。
 */
#ifndef SERVICE_MANAGER_H
#define SERVICE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    SERVICE_MODE_NONE = 0,      /*!< 没有任何服务在跑（首次启动 / 全 disabled） */
    SERVICE_MODE_TCP_CLIENT = 1,/*!< 主动连接远程服务器的长连接客户端        */
    SERVICE_MODE_TCP_SERVER = 2,/*!< 监听端口、接受远程连接的服务端          */
    SERVICE_MODE_UDP       = 3, /*!< 单 socket 收发两用的 echo              */
} service_mode_t;

/**
 * @brief 把模式枚举翻译成"tcp_client / tcp_server / udp / none"等日志用的字符串
 */
const char* service_mode_to_str(service_mode_t m);

/**
 * @brief 查当前激活的模式（无服务运行时返回 SERVICE_MODE_NONE）
 */
service_mode_t service_manager_get_active(void);

/**
 * @brief 请求切换到指定模式
 *
 * @param m 目标模式；传 SERVICE_MODE_NONE 表示停掉当前服务后停在那里
 *
 * @note 本调用是异步的：仅设置 pending + 写 NVS，真正的启/停由
 *       manager 任务完成（停旧 + 起新可能要花到 CONFIG_SERVICE_SWITCH_TIMEOUT_MS）。
 *       调用后立即读 service_manager_get_active() 仍可能是旧值，过几秒再查才是新值。
 *
 * @note 若目标模式在 menuconfig 里被禁用，会打 warn 日志并忽略。
 *
 * @return
 *     - ESP_OK              请求已受理（pending 已设）
 *     - ESP_ERR_INVALID_ARG m 不是合法枚举值
 *     - 其它                NVS 写失败
 */
esp_err_t service_manager_request_switch(service_mode_t m);

/* ===================== service_workers（main.c 实现）=====================
 * service_manager.c 的 ops 表引用这三个"对"：worker + request_stop +
 * is_running。之所以声明在头里而不是 service_manager.c 内部 extern，是为了
 * 让 main.c 里实现这两个函数时不写 static，又能保持声明集中在一处。 */
void tcp_client_service_fn(void);
void tcp_client_request_stop(void);
bool tcp_client_is_running(void);

void tcp_server_service_fn(void);
void tcp_server_request_stop(void);
bool tcp_server_is_running(void);

void udp_service_fn(void);
void udp_request_stop(void);
bool udp_is_running(void);

/**
 * @brief 初始化：注册 /api/service-mode 的 HTTP handler，读 NVS 决定初始 pending
 *
 * @note 必须在 webconfig_httpd_start() / webconfig_start() 之后调用，
 *       才能把 handler 注册到已经在跑的 HTTP 服务器上。
 *
 * @return ESP_OK 或 NVS 初始化错误码
 */
esp_err_t service_manager_init(void);

/**
 * @brief 启动 manager 任务：从这里开始按 NVS / menuconfig 跑第一个 service
 */
void service_manager_start(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_MANAGER_H */