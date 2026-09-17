idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    # esp_event / esp_wifi：适配层里转发 WiFi 事件要用（webconfig 自身不依赖它们）
                    REQUIRES uart gpio wifi webconfig esp_event esp_wifi)
