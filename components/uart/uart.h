#ifndef _UART_H_
#define _UART_H_

#include <stddef.h>

#include "driver/uart.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief UART 初始化参数
     *
     * @note 字段都有对应的默认值，推荐先用 UART_INIT_CFG_DEFAULT() 整体初始化，
     *       再覆盖需要的字段。否则被省略的字段会被编译器补 0，
     *       而波特率 0、数据位 0 都是非法值，初始会失败。
     */
    typedef struct
    {
        uart_port_t port;                /*!< 串口端口号：UART_NUM_0 ~ UART_NUM_2 */
        int baud_rate;                   /*!< 波特率，例如 115200 */
        gpio_num_t tx_pin;               /*!< TX 引脚号，UART_PIN_NO_CHANGE 表示不改动原有引脚 */
        gpio_num_t rx_pin;               /*!< RX 引脚号，UART_PIN_NO_CHANGE 表示不改动原有引脚 */
        uart_word_length_t data_bits;    /*!< 数据位长度：UART_DATA_5_BITS ~ UART_DATA_8_BITS */
        uart_stop_bits_t stop_bits;      /*!< 停止位：UART_STOP_BITS_1 / 1_5 / 2 */
        uart_parity_t parity;            /*!< 校验方式：UART_PARITY_DISABLE / EVEN / ODD */
        uart_hw_flowcontrol_t flow_ctrl; /*!< 硬件流控：UART_HW_FLOWCTRL_DISABLE 表示关闭 */
        int rx_buffer_size; /*!< 接收缓冲区大小(字节)，必须大于 UART_FIFO_LEN(128)，一般 >= 256 */
        int tx_buffer_size; /*!< 发送缓冲区大小(字节)，0 表示不用发送缓冲、直接走 FIFO */
        int queue_size;     /*!< UART 事件队列长度，0 表示不建立事件队列 */
        QueueHandle_t* uart_queue; /*!< 输出参数：事件队列句柄，不需要则传 NULL */
    } uart_init_cfg_t;

/**
 * @brief 默认配置：UART1、115200、8N1、无流控、收发缓冲 1024/0、无事件队列
 */
#define UART_INIT_CFG_DEFAULT()                                                                    \
    ((uart_init_cfg_t){                                                                            \
        .port = UART_NUM_1,                                                                        \
        .baud_rate = 115200,                                                                       \
        .tx_pin = UART_PIN_NO_CHANGE,                                                              \
        .rx_pin = UART_PIN_NO_CHANGE,                                                              \
        .data_bits = UART_DATA_8_BITS,                                                             \
        .stop_bits = UART_STOP_BITS_1,                                                             \
        .parity = UART_PARITY_DISABLE,                                                             \
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,                                                     \
        .rx_buffer_size = 1024,                                                                    \
        .tx_buffer_size = 0,                                                                       \
        .queue_size = 0,                                                                           \
        .uart_queue = NULL,                                                                        \
    })

    /**
     * @brief 初始化一个串口：安装驱动、配置通信参数、绑定收发引脚
     *
     * @param cfg 配置参数，不可为 NULL
     *
     * @note 内部按顺序完成三步：
     *       1. uart_driver_install()：按 rx_buffer_size / tx_buffer_size / queue_size 安装驱动；
     *       2. uart_param_config()：设置波特率、数据位、停止位、校验、流控；
     *       3. uart_set_pin()：绑定 tx_pin / rx_pin（RTS/CTS 保持原有配置）。
     *
     * @note 若该端口驱动已安装，会先 uart_driver_delete() 卸载再重新安装，
     *       保证以最新参数生效（会打印一条警告）。
     *
     * @note queue_size > 0 时必须同时给 uart_queue 一个有效指针，否则安装失败。
     *
     * @note 下列参数异常会在驱动安装前直接返回 ESP_ERR_INVALID_ARG，驱动不会被安装：
     *       - cfg 为 NULL
     *       - 端口号不在 UART_NUM_0 ~ UART_NUM_MAX-1 范围内
     *       - 波特率 <= 0
     *       - queue_size > 0 但 uart_queue 为 NULL
     *       - tx_pin 不是 UART_PIN_NO_CHANGE 且不支持输出
     *         （经典 ESP32 上 GPIO_NUM_34~39 是只输入脚，传给 TX 会直接报错）
     *       - rx_pin 不是 UART_PIN_NO_CHANGE 且不是合法 GPIO 号
     *       传 UART_PIN_NO_CHANGE 表示该脚位保留原驱动配置、不做改动。
     *
     * @return
     *     - ESP_OK               成功
     *     - ESP_ERR_INVALID_ARG  cfg 为 NULL、端口号非法、波特率非法
     *     - 其它                 uart_driver_install / uart_param_config / uart_set_pin
     * 返回的错误码
     */
    esp_err_t uart_config_init(const uart_init_cfg_t* cfg);

    /**
     * @brief  通过指定串口发送数据
     *
     * @param  port        串口端口号
     * @param  data        待发送数据指针，不可为 NULL
     * @param  len         待发送字节数
     * @param  written     输出参数：实际写入驱动的字节数，可为 NULL
     *
     * @return
     *     - ESP_OK               调用成功
     *     - ESP_ERR_INVALID_ARG  port 非法、data 为 NULL 或 len 为 0
     *     - ESP_FAIL             uart_write_bytes 返回负数
     *
     * @note  ESP-IDF v6.1 的 uart_write_bytes 已经没有超时参数：
     *        行为依赖初始化时的 cfg.tx_buffer_size：
     *        - > 0：数据拷进 TX ring buffer 立即返回；
     *        - = 0：阻塞到数据进入 TX FIFO 才返回。
     *        想等"全部数据从物理引脚发完"需再调 ESP-IDF 的 uart_wait_tx_done()。
     */
    esp_err_t uart_send(uart_port_t port, const void* data, size_t len, size_t* written);

    /**
     * @brief  通过指定串口接收数据
     *
     * @param  port        串口端口号
     * @param  buf         接收缓冲区，不可为 NULL
     * @param  max_len     最多读取的字节数
     * @param  timeout_ms  最长等待时间(毫秒)，< 0 表示无限等待
     * @param  read_bytes  输出参数：实际读取到的字节数(可能为 0，表示超时)，可为 NULL
     *
     * @return
     *     - ESP_OK               调用成功(读到 0 字节也视为成功，由 read_bytes 是否为 0 区分超时)
     *     - ESP_ERR_INVALID_ARG  port 非法、buf 为 NULL 或 max_len 为 0
     *     - ESP_FAIL             uart_read_bytes 返回负数
     */
    esp_err_t uart_recv(uart_port_t port, void* buf, size_t max_len, int timeout_ms,
                        size_t* read_bytes);

#ifdef __cplusplus
}
#endif

#endif
