#ifndef _WEBCONFIG_PAGE_H_
#define _WEBCONFIG_PAGE_H_

#include <stdbool.h>
#include <stddef.h>

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
 * @note 配网页面目前只有 ssid / password 两个字符串字段，编码后远小于此；
 *   超过直接回 400，不读进内存。后续加字段时记得复查这个上限。
 */
#define WEBCONFIG_JSON_BODY_MAX 512

/**
 * @brief JSON 响应体缓冲上限（字节，含结尾 '\0'）
 *
 * @note 当前最大响应是 {"ok":true,"ssid":"..."}，SSID 31 字节；cJSON 按需转义，
 *   512 字节足够。
 */
#define WEBCONFIG_JSON_MAX 512

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
int webconfig_page_config_json(char* out, size_t out_sz, const char* saved_ssid);

/**
 * @brief 生成 POST /save 成功时的响应体
 *
 * @param out    输出缓冲，写入以 '\0' 结尾的 JSON
 * @param out_sz out 的字节数，建议给 WEBCONFIG_JSON_MAX
 * @param ssid   刚刚保存的 SSID，可为 NULL；会被 cJSON 转义
 *
 * @return 同 webconfig_page_config_json()
 *
 * @note 形如 {"ok":true,"ssid":"..."}。
 */
int webconfig_page_saved_json(char* out, size_t out_sz, const char* ssid);

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
 * @brief 从 application/json 请求体里取出 ssid / password 两个字符串字段
 *
 * @param body         以 '\0' 结尾的请求体
 * @param ssid         输出缓冲，成功时写入 ssid 字段值
 * @param ssid_sz      ssid 缓冲字节数
 * @param password     输出缓冲，成功时写入 password 字段值
 * @param password_sz  password 缓冲字节数
 *
 * @return true 两个字段都取到了；false 请求体不是合法 JSON / 字段缺失 /
 *   字段类型不是字符串 / 值放不下指定缓冲（成功后 ssid / password 内容可用）。
 *
 * @note 字段值放不下（例如 SSID 太长）按"未取到"处理；调用方应该把缓冲给到
 *   大于字段最大合法长度的尺寸（SSID 32 字节、密码 64 字节），把"合法长度"
 *   的最终判断留给 webconfig_save_sta_cfg()。
 */
bool webconfig_page_parse_save(const char* body,
                                char* ssid, size_t ssid_sz,
                                char* password, size_t password_sz);

#ifdef __cplusplus
}
#endif

#endif