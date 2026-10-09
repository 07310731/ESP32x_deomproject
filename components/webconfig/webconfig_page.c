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

/* 取一个字符串字段的辅助：缺失 / 类型不对 / NULL 一律回 ""，调用方省得判空 */
static const char* s_or_empty(const cJSON* obj, const char* key)
{
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring != NULL)
    {
        return it->valuestring;
    }
    return "";
}

/* 取一个整型字段的辅助：缺失 / 类型不对一律回 0 */
static int i_or_zero(const cJSON* obj, const char* key)
{
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it))
    {
        return it->valueint;
    }
    return 0;
}

/* 取一个 bool 字段的辅助：缺失 / 类型不对一律回 false */
static bool b_or_false(const cJSON* obj, const char* key)
{
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsTrue(it);
}

/* 把字符串安全地拷进定长缓冲（cJSON valuestring 借的是树里的内存，先拷后删） */
static void copy_str(char* dst, size_t dst_sz, const char* src)
{
    if (dst == NULL || dst_sz == 0)
    {
        return;
    }
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_sz);
}

int webconfig_page_config_json(char* out, size_t out_sz, const webconfig_dev_cfg_t* cfg)
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

    /* 兼容字段：旧前端可能还在读根级 ssid。内容取自 cfg->sta_ssid
     * （AP-only 设备没填 sta_ssid，ssid 自然就是空串）。 */
    cJSON_AddStringToObject(root, "ssid", (cfg != NULL) ? cfg->sta_ssid : "");

    cJSON_AddStringToObject(root, "deviceName", (cfg != NULL) ? cfg->device_name : "");

    /* network = { wifiMode, ap:{ssid,ip,password}, sta:{ssid,password,mode,ip,mask,gateway,dns} } */
    cJSON* network = cJSON_AddObjectToObject(root, "network");
    if (network != NULL)
    {
        const char* mode = (cfg != NULL) ? cfg->wifi_mode : "";
        cJSON_AddStringToObject(network, "wifiMode", mode);

        cJSON* ap = cJSON_AddObjectToObject(network, "ap");
        if (ap != NULL)
        {
            cJSON_AddStringToObject(ap, "ssid",     (cfg != NULL) ? cfg->ap_ssid     : "");
            cJSON_AddStringToObject(ap, "ip",       (cfg != NULL) ? cfg->ap_ip       : "");
            cJSON_AddStringToObject(ap, "password", (cfg != NULL) ? cfg->ap_password : "");
        }

        cJSON* sta = cJSON_AddObjectToObject(network, "sta");
        if (sta != NULL)
        {
            cJSON_AddStringToObject(sta, "ssid",     (cfg != NULL) ? cfg->sta_ssid     : "");
            cJSON_AddStringToObject(sta, "password", (cfg != NULL) ? cfg->sta_password : "");
            cJSON_AddStringToObject(sta, "mode",     (cfg != NULL) ? cfg->sta_ip_mode  : "");
            cJSON_AddStringToObject(sta, "ip",       (cfg != NULL) ? cfg->sta_ip       : "");
            cJSON_AddStringToObject(sta, "mask",     (cfg != NULL) ? cfg->sta_netmask  : "");
            cJSON_AddStringToObject(sta, "gateway",  (cfg != NULL) ? cfg->sta_gateway  : "");
            cJSON_AddStringToObject(sta, "dns",      (cfg != NULL) ? cfg->sta_dns      : "");
        }
    }

    /* communication = { protocol, tcpServer:{port}, tcpClient:{serverIp,serverPort,heartbeat:{...}}, udp:{port} } */
    cJSON* comm = cJSON_AddObjectToObject(root, "communication");
    if (comm != NULL)
    {
        cJSON_AddStringToObject(comm, "protocol", (cfg != NULL) ? cfg->comm_protocol : "");

        /* available = menuconfig 使能的协议列表：前端协议按钮组按它显隐、
         * 右栏"可用"行只对使能协议输出——menuconfig 勾掉 TCP 服务端后，
         * 页面不会再出现/选中它（NVS 存过也无效，webconfig.c 已过滤）。 */
        cJSON* avail = cJSON_AddArrayToObject(comm, "available");
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
        cJSON_AddItemToArray(avail, cJSON_CreateString("tcpServer"));
#endif
#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
        cJSON_AddItemToArray(avail, cJSON_CreateString("tcpClient"));
#endif
#if CONFIG_SERVICE_UDP_ENABLE
        cJSON_AddItemToArray(avail, cJSON_CreateString("udp"));
#endif

        cJSON* srv = cJSON_AddObjectToObject(comm, "tcpServer");
        if (srv != NULL)
        {
            cJSON_AddNumberToObject(srv, "port", (cfg != NULL) ? (double)cfg->tcp_server_port : 0);
        }

        cJSON* cli = cJSON_AddObjectToObject(comm, "tcpClient");
        if (cli != NULL)
        {
            cJSON_AddStringToObject(cli, "serverIp",   (cfg != NULL) ? cfg->tcp_client_server_ip   : "");
            cJSON_AddNumberToObject(cli, "serverPort", (cfg != NULL) ? (double)cfg->tcp_client_server_port : 0);

            cJSON* hb = cJSON_AddObjectToObject(cli, "heartbeat");
            if (hb != NULL)
            {
                const bool en = (cfg != NULL) && cfg->tcp_client_hb_enabled;
                cJSON_AddBoolToObject(hb, "enabled", en ? cJSON_True : cJSON_False);
                cJSON_AddStringToObject(hb, "format",   (cfg != NULL) ? cfg->tcp_client_hb_format   : "");
                cJSON_AddNumberToObject(hb, "interval", (cfg != NULL) ? (double)cfg->tcp_client_hb_interval : 0);
                cJSON_AddStringToObject(hb, "content",  (cfg != NULL) ? cfg->tcp_client_hb_content  : "");
            }
        }

        cJSON* udp = cJSON_AddObjectToObject(comm, "udp");
        if (udp != NULL)
        {
            cJSON_AddNumberToObject(udp, "port", (cfg != NULL) ? (double)cfg->udp_port : 0);
        }
    }

    return cjson_to_buf(root, out, out_sz);
}

