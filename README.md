# roboot_mc —— ESP32 网络通讯工程模板

一个可以直接拿来当**新项目起点的 ESP32 工程模板**：WiFi 配网、网页配置、TCP 客户端 /
TCP 服务端 / UDP 三选一的通讯框架、断线重连、心跳保活、恢复出厂，全部做成组件，
业务代码只在 `main/main.c` 里写"收到数据怎么处理、要发什么"。

- 目标芯片：`esp32`（默认）。代码只用 `esp_wifi` / `esp_netif` / BSD socket，理论上
  同样适用于 ESP32-S3 / C3 / C6 等系列，切换用 `idf.py set-target <chip>`，**但需要自行
  重新验证**（射频参数、核心数、内存差异）。
- ESP-IDF：v6.0.2 上验证通过。
- Flash：2 MB，分区表 `partitions_singleapp.csv`（单 app，未启用 OTA）。

---

## 一、它实现了什么

| 能力 | 说明 |
|---|---|
| **WiFi 自动选路** | NVS 有完整配置 → 直连；menuconfig 有 STA 凭证 → 连它；AP-only → 直接开热点；都没有 → 开配网热点 |
| **网页配网 + 配置管理** | 连上热点后打开 `http://192.168.4.1/`，可视化配置网络 / 通讯参数；支持"只保存当前模块"（partial 保存） |
| **三种通讯服务，运行时可切** | TCP 客户端（长连接 + 重连 + 心跳）、TCP 服务端（多客户端 + 槽位回收）、UDP（echo）；同一时刻只跑一种 |
| **收发解耦** | 上行投队列、下行回调业务，两个常驻任务分别阻塞在队列上，发送不会被接收阻塞 |
| **断线自愈** | TCP 客户端自动重连；idle 心跳探测"假连接"；TCP 服务端 keepalive + 空闲超时回收僵尸连接 |
| **通讯门控** | 没网 / 处于配网热点时通讯服务自动挂起，网络恢复后自动继续，业务不用管时序 |
| **恢复出厂设置** | BOOT 键（GPIO0）长按 5 秒，或网页按钮：清空 NVS 配置并重启 |
| **基础外设封装** | GPIO 读写、UART 收发（带超时）、工厂复位按键检测 |
| **运行状态可查** | `/api/status` 返回当前 IP、RSSI、连接状态、正在跑的协议等 |

---

## 二、目录结构与分层

```
components/
├── net_io/          收发框架：共用队列 + tx/rx 常驻任务 + 三个服务的完整实现
│   ├── net_io.c            队列、消费者任务、上行入口、门控
│   ├── net_io_tcp_client.c TCP 长连接（重连、心跳、配置热重载）
│   ├── net_io_tcp_server.c TCP 服务端（accept 派发、槽位、僵尸连接回收）
│   └── net_io_udp.c        UDP 收发
├── app_wifi/        WiFi 选路、webconfig 端口回调、事件转发、门控判定
├── webconfig/       配网热点 + HTTP 服务器 + 配置页 + NVS 读写
├── service_manager/ 服务模式切换（NVS 记住上次选择，切换时安全停旧起新）
├── wifi/            Espressif WiFi 薄封装（模式/静态 IP/事件/重连任务）
├── tcp_client/      BSD socket 封装的 TCP 客户端（硬超时 connect、keepalive）
├── tcp_server/      BSD socket 封装的 TCP 服务端（select 轮询 accept）
├── udp/             BSD socket 封装的 UDP（open/sendto/recvfrom/close）
├── uart/  gpio/     外设薄封装
├── factory_reset/   按键长按检测 + 回调
└── cjson/           第三方 JSON 库（webconfig 使用）
main/
├── main.c           硬件初始化、状态灯、**业务处理体 app_on_frame()**
└── Kconfig.projbuild 服务模式 / 收发参数 / WiFi / 恢复出厂 / 版本信息
docs/WORK_NOTES.md   开发日记（决策记录、踩坑、验证结论）
```

数据流：

