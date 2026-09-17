#ifndef _WEBCONFIG_H_
#define _WEBCONFIG_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ===================== 通用 Web 配网组件 =====================
 * 组件职责：临时开一个热点 + 起 HTTP 表单页，把用户填的目标 WiFi 凭证存进 NVS。
 * 页面内容、样式与交互都收在 web/index.html 这一个文件里，页面里的动态内容由
 * 它自己的内联 JS 通过 /api/config 与 /save 两个接口读写，改页面不用动配网流程。
 *
 * 页面有两个入口（同一套页面，只是设备所处的网络不同）：
 *   - 配网中：webconfig_start() 开出配网热点，用户连热点后访问 192.168.4.1；
 *   - 已连上：webconfig_httpd_start() 只把页面挂上去（不开热点），用设备在
 *     路由器网段的 IP 访问；配合 cfg.keep_httpd = true 可长期保留。
 *
 * 与无线协议栈完全解耦——本头文件不包含任何 esp_wifi.h 之类的东西：
 *   - "开配网热点""用新凭证去连接" → 由应用层实现成回调传进来（webconfig_port_t）；
 *   - "连上了 / 断开了"            → 由应用层调 webconfig_notify_sta_*() 通知进来。
 * 因此 webconfig 不依赖 wifi 组件：换一套无线栈（ESP-NOW、其它模组）也能直接用。
 *
 * NVS 布局（命名空间 "webcfg"、键 "sta"）是本组件私有的，不与其他组件共享。
 */

    /**
     * @brief 配网热点默认名称
     *
     * @note 配网热点是开放网络（默认无密码），名字带设备标识方便用户辨认；
     *       如果部署环境不允许开放热点，给 password 填一个 8~63 位密码。
     */
#define WEBCONFIG_AP_SSID_DEFAULT "roboot_mc"

    /**
     * @brief 配网热点参数
     *
     * @note 描述的是"配网热点"本身（设备临时开出来的那个 WiFi），不是要连接的目标
     *       网络；目标网络的名称 / 密码由用户在配网页面里填写。
     *       参数合法性由 webconfig_port_t.ap_start 的实现负责校验。
     */
    typedef struct
    {
        char ssid[32];     /*!< 配网热点名称，最长 31 字符，不可为空 */
        char password[64]; /*!< 配网热点密码，留空表示开放热点；非空则 8~63 位 */
        uint8_t channel;   /*!< 配网热点信道，取值范围 1 ~ 13 */
        uint8_t max_conn;  /*!< 热点最多允许接入的客户端数，至少为 1（配网时 1~2 个足够） */
    } webconfig_ap_cfg_t;

    /**
     * @brief 默认热点参数：名称 roboot_mc、开放网络、信道 1、最多 2 个客户端
     */
