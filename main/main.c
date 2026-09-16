#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"

#include "gpio.h"
#include "uart.h"
#include "wifi.h"

/* ---- 硬件引脚 ---- */
#define RUNNING_LIGHT GPIO_NUM_21 /*!< 流水灯，每 100ms 翻转一次 */

static const char* TAG = "main";

static const gpio_init_cfg_t running_light_cfg = GPIO_INIT_CFG_DEFAULT();

static wifi_init_cfg_t wifi_cfg = { .mode = WIFI_MODE_STA,
                                    .sta_ssid = "章发财",
                                    .sta_password = "z1449642571" };

static esp_err_t hardware_init(void);
static void running_light_task(void* arg);

void app_main(void)
{
    if (hardware_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "硬件初始化失败，停在 app_main");
        return;
    }

    xTaskCreate(running_light_task, "running_light_task", 2048, NULL, 10, NULL);
}

/**
 * @brief 一次性把两颗 LED 和 UART1 都配好；UART1 自带事件队列以驱动 echo 任务。
 */
static esp_err_t hardware_init(void)
{
    ESP_ERROR_CHECK(gpio_config_init(RUNNING_LIGHT, &running_light_cfg));

    return wifi_config_init(&wifi_cfg);
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
