/**
 * @file app_wifi.c
 * @brief WiFi / 配网初始化与状态适配：业务层只需调 app_wifi_start()
 *
 * 【这一层在做什么】
 * webconfig 组件不认识 esp_wifi，它通过 webconfig_cfg_t 里的几个回调（开热点 /
 * 切 STA / 应用整份设备配置）把动作交回给应用。本文件就是那组回调的实现，
 * 同时负责：
 *   - 启动时选路径：NVS 有配置 → 直连；menuconfig 有凭证 → 连它；
 *     都没有 → 起配网热点；
 *   - 把 WiFi 事件（连上 / 掉线 / 热点启动 / 客户端进出 / 拿到 IP）翻译成
 *     "通讯层该不该开工"，通过 net_io_set_net_ready() 通知 net_io；
 *   - 网页保存配置后，同步切 WiFi 参数、切服务协议、重载 net_io 配置。
 *
 * 【为什么要先注册事件再启动 WiFi】
 * 配网路径下 webconfig_start() 内部会一路走到 esp_wifi_start()，
 * WIFI_EVENT_AP_START 在它返回前就已经触发。注册若放在 WiFi 启动之后，
 * AP 模式会错过 AP_START，门控永远是 false，TCP 任务就卡死在等待循环里、
 * 一次 connect 都不发 —— 这是"AP 模式下 TCP 没有连接动作"的根因。
 * 事件循环提前创建对 wifi_config_init() 是安全的：它内部有"已存在
 * （ESP_ERR_INVALID_STATE）不算错"的守卫，会直接复用。
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

/* 适配层（定义在本文件后半段）：webconfig 的端口回调 + 事件转发 */
static esp_err_t app_ap_start(const webconfig_ap_cfg_t* ap, void* ctx);
static esp_err_t app_sta_apply(const char* ssid, const char* password, void* ctx);
static esp_err_t app_dev_apply(const webconfig_dev_cfg_t* cfg, void* ctx);
static void wifi_event_forward(void* arg, esp_event_base_t base, int32_t id, void* data);
static void app_fill_wifi_cfg_from_menuconfig(wifi_init_cfg_t* wifi_cfg);
static void app_fill_wifi_cfg_from_dev(const webconfig_dev_cfg_t* dev, wifi_init_cfg_t* wifi_cfg);
static void log_link_diag(void);

#if CONFIG_SERVICE_ANY_ENABLE
/* 两个判定函数定义在本文件后半段，app_dev_apply 收尾也要用 */
static bool wifi_is_ap_only(void);
static void wifi_refresh_net_ready(const char* reason);
#endif

/* 当前热点是不是"配网热点"（webconfig 起的那个）。
 *
 * 【为什么不用 webconfig_is_running() 现查】
 * 那个标志是在 webconfig_start() 里调完 ap_start() 之后才置 true，而
 * wifi_config_init() → esp_wifi_start() 会在返回前就抛出 WIFI_EVENT_AP_START。
 * 在 AP_START 处理器里现查会读到"还没进入配网"，于是先把通讯服务放行了；
 * 等它置 true 已经晚了。所以改由本层自己记，且写入点都在起热点之前：
 *   - app_ap_start()（配网回调，业务 AP 不走这里）→ true
 *   - app_sta_apply() / app_dev_apply()（切到业务网络）→ false
 *
 * 配网热点里连上来的手机只是要打开 192.168.4.1 填表，没有业务可跑；
 * 业务 AP 模式下有客户端接入就该开工。两者都表现为"AP 里有设备"，
 * 只能靠这个来源区分。 */
static volatile bool s_config_ap = false;

/**
 * @brief 配网组件配置：热点参数用默认值，端口回调把 webconfig 接到 wifi 组件上
 *
 * @note keep_httpd = true：配网结束后 HTTP 服务器继续运行，设备连上路由器后
 *       仍可用它在路由器网段的 IP 打开管理页（地址见串口日志），
 *       不用重新连热点就能改配置。
 */
