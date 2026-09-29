/*
 * service_manager.c —— "当前跑哪个网络服务"的统一管理实现
 *
 * 三层决定运行哪个服务（按优先级降序）：
 *   1) 运行时 web 等渠道通过 service_manager_request_switch() 设的 pending；
 *   2) NVS 里"上次用户选的"（key = "service/mode"，type = uint8_t）；
 *   3) 都没有 → menuconfig 的 CONFIG_SERVICE_DEFAULT_MODE。
 *
 * 切换流程：旧 service worker → request_stop → 等 is_running() 变 false（带超时）→
 *           新 service worker → xTaskCreate。
 * 整套由 manager 任务独占做，避免多任务并发启停同一个 socket 端口。
 *
 * NVS 写时机：每次切换成功后立刻写；启动期读一次就够常驻值。
 * HTTP 接口：
 *   GET  /api/service-mode    -> {"active":<n>,"allowed":[...]}  (n 是当前模式枚举值)
 *   POST /api/service-mode    -> body: {"mode":<n>}              (n 是目标模式枚举值)
 *   两个 URI 都注册到 webconfig 已经开好的 HTTP 服务器上（用 webconfig_get_httpd() 拿句柄）。
 */

#include "service_manager.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "webconfig.h" /* webconfig_get_httpd()：拿现成的 HTTP 服务器句柄 */

#include "sdkconfig.h" /* menuconfig 的 CONFIG_* 都来自这里 */

/* 日志标签 */
static const char* TAG = "svc_mgr";

/* 任务原型声明（在文件下面定义，start() 里 xTaskCreate 要先 ref 到） */
static void service_manager_task(void* arg);

/* NVS 布局：本组件私有的命名空间与 key */
#define NVS_NS "service"
#define NVS_KEY_MODE "mode"

/* ---- 服务 ops 表 ----
 * 三种 service 的 worker / request_stop / is_running 都在 main.c 里实现，
 * 声明放在 service_manager.h。这里直接用。索引 = service_mode_t 枚举值。 */
typedef struct
{
    const char* name;
    void (*worker)(void);
    void (*request_stop)(void);
    bool (*is_running)(void);
} service_ops_t;

static const service_ops_t s_ops_table[] = {
    [SERVICE_MODE_NONE] = { .name = "none", NULL, NULL, NULL },
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
    [SERVICE_MODE_TCP_CLIENT] = { .name = "tcp_client",
                                  tcp_client_service_fn,
                                  tcp_client_request_stop,
                                  tcp_client_is_running },
#endif
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
    [SERVICE_MODE_TCP_SERVER] = { .name = "tcp_server",
                                  tcp_server_service_fn,
                                  tcp_server_request_stop,
                                  tcp_server_is_running },
#endif
#if CONFIG_SERVICE_UDP_ENABLE
    [SERVICE_MODE_UDP] = { .name = "udp", udp_service_fn, udp_request_stop, udp_is_running },
#endif
};

/* ---- 运行期状态 ----
 * s_active_mode 当前实际在跑的；s_pending_mode 是"切到这里"的请求位。
 * manager 每轮 100ms 看一眼，二者不同就执行切换流程。 */
static volatile service_mode_t s_active_mode = SERVICE_MODE_NONE;
static volatile service_mode_t s_pending_mode = SERVICE_MODE_NONE;

/* ---- 工具 ---- */

const char* service_mode_to_str(service_mode_t m)
{
    if (m < 0 || m >= (int)(sizeof(s_ops_table) / sizeof(s_ops_table[0])))
    {
        return "?";
    }
    return s_ops_table[m].name ? s_ops_table[m].name : "?";
}

