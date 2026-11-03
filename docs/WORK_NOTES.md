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

---

## 2026-10-08：webconfig 分模块独立保存 + 首次回显修复

### 一、今日修改 + 新增功能

#### 1. webconfig `/save` 改 partial 解析 —— 配合前端单按钮 + dirty 跟踪

前端每个 panel 头部加"● 待保存"小标，**单按钮保存**时只发本次改动过的模块字段；
后端 `/save` 改为 **partial 解析**：

- 先把 NVS 现状读到 `cfg`（`webconfig_load_dev_cfg()`）做兜底
- 再用 `webconfig_page_parse_save_partial()` 只覆盖 JSON 里出现的字段
- 关键区分：**key 缺失 → cfg 不动**（保留 NVS）；**key 是空串 → 覆盖成空串**（清空语义）；
  **key 是 null → 同空串**；**非法类型 → 跳过**
- 整段对象（`network` / `communication` / `ap` / `sta` / `tcpServer` / `tcpClient` /
  `heartbeat` / `udp`）也可以整段缺失：直接跳过整段

效果："只改了设备名点保存"不会把 NVS 里的 wifi / 通讯 / 端口抹成空。

代码：
- `components/webconfig/webconfig_page.c` 新增 `s_if_present` / `s_obj_present` /
  `u16_if_present` / `bool_if_present` 四个 helper + `webconfig_page_parse_save_partial()`
- `components/webconfig/webconfig_page.h` 暴露 `webconfig_page_parse_save_partial()`
- `components/webconfig/webconfig.c` `wc_save_post()` 改成
  "load NVS → partial parse → validate → save" 四步；错误信息里加 "如未配过请先到
  X 模块提交"提示，避免首启只点"设备"模块的保存时把半空 cfg 落盘

#### 2. 前端 dirty 跟踪 + 三个 panel 头部"待保存"提示

- 状态 `D = { device, network, comm }` 记录哪些模块被改过
- 任何 `input` / `seg__btn` / `toggle` 改动 → 调 `markDirtyFromEl(el)` → 找最近的
  `[data-panel]` → 标 `is-dirty` class + 把对应 D[k] 置 true
- 单按钮 `btnSave` 提交前看 `D`：全空时给"当前没有改动"提示直接 return；
  否则 `buildDirtyOut()` 只把 dirty 模块的字段发到 `/save`
- 提交成功后才调 `clearAllDirty()`；失败时保留 dirty 让用户重试
- CSS：`.card__dirty` 小标（默认隐藏，`is-dirty` 时淡入），使用 `--danger` 红色 + 圆点
  强调"这一块有未保存改动"

#### 3. boot 流程改成 `await apiLoad() → syncDOM() → pollStatus()`

之前是 `syncDOM(); apiLoad();` —— `syncDOM()` 用 S. 默认值（空 / placeholder）先渲染一次，
`apiLoad()` 异步拉 NVS 拿到后再 `syncDOM()` 一次，**用户会看到一帧空表才被覆盖**。

改后：
```js
(async () => {
  setModule('network');
  await apiLoad();   // 等 NVS 现状填到 S
  syncDOM();         // 用 NVS 现状渲染 input
  pollStatus();      // 右栏"设备当前配置"立即拉一次
  setInterval(pollStatus, 1000);
})();
```

`apiLoad()` 改成返回 `Promise<boolean>`，失败时 syncDOM 用 S. 默认值（=各 input
placeholder），用户至少看到"标准默认表单"而不是白屏。同时把顶层
`pollStatus(); setInterval(pollStatus, 1000);` 移除，避免和 boot 块重复调用。

#### 4. NVS 没存过时 `/api/config` 填出厂默认

`wc_config_get` 在 `webconfig_load_dev_cfg()` 失败时，不再 memset(0) 把所有字段
置空（前端 fallback 有几条路容易漏），而是直接填一份合法的出厂默认：
- `wifiMode = "ap"` / `ap_ip = "192.168.4.1"` / `sta.mode = "dhcp"`
- `protocol = "tcpServer"` / 各端口 `8080`
- `heartbeat.format = "string"` / `heartbeat.interval = 30`

