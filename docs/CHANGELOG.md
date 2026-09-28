# 开发日志（Dev Log）

> 仓库中第一个 dev log 文件。每次"阶段收尾"追加一节，标日期。用来在断了好几天后仍能一眼看清：项目已实现什么、还卡在哪、下一步要做什么。

---

## 2026-09-28：网络层共用收发队列重构 + 当日收尾

### 一、当前工程已实现的功能

#### 1. 三个网络服务（同一时刻只跑一个）

通过 `service_manager` 组件互斥管理三种模式，运行时可在网页 `/api/service-mode` 切换，启动期按 NVS 或 menuconfig 默认值挑一个：

| 模式 | 角色 | 实现位置 | 关键参数（menuconfig） |
|---|---|---|---|
| `tcp_client` | 主动连远程服务器的长连接 echo | `main/main.c` `tcp_conn_task` | `TCP_CLIENT_HOST_AP/STA`、`TCP_CLIENT_PORT`、`*_KEEPALIVE_*`、`TCP_CLIENT_HEARTBEAT_MS`、`TCP_CLIENT_SEND_SLOW_US` |
| `tcp_server` | 监听端口，接受多客户端 echo | `main/main.c` `tcp_server_task` + `tcp_server_client_task` | `TCP_SERVER_PORT`、`TCP_SERVER_MAX_CLIENTS`、`TCP_SERVER_ACCEPT_POLL_MS`、`TCP_SERVER_IDLE_TIMEOUT_MS` |
| `udp` | 单 socket 收发两用 echo | `main/main.c` `udp_task` | `UDP_PORT`、`UDP_RECV_TIMEOUT_MS`、`UDP_SEND_TIMEOUT_MS` |

#### 2. 共用收发基础设施（本次重构核心）

不再每个服务自己一份 tx/rx 队列；统一到一对常驻队列 + 一对 consumer 任务：

```
g_tx_queue ◄── tx_queue_post() ◄── 上行入口
   ▲          (tcp_app_send / srv_app_send / udp_app_send，任一任务可调)
   │
tx_consumer_task ── 看 item.kind 决定发到：
                     TCP_CLIENT → send(g_fd)
                     TCP_SERVER → send(item.fd)
                     UDP        → sendto(g_udp_fd, item.dst_ip:dst_port)

g_rx_queue ◄── rx_queue_post() ◄── 各 service 的 recv 路径
   ▲          (tcp_conn_task / tcp_server_client_task / udp_task)
   │
rx_consumer_task ── 调 tcp_app_on_recv(item)
                              │
                              ▼
                     tcp_app_handle_frame()  ← 业务处理体，当前是 echo；真协议解析替换这里
```

**为什么这能成立**：`service_manager` 保证同一时刻只有一个 service 在跑，全局资源不会冲突。

**代码位置**：`main/main.c`
- 队列元素类型：`tx_item_t` / `rx_item_t`（含 `kind` / `len` / `fd` / `dst_ip` / `dst_port` / `data[]`）
- 三个上行入口：`tcp_app_send()` / `srv_app_send()` / `udp_app_send()`
- 统一业务入口：`tcp_app_on_recv()` → `tcp_app_handle_frame()`
- 常驻任务：`tx_consumer_task` / `rx_consumer_task`（在 `app_main` 中创建，service 切换不销毁）

**菜单（`main/Kconfig.projbuild` → Service common）**：
- `CONFIG_RX_MAX_FRAME`：单帧最大字节（默认 1024，64~4096）
- `CONFIG_TX_QUEUE_LEN`：上行队列深度（默认 4，1~32）
- `CONFIG_RX_QUEUE_LEN`：下行队列深度（默认 4，1~32）

**BSS 收益**：TCPclient 旧的 `s_rx_buf`、TCPserver 槽位 `buf[]`、UDP 旧的私有 RX 缓冲全部删除，recv buffer 改为栈上的 `rx_item_t.data`（handler 私有、退出即释放）。

#### 3. 协议相关能力