/* 某个模式在 menuconfig 里是否启用（决定 NVS 里读到它时能否启动） */
static bool mode_enabled_by_menuconfig(service_mode_t m)
{
#if defined(CONFIG_SERVICE_TCP_CLIENT_ENABLE) && CONFIG_SERVICE_TCP_CLIENT_ENABLE
    if (m == SERVICE_MODE_TCP_CLIENT)
    {
        return true;
    }
#endif
#if defined(CONFIG_SERVICE_TCP_SERVER_ENABLE) && CONFIG_SERVICE_TCP_SERVER_ENABLE
    if (m == SERVICE_MODE_TCP_SERVER)
    {
        return true;
    }
#endif
#if defined(CONFIG_SERVICE_UDP_ENABLE) && CONFIG_SERVICE_UDP_ENABLE
    if (m == SERVICE_MODE_UDP)
    {
        return true;
    }
#endif
    return false;
}

/* 把 menuconfig 的 CONFIG_SERVICE_DEFAULT_MODE_* 转成 service_mode_t */
static service_mode_t menuconfig_default_mode(void)
{
#if defined(CONFIG_SERVICE_DEFAULT_TCP_SERVER) && CONFIG_SERVICE_DEFAULT_TCP_SERVER
    return SERVICE_MODE_TCP_SERVER;
#elif defined(CONFIG_SERVICE_DEFAULT_UDP) && CONFIG_SERVICE_DEFAULT_UDP
    return SERVICE_MODE_UDP;
#else
    /* 默认 TCP_CLIENT 或 fallback：都尽量凑成 TCP_CLIENT
     * —— 真全部禁用时这一支也会走到，manager 会拒绝启动。 */
    return SERVICE_MODE_TCP_CLIENT;
#endif
}

/* ---- NVS ---- */

static esp_err_t nvs_load_mode(service_mode_t* out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    uint8_t v = 0;
    err = nvs_get_u8(h, NVS_KEY_MODE, &v);
    nvs_close(h);
    if (err != ESP_OK)
    {
        return err;
    }
    /* uint8_t 范围 0~255，v < 0 恒假（编译器会警告），只判上界即可 */
    if (v >= (uint8_t)(sizeof(s_ops_table) / sizeof(s_ops_table[0])))
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (service_mode_t)v;
    return ESP_OK;
}

static esp_err_t nvs_save_mode(service_mode_t m)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)m);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- HTTP /api/service-mode ----
 * GET  返回当前模式 + 菜单允许列表
 * POST {"mode":<n>} 切到指定模式
 */

static esp_err_t http_get_handler(httpd_req_t* req)
{
    /* 列出"菜单允许的所有模式"，便于前端动态渲染下拉选项：
     * "tcp_client" / "tcp_server" / "udp"，名字与后端 enum 一一对应。 */
    char allowed[96] = { 0 };
    int off = 0;
#if defined(CONFIG_SERVICE_TCP_CLIENT_ENABLE) && CONFIG_SERVICE_TCP_CLIENT_ENABLE
    off += snprintf(allowed + off, sizeof(allowed) - off, "%s\"tcp_client\"", off ? "," : "");
#endif
#if defined(CONFIG_SERVICE_TCP_SERVER_ENABLE) && CONFIG_SERVICE_TCP_SERVER_ENABLE
    off += snprintf(allowed + off, sizeof(allowed) - off, "%s\"tcp_server\"", off ? "," : "");
#endif
#if defined(CONFIG_SERVICE_UDP_ENABLE) && CONFIG_SERVICE_UDP_ENABLE
    off += snprintf(allowed + off, sizeof(allowed) - off, "%s\"udp\"", off ? "," : "");
#endif
    char body[160];
    snprintf(body, sizeof(body), "{\"active\":%d,\"active_name\":\"%s\",\"allowed\":[%s]}\r\n",
             (int)s_active_mode, service_mode_to_str(s_active_mode), allowed);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* 从字符串里抠出 "mode=" 后的数字。极简解析，不引第三方 JSON 库。
 * 形如 "mode=2&..." 或 "?mode=2"。 */
static int parse_mode_digit(const char* str)
{
    if (!str)
        return -1;
    const char* p = strstr(str, "mode=");
    if (!p)
        return -1;
    p += 5;
    int v = 0;
    if (*p < '0' || *p > '9')
        return -1;
    while (*p >= '0' && *p <= '9')
    {
        v = v * 10 + (*p - '0');
        p++;
    }
    return v;
}

/* 形如 {"mode":2} 的 JSON body 里抠数字。极简解析。 */
static int parse_mode_json(const char* str)
{
    if (!str)
        return -1;
    const char* p = strstr(str, "\"mode\"");
    if (!p)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p < '0' || *p > '9')
        return -1;
    int v = 0;
    while (*p >= '0' && *p <= '9')
    {
        v = v * 10 + (*p - '0');
        p++;
    }
    return v;
}

