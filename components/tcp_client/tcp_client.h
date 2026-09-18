#ifndef _TCP_CLIENT_H_
#define _TCP_CLIENT_H_

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief TCP 客户端初始化参数
     *
     * @note 字段都有对应的默认值，推荐先用 TCP_CLIENT_CFG_DEFAULT() 整体初始化，
     *       再覆盖需要的字段。被省略的字段（如 host / port）会被编译器补 0，
     *       而 0 是非法值，open 时会直接失败。
     */
    typedef struct
    {
        char host[128]; /*!< 服务器主机名或点分十进制 IPv4，例如 "api.example.com" 或
                           "192.168.1.100"，最长 127 字符，不可为空 */
        uint16_t port;  /*!< 服务器端口，1 ~ 65535，不可为 0 */

        int connect_timeout_ms; /*!< connect() 总超时毫秒数，<= 0 用默认 5000ms */
        int send_timeout_ms;    /*!< send() 单次阻塞等待超时（发送缓冲区满时）毫秒数，
                                   <= 0 用默认 5000ms */
        int recv_timeout_ms;    /*!< recv() 单次阻塞等待超时毫秒数，<= 0 用默认 5000ms */

        bool keepalive;            /*!< true: 启用 TCP keepalive（SO_KEEPALIVE + TCP_KEEP*）；
                                      false: 关闭（默认） */
        int keepalive_idle_s;      /*!< keepalive 空闲秒数（连接空闲多久后发探针），
                                      <= 0 用默认 30 */
        int keepalive_interval_s;  /*!< keepalive 探针重发间隔（秒），<= 0 用默认 5 */
        int keepalive_count;        /*!< keepalive 探针失败次数（超过则断开），
                                      <= 0 用默认 3 */

        bool nodelay; /*!< true: 关闭 Nagle 算法（TCP_NODELAY），适合小包、低延迟
                         控制指令；false: 开启 Nagle（默认，吞吐更友好） */
    } tcp_client_cfg_t;

/**
 * @brief 默认配置：connect / send / recv 超时 5s、关闭 keepalive、开启 Nagle
 *
 * @note host / port 都为 0，使用前必须自行填写。
 *
 * 【怎么用】典型写法是先拿默认配置打底，再覆盖需要的字段：
 *
 *     tcp_client_cfg_t cfg = TCP_CLIENT_CFG_DEFAULT();
 *     strcpy(cfg.host, "192.168.1.100");
 *     cfg.port = 8080;
 *     cfg.nodelay = true;
 *
 * 【这个宏展开后长什么样】大致是：
 *
 *     ( (tcp_client_cfg_t){ .host = {0}, .port = 0, .connect_timeout_ms = 5000, ... } )
 *
 *   - "(tcp_client_cfg_t){ ... }" 是 C99 的"复合字面量"：
 *     现场临时造一个该结构体的值，可以直接赋给同类型的变量。
 *   - ".字段 = 值" 是"指定初始化器"：只写想设的字段，顺序随意；
 *     没写的字段自动补 0（bool 就变成 false，指针就变成 NULL）。
 *   - 最外层那对括号是为了把整个表达式包住，防止宏展开时与上下文的
 *     运算符优先级打架（写宏的标准防御手法）。
 */