- **TCP client 长连接保活**：应用层心跳（`TCP_CLIENT_HEARTBEAT_MS`，默认 10s，payload = `"PING"`）+ TCP keepalive 探针（`idle 20s / interval 5s / cnt 3`）双保险；心跳回包会跳过 echo 防乒乓。
- **TCP server 僵尸连接兜底**：TCP keepalive（组件内部硬编码）+ 应用层空闲断开（`TCP_SERVER_IDLE_TIMEOUT_MS`，默认 30s）。
- **网络掉线 / IP 变化恢复**：所有 service 跟着 `s_net_ready` 门控进出；server/UDP 还会在 IP 变化时自动重建。
- **延迟观测**：TX/RX 字节数日志、`tcp_client_send` 耗时探针（> `TCP_CLIENT_SEND_SLOW_US` 报警，默认 20ms）。

#### 4. WiFi 与配网

- **配网组件** `components/webconfig`：开热点 + HTTP 表单提交 SSID/密码；`keep_httpd=true`，配网结束后 HTTP 服务器继续跑。
- **WiFi 组件** `components/wifi`：STA / AP 模式切换，掉线自动重连。
- **关键修复**：关 `WIFI_PS_NONE`（modem sleep 关掉，延迟从 DTIM 抖动降到稳定低延迟）；在 STA_CONNECTED 上幂等再下发一次，治"配网路径下 STA 后建出来没继承 PS 设置"。

#### 5. service_manager 与 HTTP 控制

- `components/service_manager`：100ms 一轮检查 pending；停旧 → 等 `is_running()==false` → 起新；停服超时禁止并发起新（避免两个 service 并发）。
- `GET /api/service-mode`：返回 `{active, active_name, allowed[]}`。
- `POST /api/service-mode`：支持数字 `mode=2` / 字符串 `mode=tcp_server` / JSON `{"mode":2}` 三种形态；目标模式 menuconfig 禁用时 403；切换请求先落 NVS（重启仍记住）。

#### 7. 硬件层

- LED 状态指示灯（`running_light_task`），GPIO21，2Hz 闪烁，越简单越可靠。

---

### 二、需要用户确认 / 待定事项

> 这些条目需要用户确认后再做或改方案。

1. **业务处理体是 echo**：`tcp_app_handle_frame()` 当前是"原样回传"。真协议解析需要替换这里。**确认问题**：协议格式是什么？（定长？分隔符？TLV？JSON？）何时提供协议文档？
2. **目标地址写死**：TCPclient 的服务器地址仍是编译期 `CONFIG_TCP_CLIENT_HOST_STA=192.168.1.26` / `CONFIG_TCP_CLIENT_HOST_AP=192.168.4.1`。**确认问题**：什么时候切到 NVS / 网页配置可改地址？需要加哪些 UI 字段？
3. **`tx_item_t.data[]` 字段名与 Kconfig 项名不一致**：`CONFIG_RX_MAX_FRAME` 同时被 TX 和 RX 引用（共用单帧上限），但配置项名字只叫"RX"。**确认问题**：是否改名成 `CONFIG_NET_FRAME_MAX` 之类（语义中性），还是保留"RX"名 + 在 help 里注明 TX 也用？
4. **`tcp_server` 的 keepalive 是组件内部硬编码**（idle 10s / interval 3s / cnt 3），没搬到 `TCP_SERVER_*` 菜单。**确认问题**：是否需要把它暴露成 `CONFIG_TCP_SERVER_KEEPALIVE_*` 三项？（要改 `tcp_server_accept()` + `tcp_server_cfg_t`，是另一波改动）
5. **service_manager 的额外开销**：tx/rx_consumer_task 常驻、不随 service 切换销毁，意味着服务全关时也有两个空转任务占栈（各 4096 字节）。**确认问题**：当前 RAM 余量够用吗？要不要在所有 service disable 时也停 consumer、起服务时再起？（会牺牲一点延迟均匀性，换一点 RAM）
6. **RX 队列默认值 4**：默认 4 在中等数据率下够用；高频场景需要更大。**确认问题**：当前应用场景预期峰值（帧/s、平均帧长）？

---

### 三、后续工作方向（待用户确认优先级）

> 路线图，未拍板，按优先级降序排列。

#### P0：业务协议接入

- 替换 `tcp_app_handle_frame()`：按用户提供的协议解析（鉴权、命令、ack）。
- 心跳帧的格式可能也要变（目前 `"PING"` 4 字节硬编码）。

#### P1：网页配置接入

- 把 `CONFIG_TCP_CLIENT_HOST_STA/AP/PORT` 从 menuconfig 搬到 webconfig 表单，存 NVS；`tcp_app_pick_target()` 改成"先查 NVS、查不到回落 menuconfig 默认"。
- 管理页 HTML 端动态渲染"当前激活模式 / 可选列表"，复用 `/api/service-mode` 已有的接口。

#### P2：可观测性

- 加一个 `/api/stats` 接口：心跳次数、断线累计时长、当前 RX/TX 队列深度。
- 把当前散落的 `ESP_LOGI` 升级成可调级别的日志开关（区分"调试"和"用户应该看到"两类）。

#### P3：通用化 / 可复用

- 把 `tx_queue_post` / `rx_queue_post` / `tcp_app_send*` 抽到一个 `net_queue` 组件里，让其他项目（比如未来要做 BLE 网关 / 多总线桥）也能复用"一对常驻队列 + 业务分发"这套模式。

#### P4：硬件扩展（视产品形态决定）

- 外接串口 / SPI / I2C 设备后，对应写一个 "app_send" 入口 → 把数据走 `tx_queue_post` 投到上行队列；下行的命令通过 `tcp_app_on_recv` 路径回写到外设。
- GPIO 控制通道（开关量）：同理。

---

### 四、本次提交概要（2026-09-28 已推送到 origin/master）

两个 commit：

1. `3acc874` — **feat(service): 三个网络服务共用一对收发队列 + 业务入口统一**
   ```
   .vscode/settings.json                        |   2 +-   # clangd 路径迁移
   components/service_manager/service_manager.c |   6 +-   # ops_table 用 #if 包各 service 行
   components/service_manager/service_manager.h |  14 +-   # 修过时注释（tcp_tx_task 等）
   components/tcp_client/Kconfig.projbuild      |  33 +-   # 删 RX_QUEUE_LEN/MAX_FRAME，指向 Service common
   components/tcp_server/Kconfig.projbuild      |  12 +-   # 删 ECHO_BUF
   components/udp/Kconfig.projbuild             |  13 +-   # 删 RX_BUF_SIZE
   main/Kconfig.projbuild                       |  39 ++   # 新增 Service common 菜单（RX_MAX_FRAME / TX_QUEUE_LEN / RX_QUEUE_LEN）
   main/main.c                                  | 958 ++   # 核心重构：共用收发队列 + consumer 任务 + 三个上行入口 + 统一业务入口
   sdkconfig                                    |  27 +-   # 对应 Kconfig 改动 + TCP_CLIENT_HOST_STA=192.168.1.26 + 心跳默认开启
   ```
2. `f81dadd` — **docs: 新增 docs/CHANGELOG.md（开发日志起点）**

### 五、明日待办（2026-09-29）

> 今日提交后第二天要做的事。具体内容明日开工时确认。

1. **今日修改的代码部分需要确认和测试**
   - 范围：本次重构涉及的全部改动（见上方 commit `3acc874` 的 9 个文件）
   - 重点关注：
     - `tcp_app_handle_frame()` echo 行为是否与旧版完全一致（粘包 / 心跳回包 / 三种 kind 的回送路径）
     - 共用队列 + consumer 任务在三种 service 之间的切换是否有资源残留（service 切换时已 `xQueueReset`，但要观察实际行为）
     - service_manager 在新队列模式下，stop 超时、并发防御是否仍然成立
   - 待定：是否上板实测（依赖硬件是否在身边）

2. **menuconfig 还需要增加新的配置内容（具体内容还待确认）**
   - 候选方向（明日开会确认优先做哪个）：
     - 把 TCPserver keepalive 三项（idle 10s / interval 3s / cnt 3，目前组件内硬编码）暴露成 `CONFIG_TCP_SERVER_KEEPALIVE_*`（要碰 `tcp_server_accept()` + `tcp_server_cfg_t`）
     - `CONFIG_RX_MAX_FRAME` 改名成 `CONFIG_NET_FRAME_MAX` 之类，去掉"RX"语义偏向（TX 也用同一项）
     - 加"日志级别"开关：把"调试类"和"用户应该看到类"日志分开，避免排查时关掉所有日志
   - 待定：明日明确"加哪几项、加到什么范围、是否影响 ABI/默认值"