/* 把用户给的目标值（数字字符串）翻成 service_mode_t。返回 -1 = 无法识别。 */
static int parse_mode_value(const char* val)
{
    if (!val || !*val)
        return -1;
    /* 数字形式 */
    if (val[0] >= '0' && val[0] <= '9')
    {
        return parse_mode_digit(val);
    }
    /* 名字形式（"tcp_client" / "tcp_server" / "udp"） */
    const size_t n = sizeof(s_ops_table) / sizeof(s_ops_table[0]);
    for (size_t i = 0; i < n; i++)
    {
        if (s_ops_table[i].name && strcmp(val, s_ops_table[i].name) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t http_post_handler(httpd_req_t* req)
{
    char val[16] = { 0 };
    bool found = false;

    /* 1) 试 query string —— URL 里的 ?mode=2 */
    const size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 64)
    {
        char qbuf[64];
        if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK)
        {
            if (httpd_query_key_value(qbuf, "mode", val, sizeof(val) - 1) == ESP_OK)
            {
                found = true;
            }
        }
    }

    /* 2) 试 form body —— "mode=2&..." (Content-Type: application/x-www-form-urlencoded) */
    if (!found && req->content_len > 0 && req->content_len < 128)
    {
        char body[128] = { 0 };
        const int got = httpd_req_recv(req, body, req->content_len);
        if (got > 0)
        {
            if (httpd_query_key_value(body, "mode", val, sizeof(val) - 1) == ESP_OK)
            {
                found = true;
            }
            else if (parse_mode_json(body) >= 0)
            {
                strlcpy(val, body, sizeof(val)); /* JSON 路径让 parse_mode_value 走 json 分支 */
                found = true;
            }
        }
    }

    if (!found)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        const char* msg = "{\"ok\":false,\"err\":\"missing mode\"}\r\n";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    }

    const int v = parse_mode_value(val);
    if (v < 0 || v >= (int)(sizeof(s_ops_table) / sizeof(s_ops_table[0])) ||
        s_ops_table[v].worker == NULL)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        const char* msg = "{\"ok\":false,\"err\":\"invalid mode\"}\r\n";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    }

    service_mode_t target = (service_mode_t)v;
    if (!mode_enabled_by_menuconfig(target))
    {
        httpd_resp_set_status(req, "403 Forbidden");
        const char* msg = "{\"ok\":false,\"err\":\"mode disabled in menuconfig\"}\r\n";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = service_manager_request_switch(target);
    if (err != ESP_OK)
    {
        httpd_resp_set_status(req, "500 Internal Server Error");
        const char* msg = "{\"ok\":false,\"err\":\"save failed\"}\r\n";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    }

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"requested\":%d,\"name\":\"%s\"}\r\n", v,
             service_mode_to_str(target));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

/* 注册 URI。webconfig 的 HTTP 服务器可能还没起来，
 * 所以重试几次（间隔 200ms），最多 10 次。 */
