idf_component_register(
    SRCS "tcp_server.c"
    INCLUDE_DIRS "."
    # lwIP 提供 BSD socket 实现层（socket / bind / listen / accept / send /
    # recv / setsockopt 都从它出），以及 ip4addr_ntoa_r()（日志里格式化 bind
    # 地址）。其它用网络的组件（tcp_client / esp_http_server / esp_wifi 等）
    # 也会把它拉进来，这里列上是显式声明依赖。
    PRIV_REQUIRES lwip
)