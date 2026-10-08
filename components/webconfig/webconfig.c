/* ===================== 通用 Web 配网组件实现 =====================
 * 本文件不包含任何无线协议栈的头文件，也不直接调用任何无线 API：
 *   - "开配网热点""用新凭证去连接" -> webconfig_port_t 里的两个回调（应用层实现）；
 *   - "连上了 / 断开了"           -> 应用层调 webconfig_notify_sta_*() 通知进来。
 * 组件只负责：起 HTTP 服务、读写 NVS，以及把页面模块串起来。
 * 页面本身（HTML/CSS/JS）在 web/index.html，JSON 生成与表单解析在
 * webconfig_page.c，改页面不用动这个文件。
 *
 * "HTTP 服务器"与"配网热点"是分开的两件事：配网结束后服务器可以继续留着，
 * 让设备在路由器网段（STA 模式）也能访问同一个页面，见 cfg.keep_httpd。
 */

#include "webconfig.h"
#include "webconfig_page.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"  /* esp_netif_get_handle_from_ifkey / esp_netif_get_ip_info / esp_ip4addr_ntoa */
#include "cJSON.h"

#include "service_manager.h" /* service_manager_get_active() / *_is_running() */

#include "nvs.h"
#include "nvs_flash.h" /* nvs_flash_init() / nvs_flash_erase() 在这里 */

#include <errno.h>
#include "lwip/sockets.h" /* send() / select()，配合下面的 EAGAIN 重试 */

static const char* TAG = "webconfig";

/* ===================== 内部常量 ===================== */

/* NVS 存放位置：独立命名空间（"webcfg"），目前存两类 blob
 *   - key "sta"  目标 WiFi 凭证（ssid + password）—— 配网老路径写入的"最小"
 *     信息，仅供 webconfig_load_sta_cfg() 使用；
 *   - key "dev"  完整设备配置（设备名 + 网络 + 通讯）—— 配网页新路径写入，
 *     boot 时由 main.c 整体读出。 */
#define WEBCONFIG_NVS_NS "webcfg"
#define WEBCONFIG_NVS_KEY_STA "sta"
#define WEBCONFIG_NVS_KEY_DEV "dev"

/* 提交成功后延迟切换到 STA 的等待时间：必须大于一次 HTTP 往返，
 * 因为切模式会关掉热点，正在传输的响应会送不出去。
 *
 * 字面量带 LL 后缀 → int64_t，与 esp_timer_start_once 的 uint64_t 第二参数
 * 以及 esp_timer_get_time() 的 int64_t 返回值类型对齐，避免隐式转换告警
 * （开启 -Wconversion / -Wsign-conversion 时尤其重要）。
 * 2 秒远不到 int 上限，LL 不是为了防溢出——是显式声明"按 64 位微秒算"。 */
#define WEBCONFIG_APPLY_DELAY_US (2LL * 1000 * 1000)

/* 应用新凭证后的"连接失败观察期"：期内连续 N 次连不上判定为凭证错误，
 * 自动重开配网热点。观察期结束时还没连上也放弃（交给无线栈继续重试）。
 *
 * 字面量带 LL 后缀的原因同 WEBCONFIG_APPLY_DELAY_US：与 esp_timer_get_time()
 * 的 int64_t 类型对齐；60 秒同样远不到 int 上限。 */
#define WEBCONFIG_FAIL_WATCH_US (60LL * 1000 * 1000)
#define WEBCONFIG_FAIL_LIMIT 5

/* HTTP 服务器任务栈：处理器里最大的一块是 WEBCONFIG_JSON_BODY_MAX(1024B) 的请求体
 * 缓冲；加上 httpd 自己的 scratch 缓冲，4096 已不够，用 8192。
 * 注：页面改成由前端自己渲染之前，这里要放 2KB 的页面缓冲，所以曾调到 8192；
 * 现在页面直接从 flash 发出去、不再拷进 RAM。以后在处理器里加大缓冲记得同步调大。 */
#define WEBCONFIG_HTTPD_STACK 8192

/* ===================== 内部状态 =====================
 * NVS 里存下来的 blob 就是 webconfig_sta_cfg_t 本身，不另外定义类型。 */

static httpd_handle_t s_server = NULL; /*!< HTTP 服务器句柄，NULL 表示未启动 */
static bool s_running = false;         /*!< 是否处于配网状态 */
static webconfig_cfg_t s_last_cfg;     /*!< 最近一次 webconfig_start() 的参数，失败自愈重开时复用 */
static webconfig_dev_cfg_t s_pending_dev;       /*!< POST 保存后等定时器应用的完整设备配置 */
static esp_timer_handle_t s_dev_apply_timer = NULL;   /*!< 延迟应用定时器（dev_apply 路径），惰性创建 */

/* ---- 连接失败自愈的计数状态（见 WEBCONFIG_FAIL_WATCH_US 的说明） ---- */
static int s_fail_count = 0;
static int64_t s_watch_start_us = 0; /*!< 0 表示不在观察期（不关心断线事件） */

/* ---- 通讯服务的在线状态（应用层通过 webconfig_set_comm_state 告知） ----
 * 在线判据完全交给应用层（TCP server listen 成功 / UDP bind 成功 / TCP client
 * 连上服务端 → 在线；其余 → 离线）。webconfig 不参与通讯协议探测。 */
static volatile bool s_comm_online = false;

/* ---- 应用层版本（应用层通过 webconfig_set_app_version 告知） ----
 * 留空表示未配置，前端芯片会显示 —。 */
