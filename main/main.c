#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h" /* ip_event_got_ip_t / IPSTR，用于打印管理页地址 */
#include "esp_wifi.h"

#include "gpio.h"
#include "uart.h"
#include "webconfig.h"
#include "wifi.h"

/* ---- 硬件引脚 ---- */
#define RUNNING_LIGHT GPIO_NUM_21 /*!< 流水灯，每 100ms 翻转一次 */

static const char* TAG = "main";

static const gpio_init_cfg_t running_light_cfg = GPIO_INIT_CFG_DEFAULT();

static esp_err_t hardware_init(void);
static void running_light_task(void* arg);

/* 适配层（见文件末尾）：把 webconfig 的端口回调用 wifi 组件实现出来 */
static esp_err_t app_ap_start(const webconfig_ap_cfg_t* ap, void* ctx);
static esp_err_t app_sta_apply(const char* ssid, const char* password, void* ctx);
static void wifi_event_forward(void* arg, esp_event_base_t base, int32_t id, void* data);

/**
 * @brief 配网组件配置：热点参数用默认值，两个端口回调把 webconfig 接到 wifi 组件上
 *
 * @note webconfig 自己不认识 esp_wifi，开热点 / 切连接全靠这两个回调；
 *       连接结果则通过 wifi_event_forward() 反向通知给它。
 *
 * @note keep_httpd = true：配网结束后 HTTP 服务器继续运行，设备连上路由器后
 *       仍可用它在路由器网段的 IP 打开管理页（地址见串口日志），
 *       不用重新连热点就能改配置。
 */
static const webconfig_cfg_t webconfig_cfg = {
    .ap = WEBCONFIG_AP_CFG_DEFAULT(),
    .port = {
        .ap_start = &app_ap_start,
        .sta_apply = &app_sta_apply,
    },
    .keep_httpd = true,
    .ctx = NULL,
};

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
 * @brief 一次性把 LED 和 WiFi 都配好；WiFi 优先用配网组件存在 NVS 里的凭证，
 *        没配过网就开热点，等用户连上热点后通过 http://192.168.4.1/ 提交。
 *
 * @note 两条路都会把管理页面准备好，区别只是设备待在哪个网络里：
 *       - 有凭证：STA 直连路由器，页面挂在设备拿到的 IP 上（地址见串口日志）；
 *       - 没凭证：开配网热点，页面挂在 192.168.4.1 上。
 *
 * @note 最后把 WiFi 的连接结果转发给配网组件，它才能判断"新凭证是不是填错了"
 *       （连续失败会自动重开配网热点）。
 */
static esp_err_t hardware_init(void)
{
    ESP_ERROR_CHECK(gpio_config_init(RUNNING_LIGHT, &running_light_cfg));

    esp_err_t err;
    webconfig_sta_cfg_t saved;

    if (webconfig_load_sta_cfg(&saved))
    {
        ESP_LOGI(TAG, "使用已保存的 WiFi 配置连接: %s", saved.ssid);

        wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
        strlcpy(wifi_cfg.sta_ssid, saved.ssid, sizeof(wifi_cfg.sta_ssid));
        strlcpy(wifi_cfg.sta_password, saved.password, sizeof(wifi_cfg.sta_password));
        err = wifi_config_init(&wifi_cfg);

        /* 不开热点，但把管理页挂上去：连上路由器后用它拿到的 IP 访问 */
        if (err == ESP_OK)
        {
            err = webconfig_httpd_start(&webconfig_cfg);
        }
    }
    else
    {
        ESP_LOGI(TAG, "没有已保存的 WiFi 配置，启动配网热点");
        err = webconfig_start(&webconfig_cfg);
    }

    if (err != ESP_OK)
    {
        return err;
    }

    /* 注册事件转发（只影响"连不上自动回配网"和打印管理页地址，失败不拦着正常功能） */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_forward, NULL,
                                              NULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "注册 WiFi 事件转发失败，密码填错时不会自动重开配网热点: %s",
                 esp_err_to_name(err));
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_forward, NULL,
                                              NULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "注册 IP 事件转发失败，不会打印管理页地址: %s", esp_err_to_name(err));
    }

    return ESP_OK;
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

/* ===================== webconfig 适配层 =====================
 * webconfig 组件只认两个回调，不知道底下是哪套无线栈：
 *   - ap_start  : 开配网热点；
 *   - sta_apply : 关掉热点、用用户在网页上填的凭证去连接。
 * 这里用 wifi 组件把它们实现出来；反向的"连上/断开"则由 wifi_event_forward()
 * 转成 webconfig_notify_sta_*()，供配网组件做连接失败自愈。
 */

static esp_err_t app_ap_start(const webconfig_ap_cfg_t* ap, void* ctx)
{
    (void)ctx;

    /* wifi_config_init() 可重复调用：会复用已初始化的全局资源
     * （netif / 事件循环 / 驱动 / 回调），只重新下发模式与参数，
     * 所以"连接失败自动重开热点"时再调一次也是安全的。 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    wifi_cfg.mode = WIFI_MODE_AP;
    strlcpy(wifi_cfg.ap_ssid, ap->ssid, sizeof(wifi_cfg.ap_ssid));
    strlcpy(wifi_cfg.ap_password, ap->password, sizeof(wifi_cfg.ap_password));
    wifi_cfg.ap_channel = ap->channel;
    wifi_cfg.ap_max_conn = ap->max_conn;

    return wifi_config_init(&wifi_cfg);
}

static esp_err_t app_sta_apply(const char* ssid, const char* password, void* ctx)
{
    (void)ctx;

    /* 切回 STA 模式连接：wifi_update_config() 会断开当前 AP、用新凭证重连 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    strlcpy(wifi_cfg.sta_ssid, ssid, sizeof(wifi_cfg.sta_ssid));
    strlcpy(wifi_cfg.sta_password, password, sizeof(wifi_cfg.sta_password));

    return wifi_update_config(&wifi_cfg);
}

static void wifi_event_forward(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;

    if (base == WIFI_EVENT)
    {
        if (id == WIFI_EVENT_STA_CONNECTED)
        {
            webconfig_notify_sta_connected();
        }
        else if (id == WIFI_EVENT_STA_DISCONNECTED)
        {
            webconfig_notify_sta_disconnected();
        }
        return;
    }

    /* 拿到 IP 说明设备已经挂在路由器网段上：管理页就在这个地址，
     * 打出来省得去路由器后台翻（DHCP 分配的地址不固定） */
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "管理页面: http://" IPSTR "/", IP2STR(&e->ip_info.ip));
    }
}