```
业务发送  net_io_send_*() ──► tx 队列 ──► tx_consumer ──► socket send/sendto
业务接收  socket recv ──► 各服务 recv 任务 ──► rx 队列 ──► rx_consumer ──► app_on_frame()
```

**日常开发只改 `main/main.c` 里的 `app_on_frame()`**，其余都是基础设施。

---

## 三、快速开始

### 1. 环境

```bash
. $IDF_PATH/export.sh          # Windows: export.ps1
idf.py set-target esp32        # 若是别的芯片，改成对应型号
```

### 2. 编译与烧写

```bash
idf.py build
idf.py -p COM3 flash monitor   # Linux/macOS: /dev/ttyUSB0
```

### 3. 首次上电会发生什么

1. 没有 NVS 配置、menuconfig 里 STA SSID 为空 → **开配网热点**；
2. 手机 / 电脑连热点（默认 SSID `roboot_mc`，密码为空即开放热点）；
3. 浏览器打开 `http://192.168.4.1/`，填「网络」和「通讯」两页，点保存；
4. 设备按新配置重启无线：STA 模式去连路由器（串口会打印管理页 IP），
   AP 模式则等客户端接入后自动开工；
5. 再访问 `http://<设备IP>/` 可以继续改配置，不用重新连热点。

### 4. 网页接口一览

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/` | 配置页 |
| GET | `/api/config` | 读当前生效配置（含 menuconfig 兜底值） |
| GET | `/api/status` | 运行状态：IP / RSSI / 是否在线 / 当前协议 |
| POST | `/save` | 保存配置（**partial 语义**：没提交的字段保持原值） |
| POST | `/api/factory-reset` | 清空所有配置并重启 |
| GET/POST | `/api/service-mode` | 查询 / 切换当前通讯服务 |

### 5. 把本工程当模板用

1. 复制仓库，改顶层 `CMakeLists.txt` 的 `project(roboot_mc)`；
2. `idf.py menuconfig` → `Application identity` 改设备名与版本号；
3. 不需要的通讯方式可直接关掉（`Service Mode` 里取消勾选），相关代码会被编译裁剪；
4. 业务逻辑写在 `main/main.c` 的 `app_on_frame()` 里，发送用 `net_io_send_*()`。

---

## 四、对外接口

### 4.1 收发层 `net_io`（业务主要用这个）

头文件：`#include "net_io.h"`

```c
/* 启动：建队列 + 两个常驻任务。必须在 app_wifi_start() 之前调用 */
esp_err_t net_io_start(void);

/* 注册下行回调：收到数据在 rx_consumer 任务里回调 */
void net_io_set_recv_cb(net_io_recv_cb_t cb);
typedef void (*net_io_recv_cb_t)(const net_io_frame_t* frame);

/* 三个上行入口 + 原路回发：非阻塞，返回 true 只代表"已入队" */
bool net_io_send_tcp_client(const void* data, size_t len);
bool net_io_send_tcp_server(int client_fd, const void* data, size_t len);
bool net_io_send_udp(uint32_t dst_ip, uint16_t dst_port, const void* data, size_t len);
bool net_io_send_back(const net_io_frame_t* frame, const void* data, size_t len);

/* 心跳回包判定：echo 类服务器会把心跳原样发回，业务必须先过滤，否则无限乒乓 */
bool net_io_heartbeat_is_echo(const void* data, size_t len);

/* 配置热重载（目标地址 / 心跳）：网页保存后 app_wifi 内部已自动调，一般不用手动调 */
void net_io_reload_config(void);

/* 门控总闸：false 时各服务挂起等待。一般由 app_wifi 自动管理 */
void net_io_set_net_ready(bool ready);
bool net_io_is_net_ready(void);
```

收到的帧：

```c
typedef struct {
    net_io_link_t link;   // NET_IO_LINK_TCP_CLIENT / _TCP_SERVER / _UDP
    int peer_fd;          // TCP_SERVER：对端 fd，回发时原样传回
    uint32_t peer_ip;     // UDP：对端 IP（网络字节序）
    uint16_t peer_port;   // UDP：对端端口（主机字节序）
    const uint8_t* data;  // 数据；**回调返回后失效**，要留存请自行 memcpy
    uint16_t len;
} net_io_frame_t;
```

