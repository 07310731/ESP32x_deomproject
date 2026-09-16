#include "wifi.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

static const char* TAG = "wifi";

/* 断线后两次重连尝试之间的冷却时间。太短会把射频时间片耗在扫描/认证上，
 * 太长则路由器重启后要等很久才恢复，2s 是 ESP-IDF 示例的量级。 */
#define WIFI_RECONNECT_DELAY_MS 2000

/* 重连任务：平时阻塞等通知，唤醒后只调 esp_wifi_connect() 和打日志，
 * 3072 字节栈足够；优先级 5 低于 WiFi 任务和默认事件循环任务，
 * 避免抢占射频和事件处理。 */
#define WIFI_RECONNECT_TASK_STACK 3072
#define WIFI_RECONNECT_TASK_PRIO  5

/* netif / 事件循环 / WiFi 驱动 / 事件回调都是全局资源，只允许初始化一次。
 * 这里用静态标志和句柄记住状态，让 wifi_config_init() 可以安全地重复调用：
 * 重复创建 netif 会泄漏，重复注册回调会让日志成倍输出。
 */
static bool s_netif_inited = false;
static bool s_handler_registered = false;
static bool s_want_sta = false;

/* STA 静态 IP 缓存：在 wifi_config_init() 里把字符串解析出来，缓存到这里；
 * WIFI_EVENT_STA_CONNECTED 事件触发时从这里读出来下发给驱动（必须等 netif up
 * 之后才能改地址）。重复调用 wifi_config_init() 会重新写入。
 */
static bool s_sta_use_static_ip = false;
static esp_netif_ip_info_t s_sta_ip_info;
static esp_netif_dns_info_t s_sta_dns_main;
static bool s_sta_dns_main_set = false;
static esp_netif_dns_info_t s_sta_dns_backup;
static bool s_sta_dns_backup_set = false;

static esp_netif_t* s_sta_netif = NULL;
static esp_netif_t* s_ap_netif = NULL;

/* ---- wifi_update_config()（运行时更新配置）需要的状态 ----
 * - s_wifi_started：wifi_config_init() 成功过，update 才可用
 * - s_cur_cfg：上次生效的配置快照，update 用它对比哪些字段变了
 * - s_sta_dhcp_was_stopped：静态 IP 停过 DHCP 客户端；之后切回 DHCP 时必须显式
 *   esp_netif_dhcpc_start()，因为被外部停过的 DHCP 在 netif 重新 up 时不会自动恢复
 * - s_ap_ip_custom：AP 正在用自定义网段；切回 ap_use_static_ip=false 时要恢复默认网段
 * - s_sta_reconnect_pending：update 改了 STA 凭证后置位，断开完成后在事件回调里重连
 */
static bool s_wifi_started = false;
static wifi_init_cfg_t s_cur_cfg;
static bool s_sta_dhcp_was_stopped = false;
static bool s_ap_ip_custom = false;
static bool s_sta_reconnect_pending = false;

/* ---- 断线自动重连需要的状态 ----
 * - s_reconnect_task：重连任务句柄，在 wifi_config_init() 里创建一次，之后一直存在
 *   （平时阻塞等通知，不占 CPU）；NULL 表示还没创建（或创建失败）
 * - s_reconnect_attempt：自上次连接成功以来的重连尝试次数，连上 AP 时清零；
 *   只用于日志，方便判断"是刚断"还是"已经重试了很多次"
 */
static TaskHandle_t s_reconnect_task = NULL;
static uint32_t s_reconnect_attempt = 0;

/* ===================== 内部辅助：校验 / 解析 / 下发 ===================== */

/**
 * @brief  wifi_init_cfg_t 的解析结果
 *
 * @note  "校验 + 字符串到二进制"一次性做完，写进这个纯输出结构；
 *        调用方（wifi_config_init / wifi_update_config）拿到结果后再决定
 *        何时提交全局缓存，这样校验失败时不会留下写了一半的全局状态。
 */