int webconfig_page_saved_json(char* out, size_t out_sz)
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

/* ===================== 对外接口：/api/status 的 running 子树拼装 =====================
 * webconfig.c 拿 NVS 配置（cfg）+ 运行时快照（rt）传过来，本函数按 spec 拼
 * 出 {"running": { network, communication }} 这棵子树 JSON 字符串。
 *
 * 调用方拿到字符串后再 cJSON_Parse() → 取 "running" 节点 → 挂到自己的 root，
 * 同时把 fw/app/uptime/mode/state 等"页头芯片"字段加到 root 上一并输出。
 * 拆分原因：那些元信息由 webconfig.c 直接出，不让 page 层知道 fw_version /
 * uptime 是怎么算的。
 *
 * 字段取数规则：
 *   - "配置"（SSID / 密码 / 端口 / 协议名 / 对端 IP）—— 取自 cfg
 *   - "运行时"（IP / 掩码 / 网关 / DNS / RSSI / 协议是否在跑）—— 取自 rt
 *   - cfg 缺失字段（首次启动 NVS 空）—— 输出空串 / 0，前端靠 placeholder
 *
 * 不做 NULL 检查：rt 永远非 NULL，cfg 可以 NULL（首启）。 */
int webconfig_page_running_json(char* out, size_t out_sz,
                                const webconfig_dev_cfg_t* cfg,
                                const webconfig_dev_running_t* rt)
{
    if (out == NULL || out_sz == 0 || rt == NULL) return 0;

    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        out[0] = '\0';
        return 0;
    }

    /* ---- running.network ----
     * 结构跟 /api/config 一致（前端可以复用 applyIn() 填表单）；字段值
     * 全部取自 cfg（NVS 里存的"用户保存的"）——前端在右栏展示的就是
     * "我配了什么"，跟表单 S. 完全一致。运行时实际值通过 *_running
     * 子字段（ap.running、sta.ip_running 等）单独给，不混在主字段里。 */
    cJSON* rnet = cJSON_AddObjectToObject(root, "network");
    if (rnet != NULL)
    {
        cJSON_AddStringToObject(rnet, "wifiMode", (cfg != NULL) ? cfg->wifi_mode : "");

        /* AP 段：SSID / 密码 / 静态 IP 取自 cfg；running.ip 是 AP netif
         * 当前实际地址（dev_apply 之后生效的，可能跟 cfg 不一样） */
        cJSON* ap = cJSON_AddObjectToObject(rnet, "ap");
        if (ap != NULL)
        {
            cJSON_AddStringToObject(ap, "ssid",     (cfg != NULL) ? cfg->ap_ssid     : "");
            cJSON_AddStringToObject(ap, "password", (cfg != NULL) ? cfg->ap_password : "");
            cJSON_AddStringToObject(ap, "ip",       (cfg != NULL) ? cfg->ap_ip       : "");
            cJSON_AddStringToObject(ap, "running",  rt->ap_ip);
            cJSON_AddNumberToObject(ap, "stations", (double)rt->ap_sta_count);
            /* *_running = 设备当前实际在用的 SSID / 密码（esp_wifi_get_config）。
             * 前端在 NVS 没存该字段时用它兜底回显"当前使用的值"。 */
            cJSON_AddStringToObject(ap, "ssid_running",     rt->ap_ssid);
            cJSON_AddStringToObject(ap, "password_running", rt->ap_password);
        }

        /* STA 段：SSID / 密码 / IP 模式（dhcp/static）取自 cfg；ip/mask/gw/dns
         * 这四个静态配置项也取自 cfg（用户填的），但 *_running 字段显示 DHCP
         * 拿到 / 静态设置后的实际值；rssi 和 connected 是纯运行时 */
        cJSON* sta = cJSON_AddObjectToObject(rnet, "sta");
        if (sta != NULL)
        {
            cJSON_AddStringToObject(sta, "ssid",     (cfg != NULL) ? cfg->sta_ssid     : "");
            cJSON_AddStringToObject(sta, "password", (cfg != NULL) ? cfg->sta_password : "");
            cJSON_AddStringToObject(sta, "mode",     (cfg != NULL) ? cfg->sta_ip_mode  : "");
            cJSON_AddStringToObject(sta, "ip",       (cfg != NULL) ? cfg->sta_ip       : "");
            cJSON_AddStringToObject(sta, "mask",     (cfg != NULL) ? cfg->sta_netmask  : "");
            cJSON_AddStringToObject(sta, "gateway",  (cfg != NULL) ? cfg->sta_gateway  : "");
            cJSON_AddStringToObject(sta, "dns",      (cfg != NULL) ? cfg->sta_dns      : "");
            cJSON_AddStringToObject(sta, "ip_running",   rt->sta_ip);
            cJSON_AddStringToObject(sta, "mask_running", rt->sta_mask);
            cJSON_AddStringToObject(sta, "gw_running",   rt->sta_gw);
            cJSON_AddStringToObject(sta, "dns_running",  rt->sta_dns);
            /* *_running = 设备当前实际在用的 SSID / 密码（esp_wifi_get_config）。
             * 前端在 NVS 没存该字段时用它兜底回显"当前使用的值"。 */
            cJSON_AddStringToObject(sta, "ssid_running",     rt->sta_ssid);
            cJSON_AddStringToObject(sta, "password_running", rt->sta_password);
            cJSON_AddNumberToObject(sta, "rssi", (double)rt->sta_rssi);
            cJSON_AddBoolToObject(sta, "connected", rt->sta_connected ? cJSON_True : cJSON_False);
        }
    }

    /* ---- running.communication ----
     * 跟 running.network 平级；字段值全部取自 cfg（NVS 存的）——右栏展示
     * 的就是"我配的协议 = X"，跟表单 S.communication.protocol 一致。
     * 设备实际跑的协议通过 tcpServer.running / tcpClient.running /
     * udp.running 这三个 bool 单独标（"我才是当前在跑的那个"）。 */
    cJSON* rcomm = cJSON_AddObjectToObject(root, "communication");
    if (rcomm != NULL)
    {
        cJSON_AddStringToObject(rcomm, "protocol", (cfg != NULL) ? cfg->comm_protocol : "");

        /* 只输出 menuconfig 使能的协议——勾掉 TCP 服务端后 status 里没有
         * tcpServer 对象，前端右栏自然不会再渲染"✓ 在跑"那一行 */
#if CONFIG_SERVICE_TCP_SERVER_ENABLE
        cJSON* srv = cJSON_AddObjectToObject(rcomm, "tcpServer");
        if (srv != NULL)
        {
            cJSON_AddNumberToObject(srv, "port", (cfg != NULL) ? (double)cfg->tcp_server_port : 0);
            cJSON_AddBoolToObject(srv, "running", strcmp(rt->comm_proto, "tcpServer") == 0
                                                ? cJSON_True : cJSON_False);
        }
#endif

#if CONFIG_SERVICE_TCP_CLIENT_ENABLE
        cJSON* cli = cJSON_AddObjectToObject(rcomm, "tcpClient");
        if (cli != NULL)
        {
            cJSON_AddStringToObject(cli, "serverIp",   (cfg != NULL) ? cfg->tcp_client_server_ip   : "");
            cJSON_AddNumberToObject(cli, "serverPort", (cfg != NULL) ? (double)cfg->tcp_client_server_port : 0);
            cJSON_AddBoolToObject(cli, "running",     strcmp(rt->comm_proto, "tcpClient") == 0
                                                    ? cJSON_True : cJSON_False);
            cJSON_AddBoolToObject(cli, "connected",   rt->tcp_client_connected
                                                    ? cJSON_True : cJSON_False);
        }
#endif

#if CONFIG_SERVICE_UDP_ENABLE
        cJSON* udp = cJSON_AddObjectToObject(rcomm, "udp");
        if (udp != NULL)
        {
            cJSON_AddNumberToObject(udp, "port", (cfg != NULL) ? (double)cfg->udp_port : 0);
            cJSON_AddBoolToObject(udp, "running", strcmp(rt->comm_proto, "udp") == 0
                                              ? cJSON_True : cJSON_False);
        }
#endif
    }

    return cjson_to_buf(root, out, out_sz);
}

