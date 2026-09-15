#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"

#include "gpio.h"

/* 按键：多数 ESP32 开发板的 BOOT 键就接在 GPIO0 上，按下接地 */
#define KEY_PIN GPIO_NUM_0
/* LED */
#define LED_PIN GPIO_NUM_2

static const char *TAG = "app";

/* 中断给任务递信号用的信号量 */
static SemaphoreHandle_t s_key_sem;

/* ---------- 中断处理函数：跑在中断上下文，只负责递信号 ---------- */
static void IRAM_ATTR key_isr(void *arg)
{
    (void)arg;

    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_key_sem, &higher_priority_task_woken);

    /* 如果这次 Give 把某个更高优先级的任务唤醒了，中断返回时立刻切过去，
     * 不再等下一个 tick。少了这句也能跑，只是响应会慢一点。 */
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/* ---------- 任务：真正的按键处理放在这里 ---------- */
static void key_task(void *arg)
{
    (void)arg;

    int press_count = 0;
    int led_level = 0;

    ESP_LOGI(TAG, "key_task 已启动，等着按键信号量");

    while (1)
    {
        /* 阻塞等信号量，没按键时任务在这里睡觉，不占 CPU，也不进中断 */
        if (xSemaphoreTake(s_key_sem, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        /* 机械按键按下时会抖 5~20ms，一次按下可能触发好几次中断。
         * 这里等 20ms 再回头确认电平，还是低才当成一次有效按下，
         * 抖动产生的那几次会在这里被丢掉。 */
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gpio_get_level(KEY_PIN) != 0)
        {
            continue; /* 已经松开或者就是抖动，忽略 */
        }

        press_count++;
        led_level = !led_level;
        led_set(LED_PIN, led_level);

        ESP_LOGI(TAG, "第 %d 次按下，LED %s", press_count, led_level ? "点亮" : "熄灭");
    }
}

void app_main(void)
{
    /* ---- 1. LED 引脚：推挽输出 ---- */
    gpio_init_cfg_t led = GPIO_INIT_CFG_DEFAULT();
    led.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config_init(LED_PIN, &led));
    led_set(LED_PIN, 0);

    /* ---- 2. 先建信号量，再注册中断 ----
     * 顺序不能反：中断随时可能来，来了就要用这个信号量，
     * 要是中断先触发而信号量还是 NULL，xSemaphoreGiveFromISR 会直接崩。 */
    s_key_sem = xSemaphoreCreateBinary();
    if (s_key_sem == NULL)
    {
        ESP_LOGE(TAG, "创建信号量失败，内存不够");
        return;
    }

    /* ---- 3. 按键引脚：输入 + 上拉 + 下降沿中断 + 直接挂上处理函数 ---- */
    gpio_init_cfg_t key = GPIO_INIT_CFG_DEFAULT();
    key.mode = GPIO_MODE_INPUT;      /* 纯输入，不驱动输出 */
    key.pull = GPIO_PULLUP_ONLY;     /* 内部上拉，松开时是高电平 */
    key.intr_type = GPIO_INTR_NEGEDGE; /* 按下瞬间的下降沿触发 */
    key.isr = key_isr;               /* 中断处理函数 */
    key.isr_arg = NULL;              /* 不需要额外参数 */

    /* 上一次 init 时 ISR 服务已经装好了，这里会自动跳过安装、只挂函数 */
    ESP_ERROR_CHECK(gpio_config_init(KEY_PIN, &key));

    /* ---- 4. 建处理任务 ---- */
    xTaskCreate(key_task, "key_task", 3072, NULL, 10, NULL);

    ESP_LOGI(TAG, "就绪：按 GPIO%d 上的按键，切换 GPIO%d 的 LED", (int)KEY_PIN, (int)LED_PIN);
}