static char s_app_version[32];

/* ---- 底层 APP 版本（应用层通过 webconfig_set_fw_version 告知） ----
 * 与 s_app_version 平行：前者是上层 App，后者是本组件所在固件（"底层 APP"）。 */
static char s_fw_version[32];

/* ===================== NVS 读写 ===================== */

/**
 * @brief 确保 NVS 已初始化（应用层初始化无线之前就可能要先读凭证，所以自己兜一遍）
 */
static esp_err_t nvs_ensure_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        err = nvs_flash_erase();
        if (err != ESP_OK)
        {
            return err;
        }
        err = nvs_flash_init();
    }

    if (err == ESP_ERR_INVALID_STATE)
    {
        err = ESP_OK; /* 已被初始化过（例如别的组件先跑了） */
    }
    return err;
}

/**
 * @brief 读出凭证 blob；没存过 / NVS 打不开都返回非 ESP_OK
 */
static esp_err_t sta_cfg_read(webconfig_sta_cfg_t* b)
{
    nvs_handle_t h = 0;

    if (nvs_open(WEBCONFIG_NVS_NS, NVS_READONLY, &h) != ESP_OK)
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t len = sizeof(*b);
    esp_err_t err = nvs_get_blob(h, WEBCONFIG_NVS_KEY_STA, b, &len);
    nvs_close(h);
    return err;
}

/**
 * @brief 写入凭证 blob 并提交
 */
