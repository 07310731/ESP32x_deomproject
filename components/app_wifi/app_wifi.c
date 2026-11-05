/**
 * @file app_wifi.c
 * @brief WiFi / 配网初始化与状态适配：业务层只需调 app_wifi_start()
 *
 * webconfig 不认识 esp_wifi，它通过 webconfig_cfg_t 的回调（开热点 / 切 STA /
 * 应用整份设备配置）把动作交回这里。本文件负责：
 *   - 启动选路：NVS 有配置 → 直连；menuconfig 有凭证 → 连它；否则起配网热点；
 *   - 把 WiFi 事件翻译成"通讯层该不该开工"，经 net_io_set_net_ready() 通知；
 *   - 网页保存配置后：切 WiFi 参数、切服务协议、重载 net_io 配置。
 *
 * 【事件必须先注册再启动 WiFi】配网路径下 webconfig_start() 内部走到
 * esp_wifi_start()，WIFI_EVENT_AP_START 在它返回前就触发了。注册晚了会错过
 * AP_START，门控永远 false，TCP 任务卡死在等待循环里 —— 这是"AP 模式没有
 * 连接动作"的根因。事件循环提前建对 wifi_config_init() 是安全的（它有
 * ESP_ERR_INVALID_STATE 守卫）。
 */
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "app_wifi.h"
#include "net_io.h"
#include "service_manager.h"
#include "webconfig.h"
#include "wifi.h"

static const char* TAG = "app_wifi";

/* 适配层（定义在后）：webconfig 端口回调 + 事件转发 */
static esp_err_t app_ap_start(const webconfig_ap_cfg_t* ap, void* ctx);
static esp_err_t app_sta_apply(const char* ssid, const char* password, void* ctx);
static esp_err_t app_dev_apply(const webconfig_dev_cfg_t* cfg, void* ctx);
static void wifi_event_forward(void* arg, esp_event_base_t base, int32_t id, void* data);
static void app_fill_wifi_cfg_from_menuconfig(wifi_init_cfg_t* wifi_cfg);
static void app_fill_wifi_cfg_from_dev(const webconfig_dev_cfg_t* dev, wifi_init_cfg_t* wifi_cfg);
static void log_link_diag(void);

#if CONFIG_SERVICE_ANY_ENABLE
static bool wifi_is_ap_only(void);
static void wifi_refresh_net_ready(const char* reason);
#endif

/* 当前热点是不是"配网热点"。
 *
 * 不能用 webconfig_is_running() 现查：那个标志在 ap_start() 之后才置 true，
 * 而 AP_START 在 esp_wifi_start() 返回前就抛出了，会读到"还没进入配网"。
 * 所以由本层自己记，写入点都在起热点之前：
 *   app_ap_start() → true；app_sta_apply() / app_dev_apply() → false。
 * 配网热点里的设备只开网页，业务 AP 有客户端就该开工 —— 两者都表现为
 * "AP 里有设备"，只能靠来源区分。 */
static volatile bool s_config_ap = false;

static const webconfig_cfg_t webconfig_cfg = {
    .ap = WEBCONFIG_AP_CFG_DEFAULT(),
    .port = {
        .ap_start   = &app_ap_start,
        .sta_apply  = &app_sta_apply,
        .dev_apply  = &app_dev_apply,
    },
    .keep_httpd = true, /* 配网结束后管理页继续可用（IP 见串口日志） */
    .ctx = NULL,
};

esp_err_t app_wifi_start(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    /* 事件转发失败只影响"连不上自动回配网"和 TCP 开门，不拦正常功能 */
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_forward,
                                            NULL, NULL) != ESP_OK)
    {
        ESP_LOGW(TAG, "注册 WiFi 事件转发失败，密码填错时不会自动重开配网热点");
    }
    if (esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_forward,
                                            NULL, NULL) != ESP_OK)
    {
        ESP_LOGW(TAG, "注册 IP 事件转发失败，不会打印管理页地址");
    }

    webconfig_dev_cfg_t dev;
    const bool has_dev = webconfig_load_dev_cfg(&dev);

    /* 优先级：NVS 完整配置 > menuconfig STA 凭证 > 配网热点。
     * 无论如何先跑一遍 menuconfig helper，保证 AP 信道 / 静态 IP 网段这类
     * NVS 里没有的字段不丢。 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    app_fill_wifi_cfg_from_menuconfig(&wifi_cfg);

    bool need_config = true;
    if (has_dev)
    {
        ESP_LOGI(TAG, "使用 NVS 里的完整设备配置（模式=%s, 设备=%s）", dev.wifi_mode,
                 dev.device_name);
        app_fill_wifi_cfg_from_dev(&dev, &wifi_cfg);
    }
    else if (wifi_cfg.mode == WIFI_MODE_AP)
    {
        /* AP-only 且没配置：用户已在 menuconfig 决定只跑热点，跳过配网 UI */
        ESP_LOGI(TAG, "模式 = AP-only，按 menuconfig 直接开机为热点");
    }