业务处理体示例（`main/main.c` 内的现状）：

```c
static void app_on_frame(const net_io_frame_t* frame)
{
    if (frame == NULL || frame->len == 0) return;
    if (net_io_heartbeat_is_echo(frame->data, frame->len)) return;  // 过滤心跳回显

    ESP_LOGI(TAG, "收到 %u 字节", (unsigned)frame->len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, frame->data, frame->len, ESP_LOG_INFO);

    /* 要回数据：net_io_send_back(frame, buf, n) 或按通道调上面三个接口 */
}
```

### 4.2 WiFi / 配网 `app_wifi`

```c
/* 启动 WiFi 与配网（阻塞到 WiFi 起来 / 配网热点开好为止）。
 * 选路：NVS 配置 > menuconfig 的 STA 凭证 > AP-only > 配网热点。
 * 内部会在连上网络时给 net_io 开门，掉线时关门。可重复调用。 */
esp_err_t app_wifi_start(void);
```

### 4.3 服务模式 `service_manager`

```c
typedef enum {
    SERVICE_MODE_NONE = 0,
    SERVICE_MODE_TCP_CLIENT = 1,
    SERVICE_MODE_TCP_SERVER = 2,
    SERVICE_MODE_UDP = 3,
} service_mode_t;

esp_err_t service_manager_init(void);                        // 读 NVS 决定初始模式
void service_manager_start(void);                            // 起切换任务
esp_err_t service_manager_request_switch(service_mode_t m);  // 请求换协议（异步）
service_mode_t service_manager_get_active(void);
const char* service_mode_to_str(service_mode_t m);
esp_err_t service_manager_erase_mode(void);                  // 恢复出厂时清掉记住的模式
```

### 4.4 配置存储 `webconfig`

```c
/* HTTP 服务 */
esp_err_t webconfig_start(const webconfig_cfg_t* cfg);       // 开配网热点 + 网页
esp_err_t webconfig_httpd_start(const webconfig_cfg_t* cfg);  // 只开网页（不开热点）
esp_err_t webconfig_stop(void);
bool webconfig_is_running(void);

/* 设备配置（设备名 / 网络 / 通讯，一整块 NVS blob） */
bool webconfig_load_dev_cfg(webconfig_dev_cfg_t* out);
esp_err_t webconfig_save_dev_cfg(const webconfig_dev_cfg_t* cfg);
esp_err_t webconfig_erase_dev_cfg(void);
esp_err_t webconfig_erase_all_settings(void);   // 恢复出厂：设备配置 + STA 凭证 + 服务模式

/* 给网页/状态页喂数据 */
void webconfig_set_comm_state(bool online);     // 通讯是否在线（各服务自动调用）
void webconfig_set_app_version(const char* ver);
void webconfig_set_fw_version(const char* ver);

/* STA 连接结果通知（失败会自动重开配网热点） */
void webconfig_watch_sta_connect(void);
void webconfig_notify_sta_connected(void);
void webconfig_notify_sta_disconnected(void);
```

### 4.5 底层封装（一般不需要直接用）

