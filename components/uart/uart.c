#include "uart.h"
#include <stddef.h>
#include "esp_log.h"

static const char* TAG = "uart";

/**
 * @brief  UART 配置初始化
 *
 * @param  cfg 配置信息
 * @return ESP_OK 成功
 * @return 其他 失败
 *
 * @note  安装驱动 -> 配置通信参数 -> 绑定收发引脚
 */
esp_err_t uart_config_init(const uart_init_cfg_t* cfg)
{
    if (cfg == NULL)
    {
        ESP_LOGE(TAG, "cfg 不能为 NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->port < UART_NUM_0 || cfg->port >= UART_NUM_MAX)
    {
        ESP_LOGE(TAG, "串口端口号 %d 非法", (int)cfg->port);
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->baud_rate <= 0)
    {
        ESP_LOGE(TAG, "串口 %d 波特率 %d 非法", (int)cfg->port, cfg->baud_rate);
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->queue_size > 0 && cfg->uart_queue == NULL)
    {
        ESP_LOGE(TAG, "串口 %d 配了事件队列长度 %d 但没给 uart_queue 指针", (int)cfg->port,
                 cfg->queue_size);
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- 引脚合法性校验 ----
     * TX 引脚必须支持输出，且为该芯片的合法 GPIO 号。
     * 经典 ESP32 上 GPIO_NUM_34~39 是只输入脚，用它们做 TX 会被拦掉。
     * 越界或非法编号也直接在这里拦掉，驱动不会被安装。
     * 传 UART_PIN_NO_CHANGE 表示保留原配置，跳过校验。
     */
    if (cfg->tx_pin != UART_PIN_NO_CHANGE && !GPIO_IS_VALID_OUTPUT_GPIO(cfg->tx_pin))
    {
        ESP_LOGE(TAG, "串口 %d TX 引脚 %d 不可用(芯片上不支持输出或编号越界)",
                 (int)cfg->port, (int)cfg->tx_pin);
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->rx_pin != UART_PIN_NO_CHANGE && !GPIO_IS_VALID_GPIO(cfg->rx_pin))
    {
        ESP_LOGE(TAG, "串口 %d RX 引脚 %d 无效", (int)cfg->port, (int)cfg->rx_pin);
        return ESP_ERR_INVALID_ARG;
    }

    /* 端口驱动已安装时先卸载，否则 uart_driver_install 会直接失败 */
    if (uart_is_driver_installed(cfg->port))
    {
        ESP_LOGW(TAG, "串口 %d 驱动已安装，先卸载旧驱动再按新参数重装", (int)cfg->port);
        uart_driver_delete(cfg->port);
    }

    /* ---- 第一步：安装驱动，指定收发缓冲区大小和事件队列 ---- */
    esp_err_t err = uart_driver_install(cfg->port, cfg->rx_buffer_size, cfg->tx_buffer_size,
                                        cfg->queue_size, cfg->uart_queue, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "串口 %d 驱动安装失败: %s", (int)cfg->port, esp_err_to_name(err));
        return err;
    }

    /* ---- 第二步：配置通信参数 ---- */
    const uart_config_t uart_cfg = {
        .baud_rate = cfg->baud_rate,
        .data_bits = cfg->data_bits,
        .parity = cfg->parity,
        .stop_bits = cfg->stop_bits,
        .flow_ctrl = cfg->flow_ctrl,
        .source_clk = UART_SCLK_DEFAULT,
    };

    err = uart_param_config(cfg->port, &uart_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "串口 %d 参数配置失败: %s", (int)cfg->port, esp_err_to_name(err));
        uart_driver_delete(cfg->port);
        return err;
    }

    /* ---- 第三步：绑定引脚，RTS/CTS 保持不变 ---- */
    err = uart_set_pin(cfg->port, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "串口 %d 引脚配置失败: %s", (int)cfg->port, esp_err_to_name(err));
        uart_driver_delete(cfg->port);
        return err;
    }

    ESP_LOGD(TAG, "串口 %d 初始化完成: baud=%d tx=%d rx=%d", (int)cfg->port, cfg->baud_rate,
             (int)cfg->tx_pin, (int)cfg->rx_pin);
    return ESP_OK;
}

/**
 * @brief  通过指定串口发送数据
 *
 * @param  port        串口端口号
 * @param  data        待发送数据指针
 * @param  len         待发送字节数
 * @param  written     出参：实际写入驱动的字节数
 * @return ESP_OK      成功
 * @return 其他        失败
 *
 * @note  ESP-IDF v6.1 的 uart_write_bytes 已不再支持超时参数：
 *        - cfg.tx_buffer_size > 0：把数据拷进 TX ring buffer 后立即返回；
 *        - cfg.tx_buffer_size == 0：阻塞到数据被送进 TX FIFO 才返回。
 *        等待"数据从引脚发完"可再调 ESP-IDF 的 uart_wait_tx_done()。
 */
esp_err_t uart_send(uart_port_t port, const void *data, size_t len, size_t *written)
{
    if (port < UART_NUM_0 || port >= UART_NUM_MAX)
    {
        ESP_LOGE(TAG, "uart_send: 端口号 %d 非法", (int)port);
        return ESP_ERR_INVALID_ARG;
    }

    if (data == NULL || len == 0)
    {
        ESP_LOGE(TAG, "uart_send: data 为 NULL 或 len 为 0");
        return ESP_ERR_INVALID_ARG;
    }

    int n = uart_write_bytes(port, data, len);
    if (n < 0)
    {
        ESP_LOGE(TAG, "串口 %d 发送失败", (int)port);
        return ESP_FAIL;
    }

    if (written != NULL)
    {
        *written = (size_t)n;
    }

    return ESP_OK;
}

/**
 * @brief  通过指定串口接收数据
 *
 * @param  port        串口端口号
 * @param  buf         接收缓冲区
 * @param  max_len     期望读取的最大字节数
 * @param  timeout_ms  最长等待毫秒数，< 0 表示无限等待
 * @param  read_bytes  出参：实际读取到的字节数
 * @return ESP_OK      成功
 * @return 其他        失败
 */
esp_err_t uart_recv(uart_port_t port, void *buf, size_t max_len, int timeout_ms,
                     size_t *read_bytes)
{
    if (port < UART_NUM_0 || port >= UART_NUM_MAX)
    {
        ESP_LOGE(TAG, "uart_recv: 端口号 %d 非法", (int)port);
        return ESP_ERR_INVALID_ARG;
    }

    if (buf == NULL || max_len == 0)
    {
        ESP_LOGE(TAG, "uart_recv: buf 为 NULL 或 max_len 为 0");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t ticks = (timeout_ms < 0) ? (uint32_t)portMAX_DELAY
                                 : (uint32_t)pdMS_TO_TICKS(timeout_ms);
    int n = uart_read_bytes(port, buf, (uint32_t)max_len, ticks);
    if (n < 0)
    {
        ESP_LOGE(TAG, "串口 %d 接收失败", (int)port);
        return ESP_FAIL;
    }

    if (read_bytes != NULL)
    {
        *read_bytes = (size_t)n;
    }

    if (n == 0)
    {
        ESP_LOGW(TAG, "串口 %d 接收超时，没读到任何字节", (int)port);
    }

    return ESP_OK;
}
