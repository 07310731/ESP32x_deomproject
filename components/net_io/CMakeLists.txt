# net_io：网络收发层（业务只调 net_io.h 里的 3 个上行入口 + 1 个下行回调）
#
# REQUIRES  ：tcp_client / tcp_server / udp —— 只用到它们的基础 socket 封装，
#             这三个组件的头文件会出现在本组件的 public include 里（业务加了
#             net_io 依赖也不该直接调 socket，但留着不会出问题）
# PRIV_REQUIRES：webconfig（读 NVS 里的目标地址 / 心跳配置）、esp_wifi（判模式）、
#             esp_netif（取本机 IP）、esp_timer（send 耗时探针）
idf_component_register(SRCS "net_io.c"
                            "net_io_tcp_client.c"
                            "net_io_tcp_server.c"
                            "net_io_udp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES tcp_client tcp_server udp
                       PRIV_REQUIRES webconfig esp_wifi esp_netif esp_timer)