效果：`/api/config` JSON **始终是 self-contained 的**，前端拿到任何字段都不需要
再 fallback 到 JS 端的硬编码默认值。

#### 5. R 那条路径的反复（一笔带过）

中间走偏了一次：把 `R.communication.protocol` / `R.network.wifiMode` 改成了
"设备当前实际跑的"（service_manager / esp_wifi_get_mode）而不是 NVS 存的。**已回退**
——右栏"设备当前配置"展示的就是"我配的"，跟表单 S. 完全一致；设备实际跑的通过
`tcpServer.running` / `tcpClient.running` / `udp.running` 这三个 bool 单独标
（"我才是当前在跑的那个"）。同时删掉 `wifiMode_saved` / `protocol_saved` / `isCurrent`
字段以及 `webconfig_dev_running_t.wifi_mode` 字段。

**今日编译**：`Project build complete`，0 warning 0 error。

---

### 二、今日未完成的工作（待办）

1. **web 回显功能未完成**（首屏 / 切 panel 时 NVS 字段未正确回显到 input）
   - 现象：首次打开页面时，NVS 存的字段（设备名、STA 模式的 ssid/password、
     静态 IP 段的 ip/mask/gateway/dns、tcpClient 模式下的 serverIp/serverPort/心跳
     内容等）**没有全部回显**到对应的 input；切 panel / 切模式时也偶发"模块里的
     字段没填进去"。
   - 已做：partial 解析 / dirty 跟踪 / boot 流程 await / NVS 没存时填默认 / R 路径
     回退到 NVS 存的 / `wifi_mode` 等死代码清理 —— **仍不能彻底解决**，需要继续排查：
     - 浏览器控制台看 `/api/config` 实际返回的 JSON，确认字段是不是真的全
     - 看 `applyIn` 走到的分支和 `syncDOM` 写 input 的顺序
     - 怀疑 `data-branch="sta.static"` 这种"嵌套分支"的显示/隐藏切换 + input
       value 写入之间有竞态（同步设 value 但分支还没 .is-on 时，浏览器不一定
       触发 layout）
   - 明日优先：本机抓现场 / 加临时 console.log 复现。

2. **上板实测本次 webconfig 改动**
   - partial 解析：配 tcpClient → 改心跳内容 → 只提交设备名 → NVS 心跳内容应保留
   - dirty 跟踪：NVS 存了 STA + 静态 IP → 改 SSID → 头标 "● 待保存" → 保存 → 只有
     network 字段落 NVS、comm 字段不动

---

### 三、当前代码还需要确认的事情

> 在今日代码复查 + 调试过程中冒出来的新问题。

1. **`R.communication.protocol` / `R.network.wifiMode` 到底应该反映什么？**
   - 当前实现：NVS 存的（用户保存的）——右栏展示"我配的"。
   - 替代方案：设备当前实际跑的（service_manager / esp_wifi_get_mode）。
   - **待确认**：右栏的"通讯协议 / 网络模式"应该展示哪个？保存未生效 / wifi fallback
     时两个值会不一样，让用户看哪一个更能避免误判？
2. **`data-branch` 嵌套分支的同步**：STA → static 这种"二级分支"的 `is-on` 切换
   跟 input value 写入之间是否有竞态？需要看浏览器实际行为。
3. **`apiSave` 成功后是否要主动 `apiLoad()` 一次再 `syncDOM()`**？当前实现是手动
   `clearAllDirty() + renderSummary()`，依赖后端把 NVS 写正确。如果保存路径有
   任何静默失败，前端显示"已保存"但 input 实际还停在提交前的值。

---

### 四、后期的代码方向（中期路线）

1. 完善 web 和 menuconfig 中的内容（同 2026-09-28「四.1」，未动）
2. 把整个工程封装成底层，只留数据接口（同 2026-09-28「四.2」，未动）