static esp_err_t sta_cfg_write(const webconfig_sta_cfg_t* b)
{
    nvs_handle_t h = 0;

    esp_err_t err = nvs_open(WEBCONFIG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "打开 NVS 命名空间 %s 失败: %s", WEBCONFIG_NVS_NS, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, WEBCONFIG_NVS_KEY_STA, b, sizeof(*b));
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

/**
 * @brief 读出"完整设备配置"blob；NVS 没存过 / NVS 打不开 / 数据长度不对都返回非 ESP_OK
 */
static esp_err_t dev_cfg_read(webconfig_dev_cfg_t* b)
{
    nvs_handle_t h = 0;

    if (nvs_open(WEBCONFIG_NVS_NS, NVS_READONLY, &h) != ESP_OK)
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t len = sizeof(*b);
    esp_err_t err = nvs_get_blob(h, WEBCONFIG_NVS_KEY_DEV, b, &len);
    nvs_close(h);
    if (err != ESP_OK)
    {
        return err;
    }
    if (len != sizeof(*b))
    {
        /* 长度不匹配 → 旧固件写的不同尺寸，按"没存过"处理，避免读出半截 */
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/**
 * @brief 写入"完整设备配置"blob 并提交
 */
static esp_err_t dev_cfg_write(const webconfig_dev_cfg_t* b)
{
    nvs_handle_t h = 0;

    esp_err_t err = nvs_open(WEBCONFIG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "打开 NVS 命名空间 %s 失败: %s", WEBCONFIG_NVS_NS, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, WEBCONFIG_NVS_KEY_DEV, b, sizeof(*b));
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

/* ===================== 配网收尾：停服务器 / 延迟应用 ===================== */

/**
 * @brief 停掉 HTTP 服务器并释放句柄（幂等）
 */
static void httpd_stop_server(void)
{
    if (s_server != NULL)
    {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

/**
 * @brief 退出配网状态：服务器留不留由 cfg.keep_httpd 决定
 *
 * @note 保留服务器时它继续以"管理页"的身份跑在 STA 网段上：设备连上路由器后
 *       用户仍能打开页面改配置，下次 webconfig_start() 也会直接复用它。
 */
static void leave_config_mode(void)
{
    if (!s_last_cfg.keep_httpd)
    {
        httpd_stop_server();
    }
    s_running = false;
}

/**
 * @brief 延迟应用"完整设备配置"定时器回调：把保存的 dev_apply 真正推给应用层
 *
 * @note 与旧的 sta_apply 路径用的不是一个 timer——sta_apply 老路径已退役
 *       （/save 全面切到 dev_apply 之后没人再调它，留着只会留下死代码）。
 *
 * @note dev_apply 不强制 leave_config_mode：调用方（main.c 的 app_dev_apply）
 *       自己决定要不要切模式；本组件已经把 HTTP 响应发出去了，
 *       网络 / 服务的生杀大权交给应用层。
 */
static void dev_apply_timer_cb(void* arg)
{
    (void)arg;

    /* 先退出配网状态再切网络：和旧 sta_apply 路径的顺序一致，
     * 避免"还在配网中又调了 dev_apply"的中间态被任何代码看到。 */
    leave_config_mode();

    if (s_last_cfg.port.dev_apply == NULL)
    {
        return;
    }

    const esp_err_t err = s_last_cfg.port.dev_apply(&s_pending_dev, s_last_cfg.ctx);
    if (err != ESP_OK)
    {
        /* 落盘已成功，应用层只是决定不立即生效——按"用户能接受"对待 */
        ESP_LOGW(TAG, "dev_apply 报告 %s（配置已保存）", esp_err_to_name(err));
    }
}

/**
 * @brief 启动（必要时先创建）延迟应用定时器；失败返回错误码
 */
static esp_err_t dev_apply_timer_start(void)
{
    if (s_dev_apply_timer == NULL)
    {
        const esp_timer_create_args_t args = {
            .callback = &dev_apply_timer_cb,
            .name = "webcfg_dev_apply",
            .skip_unhandled_events = true,
        };
        esp_err_t err = esp_timer_create(&args, &s_dev_apply_timer);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    /* 对已运行的一次性定时器再 start 会重启计时，重复提交直接覆盖 */
    return esp_timer_start_once(s_dev_apply_timer, WEBCONFIG_APPLY_DELAY_US);
}

/* ===================== 连接失败自愈（由应用层通知驱动） =====================
 * 组件不监听任何无线事件：应用层收到"连上/断开"后调下面两个函数告诉这里即可。 */

void webconfig_notify_sta_connected(void)
{
    s_fail_count = 0;
    s_watch_start_us = 0; /* 连上了，退出观察期 */
}

void webconfig_notify_sta_disconnected(void)
{
    if (s_watch_start_us == 0)
    {
        return; /* 不在观察期：这是正常工作期间的断线，交给无线栈自己重连 */
    }

    if (esp_timer_get_time() - s_watch_start_us > WEBCONFIG_FAIL_WATCH_US)
    {
        s_watch_start_us = 0; /* 观察期结束，剩下的交给无线栈的重连 */
        return;
    }

    if (++s_fail_count >= WEBCONFIG_FAIL_LIMIT)
    {
        s_fail_count = 0;
        s_watch_start_us = 0;
        ESP_LOGW(TAG, "连续 %d 次连接失败（凭证可能有误），重新开启配网热点", WEBCONFIG_FAIL_LIMIT);
        webconfig_start(&s_last_cfg);
    }
}

void webconfig_set_comm_state(bool online)
{
    if (s_comm_online != online)
    {
        s_comm_online = online;
        ESP_LOGD(TAG, "通讯在线状态: %s", online ? "online" : "offline");
    }
}

void webconfig_set_app_version(const char* ver)
{
    if (ver == NULL || ver[0] == '\0')
    {
        s_app_version[0] = '\0';
        return;
    }
    strlcpy(s_app_version, ver, sizeof(s_app_version));
}

void webconfig_set_fw_version(const char* ver)
{
    if (ver == NULL || ver[0] == '\0')
    {
        s_fw_version[0] = '\0';
        return;
    }
    strlcpy(s_fw_version, ver, sizeof(s_fw_version));
}

/* ===================== HTTP 处理函数 ===================== */

/**
 * @brief 回一个 JSON 错误：状态码 + {"ok":false,"error":"..."}
 *
 * @note msg 都是本文件里的固定文案（不含引号等需要转义的字符），直接拼进 JSON 即可。
 */
static esp_err_t send_json_error(httpd_req_t* req, const char* status, const char* msg)
{
    /* 错误响应体也是 JSON，统一交给 page 模块生成（cJSON 在那里），
     * 这样 webconfig.c 不需要 include cJSON.h */
    char json[WEBCONFIG_JSON_MAX];
    const int n = webconfig_page_error_json(json, sizeof(json), msg);

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json, (ssize_t)n);
}

/**
 * @brief GET /：配网页面（静态 HTML，直接从 flash 发出去）
 *
 * @note 页面里没有任何需要服务端替换的占位符：SSID 回显这类动态内容由页面内联
 *       JS 通过 GET /api/config 拿到后自己填，所以改页面完全不用碰 C 代码。
 */
static esp_err_t wc_page_get(httpd_req_t* req)
{
    const char* html = NULL;
    size_t len = 0;
    webconfig_page_index(&html, &len);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, (ssize_t)len);
}

/* forward decl：fill_device_name 的实现见 /api/config 一节，但
 * wc_status_get 也会用它做"NVS 没存过时给个 Kconfig 默认名" */
static void fill_device_name(char* dst, size_t dst_sz);

/**
 * @brief 把 esp_ip4_addr_t 转成 "a.b.c.d" 字符串。buf 不够时截断并保证 '\0' 结尾。
 */
static void ipv4_to_str(const esp_ip4_addr_t* addr, char* out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) return;
    if (addr == NULL) { out[0] = '\0'; return; }
    /* esp_ip4_addr_t 在不同 IDF 版本里有时是 esp_ip4_addr_t 自己、有时是
     * 嵌套在 union 里；esp_ip4addr_ntoa() 内部已经处理过这些差异，直接传 addr 即可 */
    esp_ip4addr_ntoa(addr, out, (int)out_sz);
}

/**
 * @brief 拿 wifi + service 运行时数据填到 rt（IP / RSSI / 协议模式）
 *
 * 任何字段拿不到就留空串 / 0 —— page 函数会按"未知"处理，前端展示 "—"
 *
 * wifi netif 拿法说明：ESP-IDF 5.x 里 `esp_netif_get_handle_from_ifkey()`
 * 按 ifkey 拿 netif handle；WIFI_STA_DEF / WIFI_AP_DEF 是 netif 默认名。
 * 这俩 netif 由 wifi 组件在 wifi_config_init() 里 create_default_wifi_* 创建。
 */
static void collect_running_runtime(webconfig_dev_running_t* rt)
{
    memset(rt, 0, sizeof(*rt));

    /* ---- STA netif 实际 IP / 掩码 / 网关 / DNS / RSSI / 连接状态 ---- */
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif != NULL)
    {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK)
        {
            ipv4_to_str(&ip.ip,     rt->sta_ip,   sizeof(rt->sta_ip));
            ipv4_to_str(&ip.netmask, rt->sta_mask, sizeof(rt->sta_mask));
            ipv4_to_str(&ip.gw,     rt->sta_gw,   sizeof(rt->sta_gw));
        }
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
        {
            ipv4_to_str(&dns.ip.u_addr.ip4, rt->sta_dns, sizeof(rt->sta_dns));
        }
    }
    wifi_ap_record_t ap_rec;
    if (esp_wifi_sta_get_ap_info(&ap_rec) == ESP_OK)
    {
        rt->sta_connected = true;
        rt->sta_rssi      = ap_rec.rssi;
    }

    /* ---- AP netif 实际 IP + 客户端数 ---- */
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif != NULL)
    {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK)
        {
            ipv4_to_str(&ip.ip, rt->ap_ip, sizeof(rt->ap_ip));
        }
        wifi_sta_list_t list;
        memset(&list, 0, sizeof(list));
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK)
        {
            rt->ap_sta_count = list.num;
        }
    }

    /* ---- comm 运行时：service_manager 当前实际跑的协议（覆盖上面暂存的 wifi 模式） ---- */
    const service_mode_t svc = service_manager_get_active();
    const char* proto_s =
        (svc == SERVICE_MODE_TCP_SERVER) ? "tcpServer" :
        (svc == SERVICE_MODE_TCP_CLIENT) ? "tcpClient" :
        (svc == SERVICE_MODE_UDP)        ? "udp"       : "none";
    strncpy(rt->comm_proto, proto_s, sizeof(rt->comm_proto) - 1);
    rt->comm_proto[sizeof(rt->comm_proto) - 1] = '\0';

    /* TCP 客户端当前是否连上：用 *_is_running() 表示"协议在跑"；
     * 真正"已连上对端"目前没法直接探测（service_manager 没暴露 fd），
     * 暂用 is_running 代替——配在 tcpClient 模式下 is_running=true 通常
     * 就意味着已建连（重连退避时 is_running=false）。 */
    rt->tcp_client_connected = tcp_client_is_running();
}