```c
/* wifi：开 / 改 WiFi 参数（app_wifi 内部使用） */
esp_err_t wifi_config_init(const wifi_init_cfg_t* cfg);
esp_err_t wifi_update_config(const wifi_init_cfg_t* cfg);

/* tcp_client */
esp_err_t tcp_client_open(const tcp_client_cfg_t* cfg, int* out_fd);
esp_err_t tcp_client_send(int fd, const void* data, size_t len, size_t* written);
esp_err_t tcp_client_recv(int fd, void* buf, size_t max_len, size_t* read);
esp_err_t tcp_client_set_timeouts(int fd, int send_ms, int recv_ms);
esp_err_t tcp_client_close(int fd);

/* tcp_server */
esp_err_t tcp_server_listen(const tcp_server_cfg_t* cfg, int* listen_fd);
esp_err_t tcp_server_accept(int listen_fd, int* client_fd, int poll_ms);
esp_err_t tcp_server_send(int fd, const void* data, size_t len, size_t* written);
esp_err_t tcp_server_recv(int fd, void* buf, size_t max_len, size_t* read);
esp_err_t tcp_server_close(int fd);

/* udp（函数名带 socket_ 是为了避开 lwIP 的 udp_* 符号） */
esp_err_t udp_socket_open(const udp_cfg_t* cfg, int* sock_fd);
esp_err_t udp_socket_sendto(int fd, const void* data, size_t len,
                            uint32_t dest_ip, uint16_t dest_port, size_t* sent);
esp_err_t udp_socket_recvfrom(int fd, void* buf, size_t max_len,
                              uint32_t* src_ip, uint16_t* src_port, size_t* received);
esp_err_t udp_socket_close(int fd);

/* uart / gpio / factory_reset */
esp_err_t uart_config_init(const uart_init_cfg_t* cfg);
esp_err_t uart_send(uart_port_t port, const void* data, size_t len, size_t* written);
esp_err_t uart_recv(uart_port_t port, void* buf, size_t max_len, int timeout_ms,
                    size_t* read_bytes);

esp_err_t gpio_config_init(gpio_num_t pin, const gpio_init_cfg_t* cfg);
esp_err_t gpio_write(gpio_num_t pin, uint32_t level);
esp_err_t gpio_read(gpio_num_t pin, uint32_t* level);

/* 长按回调：erase_cb 做擦除，evt_cb 收进度事件（按下/倒计时/触发/取消） */
typedef esp_err_t (*factory_reset_erase_fn)(void* ctx);
typedef void (*factory_reset_evt_fn)(factory_reset_evt_t evt, void* ctx);
esp_err_t factory_reset_start(const factory_reset_cfg_t* cfg,
                              factory_reset_erase_fn erase_cb,
                              factory_reset_evt_fn evt_cb, void* ctx);
```

---

## 五、参数配置

### 5.1 `idf.py menuconfig`（编译期默认值）

**Service Mode** —— 决定哪些协议被编进固件、默认跑哪个

| 选项 | 默认 | 说明 |
|---|---|---|
| `SERVICE_TCP_CLIENT_ENABLE` | y | 编入 TCP 客户端 |
| `SERVICE_TCP_SERVER_ENABLE` | y | 编入 TCP 服务端 |
| `SERVICE_UDP_ENABLE` | y | 编入 UDP |
| `SERVICE_DEFAULT_MODE` | TCP Client | 首次上电默认跑哪个（之后以 NVS 记住的为准） |
| `SERVICE_SWITCH_TIMEOUT_MS` | 8000 | 切换协议时等旧服务退出的上限 |

**Service common** —— 收发框架

| 选项 | 默认 | 说明 |
|---|---|---|
| `RX_MAX_FRAME` | 1024 | 单帧上限，**超长部分会被丢弃** |
| `TX_QUEUE_LEN` | 4 | 上行队列深度（帧） |
| `RX_QUEUE_LEN` | 4 | 下行队列深度（帧） |

**WiFi Configuration**

| 选项 | 默认 | 说明 |
|---|---|---|
| `WIFI_MODE` | STA | menuconfig 只给 STA / AP 二选一；`wifi_init_cfg_t` 本身支持 APSTA，需要时自行调 `wifi_config_init()` |
| `WIFI_STA_SSID` / `_PASSWORD` | 空 | 留空 → 走配网热点 |
| `WIFI_STA_USE_DHCP` | y | 取消勾选则用下面三项静态 IP |
| `WIFI_STA_STATIC_IP` / `_NETMASK` / `_GATEWAY` / `_DNS_MAIN` / `_DNS_BACKUP` | 192.168.1.x | 静态 IP 生效时使用 |
| `WIFI_STA_FAIL_FALLBACK_COUNT` | 5 | STA 连续失败几次后自动重开配网热点 |
| `WIFI_AP_SSID` / `_PASSWORD` | roboot_mc / 空 | 本机热点；密码留空为开放热点，非空需 8~63 位 |
| `WIFI_AP_IP` / `_MAX_CONN` | 192.168.4.1 / 4 | 热点网段与最大连接数 |

