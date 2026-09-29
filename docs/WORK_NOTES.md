# 开发日志（Dev Log）

> 仓库中第一个 dev log 文件。每次"阶段收尾"追加一节，标日期。
> 用来在断了好几天后仍能一眼看清：项目已实现什么、还卡在哪、下一步要做什么。

---

## 2026-09-28：网络层共用收发队列重构 + 当日收尾

### 一、今日修改 + 新增功能

#### 1. 三个网络服务共用一对收发队列（核心改动）

不再每个服务自己一份 tx/rx 队列；统一到一对常驻队列 + 一对常驻 consumer 任务。
`service_manager` 保证同一时刻只有一个 service 在跑，所以共享资源不会冲突。

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

代码位置都在 `main/main.c`：
- 队列元素类型：`tx_item_t` / `rx_item_t`（含 `kind` / `len` / `fd` / `dst_ip` / `dst_port` / `data[]`）
- 三个上行入口：`tcp_app_send()` / `srv_app_send()` / `udp_app_send()`
- 统一业务入口：`tcp_app_on_recv()` → `tcp_app_handle_frame()`
- 常驻任务：`tx_consumer_task` / `rx_consumer_task`（在 `app_main` 中创建，service 切换不销毁）

#### 2. BSS 减小

老的 per-service 接收缓冲全部删除：
- `s_rx_buf`（TCPclient 文件级 BSS）—— 删除
- `s_server_clients[i].buf`（TCPserver 每个槽位 BSS）—— 删除
- UDP 私有 RX 缓冲 —— 删除

新做法：recv buffer 用栈上的 `rx_item_t.data[]`，handler/task 私有、退出即释放。

#### 3. menuconfig 调整

**新增 `Service common` 菜单**（`main/Kconfig.projbuild`）：
- `CONFIG_RX_MAX_FRAME`：单帧最大字节（默认 1024，64~4096）—— TX/RX 共用上限
- `CONFIG_TX_QUEUE_LEN`：上行队列深度（默认 4，1~32）
- `CONFIG_RX_QUEUE_LEN`：下行队列深度（默认 4，1~32）

**删除各 service 的私有 Kconfig 项**（已统一到 Service common）：
- TCPclient：`TCP_CLIENT_TX_QUEUE_LEN`、`TCP_CLIENT_MAX_FRAME`、`TCP_CLIENT_RX_QUEUE_LEN`、`TCP_CLIENT_RX_MAX_FRAME`
- TCPserver：`TCP_SERVER_ECHO_BUF`
- UDP：`UDP_RX_BUF_SIZE`

#### 4. service_manager 强化

- `components/service_manager/service_manager.c`：`s_ops_table` 用 `#if CONFIG_SERVICE_*_ENABLE` 包住各 service 行，避免 menuconfig 禁用某 service 时引用未定义符号。
- `components/service_manager/service_manager.h`：重写"和 tcp_app_send / tcp_app_on_recv 的关系"那段（旧的"g_tx_queue/tcp_tx_task 只在 TCP_CLIENT 期间才存在"已过时）。

#### 5. 注释清理

- `main/main.c`：清理 8 处过时引用（`tcp_tx_task` → `tx_consumer_task`、`CONFIG_TCP_CLIENT_MAX_FRAME` → `CONFIG_RX_MAX_FRAME`、删除已废除的 `CONFIG_TCP_CLIENT_RX_QUEUE_LEN=0 直通模式` 描述）；合并文件头两段重复的"三个服务共用队列"说明。
- `components/service_manager/service_manager.h`：过时注释整段重写。

#### 6. sdkconfig 默认值

- `CONFIG_TCP_CLIENT_HOST_STA`：`192.168.1.38` → `192.168.1.26`（用户家里服务器 IP 变了）
- `CONFIG_TCP_CLIENT_HEARTBEAT_MS`：默认 10000（应用层心跳开启，回包会跳过 echo 防乒乓）

#### 7. .vscode 配置

