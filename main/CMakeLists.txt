idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       # esp_event / esp_wifi：适配层里转发 WiFi 事件要用（webconfig 自身不依赖它们）
                       # esp_netif：TCP 服务端取本机 IP 作为 bind 地址
                       # esp_timer：上行发送的耗时探针用 esp_timer_get_time()
                       # factory_reset：BOOT 键长按恢复出厂设置（只依赖 gpio + esp_timer）
                       REQUIRES uart gpio wifi webconfig esp_event esp_wifi esp_netif tcp_client tcp_server udp esp_timer service_manager factory_reset)
