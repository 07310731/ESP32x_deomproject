#ifndef _WIFI_H_
#define _WIFI_H_

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief WiFi 初始化参数
     *
     * @note WiFi 没有一个"拿过来就能连"的默认 SSID，所以 WIFI_INIT_CFG_DEFAULT()
     *       只给出模式、AP 信道、最大连接数、IP 信息获取方式这类安全默认值，
     *       sta_ssid / ap_ssid 必须由调用者填写，
     *       否则 wifi_config_init() 会返回 ESP_ERR_INVALID_ARG。
     */
    typedef struct
    {
        wifi_mode_t mode; /*!< 工作模式：WIFI_MODE_STA / WIFI_MODE_AP / WIFI_MODE_APSTA */

        char sta_ssid[32];     /*!< STA 要连接的 AP 名称，最长 31 字符，不可为空 */
        char sta_password[64]; /*!< STA 密码，留空表示开放网络；非空则 8~63 位 */

        char ap_ssid[32];     /*!< 本机 AP 名称，最长 31 字符，不可为空 */
        char ap_password[64]; /*!< 本机 AP 密码，留空表示开放热点；非空则 8~63 位 */
        uint8_t ap_channel;   /*!< AP 信道，取值范围 1 ~ 13 */
        uint8_t ap_max_conn;  /*!< AP 最多允许接入的客户端数量，至少为 1 */

        /* ---- IP 信息配置 ---- */
        bool sta_use_static_ip; /*!< true: 用 sta_ip/netmask/gateway 设静态 IP 并停 DHCP；false: 用
                                   DHCP（默认） */
        char sta_ip[16]; /*!< STA 点分十进制 IPv4，例如 "192.168.1.100"，sta_use_static_ip=true
                            时必填 */
        char sta_netmask[16]; /*!< STA 子网掩码，例如 "255.255.255.0" */
        char sta_gateway[16]; /*!< STA 网关，例如 "192.168.1.1" */
        char
            sta_dns_main[16]; /*!< STA 主 DNS，留空表示不设置；仅在 sta_use_static_ip=true 时生效 */
        char sta_dns_backup[16]; /*!< STA 备用 DNS，留空表示不设置；仅在 sta_use_static_ip=true
                                    时生效 */

        bool ap_use_static_ip; /*!< true: 用 ap_ip/netmask/gateway 改 AP 网段；false: 默认
                                  192.168.4.1/24 */
        char ap_ip[16];        /*!< AP 网关 IP，例如 "192.168.4.1" */
        char ap_netmask[16];   /*!< AP 子网掩码，例如 "255.255.255.0" */
        char ap_gateway[16];   /*!< AP 网关（通常与 ap_ip 相同） */
    } wifi_init_cfg_t;

/**
 * @brief 默认配置：STA 模式、AP 信道 1、最多 4 个客户端、STA 与 AP 都用 DHCP / 默认 IP
 *
 * @note 只给了"安全"默认值，sta_ssid 为空，使用前必须自行填写。
 */
