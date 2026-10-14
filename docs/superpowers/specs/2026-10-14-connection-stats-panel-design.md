# 连接状态与统计面板 设计文档

日期：2026-10-14
状态：已与需求方确认，待实施计划

## 1. 背景与目标

AmneziaVPN 客户端在 iOS、macOS、Android 三端长时间使用 AWG/WireGuard 时出现速度逐渐下降、重连后恢复的现象。排查时发现客户端界面没有任何连接健康度信息（收发字节、速率、最近握手时间），无法在变慢时与服务器端 `awg show awg0 transfer` 做对照。

核心层其实已经把字节增量送到 `VpnConnection::bytesChanged`（`client/vpnConnection.cpp:57-60`），但没有任何控制器或 QML 消费它。本设计把这条链路接到界面，并补齐三端缺失的"最近握手时间"。

目标：三端在连接期间都能在界面上读到本次会话累计收发字节、当前速率、最近握手时间，并能一键复制成纯文本与服务器端数据对照。

本设计是三个子项目中的第一个。后续子项目（主页抽屉简化、设置页重新分组）不在本文范围内。

## 2. 范围

做：

- 新增核心单元 `ConnectionStatsController` 和 QML 包装 `ConnectionStatsUiController`。
- 主页连接按钮下方增加一行摘要，点开后显示详情抽屉，含"复制诊断信息"按钮。
- 桌面守护进程状态中增加最近握手时间；桌面端增加 1 秒状态轮询。
- iOS / macOS NE 上报最近握手时间。
- Android 恢复 1 秒统计推送，并上报最近握手时间。
- 单元测试覆盖统计计算与格式化。

不做：

- 历史曲线、统计持久化、设置开关。
- 设置页或导航结构的任何改动。
- OpenVPN / Xray 的统计支持（这两种协议没有字节回调，摘要行隐藏即可）。

## 3. 核心单元

### 3.1 ConnectionStatsController（`client/core/controllers/connectionStatsController.{h,cpp}`）

纯业务逻辑，不依赖 QML。

输入（槽，均由 `VpnConnection` 的信号驱动，在 `CoreController` 构造处接线）：

| 槽 | 来源信号 | 含义 |
|---|---|---|
| `onBytesChanged(quint64 rxDelta, quint64 txDelta)` | `VpnConnection::bytesChanged` | 自上次回调以来的字节增量 |
| `onHandshakeTimeChanged(qint64 msSinceEpoch)` | `VpnConnection::lastHandshakeChanged`（新增） | 最近一次成功握手的时间，0 表示未知 |
| `onConnectionStateChanged(Vpn::ConnectionState)` | `VpnConnection::connectionStateChanged` | 连接状态 |

内部状态：

- `m_rxTotal`、`m_txTotal`：本次会话累计字节。
- `m_rxRate`、`m_txRate`：当前速率，字节/秒。
- `m_lastHandshakeMs`：最近握手时间，毫秒纪元，0 为未知。
- `QElapsedTimer m_sinceLastSample`：上次字节回调到现在的时长。

规则：

- 速率 = 本次增量 ÷ 距上次增量的秒数。第一次回调（计时器未启动）只累加，不算速率。
- 进入 `Connected`：累计、速率、握手时间清零，计时器重启。
- 进入 `Reconnecting`：累计保留，速率归零。
- 进入 `Disconnected` 或 `Error`：全部清零。
- `isHandshakeStale()`：`m_lastHandshakeMs > 0` 且距今超过 180 秒（AWG 默认 RejectAfterTime）。未知时返回 false。

输出：

- 信号 `statsChanged()`，任一状态变化时发出。
- 只读访问器：`rxTotal()`、`txTotal()`、`rxRate()`、`txRate()`、`lastHandshakeMs()`、`isHandshakeStale()`。
- 静态纯函数 `formatBytes(quint64)` 和 `formatRate(double bytesPerSec)`，单位 B / KB / MB / GB（1024 进制），保留一位小数，小于 1 KB 不带小数。

### 3.2 ConnectionStatsUiController（`client/ui/controllers/connectionStatsUiController.{h,cpp}`）

QML 上下文属性名 `ConnectionStatsController`，在 `CoreController::setQmlRoot` 中注册，与现有 `ConnectionUiController` 注册为 `ConnectionController` 的做法一致。

只读属性（`NOTIFY statsChanged`）：

| 属性 | 示例 |
|---|---|
| `summaryText` | `↓ 1.2 MB/s  ↑ 86 KB/s · 握手 43 秒前` |
| `rxTotalText` / `txTotalText` | `1.2 GB` / `86 MB` |
| `rxRateText` / `txRateText` | `1.2 MB/s` / `86 KB/s` |
| `lastHandshakeText` | `43 秒前（14:52:07）`，未知时 `—` |
| `isHandshakeStale` | bool |