#if defined(CONFIG_WIFI_STA_SSID) && defined(CONFIG_WIFI_STA_PASSWORD)
    else if (wifi_cfg.mode == WIFI_MODE_STA && CONFIG_WIFI_STA_SSID[0] != '\0')
    {
        ESP_LOGI(TAG, "用 menuconfig 里的 STA 凭证连接: %s", CONFIG_WIFI_STA_SSID);
        strlcpy(wifi_cfg.sta_ssid, CONFIG_WIFI_STA_SSID, sizeof(wifi_cfg.sta_ssid));
        strlcpy(wifi_cfg.sta_password, CONFIG_WIFI_STA_PASSWORD, sizeof(wifi_cfg.sta_password));
    }
#endif
    else
    {
        ESP_LOGI(TAG, "没有可用的设备配置，启动配网热点");
        err = webconfig_start(&webconfig_cfg);
        need_config = false;
    }

    if (need_config)
    {
        err = wifi_config_init(&wifi_cfg);
        if (err == ESP_OK)
        {
            err = webconfig_httpd_start(&webconfig_cfg);
        }
        /* STA 模式才需要"连续失败自动回配网"的观察；AP 本身就是热点 */
        if (err == ESP_OK && wifi_cfg.mode == WIFI_MODE_STA)
        {
            webconfig_watch_sta_connect();
        }
    }

    if (err != ESP_OK)
    {
        return err;
    }

    /* 关 modem sleep 换取低延迟。配网路径下驱动还在 AP-only，可能有失败；
     * STA 连上后 wifi_event_forward() 会幂等地再下发一次并打印实际值。 */
    if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK)
    {
        ESP_LOGW(TAG, "初始化时关闭 WiFi 省电失败（STA 连上后会再试一次）");
    }

    return ESP_OK;
}

/* menuconfig 兜底：模式 + AP 参数 + STA 的 IP 策略（SSID/密码由调用方按策略填） */
static void app_fill_wifi_cfg_from_menuconfig(wifi_init_cfg_t* wifi_cfg)
{
#if defined(CONFIG_WIFI_MODE_AP)
    wifi_cfg->mode = WIFI_MODE_AP;
#else
    wifi_cfg->mode = WIFI_MODE_STA;
#endif

#if defined(CONFIG_WIFI_HAS_AP)
    strlcpy(wifi_cfg->ap_ssid, CONFIG_WIFI_AP_SSID, sizeof(wifi_cfg->ap_ssid));
    strlcpy(wifi_cfg->ap_password, CONFIG_WIFI_AP_PASSWORD, sizeof(wifi_cfg->ap_password));
    wifi_cfg->ap_channel = 8; /* 与 webconfig 配网热点一致，换模式时设备不被踢下线 */
    wifi_cfg->ap_max_conn = CONFIG_WIFI_AP_MAX_CONN;
    strlcpy(wifi_cfg->ap_netmask, "255.255.255.0", sizeof(wifi_cfg->ap_netmask));
    strlcpy(wifi_cfg->ap_gateway, CONFIG_WIFI_AP_IP, sizeof(wifi_cfg->ap_gateway));
#endif

#if defined(CONFIG_WIFI_HAS_STA)
#if CONFIG_WIFI_STA_USE_DHCP
    wifi_cfg->sta_use_static_ip = false;
#else
    wifi_cfg->sta_use_static_ip = true;
    strlcpy(wifi_cfg->sta_ip, CONFIG_WIFI_STA_STATIC_IP, sizeof(wifi_cfg->sta_ip));
    strlcpy(wifi_cfg->sta_netmask, CONFIG_WIFI_STA_STATIC_NETMASK, sizeof(wifi_cfg->sta_netmask));
    strlcpy(wifi_cfg->sta_gateway, CONFIG_WIFI_STA_STATIC_GATEWAY, sizeof(wifi_cfg->sta_gateway));
#if defined(CONFIG_WIFI_STA_DNS_MAIN)
    strlcpy(wifi_cfg->sta_dns_main, CONFIG_WIFI_STA_DNS_MAIN, sizeof(wifi_cfg->sta_dns_main));
#endif
#if defined(CONFIG_WIFI_STA_DNS_BACKUP)
    strlcpy(wifi_cfg->sta_dns_backup, CONFIG_WIFI_STA_DNS_BACKUP, sizeof(wifi_cfg->sta_dns_backup));
#endif
#endif
#endif
}