static esp_err_t register_uris_with_retry(void)
{
    static const httpd_uri_t get_uri = {
        .uri = "/api/service-mode",
        .method = HTTP_GET,
        .handler = http_get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/api/service-mode",
        .method = HTTP_POST,
        .handler = http_post_handler,
    };
    for (int i = 0; i < 50; i++)
    {
        httpd_handle_t s = webconfig_get_httpd();
        if (s != NULL)
        {
            esp_err_t err1 = httpd_register_uri_handler(s, &get_uri);
            esp_err_t err2 = httpd_register_uri_handler(s, &post_uri);
            if (err1 == ESP_OK && err2 == ESP_OK)
            {
                ESP_LOGI(TAG, "已挂载 /api/service-mode (GET + POST)");
                return ESP_OK;
            }
            ESP_LOGW(TAG, "注册 URI 失败 (get=%d post=%d)，500ms 后再试", (int)err1, (int)err2);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGE(TAG, "等了 25s 也没拿到 HTTP 服务器，/api/service-mode 未挂载");
    return ESP_FAIL;
}

/* ---- 启停 ---- */

esp_err_t service_manager_init(void)
{
    /* 1) 决定初始 pending：NVS → menuconfig default */
    service_mode_t initial = menuconfig_default_mode();
    service_mode_t saved = SERVICE_MODE_NONE;
    esp_err_t err = nvs_load_mode(&saved);
    if (err == ESP_OK && saved != SERVICE_MODE_NONE)
    {
        ESP_LOGI(TAG, "从 NVS 读到上次选择: %s", service_mode_to_str(saved));
        if (mode_enabled_by_menuconfig(saved))
        {
            initial = saved;
        }
        else
        {
            ESP_LOGW(TAG, "NVS 里的 %s 在 menuconfig 里被禁用，回退到默认 %s",
                     service_mode_to_str(saved), service_mode_to_str(initial));
        }
    }
    else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "读 NVS 失败 (%s)，用 menuconfig 默认: %s", esp_err_to_name(err),
                 service_mode_to_str(initial));
    }

    s_pending_mode = initial;

    /* 2) 挂 HTTP endpoint（webconfig 的服务器此时可能还没起，重试） */
    err = register_uris_with_retry();
    if (err != ESP_OK)
    {
        /* 不致命 —— 切模式还能走 NVS / 别的渠道，只是 web 控制不了了 */
        ESP_LOGW(TAG, "/api/service-mode 注册失败，web 切换入口不可用");
    }
    return ESP_OK; /* init 永不"失败"，失败含义也写在调用方 */
}

void service_manager_start(void)
{
    /* 简短的栈：manager 几乎只做"查 flag + 调 ops"，不开大栈 */
    xTaskCreate(service_manager_task, "svc_mgr", 3072, NULL, 4, NULL);
}

