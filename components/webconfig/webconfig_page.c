/* ===================== 配网页面实现 =====================
 * 页面相关的一切都收在这个文件里：
 *   - web/ 目录下 .html 页面的渲染（页面文件由 CMake 编进固件，见同目录 CMakeLists.txt）
 *   - SSID 回显的 HTML 转义
 *   - 表单提交的解析（application/x-www-form-urlencoded）
 * webconfig.c 只负责"把这里的渲染结果发出去""把解析出来的字段存起来"。
 *
 * 改页面（配色、文案、加输入项、加前端 JS）只改 web/ 下的 html；
 * 只有改了"回显方式"或"表单字段规则"才需要动本文件。
 */

#include "webconfig_page.h"

#include <string.h>

/* ===================== 页面文件（编译期嵌入固件） =====================
 * web/ 目录下的 .html 由 CMakeLists.txt 里的 EMBED_TXTFILES 转成汇编数据编进 flash，
 * 这里通过链接器生成的 _binary_<文件名>_start / _end 符号取到文件内容：
 *   - 符号名只取"文件名"，目录不参与命名，所以 web/index.html 对应
 *     _binary_index_html_start。别把两个同名文件放进不同目录，会撞符号；
 *   - TEXT 模式会在文件末尾补一个 '\0'，所以结尾那个符号指向的是 '\0' 之后，
 *     真实内容长度是 end - start - 1（render_template() 里的 tpl_len - 1）。 */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char saved_html_start[] asm("_binary_saved_html_start");
extern const char saved_html_end[] asm("_binary_saved_html_end");

/* ===================== 内部工具：转义 / 渲染 ===================== */

/** 页面文件里回显 SSID 的占位符，渲染时整段替换掉 */
#define SSID_PLACEHOLDER "{{ssid}}"
#define SSID_PLACEHOLDER_LEN (sizeof(SSID_PLACEHOLDER) - 1)

/** SSID 转义缓冲：31 字节的 SSID 最坏情况每个字节都变 5 字节实体，再加 '\0' */
#define SSID_ESC_MAX (31 * 5 + 1)

/**
 * @brief 把字符串转义成可安全嵌入 HTML 的形式（& < > ' 五个字符）
 *
 * @note 只用于把用户填的 SSID 回显进页面：SSID 可能含引号或尖括号，
 *       不转义会破坏页面结构。
 */
static void html_escape(char* dst, size_t cap, const char* src)
{
    size_t o = 0;

    for (size_t i = 0; src[i] != '\0' && o + 6 < cap; i++)
    {
        switch (src[i])
        {
            case '&':
                memcpy(dst + o, "&amp;", 5);
                o += 5;
                break;
            case '<':
                memcpy(dst + o, "&lt;", 4);
                o += 4;
                break;
            case '>':
                memcpy(dst + o, "&gt;", 4);
                o += 4;
                break;
            case '\'':
                memcpy(dst + o, "&#39;", 5);
                o += 5;
                break;
            default:
                dst[o++] = src[i];
                break;
        }
    }

    dst[o] = '\0';
}

/**
 * @brief 渲染一个页面文件：整体拷贝，并把 {{ssid}} 换成 HTML 转义后的 SSID
 *
 * @param tpl_begin 页面内容起始（_binary_xxx_start）
 * @param tpl_end   页面内容结束（_binary_xxx_end，指向末尾补的那个 '\0' 之后）
 *
 * @return 写入 out 的字节数（不含结尾 '\0'），可直接作为 httpd_resp_send() 的长度；
 *         缓冲不够时截断（页面尾部丢失），返回值一定小于 out_sz
 */
static int render_template(char* out, size_t out_sz, const char* tpl_begin, const char* tpl_end,
                           const char* ssid)
{
    char esc[SSID_ESC_MAX];
    html_escape(esc, sizeof(esc), (ssid != NULL) ? ssid : "");

    const size_t esc_len = strlen(esc);
    const size_t tpl_len = (size_t)(tpl_end - tpl_begin); /* 含末尾那个 '\0' */
    size_t o = 0;

    for (size_t i = 0; i + 1 < tpl_len && o + 1 < out_sz;)
    {
        /* 命中占位符就整段换成 SSID，否则原样拷贝一个字节 */
        if (i + SSID_PLACEHOLDER_LEN <= tpl_len - 1 &&
            memcmp(tpl_begin + i, SSID_PLACEHOLDER, SSID_PLACEHOLDER_LEN) == 0)
        {
            for (size_t k = 0; k < esc_len && o + 1 < out_sz; k++)
            {
                out[o++] = esc[k];
            }
            i += SSID_PLACEHOLDER_LEN;
            continue;
        }

        out[o++] = tpl_begin[i++];
    }

    out[o] = '\0';
    return (int)o;
}

/* ===================== 对外接口：渲染 ===================== */

int webconfig_page_render_form(char* out, size_t out_sz, const char* saved_ssid)
{
    if (out == NULL || out_sz == 0)
    {
        return 0;
    }

    return render_template(out, out_sz, index_html_start, index_html_end, saved_ssid);
}

int webconfig_page_render_saved(char* out, size_t out_sz, const char* ssid)
{
    if (out == NULL || out_sz == 0)
    {
        return 0;
    }

    return render_template(out, out_sz, saved_html_start, saved_html_end, ssid);
}

/* ===================== 表单解析 ===================== */

/**
 * @brief 十六进制字符转数值，非法字符返回 -1
 */
static int hexval(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * @brief URL 解码（%XX 与 '+' 转空格），解码结果写入 out
 *
 * @note SSID 可以是任意字节（包括 UTF-8 中文），本函数按字节原样透传，
 *       只处理编码形式。值超长或出现非法转义返回 false，out 内容不可用。
 */
static bool urldecode(char* out, size_t out_sz, const char* in, size_t in_len)
{
    size_t o = 0;

    for (size_t i = 0; i < in_len; i++)
    {
        char c = in[i];

        if (c == '+')
        {
            c = ' ';
        }
        else if (c == '%')
        {
            if (i + 2 >= in_len)
            {
                return false; /* % 后面没跟够两个十六进制字符 */
            }
            const int hi = hexval(in[i + 1]);
            const int lo = hexval(in[i + 2]);
            if (hi < 0 || lo < 0)
            {
                return false;
            }
            c = (char)((hi << 4) | lo);
            i += 2;
        }

        if (o + 1 >= out_sz)
        {
            return false; /* 解码结果放不下（字段超长） */
        }
        out[o++] = c;
    }

    out[o] = '\0';
    return true;
}

bool webconfig_page_form_get(const char* body, const char* key, char* out, size_t out_sz)
{
    if (body == NULL || key == NULL || out == NULL || out_sz == 0)
    {
        return false;
    }

    const size_t key_len = strlen(key);
    const char* seg = body;

    while (seg != NULL && *seg != '\0')
    {
        const char* seg_end = strchr(seg, '&');
        const size_t seg_len = (seg_end != NULL) ? (size_t)(seg_end - seg) : strlen(seg);
        const char* eq = memchr(seg, '=', seg_len);

        if (eq != NULL && (size_t)(eq - seg) == key_len && strncmp(seg, key, key_len) == 0)
        {
            return urldecode(out, out_sz, eq + 1, seg_len - key_len - 1);
        }

        seg = (seg_end != NULL) ? seg_end + 1 : NULL;
    }

    return false;
}
