#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"

#include "gpio.h"

#define running_light (GPIO_NUM_21)

const gpio_init_cfg_t running_light_cfg = GPIO_INIT_CFG_DEFAULT();

void running_light_task(void* arg);

void app_main(void)
{
    xTaskCreate(running_light_task, "running_light_task", 2048, NULL, 10, NULL);
}

void running_light_task(void* arg)
{
    gpio_config_init(running_light, &running_light_cfg);

    while (1)
    {
        gpio_write(running_light, 1);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        gpio_write(running_light, 0);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}