/**
 * @brief GET /api/status：版本、启动时长、当前模式、在线状态、运行中配置
 *
 * @note 这条路由独立于配网：即便没在配网也可以用来查看设备状态。
 *       响应体里 fw / app / uptime / mode / state 给页头芯片每秒拉一次用；
 *       running.* 块给右栏"设备当前配置"区域用，1 秒刷新一次。
 */
static esp_err_t wc_status_get(httpd_req_t* req)
{
    /* ---- 1) 顶层元信息 ---- */
    const char* fw_ver  = s_fw_version[0] ? s_fw_version : "";
    const char* app_ver = s_app_version[0] ? s_app_version : "";
    const int64_t uptime_us = esp_timer_get_time();
    const int uptime_s       = (int)(uptime_us / 1000000);

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    const char* mode_s =
        (mode == WIFI_MODE_AP)  ? "ap"  :
        (mode == WIFI_MODE_STA) ? "sta" : "none";
    const char* state_s = s_comm_online ? "online" : "offline";

    /* ---- 2) 运行时快照 ---- */
    webconfig_dev_running_t rt;
    collect_running_runtime(&rt);

    /* ---- 3) NVS 配置（首启可能没有）---- */
    webconfig_dev_cfg_t dev;
    bool have_dev = webconfig_load_dev_cfg(&dev);
    if (!have_dev)
    {
        memset(&dev, 0, sizeof(dev));
        fill_device_name(dev.device_name, sizeof(dev.device_name));
    }

    /* ---- 4) page 层拼 running 子树 JSON ---- */
    char running_str[1024];
    webconfig_page_running_json(running_str, sizeof(running_str), have_dev ? &dev : NULL, &rt);

    /* ---- 5) 整棵树 cJSON 拼装 ---- */
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(root, "fw",       fw_ver);
    cJSON_AddStringToObject(root, "app",      app_ver);
    cJSON_AddNumberToObject(root, "uptime",   (double)uptime_s);
    cJSON_AddStringToObject(root, "mode",     mode_s);
    cJSON_AddStringToObject(root, "state",    state_s);

    /* 解析 running_str，把"network"+"communication"挂到 root.running 下 */
    cJSON* rt_root = cJSON_Parse(running_str);
    if (rt_root != NULL)
    {
        /* page 函数输出就是 {network, communication} 两块，
         * 直接把 rt_root 整个挂到 root.running 下（所有权转移，不需要 Duplicate） */
        cJSON_AddItemToObject(root, "running", rt_root);
    }

    char json[2048];
    const cJSON_bool ok = cJSON_PrintPreallocated(root, json, (int)sizeof(json), 0);
    cJSON_Delete(root);
    if (!ok) return send_json_error(req, "500 Internal Server Error", "status json overflow");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json, (ssize_t)strlen(json));
}

/**
 * @brief 把 CONFIG_DEVICE_NAME_DEFAULT 解码为 UTF-8 写到 out。
 *
 * 背景：Kconfiglib menuconfig 的 TUI 不支持非 ASCII 输入，中文会被编码成
 * UTF-16 代理对写回 sdkconfig，触发 'surrogates not allowed' 报错。
 * 绕开方法：menuconfig 里写 UTF-8 HEX（C 端按 HEX 解码为 UTF-8）；
 * 纯 ASCII 直接用。
 *
 * 判别规则（满足即按 HEX 解码）：
 *   - 长度 > 0 且为偶数
 *   - 每个字符都属于 [0-9A-Fa-f]
 * 不满足时按字面量复制。
 *
 * @param src  Kconfig 字符串（ASCII 或 UTF-8 HEX）
 * @param out  解码后的 UTF-8 字符串（以 '\0' 结尾）
 * @param out_sz out 缓冲字节数；至少留 1 字节给 '\0'
 */