typedef struct
{
    bool want_sta;                       /*!< 模式包含 STA */
    bool want_ap;                        /*!< 模式包含 AP */
    size_t sta_ssid_len;                 /*!< STA ssid 长度（不含 '\0'） */
    size_t sta_pwd_len;                  /*!< STA 密码长度（不含 '\0'） */
    size_t ap_ssid_len;                  /*!< AP ssid 长度（不含 '\0'） */
    size_t ap_pwd_len;                   /*!< AP 密码长度（不含 '\0'） */
    bool sta_use_static_ip;              /*!< STA 启用静态 IP */
    esp_netif_ip_info_t sta_ip_info;     /*!< STA 静态 IP（未启用时为全 0） */
    bool sta_dns_main_set;               /*!< STA 主 DNS 已给出 */
    esp_netif_dns_info_t sta_dns_main;   /*!< STA 主 DNS（未给出时为全 0） */
    bool sta_dns_backup_set;             /*!< STA 备用 DNS 已给出 */
    esp_netif_dns_info_t sta_dns_backup; /*!< STA 备用 DNS（未给出时为全 0） */
    bool ap_ip_set;                      /*!< AP 启用静态 IP */
    esp_netif_ip_info_t ap_ip_info;      /*!< AP 静态 IP（未启用时为全 0） */
} wifi_parsed_cfg_t;

/**
 * @brief  校验并解析配置，不触碰任何全局状态
 *
 * @return ESP_OK              成功，解析结果写入 *p
 * @return ESP_ERR_INVALID_ARG 参数非法（已打印具体原因）
 */