握手过期时 `summaryText` 末尾追加"（可能已断）"。所有文案走 `tr()`。

`Q_INVOKABLE void copyReport()`：拼接以下纯文本写入 `QGuiApplication::clipboard()`，返回后由 QML 弹出"已复制"提示。服务器名称、地址、端口、协议、隧道内 IP 从现有 `ServersController` / 默认服务器配置读取，不新增数据源。

```
AmneziaVPN 连接诊断
时间: 2026-10-14 14:52:50 (+08:00)
服务器: my_vps 203.0.113.10:1234
协议: AmneziaWG 3.1
隧道内 IP: 110.0.0.4
最近握手: 2026-10-14 14:52:07 (43 秒前)
本次会话接收: 1288490189 字节
本次会话发送: 90177536 字节
当前速率: 接收 1258291 字节/秒, 发送 88064 字节/秒
```

累计字节给原始数值，方便与服务器端 `awg show awg0 transfer` 直接相减。

## 4. 三端数据源

统一汇入点：`VpnConnection` 新增信号 `lastHandshakeChanged(qint64 msSinceEpoch)`。字节增量继续走已有的 `bytesChanged`。

### 4.1 桌面（macOS / Windows / Linux）

现状：`LocalSocketController` 只在连接时查询一次状态（`client/mozilla/localsocketcontroller.cpp:122,458`）；`WireguardProtocol` 收到 `statusUpdated` 后没有调用 `setBytesChanged`（`client/core/protocols/wireGuardProtocol.cpp:20-37`）；守护进程状态 JSON 没有握手时间（`client/daemon/daemon.cpp:586-607`）。

改动：

1. `Daemon::getStatus` 增加字段 `lastHandshake`（毫秒纪元），值取自 `PeerStatus::m_handshake`。三个桌面平台共用这段代码。
2. `ControllerImpl::statusUpdated` 增加末尾参数 `qint64 lastHandshake`；`LocalSocketController` 解析 JSON 并传入，字段缺失时传 0。
3. `WireguardProtocol`：
   - 新增 1 秒 `QTimer`，进入 `Connected` 时启动并调用 `m_impl->checkStatus()`，`stop()` 或断开时停止。
   - `statusUpdated` 回调中调用 `setBytesChanged(rxBytes, txBytes)`，并发出 `VpnProtocol` 新增信号 `handshakeTimeChanged(qint64)`。
4. `VpnConnection::createProtocolConnections` 把 `VpnProtocol::handshakeTimeChanged` 转发为 `lastHandshakeChanged`。

### 4.2 iOS / macOS NE

现状：`IosController::checkStatus` 每秒解析 `tx_bytes`、`rx_bytes`、`last_handshake_time_sec`（`client/platforms/ios/ios_controller.mm:355-383`），只向外发字节增量。

改动：`IosController` 新增信号 `handshakeTimeChanged(qint64 msSinceEpoch)`，在同一处解析后发出（秒值乘 1000，小于等于 0 时发 0）；`VpnConnection` 构造函数中连接到 `lastHandshakeChanged`。

### 4.3 Android

现状：服务侧向应用推送统计的代码被注释（`client/android/src/org/amnezia/vpn/AmneziaVpnService.kt:148,384,413-424`）；`Statistics` 只有 rx/tx（`client/android/protocolApi/src/main/kotlin/Statistics.kt`）；JNI 回调签名 `(JJ)V`（`client/platforms/android/android_controller.cpp:98`）。

改动：

1. `Statistics` 增加 `lastHandshake: Long`（毫秒纪元，0 为未知），Bundle 读写同步增加键。
2. `Wireguard.kt` 的 `statistics` 解析增加 `last_handshake_time_sec=` 一行。
3. `AmneziaVpnService.kt` 恢复 `launchSendingStatistics()` 及其调用点和停止点，周期 1 秒。
4. `QtAndroidController.onStatisticsUpdate` 改为三个 Long 参数；`AndroidController::onStatisticsUpdate` 签名改为 `(JJJ)V`；`AndroidController::statisticsUpdated` 增加第三个参数。
5. `AndroidVpnProtocol` 接收后调用 `setBytesChanged` 并发出 `handshakeTimeChanged`；`VpnConnection` 的 Android 接线（`client/vpnConnection.cpp:542`）相应调整。

## 5. 界面

### 5.1 主页摘要行（`client/ui/qml/Pages2/PageHome.qml`）