**TCP Client**

| 选项 | 默认 | 说明 |
|---|---|---|
| `TCP_CLIENT_HOST_STA` / `_HOST_AP` | 192.168.1.38 / 192.168.4.1 | 服务器地址（按 WiFi 模式取值） |
| `TCP_CLIENT_PORT` | 8888 | 服务器端口 |
| `TCP_CLIENT_CONNECT_TIMEOUT_MS` | 5000 | connect 硬超时，**必须小于 `SERVICE_SWITCH_TIMEOUT_MS`** |
| `TCP_CLIENT_RECV_TIMEOUT_MS` | 1000 | 单次 recv 阻塞上限，决定关停响应速度 |
| `TCP_CLIENT_RECONNECT_MS` | 2000 | 断线重连间隔 |
| `TCP_CLIENT_KEEPALIVE_IDLE_S` / `_INTVL_S` / `_CNT` | 20 / 5 / 3 | 静默失联兜底探测 |
| `TCP_CLIENT_HEARTBEAT_MS` | 10000 | 空闲心跳间隔，0 = 关闭 |
| `TCP_CLIENT_HEARTBEAT_PAYLOAD` | PING | 心跳内容 |
| `TCP_CLIENT_HEARTBEAT_FORMAT` | text | text / hex |
| `TCP_CLIENT_SEND_SLOW_US` | 20000 | send 超过该耗时打告警（排查延迟） |
| `TCP_CLIENT_LOG_RX_DUMP` | n | 打开后把收到的数据 hexdump（**调试用，很慢**） |

**TCP Server**

| 选项 | 默认 | 说明 |
|---|---|---|
| `TCP_SERVER_PORT` | 8888 | 监听端口 |
| `TCP_SERVER_MAX_CLIENTS` | 2 | 并发连接上限（1~4） |
| `TCP_SERVER_ACCEPT_POLL_MS` | 200 | accept 轮询周期，影响停服 / IP 变化响应速度 |
| `TCP_SERVER_IDLE_TIMEOUT_MS` | 30000 | 空闲多久断开，0 = 不超时 |

**UDP**

| 选项 | 默认 | 说明 |
|---|---|---|
| `UDP_PORT` | 7777 | 绑定端口 |
| `UDP_RECV_TIMEOUT_MS` | 1000 | recvfrom 唤醒周期 |
| `UDP_SEND_TIMEOUT_MS` | 5000 | sendto 阻塞上限 |

**Factory reset / Application identity**

| 选项 | 默认 | 说明 |
|---|---|---|
| `FACTORY_RESET_ENABLE` | y | 是否启用 BOOT 键长按 |
| `FACTORY_RESET_GPIO` / `_HOLD_MS` | 0 / 5000 | 按键引脚与长按时长 |
| `DEVICE_NAME_DEFAULT` | roboot_mc | 设备名（HEX 写中文 / 纯 ASCII 写英文） |
| `FW_VERSION` / `APP_VERSION` | v1.0.0 | 底层固件版本 / 上层 App 版本 |

### 5.2 网页配置（运行期，存 NVS，优先级高于 menuconfig）

NVS 命名空间 `webcfg`，两个 key：`dev`（设备名 + 网络 + 通讯整块）、`sta`（早期版本的单独 STA 凭证，
仅用于向后兼容清理）。服务模式由 `service_manager` 存在自己的命名空间里。

配置页字段与含义：

- **网络**：`wifiMode`（ap/sta）、AP 的 SSID / 密码 / 本机 IP、STA 的 SSID / 密码 /
  IP 策略（dhcp/static）与静态 IP / 掩码 / 网关 / DNS、连接失败重试上限；