static esp_err_t wifi_cfg_parse(const wifi_init_cfg_t* cfg, wifi_parsed_cfg_t* p)
{
    memset(p, 0, sizeof(*p));

    if (cfg == NULL)
    {
        ESP_LOGE(TAG, "cfg 不能为 NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->mode != WIFI_MODE_STA && cfg->mode != WIFI_MODE_AP && cfg->mode != WIFI_MODE_APSTA)
    {
        ESP_LOGE(TAG, "工作模式 %d 非法，只支持 STA / AP / APSTA", (int)cfg->mode);
        return ESP_ERR_INVALID_ARG;
    }

    p->want_sta = (cfg->mode == WIFI_MODE_STA) || (cfg->mode == WIFI_MODE_APSTA);
    p->want_ap = (cfg->mode == WIFI_MODE_AP) || (cfg->mode == WIFI_MODE_APSTA);

    if (p->want_sta)
    {
        /* strnlen 最多看到数组大小，等于数组大小说明没找到 '\0'，即超长 */
        p->sta_ssid_len = strnlen(cfg->sta_ssid, sizeof(cfg->sta_ssid));
        if (p->sta_ssid_len == 0 || p->sta_ssid_len >= sizeof(cfg->sta_ssid))
        {
            ESP_LOGE(TAG, "STA 的 ssid 为空或过长(最长 %u 字符)",
                     (unsigned)(sizeof(cfg->sta_ssid) - 1));
            return ESP_ERR_INVALID_ARG;
        }

        p->sta_pwd_len = strnlen(cfg->sta_password, sizeof(cfg->sta_password));
        if (p->sta_pwd_len >= sizeof(cfg->sta_password) ||
            (p->sta_pwd_len > 0 && p->sta_pwd_len < 8))
        {
            ESP_LOGE(TAG, "STA 密码长度非法(留空表示开放网络，否则 8~%u 位)",
                     (unsigned)(sizeof(cfg->sta_password) - 1));
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (p->want_ap)
    {
        p->ap_ssid_len = strnlen(cfg->ap_ssid, sizeof(cfg->ap_ssid));
        if (p->ap_ssid_len == 0 || p->ap_ssid_len >= sizeof(cfg->ap_ssid))
        {
            ESP_LOGE(TAG, "AP 的 ssid 为空或过长(最长 %u 字符)",
                     (unsigned)(sizeof(cfg->ap_ssid) - 1));
            return ESP_ERR_INVALID_ARG;
        }

        p->ap_pwd_len = strnlen(cfg->ap_password, sizeof(cfg->ap_password));
        if (p->ap_pwd_len >= sizeof(cfg->ap_password) || (p->ap_pwd_len > 0 && p->ap_pwd_len < 8))
        {
            ESP_LOGE(TAG, "AP 密码长度非法(留空表示开放热点，否则 8~%u 位)",
                     (unsigned)(sizeof(cfg->ap_password) - 1));
            return ESP_ERR_INVALID_ARG;
        }

        if (cfg->ap_channel < 1 || cfg->ap_channel > 13)
        {
            ESP_LOGE(TAG, "AP 信道 %u 非法(应为 1~13)", (unsigned)cfg->ap_channel);
            return ESP_ERR_INVALID_ARG;
        }

        if (cfg->ap_max_conn == 0)
        {
            ESP_LOGE(TAG, "AP 最大连接数不能为 0");
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* STA 静态 IP：解析成二进制，调用方择机提交 */
    if (p->want_sta && cfg->sta_use_static_ip)
    {
        if (cfg->sta_ip[0] == '\0' || cfg->sta_netmask[0] == '\0' || cfg->sta_gateway[0] == '\0')
        {
            ESP_LOGE(TAG, "STA 静态 IP 需要同时填写 ip / netmask / gateway");
            return ESP_ERR_INVALID_ARG;
        }

        esp_err_t perr = esp_netif_str_to_ip4(cfg->sta_ip, &p->sta_ip_info.ip);
        if (perr != ESP_OK)
        {
            ESP_LOGE(TAG, "STA IP '%s' 格式错误", cfg->sta_ip);
            return ESP_ERR_INVALID_ARG;
        }
        perr = esp_netif_str_to_ip4(cfg->sta_netmask, &p->sta_ip_info.netmask);
        if (perr != ESP_OK)
        {
            ESP_LOGE(TAG, "STA 子网掩码 '%s' 格式错误", cfg->sta_netmask);
            return ESP_ERR_INVALID_ARG;
        }
        perr = esp_netif_str_to_ip4(cfg->sta_gateway, &p->sta_ip_info.gw);
        if (perr != ESP_OK)
        {
            ESP_LOGE(TAG, "STA 网关 '%s' 格式错误", cfg->sta_gateway);
            return ESP_ERR_INVALID_ARG;
        }
        p->sta_use_static_ip = true;

        if (cfg->sta_dns_main[0] != '\0')
        {
            perr = esp_netif_str_to_ip4(cfg->sta_dns_main, &p->sta_dns_main.ip.u_addr.ip4);
            if (perr != ESP_OK)
            {
                ESP_LOGE(TAG, "STA 主 DNS '%s' 格式错误", cfg->sta_dns_main);
                return ESP_ERR_INVALID_ARG;
            }
            p->sta_dns_main.ip.type = ESP_IPADDR_TYPE_V4;
            p->sta_dns_main_set = true;
        }

        if (cfg->sta_dns_backup[0] != '\0')
        {
            perr = esp_netif_str_to_ip4(cfg->sta_dns_backup, &p->sta_dns_backup.ip.u_addr.ip4);
            if (perr != ESP_OK)
            {
                ESP_LOGE(TAG, "STA 备用 DNS '%s' 格式错误", cfg->sta_dns_backup);
                return ESP_ERR_INVALID_ARG;
            }
            p->sta_dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
            p->sta_dns_backup_set = true;
        }
    }

    /* AP 静态 IP：解析成二进制 */
    if (p->want_ap && cfg->ap_use_static_ip)
    {
        if (cfg->ap_ip[0] == '\0' || cfg->ap_netmask[0] == '\0' || cfg->ap_gateway[0] == '\0')
        {
            ESP_LOGE(TAG, "AP 静态 IP 需要同时填写 ip / netmask / gateway");
            return ESP_ERR_INVALID_ARG;
        }

        esp_err_t perr = esp_netif_str_to_ip4(cfg->ap_ip, &p->ap_ip_info.ip);
        if (perr != ESP_OK)
        {
            ESP_LOGE(TAG, "AP IP '%s' 格式错误", cfg->ap_ip);
            return ESP_ERR_INVALID_ARG;
        }
        perr = esp_netif_str_to_ip4(cfg->ap_netmask, &p->ap_ip_info.netmask);
        if (perr != ESP_OK)
        {
            ESP_LOGE(TAG, "AP 子网掩码 '%s' 格式错误", cfg->ap_netmask);
            return ESP_ERR_INVALID_ARG;
        }
        perr = esp_netif_str_to_ip4(cfg->ap_gateway, &p->ap_ip_info.gw);
        if (perr != ESP_OK)
        {
            ESP_LOGE(TAG, "AP 网关 '%s' 格式错误", cfg->ap_gateway);
            return ESP_ERR_INVALID_ARG;
        }
        p->ap_ip_set = true;
    }

    return ESP_OK;
}

/**
 * @brief  把 STA 静态 IP 下发给 netif（停 DHCP -> 改 IP -> 可选 DNS）
 *
 * @note   要求 netif 已 up（连接成功后调用），重复调用幂等。
 */
static esp_err_t wifi_apply_sta_static_ip(void)
{
    if (s_sta_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        ESP_LOGE(TAG, "停止 STA DHCP 客户端失败: %s", esp_err_to_name(err));
        return err;
    }
    s_sta_dhcp_was_stopped = true;

    err = esp_netif_set_ip_info(s_sta_netif, &s_sta_ip_info);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "设置 STA 静态 IP 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "STA 已切换为静态 IP: " IPSTR "/" IPSTR " gw " IPSTR, IP2STR(&s_sta_ip_info.ip),
             IP2STR(&s_sta_ip_info.netmask), IP2STR(&s_sta_ip_info.gw));

    if (s_sta_dns_main_set)
    {
        err = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &s_sta_dns_main);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "设置 STA 主 DNS 失败: %s", esp_err_to_name(err));
        }
    }
    if (s_sta_dns_backup_set)
    {
        err = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &s_sta_dns_backup);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "设置 STA 备用 DNS 失败: %s", esp_err_to_name(err));
        }
    }

    return ESP_OK;
}