---

### 五、本次是否已经同步到仓库

**已 commit，push 待你裁决**。本次 commit `683f367`（6 文件 +3207/-376），已
`idf.py build` 验证 0 warning 0 error，**未 push**——因为本节"二.1 web 回显
未完成"，让你拍板要不要先 push。

修改 + 提交的文件（`master` `683f367`，本地未 push）：
- `components/webconfig/webconfig.c` —— `/save` partial 解析 + `wc_config_get` 默认值
- `components/webconfig/webconfig_page.c` —— `webconfig_page_parse_save_partial()` + 4 个 helper
- `components/webconfig/webconfig_page.h` —— 暴露新 API
- `components/webconfig/webconfig.h` —— 删 `wifi_mode` 字段
- `components/webconfig/web/index.html` —— dirty 跟踪 + 待保存 UI + boot 流程
- `docs/WORK_NOTES.md` —— 本节（2026-10-08 一节）

**未 commit**（work-in-progress，非本次 webconfig 改动，与 `fe6a47c` 同期遗留）：
- `components/webconfig/CMakeLists.txt` / `main/main.c` / `main/Kconfig.projbuild` /
  `components/tcp_client/Kconfig.projbuild` / `sdkconfig` —— 来自 `fe6a47c` 之后
  的 WiFi 配 menuconfig 改动；本次 commit **未**包含这些，避免把别人的
  work-in-progress 一起带走。

---

## 2026-10-09：右栏精简 + TCP 客户端 EMFILE 修复

### 一、今日修改 + 新增功能

#### 1. 修复"右栏已连上对端"和页头状态不同源 —— `components/webconfig/webconfig.c`

**根因**：
- 页头"状态 离线/在线"读 `s_comm_online`，只有应用层真正 connect 成功才报 true（准）
- 右栏"✓ 已连上对端"在 `collect_running_runtime()` 里读 `tcp_client_is_running()`——
  **任务在跑就 true**，重连退避期间任务还活着但 socket 根本没连上，is_running 判不出来

**结果**：截图里页头老老实实写"离线"，右栏写"✓ 已连上对端"，**自相矛盾**。

**修复**：`collect_running_runtime()` 里 `rt->tcp_client_connected` 改读 `s_comm_online`，
和页头 `state` 字段读**同一个变量**，两边必然一致。

#### 2. 右栏"设备当前配置"精简（多次迭代，只动 `components/webconfig/web/index.html`）

| 步骤 | 改动 | 原因 |
|---|---|---|
| 2.1 | 去掉"摘要 / JSON"标签 + `<pre id="sumJson">` + 填充 + 切换逻辑 + 死函数 `buildFullOut()` + 对应 CSS（`.sumtabs*` / `.sum-json`） | 用户要求"只显示配置信息" |
| 2.2 | 去掉右栏所有"状态"行（保留页头"状态"芯片）：<br>STA `连接状态 ✓ 已连上 / 未连上`<br>TCP 服务端 `TCP 服务端状态 ✓ 在跑 / 未启动`<br>TCP 客户端 `TCP 客户端状态 ✓ 已连上对端 / 重连中... / 未启动`<br>UDP `UDP 状态 ✓ 在跑 / 未启动` | 与 1. 同根因；用户截图后明确"那些状态也不要了"，澄清为"仅右栏，保留页头" |
| 2.3 | 工作模式文案：`热点 AP` / `接入 STA` → `AP 模式` / `STA 模式`；行名 `工作模式 (配置)` → `工作模式` | 用户口语化需求 |
| 2.4 | 密码改明文显示：<br>STA 段 `WiFi 密码`：`已设置` / `开放` → 明文 / `开放`<br>AP 段 `热点密码`：同上 | 后端本来就是明文输出（`cfg->*_password` + `*_password_running` 兜底），表单输入框也是 `type="text"`，本来就不"加密"，所谓"已设置"是脱裤子放屁 |
| 2.5 | 去掉三段里其它协议的"可用"行（共 6 行） | 用户认为多余——右栏只展开当前生效协议自身的配置项 |

