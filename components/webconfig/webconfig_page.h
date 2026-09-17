#ifndef _WEBCONFIG_PAGE_H_
#define _WEBCONFIG_PAGE_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ===================== 配网页面模块 =====================
 * 只干"页面"这一件事：渲染两个 HTML 页（配网表单页 / 已保存页）+ 解析表单提交。
 * 不碰 HTTP 服务器、不碰 NVS、不碰无线——怎么把它们串起来是 webconfig.c 的事。
 *
 * 页面内容在 web/index.html 与 web/saved.html 这两个文件里：要改配色、文案、
 * 输入项或加前端校验，直接编辑这两个 html 即可，重新编译就生效（不用动 C 代码）。
 * 页面里写 {{ssid}} 的地方会被替换成 HTML 转义后的 WiFi 名称。
 * 本文件只提供两个渲染接口 + 表单解析。
 */

/**
 * @brief 单个页面渲染结果的缓冲上限（字节，含结尾 '\0'）
 *
 * @note 现在的页面文件 ~1.2KB，加上 {{ssid}} 展开（最坏 155 字节）还有余量。
 *       缓冲放在 HTTP 任务栈上（栈已按这个尺寸调到 8192）；页面要是被加得很大、
 *       超过这里，只会被截断（丢失页面尾部）不会越界——真加了很多内容，记得把
 *       这个值和 webconfig.c 里的 WEBCONFIG_HTTPD_STACK 一起调大。
 */
#define WEBCONFIG_PAGE_MAX 2048

/**
 * @brief POST /save 请求体上限（字节，不含结尾 '\0'）
 *
 * @note ssid + password 做 URL 编码后远小于此；超过这个长度直接回 400，
 *       不读进内存。
 */
#define WEBCONFIG_FORM_BODY_MAX 512

/**
 * @brief 渲染配网表单页（GET /）
 *
 * @param out        输出缓冲
 * @param out_sz     out 的字节数，建议给 WEBCONFIG_PAGE_MAX
 * @param saved_ssid 回显到 SSID 输入框里的名称，可为 NULL 或空串；
 *                   内部会做 HTML 转义，调用方直接传原始值即可
 *
 * @return 实际写入 out 的字节数（不含结尾 '\0'），可直接作为 httpd_resp_send() 的长度；
 *         缓冲不够时截断，返回值一定小于 out_sz
 */
int webconfig_page_render_form(char* out, size_t out_sz, const char* saved_ssid);

/**
 * @brief 渲染"配置已保存"页（POST /save 的响应）
 *
 * @param out     输出缓冲
 * @param out_sz  out 的字节数
 * @param ssid    刚刚保存的目标 WiFi 名称，用于页面提示，可为 NULL；内部做 HTML 转义
 *
 * @return 同 webconfig_page_render_form()
 */
int webconfig_page_render_saved(char* out, size_t out_sz, const char* ssid);

/**
 * @brief 从 application/x-www-form-urlencoded 的请求体里取一个字段
 *
 * @param body   以 '\0' 结尾的请求体
 * @param key    字段名，例如 "ssid"
 * @param out    输出缓冲，成功时写入解码后的值
 * @param out_sz out 的字节数
 *
 * @note 按 '&' 分段逐个比较 "key=value"，能正确处理值里含 '=' 的情况，
 *       也不会把 "xssid=" 误认成 "ssid="。值会做 URL 解码（%XX 与 '+'），
 *       SSID 可以为任意字节（含 UTF-8 中文）。
 *
 * @return true 取到且长度合适；false 没有该字段 / 值超长 / 转义非法（out 内容不可用）
 */
bool webconfig_page_form_get(const char* body, const char* key, char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif
