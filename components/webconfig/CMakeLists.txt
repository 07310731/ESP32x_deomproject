idf_component_register(
    SRCS "webconfig.c"       # 配网流程：HTTP 服务、状态机、NVS
         "webconfig_page.c"  # 配网页面：渲染页面文件、SSID 转义、表单解析
    INCLUDE_DIRS "."     # .h 所在目录，这里用当前目录
    # 页面文件原样编进固件（字节级嵌入，不转义也不压缩）：会生成
    # _binary_<文件名>_start/_end 符号供 webconfig_page.c 读取。
    # 以后改页面只动这两个 html，重新编译即可，不用碰 C 代码。
    EMBED_TXTFILES "web/index.html"   # GET /      —— 配网表单页
                   "web/saved.html"   # POST /save —— 结果页
    # 没有 REQUIRES：webconfig.h 只用到 esp_err.h（属于 IDF 公共组件，不必声明）+
    # 标准库，不向外传递任何依赖，所以它是本项目里的一个"叶子"组件。
    PRIV_REQUIRES esp_http_server esp_timer nvs_flash  # 只在 webconfig.c 内部使用
)