/* webconfig_dev_cfg_t → wifi_init_cfg_t：boot 与 dev_apply 共用。
 * 调用方应先把 menuconfig 默认填好，再用本函数覆盖 NVS 实际值。 */
static void app_fill_wifi_cfg_from_dev(const webconfig_dev_cfg_t* dev, wifi_init_cfg_t* wifi_cfg)
{
    if (dev == NULL || wifi_cfg == NULL)
    {
        return;
    }

    wifi_cfg->mode = (strcmp(dev->wifi_mode, "ap") == 0) ? WIFI_MODE_AP : WIFI_MODE_STA;

    /* AP 段（模式是 STA 也填好，wifi 组件按 mode 决定用不用） */
    strlcpy(wifi_cfg->ap_ssid, dev->ap_ssid, sizeof(wifi_cfg->ap_ssid));
    strlcpy(wifi_cfg->ap_password, dev->ap_password, sizeof(wifi_cfg->ap_password));
    /* AP 静态 IP 三字段必须齐全，否则 wifi 组件校验失败 */
    if (dev->ap_ip[0] != '\0')
    {
        wifi_cfg->ap_use_static_ip = true;
        strlcpy(wifi_cfg->ap_ip, dev->ap_ip, sizeof(wifi_cfg->ap_ip));
        strlcpy(wifi_cfg->ap_netmask, "255.255.255.0", sizeof(wifi_cfg->ap_netmask));
        strlcpy(wifi_cfg->ap_gateway, dev->ap_ip, sizeof(wifi_cfg->ap_gateway));
    }
    else
    {
        wifi_cfg->ap_use_static_ip = false;
    }

    /* STA 段 */
    strlcpy(wifi_cfg->sta_ssid, dev->sta_ssid, sizeof(wifi_cfg->sta_ssid));
    strlcpy(wifi_cfg->sta_password, dev->sta_password, sizeof(wifi_cfg->sta_password));
    if (strcmp(dev->sta_ip_mode, "static") == 0)
    {
        wifi_cfg->sta_use_static_ip = true;
        strlcpy(wifi_cfg->sta_ip, dev->sta_ip, sizeof(wifi_cfg->sta_ip));
        strlcpy(wifi_cfg->sta_netmask, dev->sta_netmask, sizeof(wifi_cfg->sta_netmask));
        strlcpy(wifi_cfg->sta_gateway, dev->sta_gateway, sizeof(wifi_cfg->sta_gateway));
        strlcpy(wifi_cfg->sta_dns_main, dev->sta_dns, sizeof(wifi_cfg->sta_dns_main));
        wifi_cfg->sta_dns_backup[0] = '\0';
    }
    else
    {
        wifi_cfg->sta_use_static_ip = false;
    }
}

#if CONFIG_SERVICE_ANY_ENABLE
static service_mode_t protocol_to_mode(const char* p)
{
    if (strcmp(p, "tcpServer") == 0)
    {
        return SERVICE_MODE_TCP_SERVER;
    }
    if (strcmp(p, "tcpClient") == 0)
    {
        return SERVICE_MODE_TCP_CLIENT;
    }
    if (strcmp(p, "udp") == 0)
    {
        return SERVICE_MODE_UDP;
    }
    return SERVICE_MODE_NONE;
}
#endif

/* 网页保存整份配置：同步 WiFi 参数 + 切服务协议 + 重载 net_io。
 * 返回 ESP_OK 表示两边都"已受理"；NVS 已落盘，运行时是否立刻生效是另一件事。 */
