idf_component_register(
    SRCS "gpio.c"
    INCLUDE_DIRS "."      # .h 所在目录，这里用当前目录
    PRIV_REQUIRES esp_driver_gpio
)