/* ===================== 对外接口：JSON 请求解析 =====================
 * 把 buildOut() 输出的完整设备配置解析到 webconfig_dev_cfg_t。
 *
 * 字段多，但每条都只判三件事：存在 / 类型对 / 长度放得下。
 * 业务规则（wifi 合法 / 端口范围 / dhcp vs static 等）由调用方
 * （webconfig.c 的 /save handler）负责。
 *
 * cJSON 借 JSON 字符串的所有权：valuestring 指针指向树中的内存；strlcpy
 * 拷到调用方缓冲后 cJSON_Delete()，原指针随即失效——所以先拷后删，
 * 不要在删了之后还去读 valuestring。
 *
 * 网络/通讯字段全部要求存在（即使值是空串）—— 这样"用户清空了密码"和
 * "用户没填密码"能在 webconfig.c 里被分别处理：前者保存、后者当 400 拒收。
 * 实际的"是否必填"由 webconfig.c 在拿到值后再判，本层不掺和。
 */

bool webconfig_page_parse_save(const char* body, webconfig_dev_cfg_t* cfg)
{
    if (body == NULL || cfg == NULL)
    {
        return false;
    }

    cJSON* root = cJSON_Parse(body);
    if (root == NULL)
    {
        return false;
    }

    /* ---- 顶层：deviceName ---- */
    copy_str(cfg->device_name, sizeof(cfg->device_name),
             s_or_empty(root, "deviceName"));

    /* ---- network ---- */
    const cJSON* net = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (!cJSON_IsObject(net))
    {
        cJSON_Delete(root);
        return false;
    }
    copy_str(cfg->wifi_mode, sizeof(cfg->wifi_mode), s_or_empty(net, "wifiMode"));

    const cJSON* ap = cJSON_GetObjectItemCaseSensitive(net, "ap");
    if (!cJSON_IsObject(ap))
    {
        cJSON_Delete(root);
        return false;
    }
    copy_str(cfg->ap_ssid,     sizeof(cfg->ap_ssid),     s_or_empty(ap, "ssid"));
    copy_str(cfg->ap_ip,       sizeof(cfg->ap_ip),       s_or_empty(ap, "ip"));
    copy_str(cfg->ap_password, sizeof(cfg->ap_password), s_or_empty(ap, "password"));

    const cJSON* sta = cJSON_GetObjectItemCaseSensitive(net, "sta");
    if (!cJSON_IsObject(sta))
    {
        cJSON_Delete(root);
        return false;
    }
    copy_str(cfg->sta_ssid,     sizeof(cfg->sta_ssid),     s_or_empty(sta, "ssid"));
    copy_str(cfg->sta_password, sizeof(cfg->sta_password), s_or_empty(sta, "password"));
    copy_str(cfg->sta_ip_mode,  sizeof(cfg->sta_ip_mode),  s_or_empty(sta, "mode"));
    copy_str(cfg->sta_ip,       sizeof(cfg->sta_ip),       s_or_empty(sta, "ip"));
    copy_str(cfg->sta_netmask,  sizeof(cfg->sta_netmask),  s_or_empty(sta, "mask"));
    copy_str(cfg->sta_gateway,  sizeof(cfg->sta_gateway),  s_or_empty(sta, "gateway"));
    copy_str(cfg->sta_dns,      sizeof(cfg->sta_dns),      s_or_empty(sta, "dns"));

    /* ---- communication ---- */
    const cJSON* comm = cJSON_GetObjectItemCaseSensitive(root, "communication");
    if (!cJSON_IsObject(comm))
    {
        cJSON_Delete(root);
        return false;
    }
    copy_str(cfg->comm_protocol, sizeof(cfg->comm_protocol), s_or_empty(comm, "protocol"));

    const cJSON* tcpSrv = cJSON_GetObjectItemCaseSensitive(comm, "tcpServer");
    if (cJSON_IsObject(tcpSrv))
    {
        const int p = i_or_zero(tcpSrv, "port");
        cfg->tcp_server_port = (p < 0 || p > 65535) ? 0 : (uint16_t)p;
    }
    else
    {
        cfg->tcp_server_port = 0;
    }

    const cJSON* tcpCli = cJSON_GetObjectItemCaseSensitive(comm, "tcpClient");
    if (cJSON_IsObject(tcpCli))
    {
        copy_str(cfg->tcp_client_server_ip, sizeof(cfg->tcp_client_server_ip),
                 s_or_empty(tcpCli, "serverIp"));
        const int p = i_or_zero(tcpCli, "serverPort");
        cfg->tcp_client_server_port = (p < 0 || p > 65535) ? 0 : (uint16_t)p;

        const cJSON* hb = cJSON_GetObjectItemCaseSensitive(tcpCli, "heartbeat");
        if (cJSON_IsObject(hb))
        {
            cfg->tcp_client_hb_enabled = b_or_false(hb, "enabled");
            copy_str(cfg->tcp_client_hb_format, sizeof(cfg->tcp_client_hb_format),
                     s_or_empty(hb, "format"));
            const int iv = i_or_zero(hb, "interval");
            cfg->tcp_client_hb_interval = (iv < 0 || iv > 65535) ? 0 : (uint16_t)iv;
            copy_str(cfg->tcp_client_hb_content, sizeof(cfg->tcp_client_hb_content),
                     s_or_empty(hb, "content"));
        }
        else
        {
            cfg->tcp_client_hb_enabled = false;
            cfg->tcp_client_hb_format[0] = '\0';
            cfg->tcp_client_hb_interval = 0;
            cfg->tcp_client_hb_content[0] = '\0';
        }
    }
    else
    {
        cfg->tcp_client_server_ip[0] = '\0';
        cfg->tcp_client_server_port = 0;
        cfg->tcp_client_hb_enabled = false;
        cfg->tcp_client_hb_format[0] = '\0';
        cfg->tcp_client_hb_interval = 0;
        cfg->tcp_client_hb_content[0] = '\0';
    }

    const cJSON* udp = cJSON_GetObjectItemCaseSensitive(comm, "udp");
    if (cJSON_IsObject(udp))
    {
        const int p = i_or_zero(udp, "port");
        cfg->udp_port = (p < 0 || p > 65535) ? 0 : (uint16_t)p;
    }
    else
    {
        cfg->udp_port = 0;
    }

    cJSON_Delete(root);
    return true;
}