- `clangd.path` 迁移到 `C:\Espressif\tools\esp-clangd\...`（本地环境路径调整）。

#### 8. 新增本文件 `docs/CHANGELOG.md`

开发日志起点，5 块结构：今日改动 / 今日未完 / 待确认 / 后期方向 / 同步状态。

---

### 二、今日未完成的工作（待办）

> 列出今天没做完、明天或后续要继续做的事。

1. **上板实测本次重构**
   - 把今天的代码改动在板子上跑一遍，看 echo 行为、队列残留、service 切换是否符合预期。

2. **检查当前代码的 `main()` 函数逻辑是否合理**
   - 通读 `app_main()` → `service_manager_init/start` → 共用资源初始化 → 串行流水灯任务 的整体启动流程是否清晰、有无冗余 / 漏掉的关键路径。

---

### 三、当前代码还需要确认的事情

> 设计 / 需求层面卡住、推进前需要决策的点。

1. **业务处理体仍是 echo**：`tcp_app_handle_frame()` 当前是"原样回传"。真协议解析需要替换这里。**待确认**：协议格式（定长 / 分隔符 / TLV / JSON？）什么时候给协议文档？
2. **目标地址写死 vs 网页配置**：TCPclient 服务器地址仍是 menuconfig 编译期常量。**待确认**：什么时候切到 NVS / 网页配置可改地址？需要哪些 UI 字段？
3. **`CONFIG_RX_MAX_FRAME` 命名偏向 RX**：这个值 TX 也用（共用单帧上限），但项名带 "RX"。**待确认**：是否改成中性名 `CONFIG_NET_FRAME_MAX`？
4. **TCPserver keepalive 组件内硬编码**（idle 10s / interval 3s / cnt 3）。**待确认**：是否搬到 `CONFIG_TCP_SERVER_KEEPALIVE_*`？要碰 `tcp_server_accept()` + `tcp_server_cfg_t`。
5. **consumer task 常驻的 RAM 开销**：两个 consumer 任务（各 4096 栈）service 全关时也占栈。**待确认**：当前 RAM 余量够用吗？要不要在所有 service disable 时也停 consumer？
6. **RX 队列默认深度 4**：中等数据率够用；高频场景需要更大。**待确认**：当前应用场景的预期峰值（帧/s、平均帧长）？
7. **main 函数整体启动逻辑**（接"二.2"）：上面的整体启动流程通读之后，可能还会冒出具体的优化点（启动顺序、`hardware_init` 里"先建事件循环再启动 WiFi"是不是仍然合理等）。

---

### 四、后期的代码方向（中期路线）

> 后期要做什么，按用户当前给的优先级排列。

1. **完善 web 和 menuconfig 中的内容**
   - web：管理页前端展示当前激活模式、提供切换按钮（复用 `/api/service-mode` 已有接口）；可能还要加 stats / 配置表单。
   - menuconfig：补齐需要新加的配置项（参考"三、当前代码还需要确认的事情"里的待定条目：TCPserver keepalive 暴露 / Kconfig 项命名优化 / 日志级别开关等）。

2. **将整个工程打包成一个底层，流出数据接口就行**
   - 目标：上层业务不再触碰 `tcp_client_send`、`tcp_server_send` 等底层 socket API。
   - 做法：把 `tx_queue_post` / `rx_queue_post` / `tcp_app_send*` 三个上行入口 + `tcp_app_on_recv` 一个下行入口 + `tcp_app_handle_frame` 一个业务处理体作为**对外稳定接口**；socket、队列、consumer 任务、service_manager 全部封装进组件。
   - 跨项目复用：以后做 BLE 网关 / 多总线桥时，也走同一对队列 + 业务分发这套基础设施。

---

### 五、本次是否已经同步到仓库

**已同步**。本次共 3 个 commit，全部已推送到 `origin/master`（Gitee：`gitee.com/zhiaiyouxi/main-control-board-esp32`）：