/**
 * @brief  立即应用 STA 的 IP 策略（静态 IP 或恢复 DHCP），仅在已连接时调用
 *
 * @note   静态 IP 分支与 WIFI_EVENT_STA_CONNECTED 事件共用 wifi_apply_sta_static_ip()；
 *         切回 DHCP 分支必须显式重启 DHCP 客户端（见 s_sta_dhcp_was_stopped 的说明）。
 */
static void wifi_apply_sta_ip_now(void)
{
    if (s_sta_use_static_ip)
    {
        (void)wifi_apply_sta_static_ip();
    }
    else if (s_sta_dhcp_was_stopped && s_sta_netif != NULL)
    {
        esp_err_t err = esp_netif_dhcpc_start(s_sta_netif);
        if (err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
        {
            s_sta_dhcp_was_stopped = false;
            ESP_LOGI(TAG, "STA 已恢复 DHCP 获取 IP");
        }
        else
        {
            ESP_LOGE(TAG, "恢复 STA DHCP 客户端失败: %s", esp_err_to_name(err));
        }
    }
}

/**
 * @brief  把 AP 网段下发给 netif（停 DHCP 服务器 -> 改 IP -> 重启 DHCP 服务器）
 *
 * @note   AP netif up 之后即可调用，重复调用幂等。
 */
static esp_err_t wifi_apply_ap_static_ip(const esp_netif_ip_info_t* info)
{
    if (s_ap_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        ESP_LOGE(TAG, "停止 AP DHCP 服务器失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_set_ip_info(s_ap_netif, info);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "设置 AP IP 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "启动 AP DHCP 服务器失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "AP 已切换为静态 IP: " IPSTR "/" IPSTR " gw " IPSTR, IP2STR(&info->ip),
             IP2STR(&info->netmask), IP2STR(&info->gw));
    return ESP_OK;
}

/**
 * @brief  下发 STA 连接参数（ssid / 密码 / 认证门槛）给 WiFi 驱动
 */
static esp_err_t wifi_apply_sta_cfg(const wifi_init_cfg_t* cfg, const wifi_parsed_cfg_t* p)
{
    wifi_config_t sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));

    /* strnlen 得到的长度不含结尾 '\0'，剩余字节已被 memset 清 0，天然有结束符 */
    memcpy(sta_cfg.sta.ssid, cfg->sta_ssid, p->sta_ssid_len);
    memcpy(sta_cfg.sta.password, cfg->sta_password, p->sta_pwd_len);
    /* 开放网络把门槛降到 OPEN，否则至少要求 WPA2，避免连到已废弃的 WEP / WPA 网络 */
    sta_cfg.sta.threshold.authmode = (p->sta_pwd_len == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "下发 STA 配置失败: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief  下发 AP 热点参数（ssid / 密码 / 信道 / 最大连接数）给 WiFi 驱动
 */
static esp_err_t wifi_apply_ap_cfg(const wifi_init_cfg_t* cfg, const wifi_parsed_cfg_t* p)
{
    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));

    memcpy(ap_cfg.ap.ssid, cfg->ap_ssid, p->ap_ssid_len);
    /* ssid_len 显式给出长度，避免依赖字符串结束符 */
    ap_cfg.ap.ssid_len = (uint8_t)p->ap_ssid_len;
    memcpy(ap_cfg.ap.password, cfg->ap_password, p->ap_pwd_len);
    ap_cfg.ap.channel = cfg->ap_channel;
    ap_cfg.ap.max_connection = cfg->ap_max_conn;
    /* 没给密码就是开放热点，给了密码用 WPA2-PSK */
    ap_cfg.ap.authmode = (p->ap_pwd_len == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "下发 AP 配置失败: %s", esp_err_to_name(err));
    }
    return err;
}

/* ===================== 事件回调 ===================== */