static const webconfig_cfg_t webconfig_cfg = {
    .ap = WEBCONFIG_AP_CFG_DEFAULT(),
    .port = {
        .ap_start   = &app_ap_start,
        .sta_apply  = &app_sta_apply,
        .dev_apply  = &app_dev_apply,
    },
    .keep_httpd = true,
    .ctx = NULL,
};

esp_err_t app_wifi_start(void)
{
    /* ---- 先建事件循环并注册事件转发，再启动 WiFi ----
     * 【为什么必须放在最前】配网路径下 webconfig_start() 内部会一路走到
     * esp_wifi_start()，WIFI_EVENT_AP_START 在它返回前就已经触发。注册若放在
     * WiFi 启动之后，AP 模式会错过 AP_START，s_net_ready 永远是 false，
     * tcp_conn_task 就卡死在等待循环里、一次 connect 都不发 —— 这就是
     * "AP 模式下 TCP 没有连接动作"的根因。
     * 事件循环提前创建对 wifi_config_init() 是安全的：它内部有
     * "已存在（ESP_ERR_INVALID_STATE）不算错"的守卫，会直接复用。 */
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

    /* 注册事件转发（只影响"连不上自动回配网"和 TCP 就绪开门，失败不拦着正常功能） */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_forward,
                                              NULL, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "注册 WiFi 事件转发失败，密码填错时不会自动重开配网热点: %s",
                 esp_err_to_name(err));
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_forward,
                                              NULL, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "注册 IP 事件转发失败，不会打印管理页地址: %s", esp_err_to_name(err));
    }

    webconfig_dev_cfg_t dev;
    const bool has_dev = webconfig_load_dev_cfg(&dev);

    /* 用 menuconfig 默认值先把 wifi_cfg 填好（兜底）；NVS 有 dev 配置时再覆盖。
     *
     * 优先级（与 /save 流程一致）：
     *   1. NVS 有完整 dev 配置 → 用 dev 里的网络段（用户最新提交优先）
     *   2. 没 dev 但 menuconfig 给出了 STA SSID → 用 menuconfig 烧死进去的凭证
     *   3. 都没有 + 模式包含 STA → 走 web 配网
     *   4. 模式 = AP-only + 没 dev → wifi_cfg 里的 AP 参数已经齐全，
     *                                直接 wifi_config_init() 当热点开
     *
     * 无论哪一支都跑一次 menuconfig helper，所以 wifi_cfg 里那些"业务无关"
     * 的字段永远不会丢。dev 路径在它的 helper 里整体覆盖 mode / AP / STA。 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    app_fill_wifi_cfg_from_menuconfig(&wifi_cfg);

    if (has_dev)
    {
        ESP_LOGI(TAG, "使用 NVS 里的完整设备配置（模式=%s, 设备=%s）", dev.wifi_mode,
                 dev.device_name);

        /* 用 NVS 整体覆盖 wifi_cfg（不只是 ssid/password） */
        app_fill_wifi_cfg_from_dev(&dev, &wifi_cfg);
        err = wifi_config_init(&wifi_cfg);

        /* 不开热点，把管理页挂上去：连上路由器后用它拿到的 IP 访问 */
        if (err == ESP_OK)
        {
            err = webconfig_httpd_start(&webconfig_cfg);
        }

        /* STA 模式：开启"开机连续失败 → 自动切回 AP 配网"的观察。
         * 不开启的话，用 NVS 凭证开机却连不上时会一直重连（wifi 组件的
         * 重连任务没有次数上限），用户既连不上设备也没法改凭证。
         * AP 模式不需要（本身就是热点）。 */
        if (err == ESP_OK && wifi_cfg.mode == WIFI_MODE_STA)
        {
            webconfig_watch_sta_connect();
        }
    }
    else if (wifi_cfg.mode == WIFI_MODE_AP)
    {
        /* 模式 = AP-only 且没保存配置：跳过 web 配网，直接当热点开。
         * 【绕过 webconfig_start() 是有意为之】web 配网是为"未知 STA 凭证"准备的；
         * 用户已经在 menuconfig 里决定只跑 AP，根本不需要 配网 UI 这一步。
         * 管理页仍正常挂上（webconfig_httpd_start 在热点上提供），
         * 后续若有需要可在管理页加一个"切回 STA 配网"的入口。 */
        ESP_LOGI(TAG, "模式 = AP-only，按 menuconfig 直接开机为热点");
        err = wifi_config_init(&wifi_cfg);

        if (err == ESP_OK)
        {
            err = webconfig_httpd_start(&webconfig_cfg);
        }
    }
