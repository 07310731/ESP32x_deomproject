/**
 * @file app_wifi.h
 * @brief WiFi 与配网的初始化接口
 *
 * 【为什么单独一个组件】
 * main.c 只该有"业务"：初始化、收数据处理、发数据。WiFi 选路、配网回调、
 * 事件转发、门控判定这些都是基础设施，放在 main 里会把业务淹没（原先光
 * 这一块就 600 行）。封装后 main.c 只留一行 app_wifi_start()。
 *
 * 【这一层对业务的可见影响】
 *   - 连上网络（STA 拿到 IP / 业务 AP 有客户端接入）时自动给 net_io 开门；
 *   - 掉线 / 客户端全走光时自动关门，net_io 里的收发任务挂起等恢复；
 *   - 网页保存配置后自动切 WiFi 参数、切服务协议、重载 net_io 配置。
 * 业务侧不需要感知这些，照常调 net_io_send_* 即可。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 WiFi 与配网（阻塞到 WiFi 起来 / 配网热点开好为止）
 *
 * @note 选路顺序：
 *       1. NVS 里有完整设备配置 → 按它启动，管理页挂在拿到的 IP 上；
 *       2. 没配置但 menuconfig 给了 STA SSID → 用烧死的凭证连；
 *       3. 模式是 AP-only 且没配置 → 直接当热点开（跳过配网 UI）；
 *       4. 其余 → 开配网热点，等用户连上 192.168.4.1 填表。
 *
 * @note 可重复调用（组件内部复用已初始化的 netif / 事件循环），
 *       "连接失败自动重开热点"就是走重复调用。
 *
 * @return ESP_OK 启动成功；其它为失败原因（原生 esp_err_t）
 */
esp_err_t app_wifi_start(void);

#ifdef __cplusplus
}
#endif
