idf_component_register(
    SRCS "service_manager.c"
    INCLUDE_DIRS "."
    # NVS 存模式选择 / 读历史值
    PRIV_REQUIRES nvs_flash tcp_client tcp_server udp
    # webconfig：拿它起好的 HTTP 服务器句柄挂 /api/service-mode
    # esp_http_server：URI handler / httpd_* 数据类型
    REQUIRES webconfig esp_http_server)