#if defined(CONFIG_WIFI_STA_SSID) && defined(CONFIG_WIFI_STA_PASSWORD)
    /* 这一支编译期由 CONFIG_WIFI_STA_SSID 决定是否存在（depends on WIFI_HAS_STA）。
     * menuconfig 模式 = AP-only 时这两个宏都未定义、整段被预处理器吃掉，
     * 下面的 else 会自然挂到上面的 `wifi_cfg.mode == WIFI_MODE_AP` 那条上；
     * 不会留下"if / else if / (空) / else"这种 C 语言里非法的悬空结构。 */
    else if (wifi_cfg.mode == WIFI_MODE_STA && CONFIG_WIFI_STA_SSID[0] != '\0')
    {
        ESP_LOGI(TAG, "用 menuconfig 里的 STA 凭证连接: %s", CONFIG_WIFI_STA_SSID);

        strlcpy(wifi_cfg.sta_ssid, CONFIG_WIFI_STA_SSID, sizeof(wifi_cfg.sta_ssid));
        strlcpy(wifi_cfg.sta_password, CONFIG_WIFI_STA_PASSWORD, sizeof(wifi_cfg.sta_password));
        err = wifi_config_init(&wifi_cfg);

        if (err == ESP_OK)
        {
            err = webconfig_httpd_start(&webconfig_cfg);
        }

        /* 同上：编译期凭证连不上时要能自己回配网，否则用户只能重新烧录 */
        if (err == ESP_OK)
        {
            webconfig_watch_sta_connect();
        }
    }
#endif
    else
    {
        ESP_LOGI(TAG, "没有可用的设备配置，启动配网热点");
        err = webconfig_start(&webconfig_cfg);
    }

    if (err != ESP_OK)
    {
        return err;
    }

    /* 关掉 WiFi 省电（modem sleep），换取低延迟。
     * 默认的 WIFI_PS_MIN_MODEM 下，射频空闲时周期性休眠、只按 DTIM 周期
     * 醒来；数据到了要等它醒，延迟在几十~几百 ms 之间随机跳动，表现就是
     * "响应忽快忽慢"。本场景要快速响应，关掉；代价是功耗升高。
     *
     * 【务必看返回值】本函数在"配网路径"下被调用时，驱动还停在 AP-only 模式
     * （STA 接口尚未启用），这一行有失败的可能；而两个启动路径唯一的区别就
     * 在这里。失败会被记成警告，STA 连上后 wifi_event_forward() 还会再下发
     * 一次并打印实际生效值。 */
    const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK)
    {
        ESP_LOGW(TAG, "初始化时关闭 WiFi 省电失败: %s（STA 连上后会再试一次）",
                 esp_err_to_name(ps_err));
    }

    return ESP_OK;
}