/* manager 任务本体：100ms 一轮检查是否要切换 */
static void service_manager_task(void* arg)
{
    (void)arg;
    int watchdog = 0;
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(100));

        const service_mode_t want = s_pending_mode;
        if (want == s_active_mode && s_active_mode != SERVICE_MODE_NONE)
        {
            /* 看一眼当前模式是否真的活着 —— 异常崩溃（不应发生，但防御）
             * 也能自动重启。 */
            const service_ops_t* ops = &s_ops_table[s_active_mode];
            if (ops->is_running && !ops->is_running())
            {
                ESP_LOGW(TAG, "%s 已不在运行，主动重启", ops->name);
                s_active_mode = SERVICE_MODE_NONE;
                /* 下一次循环会触发"启动新 service" */
            }
            else
            {
                watchdog = 0;
                continue;
            }
        }

        /* --- 停旧的 --- */
        if (s_active_mode != SERVICE_MODE_NONE)
        {
            const service_ops_t* ops = &s_ops_table[s_active_mode];
            ESP_LOGI(TAG, "切换：停止 %s", ops->name);
            if (ops->request_stop)
            {
                ops->request_stop();
            }

            /* 等它真的退出（worker 设自己的 *_alive=false → vTaskDelete） */
            const int step_ms = 50;
            const int total_ms = CONFIG_SERVICE_SWITCH_TIMEOUT_MS;
            int waited = 0;
            while (ops->is_running && ops->is_running() && waited < total_ms)
            {
                vTaskDelay(pdMS_TO_TICKS(step_ms));
                waited += step_ms;
            }
            if (ops->is_running && ops->is_running())
            {
                /* 【绝不"按已停止处理"就启动新的】
                 * 旧 worker 没退出说明它还占着 socket / 任务栈：
                 *   - 实测过一次：tcp_conn_task 卡在阻塞的 connect 里没退，
                 *     这里却照样启动了 tcp_server，于是两个 service 同时运行
                 *     （tcp_server 已经 bind 成功 8888，tcp_client 还在重连），
                 *     完全违背"同一时刻只有一个 service"的设计前提。
                 *
                 * 正确做法：什么都不启动，pending 保持不变，下一轮重新
                 * request_stop + 等待。旧的一旦退出，切换会自动接着完成。
                 * 代价是极端情况下会一直卡在"切不过去" —— 但那说明旧 service
                 * 真有卡死 bug，应当去修它，而不是用并发把问题掩盖成偶发故障。 */
                ESP_LOGE(TAG, "%s 在 %d ms 内未退出，本次不切换（继续等它退出后重试）", ops->name,
                         total_ms);
                vTaskDelay(pdMS_TO_TICKS(500)); /* 别每轮 100ms 就刷一条 error */
                watchdog++;
                if (watchdog >= 10)
                {
                    /* 反复等不到：提示去查该 service 的停服路径，别再刷日志 */
                    ESP_LOGE(TAG,
                             "%s 连续 %d 次无法停止，已停止重试（请检查其停服逻辑，"
                             "最常见原因是阻塞调用没有超时）",
                             ops->name, watchdog);
                    watchdog = 0;
                    vTaskDelay(pdMS_TO_TICKS(10000));
                }
                continue; /* s_active_mode 保持原值，状态始终一致 */
            }
            ESP_LOGI(TAG, "切换：%s 已退出 (用了 %d ms)", ops->name, waited);
            s_active_mode = SERVICE_MODE_NONE;
        }

        /* --- 起新的 --- */
        if (want == SERVICE_MODE_NONE)
        {
            ESP_LOGI(TAG, "切换：目标 NONE,保持无服务状态");
            watchdog = 0;
            continue;
        }
        const service_ops_t* ops_new = &s_ops_table[want];
        if (ops_new->worker == NULL)
        {
            ESP_LOGE(TAG, "切换：%s 没有 worker(菜单未勾选)", ops_new->name);
            watchdog = 0;
            continue;
        }
        ESP_LOGI(TAG, "切换：启动 %s", ops_new->name);
        BaseType_t ok =
            xTaskCreate((void (*)(void*))ops_new->worker, ops_new->name, 4096, NULL, 5, NULL);
        if (ok != pdPASS)
        {
            ESP_LOGE(TAG, "切换：%s xTaskCreate 失败", ops_new->name);
            watchdog++;
            if (watchdog >= 3)
            {
                ESP_LOGE(TAG, "切换：连续 3 次启动 %s 失败，停止再试（避免刷日志）", ops_new->name);
                /* 把 pending 复位，让用户重新请求时再尝试 */
                /* s_pending_mode = SERVICE_MODE_NONE; */ /* 暂留 pending 以便排查 */
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            continue;
        }
        s_active_mode = want;
        watchdog = 0;

        /* 落盘：写到 NVS，重启后还能记住 */
        esp_err_t save_err = nvs_save_mode(s_active_mode);
        if (save_err != ESP_OK)
        {
            ESP_LOGW(TAG, "切换：%s 写入 NVS 失败 (%s)，下次重启会回到默认", ops_new->name,
                     esp_err_to_name(save_err));
        }
    }
}

/* ---- 公共查询 ---- */

service_mode_t service_manager_get_active(void)
{
    return s_active_mode;
}

esp_err_t service_manager_request_switch(service_mode_t m)
{
    if (m < 0 || m >= (int)(sizeof(s_ops_table) / sizeof(s_ops_table[0])) ||
        s_ops_table[m].worker == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mode_enabled_by_menuconfig(m))
    {
        ESP_LOGW(TAG, "request_switch：%s 在菜单禁用，忽略", service_mode_to_str(m));
        return ESP_ERR_INVALID_STATE;
    }
    s_pending_mode = m;
    /* NVS 立刻落盘（即便 manager 还没真正切，也保证重启后用户的选择保留） */
    esp_err_t err = nvs_save_mode(m);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "request_switch：NVS 写失败 (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "request_switch：已请求切换到 %s（manager 异步执行）", service_mode_to_str(m));
    return ESP_OK;
}