- **通讯**：`protocol`（tcpServer/tcpClient/udp）、TCP 服务端端口、TCP 客户端目标地址与端口、
  UDP 端口、TCP 客户端心跳（开关 / 间隔秒 / 格式 text|hex / 内容）。

保存后的动作：写 NVS → 返回 `{"ok":true}` → 延迟约 2 秒应用（先让 HTTP 响应送达）→
`wifi_update_config()` 切无线参数 + `service_manager_request_switch()` 切协议 +
`net_io_reload_config()` 重读目标地址与心跳。

### 5.3 代码里改

- **业务逻辑**：`main/main.c` 的 `app_on_frame()`（收）与 `net_io_send_*()`（发）。
- **默认值**：各组件 `Kconfig.projbuild`；改了 Kconfig 记得 `idf.py menuconfig` 重新确认，
  必要时删掉 `sdkconfig` 重新生成。
- **AP 信道**：`app_wifi.c` 里固定为 8，与配网热点一致，避免切模式时客户端被踢下线。

---

## 六、注意事项

1. **启动顺序不能换**：`net_io_start()` 必须在 `app_wifi_start()` 之前。`AP_START` 会在
   `esp_wifi_start()` 返回前同步触发，队列没建好那一下门控事件就白丢了。
2. **`frame->data` 只在回调期间有效**，它指向 rx 任务栈上的缓冲；要留存请 `memcpy`。
3. **回调运行在 `rx_consumer` 任务（栈 4096）**：不要长耗时、不要阻塞等待同一个任务。
   例如在回调里同步等一个由 rx 任务自己处理的应答，会直接死锁。
4. **发送返回 `true` 只代表入队成功**，不代表对端收到了。要可靠性请在协议层做 ACK / 重传。
5. **心跳与 echo 服务器**：如果对端会把收到的数据原样回传，业务必须先调
   `net_io_heartbeat_is_echo()` 过滤心跳，否则"心跳 → 回显 → 再回显"会无限乒乓。
   心跳内容要和服务器约定好（text / hex 两种格式）。
6. **单帧上限 `RX_MAX_FRAME`（默认 1024）**：TCP 超长会分多次送达；UDP 打满缓冲区时
   超长部分由内核静默丢弃，串口会告警，需要自己调大。
7. **配网热点里通讯服务不启动**：那是给手机填表用的，`net_io` 门控保持关闭；
   业务 AP 模式则要等有客户端接入才开工。这是设计行为，不是故障。
8. **TCP 服务端连不上时先看端口占用**：切模式时会关 socket 并等 handler 退出，
   若日志出现"仍未退出"，说明有阻塞调用卡住了 handler，此时不会强清槽位（防止 fd 复用踩踏）。
9. **`LWIP_MAX_SOCKETS` 默认 10**：频繁断开重连若不 `shutdown()` 会残留 `CLOSE_WAIT`，
   最终 `EMFILE`。本工程的 `*_close()` 已经先 shutdown 再 close。
10. **恢复出厂会重启**：BOOT 键路径擦完 300ms 重启，网页路径先把 `{"ok":true}` 发出去
    再 800ms 重启；两者都会清空设备配置、STA 凭证和服务模式选择。
11. **`CONFIG_TCP_CLIENT_LOG_RX_DUMP`** 打开后 hexdump 是同步阻塞写 UART，1 KB 要几百毫秒
    且发生在投队列之前，压测时务必关闭。
12. **本工程未启用 OTA**（`partitions_singleapp.csv`）。要 OTA 需改用双分区表，
    并补充 `esp_https_ota` / 自定义升级流程。

---

## 七、更多

- `docs/WORK_NOTES.md`：开发日记，记录了架构决策、踩过的坑（connect 硬超时、
  FD_SETSIZE、僵尸连接、心跳与 NAT 老化等）和历次验证结论，遇到"为什么这么写"先查那里。
- 组件之间不允许反向依赖：`main` → `app_wifi` / `net_io` / `service_manager`，
  `net_io` → `tcp_client` / `tcp_server` / `udp` / `webconfig`，底层只依赖 IDF。