| Commit | 说明 | 主要文件 |
|---|---|---|
| `3acc874` | **feat(service): 三个网络服务共用一对收发队列 + 业务入口统一** | 9 文件 +765/-339（main.c 958 行改动居首） |
| `f81dadd` | **docs: 新增 docs/CHANGELOG.md（开发日志起点）** | 新增 1 文件 139 行 |
| `3c1a5cb` | **docs(changelog): 重排 4/5 节 + 新增明日待办（2026-09-29）** | docs/CHANGELOG.md +36/-14 |

**本次同步涉及的范围**：
- 代码层面：`main/main.c`（共用队列重构主体）+ `main/Kconfig.projbuild`（Service common 菜单）+ `components/service_manager/*`（头文件过时注释 / 实现文件 `#if` 守卫）+ `components/{tcp_client,tcp_server,udp}/Kconfig.projbuild`（删除私有 K 项）+ `.vscode/settings.json`（clangd 路径）+ `sdkconfig`（默认值调整）
- 文档层面：`docs/CHANGELOG.md`（新增）

---

## 2026-09-29 上午：tcp_server / udp 退出时队列未清 Bug 修复

### 一、问题

`tcp_server_service_fn` 与 `udp_service_fn` 里的清队列代码放在了 `tcp_server_task(NULL)` / `udp_task(NULL)` **之后**——但这两个 worker 都以 `vTaskDelete(NULL)` 自杀，函数永不返回，所以清理代码**是死代码**。

后果（service 切换时残留 item）：
- `TX_KIND_TCP_SERVER` 残留里 `item.fd` 已是 close 掉的旧编号，tx_consumer 拿去 send 会刷一批"服务端 echo 失败"的日志；
- `RX_KIND_TCP_SERVER` 残留会再投一道到 tx 队列，仍然失败；
- UDP 残留影响较轻：`g_udp_fd` 已被前面 close + 置 -1 流程兜住，处理时落到"UDP socket 未就绪"分支被丢弃。

发现者：用户**在 review 时**指出 `tcp_conn_task` 与 client 注释那两段清队列的"位置不对称"——这就是 bug 信号。

### 二、修复

`dcb11c1`：**fix(service): tcp_server / udp 退出时清空共用队列**

- 把 `xQueueReset(g_tx_queue)` + `xQueueReset(g_rx_queue)` 下沉到 `tcp_server_task` / `udp_task` 内部、`vTaskDelete(NULL)` **之前**（与 `tcp_client_service_fn` 的清理点对齐）
- 外层 `service_fn` 里那两段死代码删除，换成注释说明"清理已下沉"

### 三、影响

- 解决了 service 切换时日志污染问题
- 顺手让三个 service 的清理语义统一：`tcp_client` 在 service_fn 内清、`tcp_server` / `udp` 在 worker 内部清（因为前者 service_fn 会正常 return，后者不会）
- 这次改动是**纯修复**，不引入新功能；不影响已有 Kconfig 与 ABI

### 四、本次是否已经同步到仓库

**已同步**：`dcb11c1 fix(service): tcp_server / udp 退出时清空共用队列` 已推送到 `origin/master`，`main/main.c +20/-7`。

### 五、待用户确认

无新增待定项。本次修复完全闭环，未引入新的设计/需求层决策。

---

## 附：工作区状态

提交并推送后，工作区**应该是干净的**，但 `git stash` 里目前还保存着一份改动（之前某次会话里留下的**IDE 自动格式化**残留）：

- 涉及文件：`components/service_manager/service_manager.c`
- 改动内容：clang-format 触发的格式重排（结构体字段对齐、`{` 换行、`ESP_LOGx` 字符串折行等），**无功能性改动**

三种处理方式（待用户决定）：
1. **直接丢掉**：`git stash drop` —— 如果确认这些格式变更不想要
2. **单独提交**：`git stash pop` 后单独一个 `chore(format):` commit，干净地留住 IDE 改动
3. **暂留**：什么都不做，下次想动这个文件时再处理

> 这是上次会话的遗留状态，**不是本次修复引入的**。建议尽快处理，避免下次改动时和这次的修复混在一起。