/**
 * @brief  WiFi / IP 事件回调
 *
 * @note  运行在默认事件循环任务里，只做日志输出，不做耗时操作。
 *        STA_START 事件里顺手发起连接，因为 esp_wifi_start() 不会自动连接。
 */
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;

    if (base == WIFI_EVENT)
    {
        switch (id)
        {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA 已启动，开始连接 AP");
                if (s_want_sta)
                {
                    esp_err_t err = esp_wifi_connect();
                    /* ESP_ERR_WIFI_CONN 表示控制块状态冲突（例如已处于连接流程中），不是真失败 */
                    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
                    {
                        ESP_LOGW(TAG, "发起连接失败: %s", esp_err_to_name(err));
                    }
                }
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "已连上 AP，处理 IP 配置");
                s_reconnect_attempt = 0; /* 连上了，重连计数归零 */
                /* STA 静态 IP：必须等 netif up 之后才能改地址，所以挂在这里做 */
                if (s_sta_use_static_ip)
                {
                    (void)wifi_apply_sta_static_ip();
                }
                else if (s_sta_dhcp_was_stopped && s_sta_netif != NULL)
                {
                    /* 从静态 IP 切回 DHCP：DHCP 客户端是被外部停掉的，
                     * netif 重新 up 后 esp_netif 不会自动恢复，必须显式重启 */
                    esp_err_t err = esp_netif_dhcpc_start(s_sta_netif);
                    if (err == ESP_OK)
                    {
                        s_sta_dhcp_was_stopped = false;
                    }
                    else
                    {
                        ESP_LOGE(TAG, "恢复 STA DHCP 客户端失败: %s", esp_err_to_name(err));
                    }
                }
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
            {
                const wifi_event_sta_disconnected_t* e = (const wifi_event_sta_disconnected_t*)data;
                ESP_LOGW(TAG, "与 AP 断开连接，原因码 %d", e->reason);

                if (s_sta_reconnect_pending)
                {
                    /* wifi_update_config() 改了凭证后触发的断开：立即用新凭证重连，
                     * 不走重连任务的冷却延时 */
                    s_sta_reconnect_pending = false;
                    esp_err_t err = esp_wifi_connect();
                    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
                    {
                        ESP_LOGW(TAG, "用新配置重连失败: %s", esp_err_to_name(err));
                        /* 立即重连没发出去，且不会再产生事件：交给重连任务兜底 */
                        if (s_want_sta && s_reconnect_task != NULL)
                        {
                            xTaskNotifyGive(s_reconnect_task);
                        }
                    }
                }
                else if (s_want_sta && s_reconnect_task != NULL)
                {
                    /* 普通断线（路由器重启、信号丢失、认证失败等）：交给重连任务按策略重试。
                     * 事件回调不能延时，所以这里只发通知，不在这里直接重连 */
                    xTaskNotifyGive(s_reconnect_task);
                }
                break;
            }

            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP 已启动");
                break;

            case WIFI_EVENT_AP_STACONNECTED:
            {
                const wifi_event_ap_staconnected_t* e = (const wifi_event_ap_staconnected_t*)data;
                ESP_LOGI(TAG, "客户端接入，AID=%d", e->aid);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED:
            {
                const wifi_event_ap_stadisconnected_t* e =
                    (const wifi_event_ap_stadisconnected_t*)data;
                ESP_LOGI(TAG, "客户端断开，AID=%d，原因码 %d", e->aid, e->reason);
                break;
            }

            default:
                break;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "获取到 IP 地址: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

/* ===================== 断线自动重连 ===================== */

/**
 * @brief  断线自动重连任务
 *
 * @note  WIFI_EVENT_STA_DISCONNECTED 的事件回调跑在默认事件循环任务里，不能延时，
 *        所以事件回调只发一个任务通知，真正的"等一会儿再连"在这里做。
 *        发起连接的结果仍由事件回调给出：连上 -> WIFI_EVENT_STA_CONNECTED；
 *        连不上 -> 再次 WIFI_EVENT_STA_DISCONNECTED -> 再次唤醒本任务，
 *        因此天然形成"断开 -> 等待 -> 重连 -> 再断开"的重试循环。
 *
 * @note  重试间隔固定为 WIFI_RECONNECT_DELAY_MS（不做指数退避），保证路由器重启后
 *        能较快恢复；只有工作模式不再包含 STA（例如切成 AP-only）时才停止重试。
 */
static void wifi_reconnect_task(void* arg)
{
    (void)arg;

    for (;;)
    {
        /* 阻塞等待断开事件通知；没有通知时完全不占 CPU */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!s_want_sta)
        {
            continue;
        }

        while (s_want_sta)
        {
            vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_DELAY_MS));
            if (!s_want_sta)
            {
                break;
            }

            ++s_reconnect_attempt;
            esp_err_t err = esp_wifi_connect();

            if (err == ESP_OK || err == ESP_ERR_WIFI_CONN)
            {
                /* 请求已发出（ESP_ERR_WIFI_CONN 表示已在连接流程中，不算失败）：
                 * 结果由事件回调给出，这里结束本轮，回到通知等待 */
                ESP_LOGI(TAG, "第 %u 次重连已发起", (unsigned)s_reconnect_attempt);
                break;
            }

            /* 同步失败（例如驱动状态不允许连接）：不会产生事件，自己等一会儿再试 */
            ESP_LOGW(TAG, "第 %u 次重连失败: %s", (unsigned)s_reconnect_attempt,
                     esp_err_to_name(err));
        }
    }
}