/* ===================== 对外接口：partial JSON 解析 =====================
 * webconfig_page_parse_save() 是"全量覆盖"——JSON 缺字段会用空串/0 覆盖
 * cfg，破坏已有 NVS 数据。这里提供"只覆盖提交的字段"版本：调用方先
 * 把 NVS 现状读到 cfg，再调本函数——JSON 里没出现的字段 cfg 不动，
 * 出现了的字段（含空串）才覆盖。
 *
 * 关键区分：
 *   - key 缺失（cJSON_GetObjectItemCaseSensitive 返回 NULL）→ 不覆盖
 *   - key 是字符串（含 ""）→ 覆盖成那个字符串
 *   - key 是 null → 视同空串（防御前端某些库会发 null）
 *   - key 是其它类型 → 忽略
 *
 * 配合 webconfig.c 的 /save handler：先 webconfig_load_dev_cfg() 读 NVS
 * 现状填底、再调本函数覆盖部分字段、再校验、再整块写回 NVS。这样
 * "只改了设备名点保存"不会把 NVS 里的 wifi / 通讯抹掉。 */

/* "key 存在 → 返回该字符串（"" 也算存在）"；"key 缺失 → 返回 NULL"。
 * 返回的指针仍属 cJSON 树，调用方必须先 copy 后 cJSON_Delete。 */