static esp_err_t app_dev_apply(const webconfig_dev_cfg_t* cfg, void* ctx)
{
    (void)ctx;

    if (cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_SERVICE_ANY_ENABLE
    /* 保存后就是业务网络了。必须在 wifi_update_config() 之前清：它可能同步
     * 重启无线并抛出 AP_START。 */
    s_config_ap = false;
#endif

    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    app_fill_wifi_cfg_from_menuconfig(&wifi_cfg);
    app_fill_wifi_cfg_from_dev(cfg, &wifi_cfg);

    const esp_err_t wifi_err = wifi_update_config(&wifi_cfg);
    if (wifi_err != ESP_OK && wifi_err != ESP_ERR_INVALID_STATE)
    {
        /* 不 return：协议可能也要切 */
        ESP_LOGE(TAG, "dev_apply: wifi_update_config 失败: %s（配置已保存）",
                 esp_err_to_name(wifi_err));
    }
    else
    {
        ESP_LOGI(TAG, "dev_apply: wifi 配置已更新（模式=%s）", cfg->wifi_mode);
    }

#if CONFIG_SERVICE_ANY_ENABLE
    const service_mode_t target = protocol_to_mode(cfg->comm_protocol);
    if (target != SERVICE_MODE_NONE)
    {
        const esp_err_t svc_err = service_manager_request_switch(target);
        if (svc_err != ESP_OK)
        {
            ESP_LOGE(TAG, "dev_apply: 切协议失败: %s（配置已保存）", esp_err_to_name(svc_err));
            return svc_err;
        }
        ESP_LOGI(TAG, "dev_apply: 通讯协议已请求切换到 %s", cfg->comm_protocol);
    }
#endif

    /* 同协议小改（只改心跳 / 目标地址）不会重启 worker，这里主动重读一次。
     * 协议变了也无害：新 worker 启动时自己还会再读。 */
    if (strcmp(cfg->comm_protocol, "tcpClient") == 0)
    {
        net_io_reload_config();
    }

#if CONFIG_SERVICE_ANY_ENABLE
    /* 只对纯 AP 补一次开门判定，两个场景只能靠这里收口：
     *   1) 配置没实质变化 → 不重启无线 → 没有 AP_START 事件；
     *   2) 从配网热点切到业务 AP，客户端还挂着 → 不会再有"接入"事件。
     * STA 模式不能调：那时 IP 还没拿到，会被"非纯 AP 直接放行"提前开门。 */
    if (wifi_is_ap_only())
    {
        wifi_refresh_net_ready("配置已应用");
    }
#endif

    return ESP_OK;
}

/* 配网热点：唯一会走这里的路径是 webconfig_start()（业务 AP 走 dev_apply） */
static esp_err_t app_ap_start(const webconfig_ap_cfg_t* ap, void* ctx)
{
    (void)ctx;

#if CONFIG_SERVICE_ANY_ENABLE
    /* 必须在 wifi_config_init() 之前置位：它在 esp_wifi_start() 返回前就抛 AP_START */
    s_config_ap = true;
#endif

    /* wifi_config_init() 可重复调用，只重新下发模式与参数，所以"连接失败
     * 自动重开热点"时再调一次也安全。 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    wifi_cfg.mode = WIFI_MODE_AP;
    strlcpy(wifi_cfg.ap_ssid, ap->ssid, sizeof(wifi_cfg.ap_ssid));
    strlcpy(wifi_cfg.ap_password, ap->password, sizeof(wifi_cfg.ap_password));
    wifi_cfg.ap_channel = ap->channel;
    wifi_cfg.ap_max_conn = ap->max_conn;

    return wifi_config_init(&wifi_cfg);
}

/* 老配网路径（已退役）：离开配网热点，用新凭证连路由器 */
static esp_err_t app_sta_apply(const char* ssid, const char* password, void* ctx)
{
    (void)ctx;

#if CONFIG_SERVICE_ANY_ENABLE
    s_config_ap = false;
#endif

    /* 只覆盖 ssid / 密码，模式与 IP 策略沿用 menuconfig，
     * 避免"网页配网"把 DHCP/静态 IP 的设定连带清掉 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    app_fill_wifi_cfg_from_menuconfig(&wifi_cfg);
    strlcpy(wifi_cfg.sta_ssid, ssid, sizeof(wifi_cfg.sta_ssid));
    strlcpy(wifi_cfg.sta_password, password, sizeof(wifi_cfg.sta_password));

    return wifi_update_config(&wifi_cfg);
}

#if CONFIG_SERVICE_ANY_ENABLE
/* 纯 AP = 只有热点内网。APSTA 返回 false（有上游网络，不该等热点客户端）；
 * 查询失败也按"非纯 AP"处理，宁可早开也不卡死业务。 */
static bool wifi_is_ap_only(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK)
    {
        return false;
    }
    return mode == WIFI_MODE_AP;
}

/* 直接查驱动的 STA 表，不自己维护计数器：AID 复用、漏事件都不会漂 */
static int wifi_ap_client_count(void)
{
    wifi_sta_list_t list;
    memset(&list, 0, sizeof(list));
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK)
    {
        return 0;
    }
    return (int)list.num;
}

/* 重算"通讯服务该不该跑"。四个触发点（AP_START / 客户端进出 / 配置已应用）
 * 规则一致，集中在这里免得改漏。非纯 AP 直接放行：开门时机由 GOT_IP /
 * STA_DISCONNECTED 决定。 */
static void wifi_refresh_net_ready(const char* reason)
{
    const bool was_ready = net_io_is_net_ready();

    if (!wifi_is_ap_only())
    {
        net_io_set_net_ready(true);
        if (!was_ready)
        {
            ESP_LOGI(TAG, "%s：放行通讯服务（STA/APSTA 有上游网络）", reason);
        }
        return;
    }

    if (s_config_ap)
    {
        net_io_set_net_ready(false);
        ESP_LOGI(TAG, "%s：配网热点只提供配置页，通讯服务不启动", reason);
        return;
    }

    const int clients = wifi_ap_client_count();
    const bool ready = (clients > 0);
    net_io_set_net_ready(ready);

    if (ready != was_ready)
    {
        ESP_LOGI(TAG, "%s：%s（客户端 %d 台）", reason,
                 ready ? "有设备在用热点，启动通讯服务" : "没有客户端，通讯服务回到待机", clients);
    }
}
#endif /* CONFIG_SERVICE_ANY_ENABLE */

static void wifi_event_forward(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;

    if (base == WIFI_EVENT)
    {
        if (id == WIFI_EVENT_STA_CONNECTED)
        {
            webconfig_notify_sta_connected();
            /* 幂等地再关一次省电并打印实际值：配网路径下 STA 接口是后建的，
             * 不保证继承初始化时那次设置；modem sleep 会让收包等 DTIM，
             * 表现为"收发慢但日志正常"。 */
            wifi_ps_type_t ps_before = WIFI_PS_MIN_MODEM;
            (void)esp_wifi_get_ps(&ps_before);
            if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK)
            {
                ESP_LOGW(TAG, "关闭 WiFi 省电失败");
            }
            wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
            (void)esp_wifi_get_ps(&ps);
            ESP_LOGI(TAG, "WiFi 省电模式: 连上前 PS=%d → 下发后 PS=%d（0=NONE 低延迟）",
                     (int)ps_before, (int)ps);
        }
        else if (id == WIFI_EVENT_STA_DISCONNECTED)
        {
            webconfig_notify_sta_disconnected();
#if CONFIG_SERVICE_ANY_ENABLE
            /* STA 侧 IP 已失效，关门等重连；GOT_IP 回来会重新放行。
             * 按"纯 STA"语义处理：将来若 APSTA 共存，AP 侧可能还活着。 */
            net_io_set_net_ready(false);
#endif
        }
        else if (id == WIFI_EVENT_AP_START)
        {
#if CONFIG_SERVICE_ANY_ENABLE
            wifi_refresh_net_ready(s_config_ap ? "AP 已启动（配网热点）" : "AP 已启动");
#endif
        }
        else if (id == WIFI_EVENT_AP_STACONNECTED)
        {
#if CONFIG_SERVICE_ANY_ENABLE
            const wifi_event_ap_staconnected_t* e = (const wifi_event_ap_staconnected_t*)data;
            ESP_LOGI(TAG, "客户端接入热点（AID=%d，共 %d 台）", (int)e->aid,
                     wifi_ap_client_count());
            wifi_refresh_net_ready("客户端接入");
#endif
        }
        else if (id == WIFI_EVENT_AP_STADISCONNECTED)
        {
#if CONFIG_SERVICE_ANY_ENABLE
            /* 最后一台断开 → 回到待机；httpd 不受影响，配置页还挂着 */
            const wifi_event_ap_stadisconnected_t* e =
                (const wifi_event_ap_stadisconnected_t*)data;
            ESP_LOGI(TAG, "客户端断开（AID=%d，剩余 %d 台）", (int)e->aid,
                     wifi_ap_client_count());
            wifi_refresh_net_ready("客户端断开");
#endif
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
#if CONFIG_SERVICE_ANY_ENABLE
        net_io_set_net_ready(true);
#endif
        ESP_LOGI(TAG, "管理页面: http://" IPSTR "/", IP2STR(&e->ip_info.ip));
        log_link_diag();
    }
}

/* 一行链路诊断：mode 1=STA 3=APSTA（还是 3 说明热点没关，射频在分时）；
 * PS 1=MIN_MODEM 说明省电没关掉；RSSI < -75 重传明显增多。 */
static void log_link_diag(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);

    wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
    (void)esp_wifi_get_ps(&ps);

    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        ESP_LOGI(TAG, "链路诊断: mode=%d PS=%d RSSI=%d 信道=%d 11b/g/n=%d/%d/%d", (int)mode,
                 (int)ps, ap.rssi, ap.primary, ap.phy_11b, ap.phy_11g, ap.phy_11n);
    }
    else
    {
        ESP_LOGI(TAG, "链路诊断: mode=%d PS=%d（此刻读不到 AP 信息）", (int)mode, (int)ps);
    }
}