**最终右栏纯配置 + 运行值（按 `wifiMode` 切 STA / AP 段）**：
- 设备名称、工作模式、WiFi SSID / 密码（明文）、IP 模式、静态 IP 段、
  IP 地址 / 子网掩码 / 网关 / DNS（运行）、信号 RSSI
- 通讯：协议、端口 / 服务器地址:端口、心跳（开启时含心跳内容）

**段首注释同步改写**：本栏只展示配置信息，通讯是否在线统一看页头"状态"芯片；
也不列其它协议的"可用"行。

#### 3. 修复 TCP 客户端 fd 耗尽 EMFILE（核心 Bug 修复）—— `components/tcp_client/tcp_client.c`

**现象**（用户实测日志）：
```
I (311192) main: 正在连接 192.168.1.41:8888
E (311192) tcp_client: socket() failed: errno=23
W (311202) main: 连接失败，2000 ms 后重试
```
"发了多次心跳没回应 → 对端关闭 → ESP break → close → 重连"**两三轮后**新 `socket()`
直接 EMFILE，再也连不上。`errno=23` = `EMFILE`（文件描述符耗尽）。

**根因**：
- ESP32 默认 `CONFIG_LWIP_MAX_SOCKETS = 10`，每次 `socket()` 消耗一个 PCB
- `tcp_client_close()` 原本只 `close(fd)`，**没 `shutdown(fd, SHUT_RDWR)`**
- 对端已 FIN / RST 过的连接，单纯 `close()` 在 LWIP 下让本端 PCB 卡在 CLOSE_WAIT
  一段时间，频繁断开重连累积撞上限 → EMFILE

**修复**（`tcp_client.c` 的 `tcp_client_close`）：
```c
shutdown(fd, SHUT_RDWR);   /* 立即让本端 PCB 从 ESTABLISHED 转入 FIN_WAIT_1 */
close(fd);                  /* 再释放描述符 */
```
shutdown 对已半关 / RST 过的 fd 是安全的，POSIX 不强制要求检查。`shutdown` 让 PCB
释放更早、更彻底。

**实测**：用户跑 10+ 分钟未复现。

**收尾编译**：`Project build complete`，0 warning 0 error。

---

### 二、今日未完成的工作（待办）

1. **将 `main/main.c` 的业务程序封装成底层，仅留出数据接口**（**你今天明确指定的明日重点**）
   - 当前：上层业务还能看到 `tcp_client_send` / `tcp_server_send` / `udp_send` 等
     底层 socket API，以及 `service_manager_request_switch` 等管理 API
   - 目标：对外只留 **3 个上行入口**（`tcp_app_send` / `srv_app_send` / `udp_app_send`）
     + **1 个下行回调**（`tcp_app_on_recv`）+ **1 个业务处理体**（`tcp_app_handle_frame`），
     socket / 队列 / consumer 任务 / service_manager 全部封装进组件
   - 这是 2026-09-28「四.2」中期路线的延续，**明日开工**

2. **确认之前留下的问题**（**你今天明确要求**）
   - 2026-10-08「二.1」**web 回显未完成**（NVS 字段没全部回显到 input）—— 仍未关闭
   - 2026-09-29「三.A」6 条代码复查遗留（`service_manager_init` `@return` 注释不符 /
     `request_switch` 行为不一致 / 534 行遗留调试注释 / 128 行孤立注释 /
     `extern "C"` 块缩进 4 格 / `bind_addr` 字段注释风格）
   - 2026-09-28「三」7 条老待定（业务体仍是 echo / 目标地址写死 / `CONFIG_RX_MAX_FRAME`
     命名 / TCPserver keepalive 组件内硬编码 / consumer task 常驻 RAM /
     RX 队列默认深度 4 / main 启动逻辑通读）