static void app_fill_wifi_cfg_from_menuconfig(wifi_init_cfg_t* wifi_cfg)
{
    /* 模式 */
#if defined(CONFIG_WIFI_MODE_AP)
    wifi_cfg->mode = WIFI_MODE_AP;
#else
    wifi_cfg->mode = WIFI_MODE_STA;
#endif

    /* AP 参数：仅在模式包含 AP 时才下发（wifi 组件内部 want_ap=false 时会忽略） */
#if defined(CONFIG_WIFI_HAS_AP)
    strlcpy(wifi_cfg->ap_ssid, CONFIG_WIFI_AP_SSID, sizeof(wifi_cfg->ap_ssid));
    strlcpy(wifi_cfg->ap_password, CONFIG_WIFI_AP_PASSWORD, sizeof(wifi_cfg->ap_password));
    wifi_cfg->ap_channel = 8;
    wifi_cfg->ap_max_conn = CONFIG_WIFI_AP_MAX_CONN;
    strlcpy(wifi_cfg->ap_netmask, "255.255.255.0", sizeof(wifi_cfg->ap_netmask));
    strlcpy(wifi_cfg->ap_gateway, CONFIG_WIFI_AP_IP, sizeof(wifi_cfg->ap_gateway));
#endif

    /* STA 的 IP 策略：menuconfig 只有 STA / AP 两种模式，
     * AP 模式下 CONFIG_WIFI_HAS_STA 未定义、整段不参与编译；
     * 所以走到这里必然是 STA-only，直接按 menuconfig 的 DHCP 选择填。 */
#if defined(CONFIG_WIFI_HAS_STA)
#if CONFIG_WIFI_STA_USE_DHCP
    wifi_cfg->sta_use_static_ip = false;
#else
    wifi_cfg->sta_use_static_ip = true;
    strlcpy(wifi_cfg->sta_ip, CONFIG_WIFI_STA_STATIC_IP, sizeof(wifi_cfg->sta_ip));
    strlcpy(wifi_cfg->sta_netmask, CONFIG_WIFI_STA_STATIC_NETMASK, sizeof(wifi_cfg->sta_netmask));
    strlcpy(wifi_cfg->sta_gateway, CONFIG_WIFI_STA_STATIC_GATEWAY, sizeof(wifi_cfg->sta_gateway));
    /* DNS 留空 → wifi 组件视为"不设置"，跳过 esp_netif_set_dns_info()。 */
#if defined(CONFIG_WIFI_STA_DNS_MAIN)
    strlcpy(wifi_cfg->sta_dns_main, CONFIG_WIFI_STA_DNS_MAIN, sizeof(wifi_cfg->sta_dns_main));
#endif
#if defined(CONFIG_WIFI_STA_DNS_BACKUP)
    strlcpy(wifi_cfg->sta_dns_backup, CONFIG_WIFI_STA_DNS_BACKUP, sizeof(wifi_cfg->sta_dns_backup));
#endif
#endif
#endif
}

/* ===================== webconfig 适配层 =====================
 * webconfig 组件只认一组回调，不知道底下是哪套无线栈：
 *   - ap_start  : 开配网热点；
 *   - sta_apply : 关掉热点、用用户在网页上填的凭证去连接（配网老路径）；
 *   - dev_apply : 推完整设备配置给应用层（web 配网页新路径）。
 * 这里用 wifi 组件把它们实现出来；反向的"连上/断开"则由 wifi_event_forward()
 * 转成 webconfig_notify_sta_*()，供配网组件做连接失败自愈。
 */

/**
 * @brief 把 webconfig_dev_cfg_t 翻译成 wifi_init_cfg_t：boot / dev_apply 都需要
 *
 * @note 整体覆盖 wifi_cfg：模式、AP / STA 凭据、IP 策略；调用方应先把
 *       menuconfig 默认填进 wifi_cfg（保证 AP 静态 IP 兜底字段之类不丢），
 *       再用本函数覆盖一遍 NVS 实际值。
 */
static void app_fill_wifi_cfg_from_dev(const webconfig_dev_cfg_t* dev, wifi_init_cfg_t* wifi_cfg)
{
    if (dev == NULL || wifi_cfg == NULL)
    {
        return;
    }

    /* 模式 */
    if (strcmp(dev->wifi_mode, "ap") == 0)
    {
        wifi_cfg->mode = WIFI_MODE_AP;
    }
    else
    {
        wifi_cfg->mode = WIFI_MODE_STA;
    }

    /* AP 段（即便模式是 STA 也填好，wifi 组件内部按 mode 决定用不用）*/
    strlcpy(wifi_cfg->ap_ssid, dev->ap_ssid, sizeof(wifi_cfg->ap_ssid));
    strlcpy(wifi_cfg->ap_password, dev->ap_password, sizeof(wifi_cfg->ap_password));
    /* AP 信道 / 最大连接数：用户没在网页里改这些，按 menuconfig 当前值保留 */
    /* AP 静态 IP：网段 ip/netmask/gateway 三个字段都必填，否则 wifi 组件校验失败 */
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

    /* STA IP 策略 */
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

/**
 * @brief dev_apply 实现：把 NVS 里的完整配置推到 wifi 组件 + service_manager
 *
 * @note 两件事分两条路：
 *       - wifi 部分：wifi_update_config() —— wifi 还没初始化时（首次配网场景下
 *         不可能发生；首次配网是 webconfig_start 开 AP，wifi 一定已经起来）
 *         报 ESP_ERR_INVALID_STATE 也忽略；
 *       - 通讯部分：service_manager_request_switch() —— 切到新协议，worker
 *         重启时从 NVS 读出新参数。port / server ip / heartbeat 等的运行期
 *         应用依赖 worker 的"启动时读 NVS"行为，main.c 里要相应改。
 *
 * @return ESP_OK 表示两边都"已受理"；任何一边报错都原样返回（webconfig.c 不
 *         据此回错 HTTP——NVS 落盘已经成功，运行时是否立即生效是另一件事）。
 */
static esp_err_t app_dev_apply(const webconfig_dev_cfg_t* cfg, void* ctx)
{
    (void)ctx;

    if (cfg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_SERVICE_ANY_ENABLE
    /* 用户保存了新配置 → 接下来按"业务模式"重配网络，当前热点不再是配网热点。
     * 必须在 wifi_update_config() 之前清：它可能同步重启无线并抛出 AP_START。 */
    s_config_ap = false;
#endif

    /* 1) wifi 段：先按 menuconfig 兜底填一份，再用 NVS dev 覆盖
     *    （dev 缺字段时不会"反向"清掉 menuconfig 默认值；这里依赖
     *    app_fill_wifi_cfg_from_menuconfig 已经把 AP 信道 / max_conn / 静态 IP
     *    网段这种 NVS 里没有的字段填好了）。 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    app_fill_wifi_cfg_from_menuconfig(&wifi_cfg);
    app_fill_wifi_cfg_from_dev(cfg, &wifi_cfg);

    const esp_err_t wifi_err = wifi_update_config(&wifi_cfg);
    if (wifi_err != ESP_OK && wifi_err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "dev_apply: wifi_update_config 失败: %s（配置已保存）",
                 esp_err_to_name(wifi_err));
        /* 不直接 return：通讯协议可能也要切，让下面继续 */
    }
    else
    {
        ESP_LOGI(TAG, "dev_apply: wifi 配置已更新（模式=%s）", cfg->wifi_mode);
    }

    /* 2) 通讯段：切到用户选的协议。service_manager 内部会停旧 worker、拉新
     *    worker，新 worker 启动时从 NVS 读最新参数（main.c 里要相应改）。 */
#if CONFIG_SERVICE_ANY_ENABLE
    service_mode_t target = SERVICE_MODE_NONE;
    if (strcmp(cfg->comm_protocol, "tcpServer") == 0)
        target = SERVICE_MODE_TCP_SERVER;
    else if (strcmp(cfg->comm_protocol, "tcpClient") == 0)
        target = SERVICE_MODE_TCP_CLIENT;
    else if (strcmp(cfg->comm_protocol, "udp") == 0)
        target = SERVICE_MODE_UDP;

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

    /* TCP 客户端"同协议小改"：service_manager 切换目标 == 当前模式时（典型是
     * 只在网页上改心跳开关/间隔/格式/内容）不会重启 worker，tcp_client_task
     * 自己也不会主动重 pick —— 必须等下次断线重连才会用上新 NVS。
     * 这里在 dev_apply 收尾时主动 pick 一次立即生效（即便协议变了也无所谓：
     * manager 稍后重启的新 worker 启动时自己再 pick 一次，结果一致）。
     *
     * 并发：dev_apply 跑在 esp_timer 任务，与 tcp_conn_task / rx_consumer_task
     * 不在同一任务。pick_heartbeat 写 s_tcp_client_hb_* 只是几个字节赋值，
     * ESP32 单核几微秒完成；最差一帧 send/recv 读到半截（错一帧 payload
     * 或 echo 防乒乓误判一帧），下帧自愈——比"必须重启"代价小得多。 */
    if (strcmp(cfg->comm_protocol, "tcpClient") == 0)
    {
        net_io_reload_config();
    }

#if CONFIG_SERVICE_ANY_ENABLE
    /* 补一次"该不该开门"的判定，但**只针对纯 AP**。两个场景只能靠这里收口：
     *   1) 新配置跟原来一模一样（例如只改了设备名）→ wifi_update_config()
     *      不重启无线 → 没有 AP_START 事件 → 纯 AP 模式下永远等不到开门；
     *   2) 从配网热点切到业务 AP、客户端还挂在上面 → 不会再有"接入"事件。
     *
     * 切到 STA 时**不能**调：那时 IP 还没拿到，wifi_refresh_net_ready 的
     * "非纯 AP 直接放行"会让服务提前开工。STA 的开门时机只由
     * IP_EVENT_STA_GOT_IP 决定（AP_START 走那条分支是为了 APSTA 的老行为）。
     * 位置放在最后：wifi 段已下发完，此刻读到的模式 / 客户端表都是最新的。 */
    if (wifi_is_ap_only())
    {
        wifi_refresh_net_ready("配置已应用");
    }
#endif

    return ESP_OK;
}

static esp_err_t app_ap_start(const webconfig_ap_cfg_t* ap, void* ctx)
{
    (void)ctx;

#if CONFIG_SERVICE_ANY_ENABLE
    /* 这个回调只有 webconfig 的配网流程会调（webconfig_start → port.ap_start），
     * 业务 AP 走 app_dev_apply。所以在这里记一笔，AP_START 事件就知道"当前
     * 热点是配网用的，客户端接进来也不该启动通讯服务"。
     * 必须在 wifi_config_init() **之前**置位：它在 esp_wifi_start() 返回前就会
     * 抛出 WIFI_EVENT_AP_START。 */
    s_config_ap = true;
#endif /* CONFIG_SERVICE_ANY_ENABLE：无通讯服务时这个标记不存在 */

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

#if CONFIG_SERVICE_ANY_ENABLE
    /* 老路径（已退役），但保持行为一致：这里一调就离开配网热点去连路由器，
     * 当前热点不再是"配网"的。 */
    s_config_ap = false;
#endif

    /* 切回 STA 模式连接：wifi_update_config() 会断开当前 AP、用新凭证重连。
     * 模式 / IP 策略沿用 menuconfig，只把用户在网页上提交的 ssid / 密码覆盖上去
     * —— 这样 web 配网不会把"DHCP vs 静态 IP"这条设定也连带清掉。 */
    wifi_init_cfg_t wifi_cfg = WIFI_INIT_CFG_DEFAULT();
    app_fill_wifi_cfg_from_menuconfig(&wifi_cfg);
    strlcpy(wifi_cfg.sta_ssid, ssid, sizeof(wifi_cfg.sta_ssid));
    strlcpy(wifi_cfg.sta_password, password, sizeof(wifi_cfg.sta_password));

    return wifi_update_config(&wifi_cfg);
}

#if CONFIG_SERVICE_ANY_ENABLE
/**
 * @brief 当前是不是"纯 AP"模式（只有热点内网，没有上游网络）
 *
 * @note APSTA 返回 false：那种模式 STA 侧拿到 IP 就会开门，不该等热点客户端。
 *       查询失败时按"非纯 AP"处理（保持旧行为，宁可早开也不卡死业务）。
 */
static bool wifi_is_ap_only(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK)
    {
        return false;
    }
    return mode == WIFI_MODE_AP;
}

