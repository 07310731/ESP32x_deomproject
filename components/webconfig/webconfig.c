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
#include "nvs.h"
#include "nvs_flash.h" /* nvs_flash_init() / nvs_flash_erase() 在这里 */

static const char* TAG = "webconfig";

/* ===================== 内部常量 ===================== */

/* NVS 存放位置：独立命名空间，只存配网凭证一个 blob（本组件私有布局） */
#define WEBCONFIG_NVS_NS "webcfg"
#define WEBCONFIG_NVS_KEY_STA "sta"

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

/* HTTP 服务器任务栈：处理器里最大的一块是 WEBCONFIG_JSON_BODY_MAX(512B) 的请求体
 * 缓冲，4096（IDF 默认值）够用。
 * 注：页面改成由前端自己渲染之前，这里要放 2KB 的页面缓冲，所以曾调到 8192；
 * 现在页面直接从 flash 发出去、不再拷进 RAM。以后在处理器里加大缓冲记得同步调大。 */
#define WEBCONFIG_HTTPD_STACK 4096

/* ===================== 内部状态 =====================
 * NVS 里存下来的 blob 就是 webconfig_sta_cfg_t 本身，不另外定义类型。 */

static httpd_handle_t s_server = NULL; /*!< HTTP 服务器句柄，NULL 表示未启动 */
static bool s_running = false;         /*!< 是否处于配网状态 */
static webconfig_cfg_t s_last_cfg;     /*!< 最近一次 webconfig_start() 的参数，失败自愈重开时复用 */
static webconfig_sta_cfg_t s_pending_sta;       /*!< POST 保存后等定时器应用的凭证 */
static esp_timer_handle_t s_apply_timer = NULL; /*!< 延迟应用定时器，惰性创建 */

/* ---- 连接失败自愈的计数状态（见 WEBCONFIG_FAIL_WATCH_US 的说明） ---- */
static int s_fail_count = 0;
static int64_t s_watch_start_us = 0; /*!< 0 表示不在观察期（不关心断线事件） */

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
 * @brief 延迟应用定时器回调：把 POST 保存的配置真正生效
 *
 * @note 此刻 HTTP 响应早已送达，切模式关掉热点不会影响用户看到"已保存"页面。
 */
static void apply_timer_cb(void* arg)
{
    (void)arg;

    leave_config_mode();

    /* 怎么切、切成什么样都是应用层的事：这里只把凭证交出去 */
    esp_err_t err =
        s_last_cfg.port.sta_apply(s_pending_sta.ssid, s_pending_sta.password, s_last_cfg.ctx);
    if (err != ESP_OK)
    {
        /* 凭证已存 NVS，即使这次没切换成功，重启后也会走已保存配置 */
        ESP_LOGE(TAG, "应用新配置失败: %s（凭证已保存，重启后会再次尝试）", esp_err_to_name(err));
        return;
    }

    /* 进入连接失败观察期：密码填错时 5 次连不上就重开配网热点 */
    s_fail_count = 0;
    s_watch_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "新配置已应用，正在连接 %s", s_pending_sta.ssid);
}

/**
 * @brief 启动（必要时先创建）延迟应用定时器；失败返回错误码
 */