3. **上板实测今日 webconfig 改动**（2026-10-08「二.2」遗留）
   - partial 解析：改心跳内容后只提交设备名 → NVS 心跳内容应保留
   - dirty 跟踪：NVS 存了 STA + 静态 IP → 改 SSID → 头标"● 待保存" → 只发 network 字段

4. **上板实测今日 TCP 客户端 EMFILE 修复** —— **已确认**：用户实测 10+ 分钟没问题

---

### 三、当前代码还需要确认的事情

今日改动是确定性的 bug fix + UI 精简，**无新增待定**。老的待定清单见 2026-10-08
「三」+ 2026-09-29「三」+ 2026-09-28「三」，明日封装时一并过一遍。

---

### 四、后期的代码方向（中期路线）

1. 完善 web 和 menuconfig 中的内容（2026-09-28「四.1」，未动）
2. **把整个工程封装成底层，只留数据接口**（2026-09-28「四.2」，未动）——
   **明日启动**，范围见「二.1」

---

### 五、本次是否已经同步到仓库

**未同步**。本节完成时的工作区状态：

```
M components/service_manager/service_manager.c   ← fe6a47c 之后遗留（别人的 WIP）
M components/tcp_client/Kconfig.projbuild         ← fe6a47c 之后遗留
M components/webconfig/CMakeLists.txt              ← fe6a47c 之后遗留
M main/Kconfig.projbuild                          ← fe6a47c 之后遗留
M main/main.c                                     ← fe6a47c 之后遗留
M sdkconfig                                       ← fe6a47c 之后遗留
M components/webconfig/webconfig.h                ← fe6a47c 之后遗留
M components/webconfig/webconfig_page.c           ← fe6a47c 之后遗留
M components/tcp_client/tcp_client.c              ← 今日 1.3 EMFILE 修复
M components/webconfig/webconfig.c                ← 今日 1.1 tcp_client_connected 同源
M components/webconfig/web/index.html             ← 今日 1.2 右栏精简
```

`master` 比 `origin/master` 多 3 个 commit（**未 push**）：
| Commit | 说明 |
|---|---|
| `683f367` | feat(webconfig): /save partial 解析 + 分模块独立保存 + boot 流程首次回显修复 |
| `b5e1e3b` | docs(work_notes): 2026-10-08 五节改为"已 commit" |
| `e1c75b2` | feat(webconfig): 右栏"设备当前配置"改为全量显示（首次打开即全部配置） |

**待你裁决**：
- 今日 2 个新 commit 如何切分（建议：① 右栏精简 + 状态同源 1 commit；② EMFILE 修复 1 commit）
- `fe6a47c` 之后遗留的 8 个文件 WiFi menuconfig 改动：单独 1 commit 带上，还是继续
  留工作区等你回看？
- push 计划：先 push 老的 3 commit + 今日 2 commit（共 5 commit），遗留 WiFi 改动
  单独再 push 一次？

---

## 2026-11-03：仓库裁决收尾 + ESP-IDF 6.0.2 环境对齐

### 一、今日修改 + 新增功能

#### 1. 2026-10-09「五」的三条裁决，已在此前落地

本节开工前 `master` 与 `origin/master` 已一致（`654be16`），逐条核对结果：

| 2026-10-09 待裁决项 | 结论 | 落点 |
|---|---|---|
| 今日 2 个新 commit 如何切分 | 已按建议切分 | `00e68fe`（右栏精简 + 状态同源）、`f514c84`（EMFILE 修复） |
| `fe6a47c` 之后 8 个文件 WiFi menuconfig 改动 | 单独 1 commit 带上 | `bba97b5`（含 `23b5c93`、`7b30922`） |
| push 计划 | 老 3 commit + 新 commit 均已推送 | `origin/master` = `654be16` |

即 2026-10-09 遗留的裁决问题**全部闭环**，本节只处理工作区剩下的两个环境残留文件。

#### 2. `.vscode/settings.json` 对齐本机 ESP-IDF 6.0.2

本机 `C:\esp` 下只剩 `v6.0.2`，`v6.1` 已移除，因此：

