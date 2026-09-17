/* ===================== 配网页面实现 =====================
 * 页面相关的一切都收在这个文件里：
 *   - web/index.html（含内联 CSS/JS）的取用（页面文件由 CMake 编进固件）
 *   - 给前端准备的 JSON 响应体生成（GET /api/config、POST /save 的成功 / 失败响应）
 *   - 配网提交体的解析（application/json）
 * webconfig.c 只负责"把这里生成的内容发出去""把解析出来的字段存起来"。
 *
 * 改页面（配色、文案、布局、输入项、前端 JS）只改 web/index.html；
 * 只有改了"JSON 字段"或"提交体字段规则"才需要动本文件。
 *
 * 序列化 / 反序列化都交给 cJSON（ESP-IDF 内置组件 json）：
 *   - 生成响应时建一棵 cJSON 树，用 cJSON_PrintPreallocated() 序列化到调用方缓冲——
 *     保留"输出写进调用方缓冲"的对外契约，只在"建树"这一步用堆（约 150-200B，
 *     请求结束释放）；
 *   - 解析请求时直接 cJSON_Parse()，按字段名取字符串。
 * 这样避免手写 JSON 转义与 URL 解码带来的正确性风险，也方便后续扩展数组 / 嵌套。
 */

#include "webconfig_page.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ===================== 页面文件（编译期嵌入固件） =====================
 * web/ 目录下的 .html 靠 CMakeLists.txt 里的 EMBED_TXTFILES 编进 flash：
 * 构建时为每个文件生成一个汇编源，用 .incbin 把字节【原样】收进 .rodata 段。
 * 过的是汇编器而非 C 编译器，所以 HTML 里的引号 / 反斜杠 / 换行 / 百分号都
 * 【不需要转义】，写什么固件里就是什么，中间没有"转换"这一步（唯一的附加是
 * TEXT 模式末尾补的那个 '\0'）。
 *
 * 文件内容通过链接器生成的 _binary_<文件名>_start / _end 符号取到：
 *   - 符号名只取"文件名"，目录不参与命名，所以 web/index.html 对应
 *     _binary_index_html_start。别把两个同名文件放进不同目录，会撞符号；
 *   - TEXT 模式会在文件末尾补一个 '\0'，所以结尾那个符号指向的是 '\0' 之后，
 *     真实内容长度是 end - start - 1（见 webconfig_page_index()）。
 * 这段数据只读、常驻 flash 不占 RAM。 */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* ===================== 对外接口：页面 ===================== */

void webconfig_page_index(const char** data, size_t* len)
{
    if (data != NULL)
    {
        *data = index_html_start;
    }

    if (len != NULL)
    {
        /* _binary_xxx_end 指向 TEXT 模式补的那个 '\0' 之后，所以要减 1 */
        *len = (size_t)(index_html_end - index_html_start) - 1;
    }
}

/* ===================== JSON 生成通用助手 =====================
 * 三个对外函数（config / saved / error）都遵守同一规则：把 JSON 写进调用方
 * 提供的缓冲，返回字节数。用 cJSON_PrintPreallocated() 而不是 cJSON_PrintUnformatted()——
 * 前者把序列化结果写入调用方给的缓冲（不开新堆），后者会 malloc 一块返回；
 * 这样保留了原"零额外输出堆分配"的对外契约，只在"建 cJSON 树"这一步用堆。
 *
 * 失败兜底：建树或序列化失败时把缓冲清成空串、返回 0；调用方拿到 0 长度
 * 交给 httpd_resp_send() 不会出错。
 *
 * cJSON_PrintPreallocated() 在缓冲足够时返回 cJSON_true，缓冲不够时返回 cJSON_false
 * 且不同版本对"已写入部分内容"的行为不一致——失败按安全侧处理，清空缓冲。
 */
static int cjson_to_buf(cJSON* root, char* out, size_t out_sz)
{
    /* cJSON_PrintPreallocated 的长度参数是 int；缓冲字节数远不到 INT_MAX，直接转 */
    const cJSON_bool ok = cJSON_PrintPreallocated(root, out, (int)out_sz, 0);
    const int len = ok ? (int)strlen(out) : 0;
    if (!ok)
    {
        out[0] = '\0';
    }
    cJSON_Delete(root);
    return len;
}

/* ===================== 对外接口：JSON 响应 ===================== */

int webconfig_page_config_json(char* out, size_t out_sz, const char* saved_ssid)
{
    if (out == NULL || out_sz == 0)
    {
        return 0;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        out[0] = '\0';
        return 0;
    }

    cJSON_AddStringToObject(root, "ssid", (saved_ssid != NULL) ? saved_ssid : "");
    return cjson_to_buf(root, out, out_sz);
}

int webconfig_page_saved_json(char* out, size_t out_sz, const char* ssid)
{
    if (out == NULL || out_sz == 0)
    {
        return 0;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        out[0] = '\0';
        return 0;
    }

    cJSON_AddBoolToObject(root, "ok", cJSON_True);
    cJSON_AddStringToObject(root, "ssid", (ssid != NULL) ? ssid : "");
    return cjson_to_buf(root, out, out_sz);
}

int webconfig_page_error_json(char* out, size_t out_sz, const char* msg)
{
    if (out == NULL || out_sz == 0)
    {
        return 0;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        out[0] = '\0';
        return 0;
    }

    cJSON_AddBoolToObject(root, "ok", cJSON_False);
    cJSON_AddStringToObject(root, "error", (msg != NULL) ? msg : "");
    return cjson_to_buf(root, out, out_sz);
}

/* ===================== 对外接口：JSON 请求解析 =====================
 * 一次只取 ssid / password 两个字符串字段：失败原因（语法错 / 字段缺失 / 类型不对 /
 * 值超长）都归一为 false，由调用方回 400 + 统一错误文案。
 *
 * cJSON 借 JSON 字符串的所有权：valuestring 指针指向树中的内存；strlcpy 拷到
 * 调用方缓冲后 cJSON_Delete()，原指针随即失效——所以先拷后删，不要在删了之后
 * 还去读 valuestring。
 */

bool webconfig_page_parse_save(const char* body,
                                char* ssid, size_t ssid_sz,
                                char* password, size_t password_sz)
{
    if (body == NULL || ssid == NULL || password == NULL ||
        ssid_sz == 0 || password_sz == 0)
    {
        return false;
    }

    cJSON* root = cJSON_Parse(body);
    if (root == NULL)
    {
        return false;
    }

    const cJSON* ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON* pass_item = cJSON_GetObjectItemCaseSensitive(root, "password");

    /* 字段缺失、类型不对（数字 / null / 对象 / 数组）、值为 NULL 都不能用完事；
     * 值放不下指定缓冲也按失败（调用方给的尺寸本就应该够用——SSID 32、密码 64）。 */
    const bool got_ssid =
        cJSON_IsString(ssid_item) && ssid_item->valuestring != NULL &&
        strlen(ssid_item->valuestring) < ssid_sz;

    const bool got_pass =
        cJSON_IsString(pass_item) && pass_item->valuestring != NULL &&
        strlen(pass_item->valuestring) < password_sz;

    const bool all_ok = got_ssid && got_pass;
    if (all_ok)
    {
        strlcpy(ssid, ssid_item->valuestring, ssid_sz);
        strlcpy(password, pass_item->valuestring, password_sz);
    }

    cJSON_Delete(root);
    return all_ok;
}