/**
 * @brief 当前接入热点的客户端数量（0 = 没有设备在用这个热点）
 *
 * @note 直接查驱动的 STA 表，而不是自己维护计数器：AID 复用、漏事件都不会漂。
 */
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

/**
 * @brief 重算"通讯服务该不该跑"，变化时打一行日志
 *
 * @param reason 触发原因，只用于日志（上板时对时序用）
 *
 * @note 四个触发点（AP_START / 客户端接入 / 客户端断开 / 配置已应用）规则完全
 *       一样，集中在这里判断，免得散着写改漏一处。
 *
 * @note 非纯 AP（STA / APSTA）直接置 true：那两种模式有上游网络，开门时机由
 *       IP_EVENT_STA_GOT_IP / STA_DISCONNECTED 决定；AP_START 时置 true 是保留
 *       "APSTA 热点一起来就能跑业务"的老行为。
 */
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
        /* 配网热点：连上来的手机 / 电脑只是要打开 192.168.4.1 填表 */
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

            /* 【低延迟的关键一步，也是"配网路径变慢"的修复点】
             * hardware_init() 里那次 esp_wifi_set_ps(WIFI_PS_NONE) 是在 WiFi 刚
             * 起来时调的。两条启动路径在那一刻的驱动状态不同：
             *   - 重启直连（有凭证）：那一刻模式已经是 STA，设置直接落在 STA 上；
             *   - 首次配网：那一刻模式还是 AP-only（STA 接口未启用），
             *     httpd 回调里 wifi_update_config() 才把模式切成 STA 并新建
             *     STA 接口 —— 这个"后出生"的 STA 接口不保证继承之前那次设置，
             *     可能退回默认的 WIFI_PS_MIN_MODEM（modem sleep）。
             * modem sleep 一旦生效，收包要等 DTIM 周期才醒：服务器发来的数据晚
             * 几十~几百毫秒才被收到，应答/上行跟着一起晚，表现就是"收发不及时
             * 但代码日志一切正常"。所以这里每次连上都幂等地再下发一次，
             * 并把驱动里实际生效的值打印出来（0=NONE 才是我们要的）。 */
            /* 先读"连上前"的实际值：这是判断根因的关键证据 ——
             * 配网路径下若这里是 1（MIN_MODEM），就说明 hardware_init() 那次
             * 设置没落到这个"后建出来"的 STA 接口上，问题实锤。 */
            wifi_ps_type_t ps_before = WIFI_PS_MIN_MODEM;
            (void)esp_wifi_get_ps(&ps_before);

            const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
            if (ps_err != ESP_OK)
            {
                ESP_LOGW(TAG, "关闭 WiFi 省电失败: %s", esp_err_to_name(ps_err));
            }
            /* 连上后再读一次实际生效值：0=NONE（我们要的），1=MIN_MODEM。
             * 若这里还是 1，说明这次下发没生效，得继续查驱动状态。 */
            wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
            (void)esp_wifi_get_ps(&ps);
            ESP_LOGI(TAG,
                     "WiFi 省电模式: 连上前 PS=%d → 下发后 PS=%d（0=NONE 低延迟；1=MIN_MODEM 会有 "
                     "DTIM 级延迟）",
                     (int)ps_before, (int)ps);
        }
        else if (id == WIFI_EVENT_STA_DISCONNECTED)
        {
            webconfig_notify_sta_disconnected();
#if CONFIG_SERVICE_ANY_ENABLE
            /* STA 掉线 → STA 侧 IP 已无效：清掉就绪标志，让 tcp_conn_task
             * 暂停等 WiFi 重新连上。esp_wifi 默认会自己重连，GOT_IP 事件
             * 一回来又会置回 true。
             *
             * 注意：这里按"纯 STA"语义处理。将来若改成 APSTA 共存，STA 掉线
             * 时 AP 侧可能还在、连接也可能还活着，就不能无条件清这个标志了。 */
            net_io_set_net_ready(false);
#endif /* CONFIG_SERVICE_ANY_ENABLE：无服务时这个标志不存在 */
        }
        else if (id == WIFI_EVENT_AP_START)
        {
#if CONFIG_SERVICE_ANY_ENABLE
            /* 热点起来了，但"能不能跑业务"要按情况判定，统一交给
             * wifi_refresh_net_ready()：
             *   - 配网热点 → 不开门（连上来的只开网页填表）；
             *   - 业务 AP + 已有客户端 → 开门（切模式重开时老客户端还挂着）；
             *   - 业务 AP + 0 客户端    → 待机，等 AP_STACONNECTED；
             *   - APSTA                → 开门（有上游网络，不该被热点流量拖住）。 */
            wifi_refresh_net_ready(s_config_ap ? "AP 已启动（配网热点）" : "AP 已启动");
#endif /* CONFIG_SERVICE_ANY_ENABLE：无服务时不需要"放行 TCP" */
        }
        else if (id == WIFI_EVENT_AP_STACONNECTED)
        {
#if CONFIG_SERVICE_ANY_ENABLE
            /* 纯 AP 模式下通讯服务的**启动时机**：真有设备连上热点。
             * 配网热点里接进来的客户端不算（判定在 wifi_refresh_net_ready 里）。 */
            const wifi_event_ap_staconnected_t* e =
                (const wifi_event_ap_staconnected_t*)data;
            ESP_LOGI(TAG, "客户端接入热点（AID=%d，共 %d 台）", (int)e->aid,
                     wifi_ap_client_count());
            wifi_refresh_net_ready("客户端接入");
#endif /* CONFIG_SERVICE_ANY_ENABLE */
        }
        else if (id == WIFI_EVENT_AP_STADISCONNECTED)
        {
#if CONFIG_SERVICE_ANY_ENABLE
            /* 最后一台设备断开 → 热点里没人用了，通讯服务回到待机。
             * httpd 不受影响：配置页仍挂在 192.168.4.1，等着下一次接入。 */
            const wifi_event_ap_stadisconnected_t* e =
                (const wifi_event_ap_stadisconnected_t*)data;
            ESP_LOGI(TAG, "客户端断开（AID=%d，剩余 %d 台）", (int)e->aid,
                     wifi_ap_client_count());
            wifi_refresh_net_ready("客户端断开");
#endif /* CONFIG_SERVICE_ANY_ENABLE */
        }
        return;
    }

    /* 拿到 IP 说明设备已经挂在路由器网段上：管理页就在这个地址，
     * 打出来省得去路由器后台翻（DHCP 分配的地址不固定） */
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
#if CONFIG_SERVICE_ANY_ENABLE
        /* 拿到 IP = 有上游网络：给通讯层开门（TCP 长连接任务会立刻往下走） */
        net_io_set_net_ready(true);
#endif /* CONFIG_SERVICE_ANY_ENABLE */
        ESP_LOGI(TAG, "管理页面: http://" IPSTR "/", IP2STR(&e->ip_info.ip));
        log_link_diag(); /* 一行链路诊断，便于对比"配网路径"和"重启直连"的差异 */
    }
}

/**
 * @brief 打印一行链路诊断（模式 / 省电 / RSSI / 信道 / 协议）
 *
 * @note 只为排查"两条启动路径行为不一致"这类问题，正常运行时也可以留着
 *       （每拿到一次 IP 才打印一行）。看三个重点值：
 *       - mode：1=STA，3=APSTA。配网后若还是 3，说明热点没关掉，射频要在
 *         AP 与 STA 之间分时，收发必然变慢；
 *       - PS  ：1=MIN_MODEM 表示 modem sleep 还开着 → 收包要等 DTIM 周期；
 *       - RSSI：低于 -75 dBm 时重传明显增多，延迟抖动变大。
 */
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