#define WEBCONFIG_AP_CFG_DEFAULT()         \
    ((webconfig_ap_cfg_t){                 \
        .ssid = WEBCONFIG_AP_SSID_DEFAULT, \
        .password = { 0 },                 \
        .channel = 1,                      \
        .max_conn = 2,                     \
    })

    /**
     * @brief 目标 WiFi 的凭证（用户填的、设备要连接的那个网络）
     */
    typedef struct
    {
        char ssid[32];     /*!< 目标 WiFi 名称，1~31 字符 */
        char password[64]; /*!< 目标 WiFi 密码，留空表示开放网络；非空则 8~63 位 */
    } webconfig_sta_cfg_t;

    /* ===================== 端口回调：webconfig 与外部的唯一接口 =====================
     * 组件内部不直接调用任何无线 API，"开热点"和"用凭证去连接"这两件事由应用层
     * 实现成下面两个回调传进来。回调里用哪套无线栈，webconfig 完全不关心。 */

    /**
     * @brief 开启配网热点
     *
     * @param ap  热点参数，取自 webconfig_cfg_t.ap
     * @param ctx 上下文，即 webconfig_cfg_t.ctx
     *
     * @note 参数校验由实现方负责（名称非空、密码长度、信道范围等），非法时返回
     *       ESP_ERR_INVALID_ARG——webconfig 不做二次校验。
     *
     * @note 本回调会被调用多次："连接失败自动重开热点"时会用同一份参数再调一次，
     *       所以实现必须可重复调用（已开热点时重新下发参数即可，不能报错）。
     *
     * @return ESP_OK 表示热点已就绪；其它错误码会原样返回给 webconfig_start() 的调用者
     */
    typedef esp_err_t (*webconfig_ap_start_fn)(const webconfig_ap_cfg_t* ap, void* ctx);

    /**
     * @brief 应用新凭证：关掉配网热点，用该凭证去连接目标网络
     *
     * @param ssid     目标 WiFi 名称，已校验非空
     * @param password 目标 WiFi 密码，可能为空字符串（开放网络）
     * @param ctx      上下文，即 webconfig_cfg_t.ctx
     *
     * @note 本回调在"回复完用户的'已保存'页面之后"才被调用（延迟 2 秒），
     *       所以实现方可以放心关掉热点，不会打断正在传输的响应。
     *
     * @return ESP_OK 表示已受理；失败不影响凭证已经存进 NVS 的事实
     */
    typedef esp_err_t (*webconfig_sta_apply_fn)(const char* ssid, const char* password, void* ctx);

    /**
     * @brief 端口回调集合，两个都必填
     */
    typedef struct
    {
        webconfig_ap_start_fn ap_start;   /*!< 开配网热点 */
        webconfig_sta_apply_fn sta_apply; /*!< 用凭证切过去连接 */
    } webconfig_port_t;

    /**
     * @brief Web 配网初始化参数
     */
    typedef struct
    {
        webconfig_ap_cfg_t ap; /*!< 配网热点参数 */
        webconfig_port_t port; /*!< 端口回调，两个都必填 */

        /*!< 配网结束后是否保留 HTTP 服务器：true 时设备连上目标网络后，仍可通过它在
         *   路由器网段的 IP 访问同一个页面（随时改配置 / 看状态）；false 时配网一结束
         *   就关掉服务器省内存——那样设备连上路由器后就再也没有页面了。 */
        bool keep_httpd;

        void* ctx; /*!< 透传给两个回调的上下文，可为 NULL */
    } webconfig_cfg_t;

    /**
     * @brief 开启 Web 配网：起配网热点 + HTTP 服务器
     *
     * @param cfg 配置（热点参数 + 端口回调），不可为 NULL
     *
     * @note 完整流程：
     *       1. 调 cfg->port.ap_start() 开出配网热点（用默认网段的话是 192.168.4.1）；
     *       2. 启动 HTTP 服务器，注册三个路由：
     *          - GET  /            配网页面（web/index.html，静态 HTML，原样发出）；
     *          - GET  /api/config  把设备已保存的 SSID 以 JSON 回给页面的内联 JS；
      *          - POST /save        保存凭证（请求体 application/json，
      *                              形如 {"ssid":"...","password":"..."}），成功也回 JSON；
     *       3. 用户提交后：凭证先写入 NVS，回复 {"ok":true,...}，延迟 2 秒再调
     *          cfg->port.sta_apply() 切过去连接（延迟是为了让响应先送达，
     *          因为关热点会让页面打不开）。
     *
     * @note 连接失败自愈：调用 sta_apply() 之后的 60 秒内，若收到 5 次
     *       webconfig_notify_sta_disconnected()（通常意味着密码填错了），
     *       会自动重新开启配网热点让用户重填，不会停在"连不上又改不了"的状态；
     *       期间一旦收到 webconfig_notify_sta_connected() 就提前结束观察。
     *
     * @note 已在配网中时重复调用是幂等的，直接返回 ESP_OK。
     *
     * @note 用户使用方式：手机/电脑连接热点后，访问 http://192.168.4.1/。
     *
     * @note 配网结束后 HTTP 服务器是否保留由 cfg.keep_httpd 决定：
     *       true  -> 服务器继续跑（只退出"配网状态"），设备连上目标网络后可以用它在
     *                路由器网段的 IP 访问同一个页面；
     *       false -> 直接关掉服务器，之后想改配置只能重新开一次配网热点。
     *
     * @return
     *     - ESP_OK               成功（或已在配网中）
     *     - ESP_ERR_INVALID_ARG  cfg 为 NULL，或 port 回调没填全
     *     - 其它                 ap_start() / httpd_start() 返回的错误码
     */
    esp_err_t webconfig_start(const webconfig_cfg_t* cfg);

    /**
     * @brief 只把 HTTP 管理页挂上去，不碰热点（配网结束后的常驻用法）
     *
     * @param cfg 配置，port 两个回调必须填全（失败自愈时会用它们重新开热点）；
     *            ap 参数在这里用不到，但重新配网时会用到，一并填好
     *
     * @note 用途：设备已经有凭证、开机直接以 STA 方式连路由器时调用本函数，
     *       用户就能用设备在路由器网段的 IP 访问同一个页面，不必重新连热点。
     *       通常与 cfg.keep_httpd = true 一起用。
     *
     * @note 本函数不改变配网状态：webconfig_is_running() 仍为 false，也不会开热点。
     *       页面上提交新凭证后照常生效；若凭证连不上，观察期过后会自动重开配网热点。
     *
     * @note 幂等：服务器已经在运行时直接返回 ESP_OK（不会重复注册路由）。
     *
     * @return
     *     - ESP_OK               成功（或服务器已在运行）
     *     - ESP_ERR_INVALID_ARG  cfg 为 NULL，或 port 回调没填全
     *     - 其它                 httpd_start() 返回的错误码
     */
    esp_err_t webconfig_httpd_start(const webconfig_cfg_t* cfg);

    /**
     * @brief 关闭 Web 配网：停 HTTP 服务器，并尝试用已保存的凭证恢复连接
     *
     * @note 停止后若 NVS 里有已保存的凭证，会调 port.sta_apply() 切过去连接；
     *       没保存过则维持热点（此时设备无事可做）。
     *
     * @note 正常流程不需要手动调用：提交凭证后组件会自动收尾。
     *       本函数供"主动退出配网"的场景使用（例如串口命令）。
     *
     * @return
     *     - ESP_OK                 成功
     *     - ESP_ERR_INVALID_STATE  当前不在配网中
     *     - 其它                   sta_apply() 返回的错误码
     */
    esp_err_t webconfig_stop(void);

    /**
     * @brief 查询是否正在配网
     *
     * @return true 正在配网（热点 + HTTP 服务器运行中）；false 否则
     */
    bool webconfig_is_running(void);

    /**
     * @brief 从 NVS 读取已保存的目标 WiFi 凭证
     *
     * @param out 输出参数，成功时被整体覆盖；不可为 NULL
     *
     * @return true 读到有效凭证（ssid 非空）；false 没配过网或 NVS 不可用
     *
     * @note 典型用法（开机时决定走哪条路）：
     * @code
     *       webconfig_sta_cfg_t saved;
     *       if (webconfig_load_sta_cfg(&saved)) {
     *           // 用 saved.ssid / saved.password 去连接
     *       } else {
     *           webconfig_start(&wc_cfg);   // 没配过网，开热点等用户配网
     *       }
     * @endcode
     *
     * @note 本函数与 webconfig_save_sta_cfg() 只读写 NVS、不碰无线运行状态，
     *       所以无线栈还没初始化时也可以调用。
     */
    bool webconfig_load_sta_cfg(webconfig_sta_cfg_t* out);

    /**
     * @brief 把目标 WiFi 凭证保存到 NVS（不改变当前运行状态）
     *
     * @param ssid     目标 WiFi 名称，1~31 字符
     * @param password 目标 WiFi 密码，留空表示开放网络；非空则 8~63 位
     *
     * @note 只落盘、不生效：想让设备立刻用新凭证连接，接着调
     *       webconfig_cfg_t.port.sta_apply()（配网页面内部就是这么做的）。
     *
     * @return
     *     - ESP_OK              成功
     *     - ESP_ERR_INVALID_ARG 参数为 NULL 或长度非法（已打印具体原因）
     *     - 其它                NVS 打开 / 写入 / 提交返回的错误码
     */
    esp_err_t webconfig_save_sta_cfg(const char* ssid, const char* password);

    /**
     * @brief 清除 NVS 里保存的 WiFi 凭证（恢复出厂 / 重新配网用）
     *
     * @note 只清凭证，不影响无线当前运行状态；清除后下次开机
     *       webconfig_load_sta_cfg() 返回 false，从而重新走配网流程。
     *       本函数没有对应的网页按钮，供串口命令等渠道调用。
     *
     * @return
     *     - ESP_OK  成功（包括本来就没存过的情况）
     *     - 其它    NVS 返回的错误码
     */
    esp_err_t webconfig_erase_sta_cfg(void);

    /**
     * @brief 通知 webconfig：STA 已成功连上目标网络
     *
     * @note 由应用层在收到无线的"已连接"事件时调用，作用是结束"连接失败观察期"
     *       （见 webconfig_start() 的说明）。平时调用它没有任何副作用。
     */
    void webconfig_notify_sta_connected(void);

    /**
     * @brief 通知 webconfig：STA 与目标网络断开
     *
     * @note 由应用层在收到无线的"断开"事件时调用。只有"刚应用完新凭证"的观察期内
     *       才会被计数，连续 5 次失败会自动重新开启配网热点；
     *       其余时间（正常工作期间路由器抖动等）调用它没有任何副作用——
     *       断线重连是无线栈自己的事，不归配网组件管。
     */
    void webconfig_notify_sta_disconnected(void);

#ifdef __cplusplus
}
#endif

#endif