#define WIFI_INIT_CFG_DEFAULT()                                                                    \
    ((wifi_init_cfg_t){                                                                            \
        .mode = WIFI_MODE_STA,                                                                     \
        .sta_ssid = { 0 },                                                                         \
        .sta_password = { 0 },                                                                     \
        .ap_ssid = { 0 },                                                                          \
        .ap_password = { 0 },                                                                      \
        .ap_channel = 1,                                                                           \
        .ap_max_conn = 4,                                                                          \
        .sta_use_static_ip = false,                                                                \
        .sta_ip = { 0 },                                                                           \
        .sta_netmask = { 0 },                                                                      \
        .sta_gateway = { 0 },                                                                      \
        .sta_dns_main = { 0 },                                                                     \
        .sta_dns_backup = { 0 },                                                                   \
        .ap_use_static_ip = false,                                                                 \
        .ap_ip = { 0 },                                                                            \
        .ap_netmask = { 0 },                                                                       \
        .ap_gateway = { 0 },                                                                       \
    })

    /**
     * @brief 初始化 WiFi：按配置把协议栈、驱动、参数一次性配好并启动
     *
     * @param cfg 配置参数，不可为 NULL
     *
     * @note 内部按顺序完成以下步骤，任意一步失败都会返回该步的错误码：
     *       1. 参数校验（无任何资源申请，校验失败不留半初始化状态）；
     *       2. nvs_flash_init()：WiFi 驱动需要 NVS 存放校准数据和配置
     *          （分区无空闲页 / 版本不匹配时会先 nvs_flash_erase() 再重试）；
     *       3. esp_netif_init() + esp_event_loop_create_default()：全局只做一次；
     *       4. esp_netif_create_default_wifi_sta() / _ap()：按 mode 创建默认 netif，
     *          已创建过则复用，不会重复创建；
     *       5. esp_wifi_init()；
     *       6. 注册 WiFi / IP 事件回调，把连接、断开、拿到 IP 打印到日志（只注册一次）；
     *       7. esp_wifi_set_mode()；
     *       8. esp_wifi_set_config() 下发 STA / AP 参数；
     *       9. 创建断线重连任务（只创建一次）→ esp_wifi_start()；
     *      10. AP 静态 IP：若 ap_use_static_ip，停 DHCP 服务器、改 IP、再启动 DHCP 服务器；
     *      11. STA 静态 IP：在 WIFI_EVENT_STA_CONNECTED 事件里停 DHCP 客户端、改 IP、
     *          可选设置主/备 DNS（每次成功连接都会重新生效）。
     *
     * @note STA / APSTA 模式下，收到 WIFI_EVENT_STA_START 事件时会自动调用
     *       esp_wifi_connect() 发起连接（esp_wifi_start() 本身不会自动连）。
     *
     * @note 断线自动重连：内部有一个常驻重连任务，收到 WIFI_EVENT_STA_DISCONNECTED
     *       后会隔 2 秒发起一次重连，连不上就继续重试（间隔固定 2 秒，不做退避），
     *       直到连上、或工作模式不再包含 STA（例如切成 AP-only）才停止。
     *       由 wifi_update_config() 改凭证触发的断开不走这 2 秒冷却，立即用新凭证重连。
     *       重连行为没有对外开关，延时/栈/优先级是 wifi.c 里的内部常量。
     *
     * @note 可重复调用：netif、事件循环、驱动、事件回调这些全局资源只会初始化一次，
     *       再次调用会复用它们并重新下发模式与参数；IP 配置也会被重新解析并生效。
     *
     * @note 下列参数异常会在申请任何资源之前直接返回 ESP_ERR_INVALID_ARG：
     *       - cfg 为 NULL
     *       - mode 不是 STA / AP / APSTA
     *       - STA 模式下 sta_ssid 为空或超过 31 字符
     *       - STA 密码非空但不足 8 位或超过 63 位
     *       - AP 模式下 ap_ssid 为空或超过 31 字符
     *       - AP 密码非空但不足 8 位或超过 63 位
     *       - ap_channel 不在 1 ~ 13 范围内
     *       - ap_max_conn 为 0
     *       - sta_use_static_ip=true 但 sta_ip / sta_netmask / sta_gateway 缺失或格式错误
     *       - sta_dns_main / sta_dns_backup 非空但格式错误
     *       - ap_use_static_ip=true 但 ap_ip / ap_netmask / ap_gateway 缺失或格式错误
     *
     * @return
     *     - ESP_OK               成功
     *     - ESP_ERR_INVALID_ARG  参数非法
     *     - 其它                 NVS / netif / 事件循环 / WiFi 驱动 / IP 设置返回的错误码
     */
    esp_err_t wifi_config_init(const wifi_init_cfg_t* cfg);

    /**
     * @brief 运行时更新 WiFi 配置（供 Web 页面 / 蓝牙 / 串口等配置渠道调用）
     *
     * @param cfg 新配置，字段含义与 wifi_config_init() 相同，不可为 NULL
     *
     * @note 必须先成功调用过 wifi_config_init()，否则返回 ESP_ERR_INVALID_STATE。
     *
     * @note 行为要点：
     *       - 参数校验规则与 wifi_config_init() 完全一致，非法参数返回
     *         ESP_ERR_INVALID_ARG，且不改变当前任何运行状态；
     *       - 改 STA 的 ssid / 密码后会自动断开并用新凭证重连
     *         （新凭证只在重新连接时才会生效）；
     *       - 改 STA IP 配置（静态 IP 参数、静态 <-> DHCP 切换）时，若当前已连接
     *         则立即生效，未连接则在下一次连接成功时生效；
     *       - 改 AP 参数（ssid / 密码 / 信道 / 最大连接数）立即生效，
     *         已接入的客户端会被断开；
     *       - 改 AP 网段（ap_use_static_ip / ap_ip 等）立即生效；
     *         ap_use_static_ip 从 true 改回 false 时恢复默认网段 192.168.4.1/24；
     *       - 工作模式可以切换（例如 STA <-> APSTA），缺少的 netif 会按需创建，
     *         已创建的 netif 不会被销毁，切回去时直接复用。
     *
     * @note 本函数不做配置持久化：如需保存到 NVS / 配置文件，由调用方负责
     *       （web / 蓝牙渠道先落盘再调本函数，重启后再用保存的配置调
     *       wifi_config_init()）。
     *
     * @note 本函数与 wifi_config_init() 都不是线程安全的，
     *       不要在多个任务里并发调用。
     *
     * @return
     *     - ESP_OK                  成功
     *     - ESP_ERR_INVALID_ARG     参数非法（当前配置不受影响）
     *     - ESP_ERR_INVALID_STATE   尚未成功调用过 wifi_config_init()
     *     - 其它                   esp_wifi_set_mode() / esp_wifi_set_config() /
     *                              IP 设置返回的错误码
     */
    esp_err_t wifi_update_config(const wifi_init_cfg_t* cfg);

#ifdef __cplusplus
}
#endif

#endif