#define TCP_CLIENT_CFG_DEFAULT()                                                                    \
    ((tcp_client_cfg_t){                                                                            \
        .host = { 0 },                                                                              \
        .port = 0,                                                                                  \
        .connect_timeout_ms = 5000,                                                                 \
        .send_timeout_ms = 5000,                                                                    \
        .recv_timeout_ms = 5000,                                                                    \
        .keepalive = false,                                                                         \
        .keepalive_idle_s = 30,                                                                     \
        .keepalive_interval_s = 5,                                                                  \
        .keepalive_count = 3,                                                                       \
        .nodelay = false,                                                                           \
    })

    /**
     * @brief  打开一个 TCP 客户端连接：解析主机名、建 socket、连服务器、配置超时 / keepalive
     *
     * @param  cfg    连接参数，不可为 NULL
     * @param  out_fd 输出参数：成功时写入 socket fd（>= 0），失败时写入 -1，不可为 NULL
     *
     * @note 内部按顺序完成以下步骤，任意一步失败都会关闭已建资源并返回该步的错误码：
     *       1. 参数校验（不分配任何资源，失败时 *out_fd 被置 -1）；
     *       2. getaddrinfo() 解析 host（目前仅支持 IPv4 / 主机名 → IPv4）；
     *       3. socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) 建 TCP socket；
     *       4. setsockopt() 配置 SO_SNDTIMEO / SO_RCVTIMEO（来自 cfg）；
     *       5. cfg.keepalive 为 true 时配置 SO_KEEPALIVE + TCP_KEEPIDLE / TCP_KEEPINTVL /
     *          TCP_KEEPCNT；
     *       6. cfg.nodelay 为 true 时配置 TCP_NODELAY；
     *       7. connect()，lwIP 的 connect 内部读 SO_SNDTIMEO 作为总超时。
     *
     * @note connect() / send() / recv() 的三个 timeout 都是"< 0 用 5s 默认"的语义；
     *       实际下到 socket 的毫秒数由 cfg 决定，运行期可调，调用 tcp_client_set_timeouts()。
     *
     * @note 返回的 fd 是真实的 lwIP socket 描述符，可直接用于 select() / poll() / fcntl() /
     *       getsockopt() 等标准 POSIX API；调用方负责不要并发操作同一个 fd。
     *
     * @note 下列参数异常会在建立任何资源之前直接返回 ESP_ERR_INVALID_ARG，*out_fd 被置 -1：
     *       - cfg 或 out_fd 为 NULL
     *       - host 为空
     *       - port 为 0
     *
     * @return
     *     - ESP_OK               成功
     *     - ESP_ERR_INVALID_ARG  参数非法
     *     - ESP_ERR_NOT_FOUND    getaddrinfo() 解析失败（主机不存在 / 网络未就绪 / DNS 失败）
     *     - ESP_ERR_TIMEOUT      connect() 超时
     *     - ESP_FAIL             socket() / setsockopt() / connect() 失败（errno 见日志）
     */
    esp_err_t tcp_client_open(const tcp_client_cfg_t* cfg, int* out_fd);

    /**
     * @brief  通过已打开的 socket 发送数据
     *
     * @param  fd      tcp_client_open() 返回的 socket fd（>= 0）
     * @param  data    待发送数据指针，不可为 NULL
     * @param  len     待发送字节数，> 0
     * @param  written 输出参数：实际写入 socket 的字节数（可能 < len，因超时或对端关闭中断），
     *                 可为 NULL
     *
     * @note 行为依赖 cfg.send_timeout_ms（或 tcp_client_set_timeouts() 后续修改）：
     *       - 超时时间内尽可能多写，返回实际写入字节数（len 很大时可能只写一部分）；
     *       - 超时返回 ESP_ERR_TIMEOUT，*written 写入已发送字节数；
     *       - 对端关闭返回 ESP_FAIL，*written 写入已发送字节数。
     *
     * @return
     *     - ESP_OK               调用成功（written == len）
     *     - ESP_ERR_INVALID_ARG  fd < 0、data 为 NULL 或 len 为 0
     *     - ESP_ERR_TIMEOUT      超时（部分写入，written < len）
     *     - ESP_FAIL             send() 失败或对端关闭连接
     */
    esp_err_t tcp_client_send(int fd, const void* data, size_t len, size_t* written);

    /**
     * @brief  通过已打开的 socket 接收数据
     *
     * @param  fd      tcp_client_open() 返回的 socket fd（>= 0）
     * @param  buf     接收缓冲区，不可为 NULL
     * @param  max_len 最多读取的字节数，> 0
     * @param  read    输出参数：实际读取到的字节数（0 表示超时），可为 NULL
     *
     * @note 行为依赖 cfg.recv_timeout_ms（或 tcp_client_set_timeouts() 后续修改）：
     *       - 超时时间内有数据就读，读到多少算多少（可能 < max_len，不保证填满）；
     *       - 超时返回 ESP_OK，*read 为 0；
     *       - 对端关闭连接（recv 返回 0）返回 ESP_ERR_INVALID_STATE，
     *         此时调用方应 tcp_client_close() 释放 fd。
     *
     * @return
     *     - ESP_OK                  调用成功（读到 0 字节视为超时，*read = 0）
     *     - ESP_ERR_INVALID_ARG     fd < 0、buf 为 NULL 或 max_len 为 0
     *     - ESP_ERR_INVALID_STATE   对端关闭连接（应 close）
     *     - ESP_FAIL                recv() 失败（errno 见日志）
     */
    esp_err_t tcp_client_recv(int fd, void* buf, size_t max_len, size_t* read);

    /**
     * @brief  运行时修改 socket 的 send / recv 阻塞超时（不影响 keepalive / TCP_NODELAY 等
     *         其它 socket 选项）
     *
     * @param  fd              socket fd（>= 0）
     * @param  send_timeout_ms send() 单次阻塞超时毫秒数，
     *                         -1 表示无限等待（timeval 字段全 0），
     *                          0 表示恢复默认（5s），
     *                         > 0 表示该毫秒数
     * @param  recv_timeout_ms recv() 单次阻塞超时毫秒数，语义同 send_timeout_ms
     *
     * @return
     *     - ESP_OK               成功
     *     - ESP_ERR_INVALID_ARG  fd < 0
     *     - ESP_FAIL             setsockopt() 失败（errno 见日志）
     */
    esp_err_t tcp_client_set_timeouts(int fd, int send_timeout_ms, int recv_timeout_ms);

    /**
     * @brief  关闭 socket（对 fd < 0 安全：直接返回 ESP_OK）
     *
     * @param  fd socket fd（>= 0）
     *
     * @return
     *     - ESP_OK  总是成功
     */
    esp_err_t tcp_client_close(int fd);

#ifdef __cplusplus
}
#endif

#endif