static void decode_device_name(const char* src, char* out, size_t out_sz)
{
    if (out == NULL || out_sz == 0)
    {
        return;
    }
    out[0] = '\0';

    if (src == NULL)
    {
        return;
    }

    const size_t in_len = strlen(src);
    if (in_len == 0)
    {
        return;
    }

    /* 试探是否全 HEX 字符 + 偶数长度 */
    bool is_hex = (in_len % 2 == 0);
    for (size_t i = 0; is_hex && i < in_len; ++i)
    {
        const char c = src[i];
        is_hex = (c >= '0' && c <= '9') ||
                 (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F');
    }

    if (is_hex)
    {
        /* HEX → UTF-8。需要 (in_len/2 + 1) 字节，至少留 1 字节给 '\0' */
        const size_t need = in_len / 2 + 1;
        if (need > out_sz)
        {
            /* 缓冲不够：截断解码（按 HEX 一字节一字节写），
             * 仍保证 '\0' 结尾，避免上层 strlen 越界 */
            const size_t cap = (out_sz > 0) ? (out_sz - 1) : 0;
            for (size_t i = 0; i < cap; ++i)
            {
                unsigned int byte = 0;
                if (sscanf(src + (i * 2), "%2x", &byte) != 1)
                {
                    break;
                }
                out[i] = (char)byte;
            }
            out[cap] = '\0';
            return;
        }

        for (size_t i = 0; i < in_len; i += 2)
        {
            unsigned int byte = 0;
            if (sscanf(src + i, "%2x", &byte) != 1)
            {
                out[0] = '\0';
                return;
            }
            out[i / 2] = (char)byte;
        }
        out[in_len / 2] = '\0';
        return;
    }

    /* 非 HEX：按字面量当 ASCII 复制 */
    strlcpy(out, src, out_sz);
}

/* ===================== /api/config 的设备名兜底 =====================
 * Kconfig 里只有默认设备名，NVS 里可能存着用户改过的——优先用 NVS 里的值；
 * NVS 还没有（首次打开 / 重置）时再用 Kconfig 默认。
 * 返回的是写入 webconfig_dev_cfg_t.device_name 的字符串，不在外面再做拷贝。 */
static void fill_device_name(char* dst, size_t dst_sz)
{
    webconfig_dev_cfg_t dev;
    if (webconfig_load_dev_cfg(&dev) && dev.device_name[0] != '\0')
    {
        /* NVS 已有用户填的名字（含 UTF-8 中文） */
        strlcpy(dst, dev.device_name, dst_sz);
        return;
    }

    /* NVS 还没有 → 退回 Kconfig 默认（可能为 UTF-8 HEX 写法） */
    decode_device_name(CONFIG_DEVICE_NAME_DEFAULT, dst, dst_sz);
}

/**
 * @brief GET /api/config：把设备已保存的全量配置交给前端 JS
 *
 * NVS 没存过时也填一份"出厂默认"——保证 JSON 始终是 self-contained 的，
 * 前端拿到任何字段都不需要再 fallback 到 JS 端的硬编码默认值。
 * 之前是 memset(0) 然后让前端 fallback，但前端 fallback 有几条路
 * （wifiMode / sta.mode / protocol / heartbeat.format）漏一个就
 * 会出"输入框空、seg 按钮也没选中"的尴尬状态；现在 C 端直接填好。
 */
static esp_err_t wc_config_get(httpd_req_t* req)
{
    webconfig_dev_cfg_t dev;
    memset(&dev, 0, sizeof(dev));
    if (webconfig_load_dev_cfg(&dev))
    {
        /* 读到了 NVS 内容 */
    }
    else
    {
        /* 没存过：填一份出厂默认，JSON 出来就是一份"完整 + 合法"配置。 */
        fill_device_name(dev.device_name, sizeof(dev.device_name));
        strlcpy(dev.wifi_mode, "ap", sizeof(dev.wifi_mode));
        strlcpy(dev.ap_ip, "192.168.4.1", sizeof(dev.ap_ip));
        strlcpy(dev.sta_ip_mode, "dhcp", sizeof(dev.sta_ip_mode));
        strlcpy(dev.comm_protocol, "tcpServer", sizeof(dev.comm_protocol));
        dev.tcp_server_port       = 8080;
        dev.tcp_client_server_port = 8080;
        dev.udp_port              = 8080;
        strlcpy(dev.tcp_client_hb_format, "string", sizeof(dev.tcp_client_hb_format));
        dev.tcp_client_hb_interval = 30;
    }

    char json[WEBCONFIG_JSON_MAX];
    const int n = webconfig_page_config_json(json, sizeof(json), &dev);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json, (ssize_t)n);
}

/* ===================== /save 的业务校验 =====================
 * 业务规则（wifi 合法 / 端口范围 / dhcp vs static）不属于 webconfig_page 那一层
 * （它只保证"前端给的东西我们读得出来"），所以放在这里：
 *   - wifi 部分交给 wifi 组件的 wifi_update_config() 校验（在 dev_apply 里做）；
 *   - 端口范围、协议名这类简单规则在 /save 这里就挡，省得一路跑到应用层才拒。
 * 校验失败统一 send_json_error() 回 400。 */

