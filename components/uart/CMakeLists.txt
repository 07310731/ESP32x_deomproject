idf_component_register(
    SRCS "uart.c"
    INCLUDE_DIRS "."      # .h 所在目录，这里用当前目录
    REQUIRES esp_driver_uart esp_driver_gpio
)
