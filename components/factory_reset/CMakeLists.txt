idf_component_register(
    SRCS "factory_reset.c"
    INCLUDE_DIRS "."
    # gpio：按键引脚配置与读取（走项目自己的 gpio 封装）
    REQUIRES gpio
    # esp_timer：按住时长用时间戳算，不受任务调度抖动影响
    PRIV_REQUIRES esp_timer
)
