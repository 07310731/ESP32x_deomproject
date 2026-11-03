idf_component_register(
    SRCS "webconfig.c"       # 配网流程：HTTP 服务、状态机、NVS
         "webconfig_page.c"  # 配网页面：渲染页面文件、SSID 转义、表单解析
    INCLUDE_DIRS "."     # .h 所在目录，这里用当前目录
    # 页面文件原样编进固件（字节级嵌入，不转义也不压缩）：会生成
    # _binary_<文件名>_start/_end 符号供 webconfig_page.c 读取。
    # 以后改页面只动这两个 html，重新编译即可，不用碰 C 代码。
    # 只有一个页面："表单"和"已保存"两个视图都在里面，由页面内联 JS 切换
    EMBED_TXTFILES "web/index.html"   # GET / —— 配网页（静态，无占位符）
    # REQUIRES esp_http_server：webconfig.h 里直接 include 了 esp_http_server.h
    # （httpd_req_t 出现在公开接口上），属于"公开头依赖"，必须传递出去 ——
    # 否则依赖 webconfig 的组件（如 net_io）单独编译时会报头文件找不到。
    REQUIRES esp_http_server
    PRIV_REQUIRES esp_timer esp_wifi app_update nvs_flash cjson service_manager  # esp_wifi: esp_wifi_get_mode / esp_wifi_sta_get_ap_info; service_manager: service_manager_get_active() / *_is_running() —— /api/status 拿运行中协议用; app_update: esp_app_get_description()，供 /api/status 用
)