- `idf.currentSetup`：`C:\esp\v6.1\esp-idf` → `C:\esp\v6.0.2\esp-idf`
- `git.repositoryScanIgnoredFolders`：同步改为 `C:\esp\v6.0.2\esp-idf`
- `clangd.arguments` 的 `--compile-commands-dir`：旧工程路径
  `d:\project\esp32_part\roboot_mc\build` → 当前工作区 `build`（该目录已确认存在，内含
  `compile_commands.json`）
- 补回文件末尾缺失的换行

#### 3. `sdkconfig` 由 ESP-IDF 6.0.2 重新生成

差异为 **22 增 / 280 删**，全部来自 IDF 版本自动生成项（`SOC_RTC_TIMER_*`、
`ESP_WIFI_REMOTE_*` / `WIFI_RMT_*`、`CONFIG_IDF_FIRMWARE_CHIP_ID` 相关段等）。

**已逐项核对**：项目自定义配置项零变化 —— `TCP_CLIENT_*` / `TCP_SERVER_*` / `UDP_*` /
`SERVICE_*` / `WEB_CONFIG_*` / `RX_MAX_FRAME` / `RX_QUEUE_LEN` / `TX_QUEUE_LEN` /
`HEARTBEAT_*` 等均无增删改。

判定：属"IDF 版本对齐"而非配置变更，予以提交，避免之后每次 build 都被重新生成、
长期挂在工作区。

#### 4. 本次 commit

| Commit | 说明 | 文件 |
|---|---|---|
| `55c5007` | chore(vscode): 环境对齐 ESP-IDF 6.0.2 + 修正 clangd 编译数据库路径 | `.vscode/settings.json`（+3/-3） |
| `ebf6f54` | chore(sdkconfig): 由 ESP-IDF 6.0.2 重新生成（项目配置项无变化） | `sdkconfig`（+22/-280） |

> 备注：首次提交时 `sdkconfig` 已被 `git add`，误与 `.vscode` 混进同一个 commit；
> 已 `git reset --soft HEAD~1` 撤回（未 push，无影响）后重新按文件拆分。

---

### 二、今日未完成的工作（待办）

1. **push `55c5007` / `ebf6f54`** —— **已完成**
   - 首次 `git push` 遇 Gitee HTTPS 凭据失效（`Incorrect username or password
     (access token)`），重新登录后推送成功，`654be16..ebf6f54 master -> master`。

2. 2026-10-09「二」的 3 条仍未动：业务封装成底层（首要）、web 回显 bug、上板实测
   webconfig（partial 解析 / dirty 跟踪）。

---

### 三、当前代码还需要确认的事情

**无新增**。本次只动环境配置文件，未触碰任何 `.c` / `.h` / Kconfig。

历史待定清单继续挂着，见 2026-10-08「三」+ 2026-09-29「三.A」6 条 + 2026-09-28「三」7 条。

---

### 四、后期的代码方向（中期路线）

1. 完善 web 和 menuconfig 中的内容（2026-09-28「四.1」，未动）
2. 把整个工程封装成底层，只留数据接口（2026-09-28「四.2」，**首要**，未动）

---

### 五、本次是否已经同步到仓库

**已同步**。`654be16..ebf6f54 master -> master` 推送成功，`master` 与 `origin/master`
一致，工作区干净（本文件随后单独提交）。

本节 3 个 commit：

| Commit | 说明 |
|---|---|
| `55c5007` | chore(vscode): 环境对齐 ESP-IDF 6.0.2 + 修正 clangd 编译数据库路径 |
| `ebf6f54` | chore(sdkconfig): 由 ESP-IDF 6.0.2 重新生成（项目配置项无变化） |
| 本节文档 | docs(work_notes): 记录 2026-11-03 仓库裁决收尾 |

至此 2026-09-28 以来累积的「仓库裁决」类问题（commit 切分、遗留 WiFi menuconfig 改动、
push 计划）**全部关闭**。