/* 把字符串转成"小写非空字母 + 数字 + 下划线"开头的 token 校验；只允许 1~15 个字符 */
static bool is_safe_token(const char* s, size_t max_len)
{
    if (s == NULL || s[0] == '\0')
    {
        return false;
    }
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p != '\0'; ++p)
    {
        if (n >= max_len)
        {
            return false;
        }
        const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                        (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
        if (!ok)
        {
            return false;
        }
        ++n;
    }
    return n > 0;
}

static esp_err_t validate_comm(const webconfig_dev_cfg_t* cfg)
{
    if (!is_safe_token(cfg->comm_protocol, sizeof(cfg->comm_protocol) - 1))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(cfg->comm_protocol, "tcpServer") == 0)
    {
        if (cfg->tcp_server_port == 0)
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(cfg->comm_protocol, "tcpClient") == 0)
    {
        if (cfg->tcp_client_server_ip[0] == '\0' || cfg->tcp_client_server_port == 0)
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (strcmp(cfg->comm_protocol, "udp") == 0)
    {
        if (cfg->udp_port == 0)
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    else
    {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/**
 * @brief POST /save：partial 解析 -> 校验 -> 存 NVS -> 回 JSON -> 延迟 dev_apply
 *
 * @note "partial" 语义：JSON 里没出现的字段 = 不动，保留 NVS 现状。
 *       实现：先把 NVS 现状读到 cfg，再用 parse_save_partial() 覆盖 JSON
 *       里有的字段。这样"只改了设备名点保存"不会把 NVS 里的 wifi / 通讯
 *       抹成空。前端每个 panel 头部那个"保存本模块"按钮正是利用这个语义。
 *
 * @note 响应体只回 {"ok":true}，不重复回传保存内容——前端按成功后会自己 GET
 *       /api/config 拉最新值，避免一次提交产生两份数据要保持一致。
 */
static esp_err_t wc_save_post(httpd_req_t* req)
{
    char body[WEBCONFIG_JSON_BODY_MAX + 1];

    /* ---- 读请求体（Content-Length 由框架填好） ---- */
    const size_t total = req->content_len;
    if (total > WEBCONFIG_JSON_BODY_MAX)
    {
        return send_json_error(req, "400 Bad Request", "请求体过大");
    }

    size_t got = 0;
    while (got < total)
    {
        const int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0)
        {
            return ESP_FAIL; /* 连接中断，响应由框架收尾 */
        }
        got += (size_t)r;
    }
    body[got] = '\0';

    /* ---- 1) 先把 NVS 现状读到 cfg（partial 解析的"兜底"）
     *      NVS 没存过时 webconfig_load_dev_cfg() 返回 false 且不改 out
     *      —— 所以默认走 memset 0，行为跟之前的"全量提交"一致。 */
    webconfig_dev_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    (void)webconfig_load_dev_cfg(&cfg);

    /* ---- 2) partial 解析：JSON 缺字段 → cfg 不动，提交了的字段值覆盖 ---- */
    if (!webconfig_page_parse_save_partial(body, &cfg))
    {
        return send_json_error(req, "400 Bad Request", "请求体不是合法 JSON");
    }

    /* ---- 3) wifi 业务规则 ----
     * 校验的语义："最终 cfg 里这些字段必须是合法值"。
     *   - partial 提交了 wifiMode / ssid / password → 这里看到的就是新值
     *   - partial 没提交这些字段 → cfg 里是 NVS 现状（已合法过或从未配过）
     *
     * "从未配过"的兜底：用户首启直接点"设备"模块的"保存"，wifi 字段
     * 全是空 → 拒收，提示去配网络/通讯。避免 NVS 残留"半空"的不可用状态。 */
    if (!is_safe_token(cfg.wifi_mode, sizeof(cfg.wifi_mode) - 1) ||
        (strcmp(cfg.wifi_mode, "ap") != 0 && strcmp(cfg.wifi_mode, "sta") != 0))
    {
        return send_json_error(req, "400 Bad Request",
                               "网络模块必填：network.wifiMode 必须是 ap 或 sta；"
                               "如未配过请先到「网络」标签页提交");
    }

    if (strcmp(cfg.wifi_mode, "sta") == 0)
    {
        if (cfg.sta_ssid[0] == '\0')
        {
            return send_json_error(req, "400 Bad Request", "STA 模式的热点名称不能为空");
        }
        const size_t pwd_len = strlen(cfg.sta_password);
        if (pwd_len > 0 && pwd_len < 8)
        {
            return send_json_error(req, "400 Bad Request", "STA 密码留空表示开放网络，非空需 8~63 位");
        }
    }
    else /* ap */
    {
        if (cfg.ap_ssid[0] == '\0')
        {
            return send_json_error(req, "400 Bad Request", "AP 模式的热点名称不能为空");
        }
        const size_t pwd_len = strlen(cfg.ap_password);
        if (pwd_len > 0 && pwd_len < 8)
        {
            return send_json_error(req, "400 Bad Request", "AP 密码留空表示开放热点，非空需 8~63 位");
        }
    }

    /* ---- 4) 通讯参数校验 ---- */
    if (validate_comm(&cfg) != ESP_OK)
    {
        return send_json_error(req, "400 Bad Request",
                               "通讯模块必填：communication.protocol 必须是 tcpServer / tcpClient / udp 之一，"
                               "并填齐对应端口 / 服务端地址；如未配过请先到「通讯」标签页提交");
    }

    /* ---- 5) 落盘：完整设备配置写进 NVS，"dev" key ---- */
    esp_err_t err = webconfig_save_dev_cfg(&cfg);
    if (err != ESP_OK)
    {
        return send_json_error(req, "500 Internal Server Error", "保存失败");
    }

    /* ---- 回 JSON 给前端；dev_apply 推给运行层延后 2 秒，让响应先送达 ---- */
    char json[WEBCONFIG_JSON_MAX];
    const int n = webconfig_page_saved_json(json, sizeof(json));

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    err = httpd_resp_send(req, json, (ssize_t)n);

    /* 准备 dev_apply 用的快照（dev_apply 在 2s 后才会读） */
    s_pending_dev = cfg;

    if (dev_apply_timer_start() != ESP_OK)
    {
        /* 定时器建不起来就直接调：响应可能收不到，但配置已经保存 */
        dev_apply_timer_cb(NULL);
    }

    return err;
}

/* ===================== 大响应发送兜底：EAGAIN 重试 =====================
 * 本 IDF 版本的 httpd 内部 httpd_send_all 对 send() 返回 -1 一律立即判失败，
 * 不区分 errno：页面涨到几十 KB 后，LWIP 的 TCP 发送缓冲（十几 KB）先被塞满、
 * 浏览器还没收走时非阻塞 send() 就会回 EAGAIN(11)，表现为
 *   httpd_sock_err: error in send : 11 + uri handler execution failed
 * 页面彻底打不开（老页面只有 12KB、侥幸塞得下）。
 *
 * 这里按 socket 覆写 send_fn：EAGAIN 时等 socket 可写再重试，最多等 10 秒；
 * 超时或真错误（对端断开）仍返回 -1，交回框架收尾。 */
#define WEBCONFIG_SEND_TIMEOUT_US (10LL * 1000 * 1000)

static int wc_send_fn(httpd_handle_t hd, int sockfd, const char* buf, size_t buf_len, int flags)
{
    (void)hd;
    const int64_t deadline_us = esp_timer_get_time() + WEBCONFIG_SEND_TIMEOUT_US;

    size_t sent = 0;
    while (sent < buf_len)
    {
        const int n = send(sockfd, buf + sent, buf_len - sent, flags);
        if (n > 0)
        {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            return -1; /* 真错误（对端断开等）：交给框架收尾 */
        }

        if (esp_timer_get_time() > deadline_us)
        {
            return -1; /* 客户端太久收不走，放弃这条响应 */
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sockfd, &wfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        if (select(sockfd + 1, NULL, &wfds, NULL, &tv) < 0)
        {
            return -1;
        }
        /* select 返回 0（一秒内仍不可写）就回循环重查 deadline，继续等 */
    }
    return (int)sent;
}

/**
 * @brief 每个新连接建立时把上面的 send_fn 装上去
 */
static esp_err_t wc_open_fn(httpd_handle_t hd, int sockfd)
{
    return httpd_sess_set_send_override(hd, sockfd, wc_send_fn);
}

/* ===================== 对外接口 ===================== */

/**
 * @brief 检查调用方传进来的配置：port 三个回调都必须实现
 */
static esp_err_t cfg_check(const webconfig_cfg_t* cfg)
{
    if (cfg == NULL)
    {
        ESP_LOGE(TAG, "cfg 不能为 NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->port.ap_start == NULL || cfg->port.sta_apply == NULL || cfg->port.dev_apply == NULL)
    {
        ESP_LOGE(TAG, "port 回调没填全：ap_start / sta_apply / dev_apply 都必须实现");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/**
 * @brief 确保 HTTP 服务器与三个路由已就绪（幂等）
 *
 * @note 只在第一次调用时真正 httpd_start() + 注册路由：配网结束后若保留了服务器，
 *       它会在 STA 模式下继续跑，之后"重新配网"再进来时直接复用它。
 */
static esp_err_t ensure_httpd(void)
{
    if (s_server != NULL)
    {
        return ESP_OK;
    }

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.stack_size = WEBCONFIG_HTTPD_STACK;
    httpd_cfg.open_fn = &wc_open_fn; /* 每个连接装上 EAGAIN 重试的 send（见 wc_send_fn） */

    esp_err_t err = httpd_start(&s_server, &httpd_cfg);
    if (err != ESP_OK)
    {
        s_server = NULL;
        ESP_LOGE(TAG, "启动 HTTP 服务器失败: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t page_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = &wc_page_get,
        .user_ctx = NULL,
    };
    const httpd_uri_t config_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = &wc_config_get,
        .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = &wc_status_get,
        .user_ctx = NULL,
    };
    const httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = &wc_save_post,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_server, &page_uri);
    httpd_register_uri_handler(s_server, &config_uri);
    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &save_uri);
    return ESP_OK;
}

esp_err_t webconfig_httpd_start(const webconfig_cfg_t* cfg)
{
    esp_err_t err = cfg_check(cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    err = ensure_httpd();
    if (err != ESP_OK)
    {
        return err;
    }

    /* 记下配置：页面上提交凭证后要用 sta_apply 切网络，失败自愈时还要用 ap_start */
    s_last_cfg = *cfg;

    ESP_LOGI(TAG, "管理页已就绪（未开配网热点），可在设备当前网络的 IP 上访问");
    return ESP_OK;
}

esp_err_t webconfig_start(const webconfig_cfg_t* cfg)
{
    esp_err_t err = cfg_check(cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    if (s_running)
    {
        return ESP_OK; /* 已在配网中，幂等 */
    }

    /* 若正好有待应用的定时器（刚提交完又马上手动重开），取消它 */
    if (s_dev_apply_timer != NULL)
    {
        (void)esp_timer_stop(s_dev_apply_timer);
    }

    /* ---- 1. 把"开热点"交给应用层：那份实现必须可重复调用（连接失败自愈时会
     *        带着同一份参数再开一次），热点参数的校验也在里面做，非法就返回
     *        ESP_ERR_INVALID_ARG ---- */
    err = cfg->port.ap_start(&cfg->ap, cfg->ctx);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "开启配网热点失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 2. HTTP 服务器（可能已经在跑：keep_httpd 保留下来的那个管理页） ---- */
    err = ensure_httpd();
    if (err != ESP_OK)
    {
        return err;
    }

    /* ---- 3. 记下本次配置：连接失败自愈时要复用热点参数与两个回调 ---- */
    s_last_cfg = *cfg;
    s_running = true;

    ESP_LOGI(TAG, "配网热点已开启: SSID=%s 信道=%u，连接后访问 http://192.168.4.1/", cfg->ap.ssid,
             (unsigned)cfg->ap.channel);
    return ESP_OK;
}

esp_err_t webconfig_stop(void)
{
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_dev_apply_timer != NULL)
    {
        (void)esp_timer_stop(s_dev_apply_timer);
    }

    /* 主动关闭配网：服务器一并停掉（不留作管理页），"关"就是关干净 */
    httpd_stop_server();
    s_running = false;

    /* 恢复成"用已保存的设备配置运行"：NVS 里有 dev 配置就调 dev_apply 把它推给
     * 运行层（wifi_update_config + service_manager_request_switch）。
     * 没存过就维持热点（设备无事可做）。 */
    webconfig_dev_cfg_t dev;
    if (webconfig_load_dev_cfg(&dev))
    {
        return s_last_cfg.port.dev_apply(&dev, s_last_cfg.ctx);
    }
    return ESP_OK;
}

bool webconfig_is_running(void)
{
    return s_running;
}

httpd_handle_t webconfig_get_httpd(void)
{
    /* 即便 s_running 为 false（已经退出配网但 keep_httpd 保留服务器），
     * s_server 也可能仍然指向有效句柄 —— getter 把"是否还有可用 HTTP 服务器"
     * 这件事收口在一处，让 service_manager 这种外部使用者不用关心配网生命周期。 */
    return s_server;
}

bool webconfig_load_sta_cfg(webconfig_sta_cfg_t* out)
{
    if (out == NULL)
    {
        return false;
    }

    if (nvs_ensure_init() != ESP_OK)
    {
        return false;
    }

    webconfig_sta_cfg_t blob;
    if (sta_cfg_read(&blob) != ESP_OK || blob.ssid[0] == '\0')
    {
        return false;
    }

    *out = blob;
    return true;
}

esp_err_t webconfig_save_sta_cfg(const char* ssid, const char* password)
{
    if (ssid == NULL || password == NULL)
    {
        ESP_LOGE(TAG, "ssid / password 不能为 NULL");
        return ESP_ERR_INVALID_ARG;
    }

    const size_t ssid_len = strlen(ssid);
    const size_t pwd_len = strlen(password);
    if (ssid_len == 0 || ssid_len > 31)
    {
        ESP_LOGE(TAG, "ssid 长度非法(1~31)");
        return ESP_ERR_INVALID_ARG;
    }
    if (pwd_len > 63 || (pwd_len > 0 && pwd_len < 8))
    {
        ESP_LOGE(TAG, "密码长度非法(留空表示开放网络，否则 8~63 位)");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_ensure_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    webconfig_sta_cfg_t blob;
    memset(&blob, 0, sizeof(blob));
    memcpy(blob.ssid, ssid, ssid_len);
    memcpy(blob.password, password, pwd_len);

    return sta_cfg_write(&blob);
}

esp_err_t webconfig_erase_sta_cfg(void)
{
    esp_err_t err = nvs_ensure_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t h = 0;
    err = nvs_open(WEBCONFIG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "打开 NVS 命名空间 %s 失败: %s", WEBCONFIG_NVS_NS, esp_err_to_name(err));
        return err;
    }

    /* key 不存在也按成功处理（本来就没存过） */
    err = nvs_erase_key(h, WEBCONFIG_NVS_KEY_STA);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = ESP_OK;
    }

    nvs_close(h);
    return err;
}

bool webconfig_load_dev_cfg(webconfig_dev_cfg_t* out)
{
    if (out == NULL)
    {
        return false;
    }

    if (nvs_ensure_init() != ESP_OK)
    {
        return false;
    }

    webconfig_dev_cfg_t blob;
    if (dev_cfg_read(&blob) != ESP_OK)
    {
        return false;
    }

    *out = blob;
    return true;
}

esp_err_t webconfig_save_dev_cfg(const webconfig_dev_cfg_t* cfg)
{
    if (cfg == NULL)
    {
        ESP_LOGE(TAG, "cfg 不能为 NULL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_ensure_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    return dev_cfg_write(cfg);
}

esp_err_t webconfig_erase_dev_cfg(void)
{
    esp_err_t err = nvs_ensure_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t h = 0;
    err = nvs_open(WEBCONFIG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "打开 NVS 命名空间 %s 失败: %s", WEBCONFIG_NVS_NS, esp_err_to_name(err));
        return err;
    }

    /* key 不存在也按成功处理（本来就没存过） */
    err = nvs_erase_key(h, WEBCONFIG_NVS_KEY_DEV);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = ESP_OK;
    }

    nvs_close(h);
    return err;
}
