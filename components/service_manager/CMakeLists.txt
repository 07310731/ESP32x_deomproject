idf_component_register(
    SRCS "service_manager.c"
    INCLUDE_DIRS "."
    # NVS 存模式选择 / 读历史值
    # net_io：三个 *_service_fn / *_request_stop / *_is_running 的实现放在那里
    # （socket 任务已下沉进组件）。这里只在链接期需要 —— ops 表按名字引用它们，
    # 头文件不必暴露，所以用 PRIV_REQUIRES。
    PRIV_REQUIRES nvs_flash tcp_client tcp_server udp net_io
    # webconfig：拿它起好的 HTTP 服务器句柄挂 /api/service-mode
    # esp_http_server：URI handler / httpd_* 数据类型
    REQUIRES webconfig esp_http_server)