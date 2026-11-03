idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       # 业务层只依赖"接口"组件，实现都在各组件内部：
                       #   app_wifi        WiFi / 配网初始化（app_wifi_start）
                       #   net_io          数据收发（3 个上行入口 + 1 个下行回调 + 门控）
                       #   service_manager 三种服务模式的切换管理
                       #   factory_reset   BOOT 键长按恢复出厂
                       #   gpio            指示灯 / 按键引脚
                       #   webconfig       版本号上报、恢复出厂擦除
                       REQUIRES gpio webconfig app_wifi net_io service_manager factory_reset)