static const char* s_if_present(const cJSON* obj, const char* key)
{
    if (obj == NULL) return NULL;
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (it == NULL) return NULL;                 /* 缺失 */
    if (cJSON_IsString(it)) return it->valuestring;
    if (cJSON_IsNull(it))   return "";           /* 显式 null → 空串 */
    return NULL;                                 /* 其它类型当作缺失 */
}

/* 整段对象存在性 + 类型判断：用来决定要不要进字段循环 */
static bool s_obj_present(const cJSON* obj, const char* key)
{
    if (obj == NULL) return false;
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsObject(it);
}

/* 数字字段：缺失 / 非数字 / 越界都视为缺失 */
static bool u16_if_present(const cJSON* obj, const char* key, uint16_t* out)
{
    if (obj == NULL) return false;
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (it == NULL || !cJSON_IsNumber(it)) return false;
    const int v = it->valueint;
    if (v < 0 || v > 65535) return false;
    *out = (uint16_t)v;
    return true;
}

/* bool 字段：缺失视为缺失；true/false/null 都视作有值 */
static bool bool_if_present(const cJSON* obj, const char* key, bool* out)
{
    if (obj == NULL) return false;
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (it == NULL) return false;
    if (cJSON_IsTrue(it))  { *out = true;  return true; }
    if (cJSON_IsFalse(it)) { *out = false; return true; }
    return false;
}