- 位置：`ConnectButton`（`:159-166`）之后、分流按钮之前。
- 控件：`BasicButtonType`，样式与分流按钮相同（透明背景、灰字、无边框），文字绑定 `ConnectionStatsController.summaryText`，右侧 `chevron-down.svg`。
- 显示条件：`ConnectionController.isConnected`。Reconnecting 时隐藏，由现有 `connectionStateText` 承担提示。
- `isHandshakeStale` 为真时文字改用警告色。
- 点击打开详情抽屉。

### 5.2 详情抽屉（新文件 `client/ui/qml/Components/ConnectionStatsDrawer.qml`）

结构照 `HomeSplitTunnelingDrawer.qml`：`DrawerType2` + `Header2Type` + 若干 `LabelWithButtonType` + `DividerType`，`parent: root`，`expandedHeight` 按内容高度。

行顺序（标题在 `text`，值在 `descriptionText`）：

1. 服务器 —— `名称 · 地址:端口`
2. 协议 —— `AmneziaWG 3.1`
3. 隧道内 IP
4. 最近握手 —— `43 秒前（14:52:07）`，过期时警告色
5. 本次会话 —— `接收 1.2 GB · 发送 86 MB`
6. 当前速率 —— `↓ 1.2 MB/s  ↑ 86 KB/s`
7. 底部 `BasicButtonType`"复制诊断信息"，调用 `ConnectionStatsController.copyReport()`，然后 `PageController.showNotificationMessage(qsTr("Copied"))`。

抽屉内数值随 `statsChanged` 每秒刷新，不额外轮询。

### 5.3 异常状态

| 情况 | 表现 |
|---|---|
| 握手时间未知（守护进程未升级、刚连上） | 摘要行显示 `握手 —`，抽屉第 4 行显示 `—` |
| 握手超过 180 秒 | 摘要行与抽屉第 4 行警告色，摘要末尾追加"（可能已断）" |
| OpenVPN / Xray | 没有字节回调，摘要行一直隐藏，不报错 |
| 重连中 | 摘要行隐藏，累计值保留 |
| 断开 | 清零，摘要行隐藏 |

## 6. 测试

### 6.1 单元测试

新增 `client/tests/testConnectionStats.cpp`，注册到 `client/tests/CMakeLists.txt`，测试名 `ConnectionStatsTest`。不需要真实 VPN 配置，不使用 `QSKIP`。直接调用 `ConnectionStatsController` 的槽：

- 进入 Connected 后累计为零。
- 两次增量后累计和速率正确（速率用 `QTest::qWait` 制造已知间隔，允许 20% 误差）。
- Reconnecting 保留累计、速率归零。
- Disconnected 清零。
- `formatBytes` 边界：999 B、1.0 KB、1.0 MB、1.0 GB。
- `formatRate` 同上带 `/s`。
- 握手过期判断：179 秒为 false，181 秒为 true，0 为 false。

### 6.2 手动验证

- 桌面：连接后用脚本读 `/var/run/amneziawg/*.sock` 的 `rx_bytes`、`tx_bytes`、`last_handshake_time_sec`，与界面数值对比，误差应在一次轮询内。
- iOS / Android 真机：断网再恢复时握手行先变警告色后恢复；锁屏回来累计值不清零。
- 回归：OpenVPN / Xray 连接下主页摘要行隐藏，无 QML 警告。

## 7. 涉及文件清单

新增：

- `client/core/controllers/connectionStatsController.{h,cpp}`
- `client/ui/controllers/connectionStatsUiController.{h,cpp}`
- `client/ui/qml/Components/ConnectionStatsDrawer.qml`
- `client/tests/testConnectionStats.cpp`

修改：

- `client/vpnConnection.{h,cpp}`
- `client/core/protocols/vpnProtocol.h`
- `client/core/protocols/wireGuardProtocol.{h,cpp}`
- `client/core/protocols/androidVpnProtocol.{h,cpp}`
- `client/core/controllers/coreController.{h,cpp}`
- `client/daemon/daemon.cpp`
- `client/mozilla/controllerimpl.h`、`client/mozilla/localsocketcontroller.cpp`
- `client/platforms/ios/ios_controller.{h,mm}`
- `client/platforms/android/android_controller.{h,cpp}`
- `client/android/protocolApi/src/main/kotlin/Statistics.kt`
- `client/android/wireguard/src/main/kotlin/org/amnezia/vpn/protocol/wireguard/Wireguard.kt`
- `client/android/src/org/amnezia/vpn/AmneziaVpnService.kt`
- `client/android/src/org/amnezia/vpn/AmneziaActivity.kt`
- `client/android/src/org/amnezia/vpn/qt/QtAndroidController.kt`
- `client/ui/qml/Pages2/PageHome.qml`
- `client/ui/qml/qml.qrc`
- `client/CMakeLists.txt`、`client/tests/CMakeLists.txt`
