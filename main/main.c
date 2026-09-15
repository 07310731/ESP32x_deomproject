#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"

#include "gpio.h"
#include "uart.h"

/* ---- 硬件引脚 ---- */
#define RUNNING_LIGHT GPIO_NUM_21 /*!< 流水灯，每 100ms 翻转一次 */
#define COMMAND_LED GPIO_NUM_5    /*!< 受 UART 命令控制的灯 */
#define UART_TX_PIN GPIO_NUM_17
#define UART_RX_PIN GPIO_NUM_16
#define UART_PORT_NUM UART_NUM_1
#define UART_BAUD 115200

static const char* TAG = "main";

static const gpio_init_cfg_t running_light_cfg = GPIO_INIT_CFG_DEFAULT();
static const gpio_init_cfg_t command_led_cfg = GPIO_INIT_CFG_DEFAULT();

/* UART 事件队列句柄，交给 uart_driver_install 内部创建并填到这里 */
static QueueHandle_t s_uart_queue;

static void running_light_task(void* arg);
static void uart_echo_task(void* arg);
static esp_err_t hardware_init(void);

void app_main(void)
{
    if (hardware_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "硬件初始化失败，停在 app_main");
        return;
    }

    ESP_LOGI(TAG, "UART echo ready at %d 8N1 on TX=%d RX=%d. Send '1'/'0' to control GPIO %d.",
             UART_BAUD, (int)UART_TX_PIN, (int)UART_RX_PIN, (int)COMMAND_LED);

    xTaskCreate(running_light_task, "running_light_task", 2048, NULL, 10, NULL);
    xTaskCreate(uart_echo_task, "uart_echo_task", 4096, NULL, 9, NULL);
}

/**
 * @brief 一次性把两颗 LED 和 UART1 都配好；UART1 自带事件队列以驱动 echo 任务。
 */
static esp_err_t hardware_init(void)
{
    ESP_ERROR_CHECK(gpio_config_init(RUNNING_LIGHT, &running_light_cfg));
    ESP_ERROR_CHECK(gpio_config_init(COMMAND_LED, &command_led_cfg));

    uart_init_cfg_t cfg = UART_INIT_CFG_DEFAULT();
    cfg.port = UART_PORT_NUM;
    cfg.baud_rate = UART_BAUD;
    cfg.tx_pin = UART_TX_PIN;
    cfg.rx_pin = UART_RX_PIN;
    cfg.queue_size = 10; /*!< 建一个事件队列，把数据到达事件通知 echo 任务 */
    cfg.uart_queue = &s_uart_queue;

    return uart_config_init(&cfg);
}

static void running_light_task(void* arg)
{
    (void)arg;

    while (1)
    {
        gpio_write(RUNNING_LIGHT, 1);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        gpio_write(RUNNING_LIGHT, 0);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

static void uart_echo_task(void* arg)
{
    (void)arg;

    uart_event_t event;
    uint8_t buf[128];

    for (;;)
    {
        /* 等 UART 事件：阻塞直到驱动通知有数据到来 */
        if (xQueueReceive(s_uart_queue, &event, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (event.type != UART_DATA)
        {
            continue;
        }

        /* 拉一次数据：最多等 100ms 把缓冲区里的数据读出来 */
        size_t got = 0;
        if (uart_recv(UART_PORT_NUM, buf, sizeof(buf), 100, &got) != ESP_OK || got == 0)
        {
            continue;
        }

        /* 命令解析：首字节是 '1' 点亮 COMMAND_LED，'0' 熄灭，其它任意字节也回发 */
        if (buf[0] == '1')
        {
            gpio_write(COMMAND_LED, 1);
        }
        else if (buf[0] == '0')
        {
            gpio_write(COMMAND_LED, 0);
        }

        /* 回发同样的内容给对端 */
        size_t sent = 0;
        uart_send(UART_PORT_NUM, buf, got, &sent);
    }
}