bool webconfig_page_parse_save_partial(const char* body, webconfig_dev_cfg_t* cfg)
{
    if (body == NULL || cfg == NULL) return false;

    cJSON* root = cJSON_Parse(body);
    if (root == NULL) return false;

    /* ---- 顶层 deviceName ---- */
    const char* v = s_if_present(root, "deviceName");
    if (v != NULL) copy_str(cfg->device_name, sizeof(cfg->device_name), v);

    /* ---- network（整段可能缺失 → 整段跳过） ---- */
    const cJSON* net = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (cJSON_IsObject(net))
    {
        v = s_if_present(net, "wifiMode");
        if (v != NULL) copy_str(cfg->wifi_mode, sizeof(cfg->wifi_mode), v);

        if (s_obj_present(net, "ap"))
        {
            const cJSON* ap = cJSON_GetObjectItemCaseSensitive(net, "ap");
            v = s_if_present(ap, "ssid");
            if (v != NULL) copy_str(cfg->ap_ssid,     sizeof(cfg->ap_ssid),     v);
            v = s_if_present(ap, "ip");
            if (v != NULL) copy_str(cfg->ap_ip,       sizeof(cfg->ap_ip),       v);
            v = s_if_present(ap, "password");
            if (v != NULL) copy_str(cfg->ap_password, sizeof(cfg->ap_password), v);
        }

        if (s_obj_present(net, "sta"))
        {
            const cJSON* sta = cJSON_GetObjectItemCaseSensitive(net, "sta");
            v = s_if_present(sta, "ssid");
            if (v != NULL) copy_str(cfg->sta_ssid,     sizeof(cfg->sta_ssid),     v);
            v = s_if_present(sta, "password");
            if (v != NULL) copy_str(cfg->sta_password, sizeof(cfg->sta_password), v);
            v = s_if_present(sta, "mode");
            if (v != NULL) copy_str(cfg->sta_ip_mode,  sizeof(cfg->sta_ip_mode),  v);
            v = s_if_present(sta, "ip");
            if (v != NULL) copy_str(cfg->sta_ip,       sizeof(cfg->sta_ip),       v);
            v = s_if_present(sta, "mask");
            if (v != NULL) copy_str(cfg->sta_netmask,  sizeof(cfg->sta_netmask),  v);
            v = s_if_present(sta, "gateway");
            if (v != NULL) copy_str(cfg->sta_gateway,  sizeof(cfg->sta_gateway),  v);
            v = s_if_present(sta, "dns");
            if (v != NULL) copy_str(cfg->sta_dns,      sizeof(cfg->sta_dns),      v);
        }
    }

    /* ---- communication（整段可能缺失） ---- */
    const cJSON* comm = cJSON_GetObjectItemCaseSensitive(root, "communication");
    if (cJSON_IsObject(comm))
    {
        v = s_if_present(comm, "protocol");
        if (v != NULL) copy_str(cfg->comm_protocol, sizeof(cfg->comm_protocol), v);

        if (s_obj_present(comm, "tcpServer"))
        {
            const cJSON* tcpSrv = cJSON_GetObjectItemCaseSensitive(comm, "tcpServer");
            uint16_t p;
            if (u16_if_present(tcpSrv, "port", &p)) cfg->tcp_server_port = p;
        }

        if (s_obj_present(comm, "tcpClient"))
        {
            const cJSON* tcpCli = cJSON_GetObjectItemCaseSensitive(comm, "tcpClient");
            v = s_if_present(tcpCli, "serverIp");
            if (v != NULL) copy_str(cfg->tcp_client_server_ip,
                                    sizeof(cfg->tcp_client_server_ip), v);
            uint16_t p;
            if (u16_if_present(tcpCli, "serverPort", &p)) cfg->tcp_client_server_port = p;

            if (s_obj_present(tcpCli, "heartbeat"))
            {
                const cJSON* hb = cJSON_GetObjectItemCaseSensitive(tcpCli, "heartbeat");
                bool b;
                if (bool_if_present(hb, "enabled", &b)) cfg->tcp_client_hb_enabled = b;
                v = s_if_present(hb, "format");
                if (v != NULL) copy_str(cfg->tcp_client_hb_format,
                                        sizeof(cfg->tcp_client_hb_format), v);
                uint16_t iv;
                if (u16_if_present(hb, "interval", &iv)) cfg->tcp_client_hb_interval = iv;
                v = s_if_present(hb, "content");
                if (v != NULL) copy_str(cfg->tcp_client_hb_content,
                                        sizeof(cfg->tcp_client_hb_content), v);
            }
        }

        if (s_obj_present(comm, "udp"))
        {
            const cJSON* udp = cJSON_GetObjectItemCaseSensitive(comm, "udp");
            uint16_t p;
            if (u16_if_present(udp, "port", &p)) cfg->udp_port = p;
        }
    }

    cJSON_Delete(root);
    return true;
}