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

## 2026-09-29：队列清理死代码修复 + 格式 / 注释收尾

### 一、今日修改 + 新增功能

#### 1. 修复 tcp_server / udp 退出时共用队列未清（真 Bug：死代码）—— `dcb11c1`

**问题**：`tcp_server_task` / `udp_task` 结尾用 `vTaskDelete(NULL)` 自杀，而"清空
`g_tx_queue` / `g_rx_queue`"的代码被写在各自的 `service_fn` 里、`vTaskDelete` **之后**
—— 永远执行不到，是死代码。

**后果**（服务切换 `tcp_server` / `udp` → 其它时）：
- `g_tx_queue` 里残留的 `TX_KIND_TCP_SERVER` item 带着"已经 close 掉的旧 fd"，
  `tx_consumer_task` 拿去 `send` 会刷一批失败日志；
- 残留 item 会被 `rx_consumer_task` 当成"上一个服务的回声"处理；
- UDP 残留影响较轻（`g_udp_fd` 已置 -1，处理时走"UDP socket 未就绪"分支被安全丢弃），
  但会多跑一轮无效分发。

**修复**：把 `xQueueReset(g_tx_queue)` / `xQueueReset(g_rx_queue)` 下沉到
`tcp_server_task` / `udp_task` **内部、`vTaskDelete` 之前**，与 `tcp_client` 的清理点
对齐；`service_fn` 里那段替换为注释，说明已不再执行。
`main/main.c` +20/-7。

#### 2. 统一 clang-format 格式 + 修正 3 处注释 —— `811f61e`

**格式**（纯空白 / 拆行 / 对齐，无语义改动，2 文件 +171/-126）：
- `main/main.c`：enum / struct 字段对齐、单行 `ESP_LOG` 拆多行、单行 `if`
  （`xQueueReset`）展开成大括号形式；
- `components/service_manager/service_manager.c`：`#define` 对齐、ops 表
  designated initializer 换行、长 `ESP_LOG` 拆行、补文件末尾换行 —— 这批改动
  之前一直挂在 `git stash` 里，今天一并落地（**stash 现已清空**）。

**注释修正 3 处**（都在 `main/main.c`）：
- `313` 行与文件末尾重名的 section header `service_manager 接口实现`（那段实际只放
  `static` 停服 / 存活标志，没实现任何接口）→ 改名 `service 运行期状态变量`；
- 去掉过时的 `阶段 2 的核心` 说法 → 改为中性的共用收发基础设施描述；
- `tcp_conn_task` 步骤编号出现两个 `4)`（建立长连接 / 发布 fd）→ 顺延为 `4) 5) 6) 7)`。

**编译验证**：ninja 完整构建 **0 warning 0 error**，`roboot_mc.bin` = `0xdb590`
（app 分区余 14%）。

#### 3. 文档：一条 docs commit 主动撤回

`c46deb9`（docs：记录上面那条队列清理修复）随后被 `3478ea9` revert —— 不要这次日志记录。
撤回的只是文档，**代码修复 `dcb11c1` 不受影响**、仍在仓库里。

---

### 二、今日未完成的工作（待办）

1. **上板实测**（2026-09-28 遗留 + 今日新增）
   - 验证队列清理修复 `dcb11c1`：`tcp_server` / `udp` 切走再切回时，不再出现旧 fd 的
     `send` 失败日志、不再有"上一个服务的回声"。
   - 2026-09-28 那条"把重构在板子上跑一遍"仍未做。

---

### 三、当前代码还需要确认的事情

> 设计 / 需求层面卡住、推进前需要决策的点。

**A. 今日代码复查新发现（待你裁决）**

1. `service_manager_init()` 的 `@return` 注释与实现不符：头文件（`service_manager.h:94`）
   写"ESP_OK 或 NVS 初始化错误码"，实现恒 `return ESP_OK`。→ 改注释还是改实现？
2. `service_manager_request_switch()` 在目标模式被 menuconfig 禁用时，实现
   `return ESP_ERR_INVALID_STATE`，但头文件（`service_manager.h:63`）写的是"会打 warn
   日志并忽略"。→ 以哪边为准？
3. `service_manager.c:534` 遗留注释掉的调试代码
   `/* s_pending_mode = SERVICE_MODE_NONE; */ /* 暂留 pending 以便排查 */`。→ 留着还是删？
4. `main/main.c:128` 一行孤立注释 `/* 上行队列的元素：一帧待发送数据。 */`，与紧随其后的
   多行块注释（`队列元素：tx_item_t / rx_item_t`）重复。→ 删？
5. `components/tcp_client/tcp_client.h`、`components/tcp_server/tcp_server.h`：
   `extern "C" { ... }` 块内整体比常规多缩进 4 格。→ 刻意风格，还是 clang-format 没覆盖到
   头文件？要保持还是拉平？
6. `components/tcp_server/tcp_server.h` 的 `bind_addr` 字段注释用段落式，而同一结构体其余
   字段是 `/*!< */` 单行风格。→ 统一？

**B. 2026-09-28「三」里的 7 条全部仍未关闭**

业务处理体仍是 echo / 目标地址写死 / `CONFIG_RX_MAX_FRAME` 命名偏向 RX / TCPserver
keepalive 组件内硬编码 / consumer task 常驻 RAM 开销 / RX 队列默认深度 4 / main 启动逻辑
通读（其中最后一条就是 2026-09-28 的「二.2」）。

---

### 四、后期的代码方向（中期路线）

1. **完善 web 和 menuconfig 中的内容**（2026-09-28「四.1」，未动）
   - web：管理页展示当前激活模式 + 切换按钮（复用已有 `/api/service-mode`）；可能再加
     stats / 配置表单。
   - menuconfig：补齐待定项（TCPserver keepalive 暴露 / Kconfig 命名优化 / 日志级别开关等）。

2. **把整个工程封装成底层，只留数据接口**（2026-09-28「四.2」，未动）
   - 上层不再碰 `tcp_client_send` / `tcp_server_send` 等底层 socket API；对外只留
     `tcp_app_send*` 三个上行 + `tcp_app_on_recv` 一个下行 + `tcp_app_handle_frame`
     一个业务处理体。

---

### 五、本次是否已经同步到仓库

**已同步**。`master` 与 `origin/master` 一致（Gitee：`gitee.com/zhiaiyouxi/main-control-board-esp32`），
工作区干净，`git stash` 已清空。

今日 4 个 commit：

| Commit | 时间 | 说明 | 主要文件 |
|---|---|---|---|
| `dcb11c1` | 15:12 | **fix(service): tcp_server / udp 退出时清空共用队列** | `main/main.c` +20/-7 |
| `c46deb9` | 15:13 | docs(work_notes): 记录该修复 —— **随后被 revert** | `docs/WORK_NOTES.md` |
| `3478ea9` | 15:27 | **Revert** 上面那条 docs commit | `docs/WORK_NOTES.md` |
| `811f61e` | 15:49 | **chore: 统一 clang-format 格式 + 修正 3 处注释** | `main/main.c` + `components/service_manager/service_manager.c`（2 文件 +171/-126） |

> 说明：`c46deb9` + `3478ea9` 是一对"加日志 → 撤日志"，对文件净效果为零。
> 本次实际留下来的内容 = **1 条代码修复 + 1 条格式 / 注释收尾**。