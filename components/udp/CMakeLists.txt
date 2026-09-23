idf_component_register(
    SRCS "udp.c"
    INCLUDE_DIRS "."
    # lwIP 提供 BSD socket 实现层（socket / bind / sendto / recvfrom /
    # setsockopt 都从它出），以及 ip4addr_ntoa_r()（日志里格式化 bind 地址）。
    PRIV_REQUIRES lwip
)