/**
 * @brief  初始化 NVS
 *
 * @return ESP_OK 成功
 * @return 其他   失败
 *
 * @note  WiFi 驱动要往 NVS 存校准数据和配置，所以必须先初始化 NVS。
 */
static esp_err_t wifi_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    /* 分区没有空闲页或 NVS 版本不匹配：擦掉重建，这是 ESP-IDF 的标准做法 */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS 需要先擦除再初始化: %s", esp_err_to_name(err));

        err = nvs_flash_erase();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "擦除 NVS 失败: %s", esp_err_to_name(err));
            return err;
        }

        err = nvs_flash_init();
    }

    return err;
}

/**
 * @brief  WiFi 配置初始化
 *
 * @param  cfg 配置信息
 * @return ESP_OK 成功
 * @return 其他   失败
 *
 * @note  NVS -> netif -> 事件循环 -> 默认 netif -> WiFi 驱动 -> 事件回调 -> 模式 -> 参数 -> 启动 ->
 * AP IP
 */
esp_err_t wifi_config_init(const wifi_init_cfg_t* cfg)
{
    /* ---- 参数校验与解析：全部放在申请任何资源之前，失败时不会留下半初始化状态 ---- */
    wifi_parsed_cfg_t p;
    esp_err_t err = wifi_cfg_parse(cfg, &p);
    if (err != ESP_OK)
    {
        /* wifi_cfg_parse() 已打印具体错误 */
        return err;
    }

    /* ---- 提交 STA 静态 IP 缓存（连接成功时由事件回调下发） ---- */
    s_sta_use_static_ip = p.sta_use_static_ip;
    s_sta_ip_info = p.sta_ip_info;
    s_sta_dns_main = p.sta_dns_main;
    s_sta_dns_main_set = p.sta_dns_main_set;
    s_sta_dns_backup = p.sta_dns_backup;
    s_sta_dns_backup_set = p.sta_dns_backup_set;

    /* ---- 第一步：NVS ---- */
    err = wifi_nvs_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 第二步：netif 与默认事件循环，全局只做一次 ---- */
    if (!s_netif_inited)
    {
        err = esp_netif_init();
        /* 已经初始化过会返回 ESP_ERR_INVALID_STATE，不算错误 */
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "创建默认事件循环失败: %s", esp_err_to_name(err));
            return err;
        }

        s_netif_inited = true;
    }

    /* ---- 第三步：按模式创建默认 netif，已创建过则复用 ---- */
    if (p.want_sta && s_sta_netif == NULL)
    {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL)
        {
            ESP_LOGE(TAG, "创建默认 STA netif 失败");
            return ESP_FAIL;
        }
    }

    if (p.want_ap && s_ap_netif == NULL)
    {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL)
        {
            ESP_LOGE(TAG, "创建默认 AP netif 失败");
            return ESP_FAIL;
        }
    }

    /* ---- 第四步：初始化 WiFi 驱动 ---- */
    wifi_init_config_t drv_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&drv_cfg);
    if (err == ESP_ERR_INVALID_STATE)
    {
        /* 本函数被重复调用时会走到这里，不算错误，继续更新参数即可 */
        ESP_LOGW(TAG, "WiFi 驱动已初始化，跳过 esp_wifi_init");
        err = ESP_OK;
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 第五步：注册事件回调，只注册一次 ---- */
    if (!s_handler_registered)
    {
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
                                                  NULL, NULL);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "注册 WiFi 事件回调失败: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler,
                                                  NULL, NULL);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "注册 IP 事件回调失败: %s", esp_err_to_name(err));
            return err;
        }

        s_handler_registered = true;
    }

    /* ---- 第六步：设置工作模式 ---- */
    err = esp_wifi_set_mode(cfg->mode);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "设置 WiFi 模式失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 第七步：下发 STA 参数 ---- */
    if (p.want_sta)
    {
        err = wifi_apply_sta_cfg(cfg, &p);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    /* ---- 第八步：下发 AP 参数 ---- */
    if (p.want_ap)
    {
        err = wifi_apply_ap_cfg(cfg, &p);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    /* STA_START 事件里要据此决定是否自动连接，必须在 esp_wifi_start() 之前设好 */
    s_want_sta = p.want_sta;

    /* ---- 创建断线重连任务（必须在 esp_wifi_start() 之前，只创建一次）----
     * 启动阶段的第一次连接就可能失败，那时会产生 DISCONNECTED 事件，
     * 任务要先建好才能接手，否则设备会一直停在未连接状态。
     * 创建失败只告警不返回错误：没有重连也比整个 WiFi 起不来强。 */
    if (s_reconnect_task == NULL)
    {
        if (xTaskCreate(wifi_reconnect_task, "wifi_reconn", WIFI_RECONNECT_TASK_STACK, NULL,
                        WIFI_RECONNECT_TASK_PRIO, &s_reconnect_task) != pdPASS)
        {
            s_reconnect_task = NULL;
            ESP_LOGW(TAG, "创建重连任务失败，断线后不会自动重连");
        }
    }

    /* ---- 第九步：启动 WiFi ---- */
    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 第十步：AP 静态 IP（AP netif 在 esp_wifi_start() 后即 up，可直接下发） ---- */
    if (p.ap_ip_set)
    {
        err = wifi_apply_ap_static_ip(&p.ap_ip_info);
        if (err != ESP_OK)
        {
            return err;
        }
        s_ap_ip_custom = true;
    }
    else
    {
        s_ap_ip_custom = false;
    }

    /* STA 静态 IP 在 WIFI_EVENT_STA_CONNECTED 事件回调里下发 */

    s_wifi_started = true;
    s_cur_cfg = *cfg;

    ESP_LOGI(TAG, "WiFi 初始化完成: mode=%d", (int)cfg->mode);
    return ESP_OK;
}

/**
 * @brief  运行时更新 WiFi 配置（供 Web / 蓝牙等配置渠道调用）
 *
 * @note  与 wifi_config_init() 的区别：本函数假设 WiFi 已经在运行，只做
 *        "参数变更 -> 让新参数生效" 这一步，不会重新走 NVS / netif / 驱动初始化。
 *
 * @note  新 STA 凭证只在重新连接时生效：改了 ssid / 密码会自动断开并重连；
 *        新静态 IP 已连接时立即生效；AP 网段变更立即生效。
 */
esp_err_t wifi_update_config(const wifi_init_cfg_t* cfg)
{
    /* ---- 前置检查：必须先成功跑过 wifi_config_init() ---- */
    if (!s_wifi_started)
    {
        ESP_LOGE(TAG, "WiFi 尚未初始化，请先调用 wifi_config_init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- 参数校验与解析：失败时直接返回，当前运行状态不受影响 ---- */
    wifi_parsed_cfg_t p;
    esp_err_t err = wifi_cfg_parse(cfg, &p);
    if (err != ESP_OK)
    {
        return err;
    }

    /* ---- 旧状态快照：判断哪些地方需要动作 ---- */
    const bool was_sta_static = s_sta_use_static_ip;
    const bool was_dns_main = s_sta_dns_main_set;
    const bool was_dns_backup = s_sta_dns_backup_set;
    const esp_netif_ip_info_t old_sta_ip = s_sta_ip_info;
    const esp_netif_dns_info_t old_dns_main = s_sta_dns_main;
    const esp_netif_dns_info_t old_dns_backup = s_sta_dns_backup;

    wifi_ap_record_t ap_rec;
    const bool sta_connected = (esp_wifi_sta_get_ap_info(&ap_rec) == ESP_OK);

    /* ---- 提交 STA 静态 IP 缓存（立即生效部分见下面，未连接部分由事件回调生效） ---- */
    s_sta_use_static_ip = p.sta_use_static_ip;
    s_sta_ip_info = p.sta_ip_info;
    s_sta_dns_main = p.sta_dns_main;
    s_sta_dns_main_set = p.sta_dns_main_set;
    s_sta_dns_backup = p.sta_dns_backup;
    s_sta_dns_backup_set = p.sta_dns_backup_set;

    /* ---- 模式变化：补创建缺失的 netif（只增不减，切走的 netif 保留待复用） ---- */
    if (p.want_sta && s_sta_netif == NULL)
    {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL)
        {
            ESP_LOGE(TAG, "创建默认 STA netif 失败");
            return ESP_FAIL;
        }
    }

    if (p.want_ap && s_ap_netif == NULL)
    {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL)
        {
            ESP_LOGE(TAG, "创建默认 AP netif 失败");
            return ESP_FAIL;
        }
    }

    /* ---- 设置工作模式（没变时是幂等操作） ---- */
    err = esp_wifi_set_mode(cfg->mode);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "设置 WiFi 模式失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 下发 STA / AP 参数（已连接的 STA 保持连接，新凭证等重连时生效） ---- */
    if (p.want_sta)
    {
        err = wifi_apply_sta_cfg(cfg, &p);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (p.want_ap)
    {
        err = wifi_apply_ap_cfg(cfg, &p);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    /* STA_START / DISCONNECTED 事件里要据此决定是否连接 */
    s_want_sta = p.want_sta;

    /* ---- 对比新旧配置，让变更生效 ---- */
    const bool sta_cred_changed =
        p.want_sta && (strcmp(s_cur_cfg.sta_ssid, cfg->sta_ssid) != 0 ||
                       strcmp(s_cur_cfg.sta_password, cfg->sta_password) != 0);

    bool sta_ip_changed = (was_sta_static != p.sta_use_static_ip);
    if (!sta_ip_changed && p.sta_use_static_ip)
    {
        /* 静态 -> 静态：逐项比较是否有变化 */
        sta_ip_changed =
            memcmp(&old_sta_ip, &p.sta_ip_info, sizeof(old_sta_ip)) != 0 ||
            was_dns_main != p.sta_dns_main_set || was_dns_backup != p.sta_dns_backup_set ||
            (was_dns_main && memcmp(&old_dns_main, &p.sta_dns_main, sizeof(old_dns_main)) != 0) ||
            (was_dns_backup &&
             memcmp(&old_dns_backup, &p.sta_dns_backup, sizeof(old_dns_backup)) != 0);
    }

    if (p.want_sta)
    {
        if (sta_cred_changed && sta_connected)
        {
            /* 新凭证只在重新连接时生效：主动断开再连。
             * esp_wifi_disconnect() 是异步的，立即 esp_wifi_connect() 可能因
             * 断开流程未完成而失败（ESP_ERR_WIFI_CONN），由 DISCONNECTED 事件兜底重连 */
            s_sta_reconnect_pending = true;
            (void)esp_wifi_disconnect();
            (void)esp_wifi_connect();
        }
        else if (sta_connected && sta_ip_changed)
        {
            /* 凭证没变、只有 IP 配置变了：已连接就立即生效 */
            wifi_apply_sta_ip_now();
        }
        else if (!sta_connected)
        {
            /* 本来就没连上（例如刚从 AP-only 切过来，或上次连接失败）：直接发起连接。
             * 这是 STA_START 事件自动连接的兜底，重复调用无害 */
            esp_err_t cerr = esp_wifi_connect();
            if (cerr != ESP_OK && cerr != ESP_ERR_WIFI_CONN)
            {
                ESP_LOGW(TAG, "发起连接失败: %s", esp_err_to_name(cerr));
            }
        }
    }

    /* ---- AP 网段变更：立即生效；从自定义网段切回默认时恢复 192.168.4.1/24 ---- */
    if (p.want_ap)
    {
        if (p.ap_ip_set)
        {
            err = wifi_apply_ap_static_ip(&p.ap_ip_info);
            if (err != ESP_OK)
            {
                return err;
            }
            s_ap_ip_custom = true;
        }
        else if (s_ap_ip_custom)
        {
            /* ap_use_static_ip 从 true 改回 false：恢复 esp_netif 的默认网段 */
            esp_netif_ip_info_t def = { 0 };
            if (esp_netif_str_to_ip4("192.168.4.1", &def.ip) == ESP_OK &&
                esp_netif_str_to_ip4("255.255.255.0", &def.netmask) == ESP_OK &&
                esp_netif_str_to_ip4("192.168.4.1", &def.gw) == ESP_OK)
            {
                err = wifi_apply_ap_static_ip(&def);
                if (err != ESP_OK)
                {
                    return err;
                }
                s_ap_ip_custom = false;
            }
            else
            {
                ESP_LOGE(TAG, "恢复 AP 默认网段失败（内置常量解析异常）");
            }
        }
    }
    /* AP 未激活时不处理网段，netif 里残留的 IP 等切回 AP 时按上述规则重新下发 */

    s_cur_cfg = *cfg;

    ESP_LOGI(TAG, "WiFi 配置已更新: mode=%d", (int)cfg->mode);
    return ESP_OK;
}
