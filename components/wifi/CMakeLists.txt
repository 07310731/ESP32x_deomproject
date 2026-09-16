idf_component_register(
    SRCS "wifi.c"
    INCLUDE_DIRS "."      # .h 所在目录，这里用当前目录
    REQUIRES esp_wifi esp_netif esp_event   # wifi.h 公开包含 esp_wifi.h，必须用 REQUIRES 才能传递给消费者
    PRIV_REQUIRES nvs_flash                 # nvs_flash 只在 wifi.c 内部使用
)
