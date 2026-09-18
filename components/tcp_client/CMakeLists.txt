idf_component_register(
    SRCS "tcp_client.c"
    INCLUDE_DIRS "."
    # lwIP 是 ESP-IDF 自带的 BSD socket 实现层（getaddrinfo / socket / connect /
    # send / recv / setsockopt 都从它出）。其它用网络的组件（esp_http_server /
    # esp_wifi 等）也会把它拉进来，所以这里列不列都能链上；列上是显式、避免
    # 以后把 webconfig / wifi 卸掉之后没人拉它。
    PRIV_REQUIRES lwip
)