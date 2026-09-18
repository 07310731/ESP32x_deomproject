idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       # esp_event / esp_wifi：适配层里转发 WiFi 事件要用（webconfig 自身不依赖它们）
                       # esp_timer：上行发送的耗时探针用 esp_timer_get_time()
                       REQUIRES uart gpio wifi webconfig esp_event esp_wifi tcp_client esp_timer)