static esp_err_t apply_timer_start(void)
{
    if (s_apply_timer == NULL)
    {
        const esp_timer_create_args_t args = {
            .callback = &apply_timer_cb,
            .name = "webcfg_apply",
            .skip_unhandled_events = true,
        };
        esp_err_t err = esp_timer_create(&args, &s_apply_timer);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    /* 对已运行的一次性定时器再 start 会重启计时，重复提交直接覆盖 */
    return esp_timer_start_once(s_apply_timer, WEBCONFIG_APPLY_DELAY_US);
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

/**
 * @brief GET /api/config：把设备已保存的凭证信息交给前端 JS
 */
static esp_err_t wc_config_get(httpd_req_t* req)
{
    /* SSID 回显给页面，让用户知道设备当前配的是哪个网络 */
    webconfig_sta_cfg_t blob;
    memset(&blob, 0, sizeof(blob)); /* NVS 里的旧 blob 可能比结构体短，先清零更安全 */
    const bool has_saved = (sta_cfg_read(&blob) == ESP_OK && blob.ssid[0] != '\0');

    char json[WEBCONFIG_JSON_MAX];
    const int n = webconfig_page_config_json(json, sizeof(json), has_saved ? blob.ssid : "");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json, (ssize_t)n);
}

/**
 * @brief POST /save：解析表单 -> 校验 -> 存 NVS -> 回 JSON -> 定时器延迟切换到 STA
 *
 * @note 响应体是 JSON 而不是页面：前端 JS 拿到 {"ok":true,...} 后自己切到"已保存"
 *       视图，所以这里不用再渲染页面。失败时统一用 send_json_error() 回错误文案。
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

    /* ---- 解析并校验（长度规则遵循 802.11：SSID 1~31，密码留空或 8~63 位） ---- */
    char ssid[32] = { 0 };
    char password[64] = { 0 };

    if (!webconfig_page_parse_save(body, ssid, sizeof(ssid), password, sizeof(password)))
    {
        return send_json_error(req, "400 Bad Request", "请求体不是合法 JSON 或 ssid/password 字段缺失");
    }

    if (ssid[0] == '\0')
    {
        return send_json_error(req, "400 Bad Request", "WiFi 名称不能为空");
    }
    const size_t pwd_len = strlen(password);
    if (pwd_len > 0 && pwd_len < 8)
    {
        return send_json_error(req, "400 Bad Request", "密码留空表示开放网络，非空需 8~63 位");
    }

    /* ---- 落盘 ---- */
    esp_err_t err = webconfig_save_sta_cfg(ssid, password);
    if (err != ESP_OK)
    {
        return send_json_error(req, "500 Internal Server Error", "保存失败");
    }

    /* ---- 回 JSON 给前端；切模式的动作延后 2 秒，让响应先送达 ---- */
    char json[WEBCONFIG_JSON_MAX];
    const int n = webconfig_page_saved_json(json, sizeof(json), ssid);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    err = httpd_resp_send(req, json, (ssize_t)n);

    memset(&s_pending_sta, 0, sizeof(s_pending_sta));
    strlcpy(s_pending_sta.ssid, ssid, sizeof(s_pending_sta.ssid));
    strlcpy(s_pending_sta.password, password, sizeof(s_pending_sta.password));

    if (apply_timer_start() != ESP_OK)
    {
        /* 定时器建不起来就直接切：响应可能收不到，但凭证已经保存 */
        apply_timer_cb(NULL);
    }

    return err;
}

/* ===================== 对外接口 ===================== */

/**
 * @brief 检查调用方传进来的配置：port 两个回调都必须实现
 */
static esp_err_t cfg_check(const webconfig_cfg_t* cfg)
{
    if (cfg == NULL)
    {
        ESP_LOGE(TAG, "cfg 不能为 NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->port.ap_start == NULL || cfg->port.sta_apply == NULL)
    {
        ESP_LOGE(TAG, "port 回调没填全：ap_start / sta_apply 都必须实现");
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
    const httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = &wc_save_post,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_server, &page_uri);
    httpd_register_uri_handler(s_server, &config_uri);
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
    if (s_apply_timer != NULL)
    {
        (void)esp_timer_stop(s_apply_timer);
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

    if (s_apply_timer != NULL)
    {
        (void)esp_timer_stop(s_apply_timer);
    }

    /* 主动关闭配网：服务器一并停掉（不留作管理页），"关"就是关干净 */
    httpd_stop_server();
    s_running = false;

    /* 恢复成"用已保存的凭证连接"；没保存过就维持热点（设备无事可做） */
    webconfig_sta_cfg_t saved;
    if (webconfig_load_sta_cfg(&saved))
    {
        return s_last_cfg.port.sta_apply(saved.ssid, saved.password, s_last_cfg.ctx);
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
