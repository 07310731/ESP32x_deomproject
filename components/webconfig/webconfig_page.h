#ifndef _WEBCONFIG_PAGE_H_
#define _WEBCONFIG_PAGE_H_

#include <stdbool.h>
#include <stddef.h>

#include "webconfig.h" /* webconfig_dev_cfg_t —— parse_save / config_json 都用它 */

#ifdef __cplusplus
extern "C"
{
#endif

/* ===================== 配网页面模块 =====================
 * 只干"页面内容"这一件事：把编进固件的静态页面交出去、生成 JSON 响应、解析 JSON 请求。
 * 不碰 HTTP 服务器、不碰 NVS、不碰无线——怎么把它们串起来是 webconfig.c 的事。
 *
 * 设计原则：**页面里的一切动态内容都由前端负责**，C 侧只通过两个 JSON 接口交换数据。
 * 页面就是 web/index.html 这一个文件（含内联 CSS/JS）：
 *   - 改配色 / 文案 / 布局 / 输入项 / 加交互，只动这个 html，重新编译即可，
 *     完全不需要碰 C 代码；
 *   - 页面通过 GET /api/config 读数据、POST /save 写数据（路由在 webconfig.c）；
 *     接口的 JSON 序列化与反序列化都收在本文件里。
 *
 * 早先那种"服务端替换 {{ssid}} 占位符"和"手写 JSON 转义 / 表单解析"的做法都已经去掉——
 * 前者让前端同学没法独立维护页面，后者正确性自负其责、维护成本高，而且难以扩展（一旦要
 * 加数组 / 嵌套 / 数字字段就会变成逗号地狱）。现在收发两端都走 cJSON（项目本地 vendored
 * 在 components/cjson/）：
 *   - 响应仍写进调用方缓冲（用 cJSON_PrintPreallocated()），对外契约不变；
 *   - 请求直接 cJSON_Parse() 按字段名取字符串。
 * 因此"加新字段"在本文件里就是几行 API 调用，前后端只改字段名就完事。
 */

/**
 * @brief JSON 请求体上限（字节，不含结尾 '\0'）
 *
 * @note 新配置页按"设备配置 JSON 包 v1.0"产出完整快照（deviceName/network/
 *   communication），实测最大 581 B，另为根级桥接键（ssid/password）和后续
 *   字段扩展留余量；超过直接回 400，不读进内存。
 *   注意：改这里要同步核对 WEBCONFIG_HTTPD_STACK（处理器里请求体缓冲在栈上）。
 */
#define WEBCONFIG_JSON_BODY_MAX 1024

/**
 * @brief JSON 响应体缓冲上限（字节，含结尾 '\0'）
 *
 * @note GET /api/config 现在回全量设备配置（设备名 + 网络 + 通讯三块），典型
 *   1KB 上下；cJSON 按需转义，1536 字节留足余量，溢出时由 cjson_to_buf() 兜底清空。
 */
#define WEBCONFIG_JSON_MAX 1536

/**
 * @brief 取出编进固件的配网页面（静态 HTML，原样发送，无需任何渲染）
 *
 * @param data 输出：页面内容起始地址（指向 flash，只读），不可为 NULL
 * @param len  输出：页面字节数（不含 CMake 在末尾补的那个 '\0'），不可为 NULL
 *
 * @note 拿到后直接交给 httpd_resp_send() 发即可：数据常驻 flash，
 *   不占 RAM、不用拷贝。
 */
void webconfig_page_index(const char** data, size_t* len);

/**
 * @brief 生成 GET /api/config 的响应体：把设备已保存的 SSID 告诉前端
 *
 * @param out        输出缓冲，写入以 '\0' 结尾的 JSON
 * @param out_sz     out 的字节数，建议给 WEBCONFIG_JSON_MAX
 * @param saved_ssid 已保存的 SSID，可为 NULL 或空串（这时字段值就是空字符串）
 *
 * @return 写入 out 的字节数（不含结尾 '\0'），可直接作为 httpd_resp_send() 的长度；
 *   缓冲不够 / 内部失败时清空缓冲并返回 0。
 *
 * @note 形如 {"ssid":"..."}。SSID 里的引号 / 反斜杠 / 控制字符由 cJSON 自动转义，
 *   前端可以放心用 JSON.parse()。
 */
int webconfig_page_config_json(char* out, size_t out_sz, const webconfig_dev_cfg_t* cfg);

/**
 * @brief 拼装 /api/status 的 JSON：把 NVS 配置 + 运行时快照合并回给前端
 *
 * 输出形如：
 *   {
 *     "deviceName": "...", "fw": "...", "app": "...", "uptime": N,
 *     "wifiMode": "ap" | "sta" | "none",
 *     "state": "online" | "offline",
 *     "running": {
 *       "network": { "wifiMode":..., "ap":{ssid,ip,running,stations},
 *                                 "sta":{ssid,ip, mask,gateway,dns,rssi,connected} },
 *       "communication": { "protocol":..., "tcpServer":{port,running},
 *                                       "tcpClient":{serverIp,serverPort,connected},
 *                                       "udp":{port,running} }
 *     }
 *   }
 *
 * @param cfg  NVS 里的全量配置（用户保存过的）；首次启动 NVS 空时传 NULL，
 *             各字段会按空串输出，前端会用表单 placeholder 兜底
 * @param rt   设备**当前实际在用**的配置快照；由 webconfig.c 从 wifi /
 *             service_manager / esp_netif 等运行时 API 拉
 * @return 同 webconfig_page_config_json()
 */
/**
 * @brief 拼装 /api/status 里 "running" 那块的 JSON（不含顶层元信息）
 *
 * 输出形如：
 *   { "network":        { wifiMode, ap:{ssid,password,ip,running,stations},
 *                                 sta:{ssid,password,mode,ip,mask,gateway,dns,
 *                                      ip_running,mask_running,gw_running,dns_running,
 *                                      rssi,connected} },
 *     "communication": { protocol, tcpServer:{port,running},
 *                                tcpClient:{serverIp,serverPort,running,connected},
 *                                udp:{port,running} } }
 *
 * 调用方（webconfig.c）拿到这个 JSON 字符串后 cJSON_Parse() → 取根对象 →
 * cJSON_AddItemToObject(自己 root, "running", cJSON_Duplicate(...)) 挂上去，
 * 同时把 fw / app / uptime / mode / state 等页头芯片字段加到自己的 root。
 * 拆分原因：那些元信息由 webconfig.c 直接出，不让 page 层知道 fw_version /
 * uptime 是怎么算的。
 *
 * @param cfg  NVS 里的全量配置（用户保存过的）；首次启动 NVS 空时传 NULL，
 *             各字段会按空串输出
 * @param rt   设备**当前实际在用**的配置快照；由 webconfig.c 从 wifi /
 *             service_manager / esp_netif 等运行时 API 拉
 * @return 写入的字节数；0 = 失败或空字符串
 */
int webconfig_page_running_json(char* out, size_t out_sz,
                                const webconfig_dev_cfg_t* cfg,
                                const webconfig_dev_running_t* rt);

/**
 * @brief 生成 POST /save 成功时的响应体
 *
 * @param out    输出缓冲，写入以 '\0' 结尾的 JSON
 * @param out_sz out 的字节数，建议给 WEBCONFIG_JSON_MAX
 *
 * @return 同 webconfig_page_config_json()
 *
 * @note 形如 {"ok":true}。保存结果（设备名 / 网络 / 通讯）由前端自己用
 *   GET /api/config 拉最新值，不在成功响应里回传——避免一次提交产生两套数据。
 */
int webconfig_page_saved_json(char* out, size_t out_sz);

/**
 * @brief 生成 POST /save 失败时的响应体
 *
 * @param out    输出缓冲，写入以 '\0' 结尾的 JSON
 * @param out_sz out 的字节数，建议给 WEBCONFIG_JSON_MAX
 * @param msg    错误文案（中文 / 英文均可），会被 cJSON 转义
 *
 * @return 同 webconfig_page_config_json()
 *
 * @note 形如 {"ok":false,"error":"..."}。错误文案属于流程逻辑，所以这个函数
 *   暴露给 webconfig.c 而不是写死在这里——出错信息由调用方决定。
 */
int webconfig_page_error_json(char* out, size_t out_sz, const char* msg);

/**
 * @brief 从 application/json 请求体里解析出完整设备配置（buildOut() 的 C 端镜像）
 *
 * @param body 以 '\0' 结尾的请求体
 * @param cfg  输出：成功时被填满；失败时保持调用方传入的值（本函数不清零）
 *
 * @return true JSON 合法、所有字符串字段都拿到了；false JSON 不合法 / 字段缺失 /
 *   字段类型不是字符串 / 值放不下指定缓冲
 *
 * @note 只做"存在性 + 类型 + 长度"三道闸，不做业务校验（wifi 合法 / 端口范围
 *   等由 webconfig.c 的 dev_apply 路径负责）—— 这一层只保证"前端给的东西我们
 *   读得出来"，业务规则是后端的事。
 *
 * @note 不接受 "根级 ssid / password" 的兼容旧格式：webconfig.c 不再依赖
 *   这两个字段做凭证桥接，前端必须按 buildOut() 的 spec 提交完整结构。
 *   字段值放不下（例如 SSID 太长）按"未取到"处理，调用方应保证 cfg 里
 *   的字段宽度足够（SSID 32、密码 64、IP 16、设备名 60 等，见 webconfig_dev_cfg_t）。
 */
bool webconfig_page_parse_save(const char* body, webconfig_dev_cfg_t* cfg);

/**
 * @brief partial 版本的解析：只覆盖 JSON 里出现的字段
 *
 * 调用方在使用前应先把 NVS 现状读到 cfg（如 webconfig_load_dev_cfg()），
 * 本函数对 cfg 字段的处理规则：
 *   - JSON 缺字段 → cfg 不动（保留 NVS 现状）
 *   - JSON 是空串 → 覆盖成空串（清空语义生效）
 *   - JSON 是 null  → 同空串
 *   - JSON 是非法类型 → 跳过，不写 cfg
 *
 * 整段对象（network / communication / ap / sta / tcpServer / tcpClient /
 * heartbeat / udp）也可以整段缺失：直接跳过整段。配合 /save handler 的
 * "分模块独立保存" 按钮使用。
 *
 * @return true=JSON 解析成功（不论有没有覆盖到字段），false=JSON 不合法
 */
bool webconfig_page_parse_save_partial(const char* body, webconfig_dev_cfg_t* cfg);

#ifdef __cplusplus
}
#endif

#endif