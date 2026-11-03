# app_wifi：WiFi / 配网初始化与状态适配
#
# 把 main.c 里原先 600 多行的 WiFi 选路、webconfig 端口回调、事件转发、
# 通讯门控判定收进来，对外只暴露 app_wifi_start()。
#
# REQUIRES：wifi（wifi_config_init / wifi_update_config）、webconfig（端口回调
#           与 NVS 读取）、net_io（门控 + 配置重载）、service_manager（网页
#           改协议时请求切换）
# PRIV_REQUIRES：esp_wifi / esp_event / esp_netif —— 只在 .c 里用
idf_component_register(SRCS "app_wifi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES wifi webconfig net_io service_manager
                       PRIV_REQUIRES esp_wifi esp_event esp_netif)
