# 连接状态与统计面板 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 iOS、macOS、Android 三端在连接期间能在主页看到本次会话累计收发字节、当前速率和最近握手时间，并能一键复制成纯文本与服务器端 `awg show awg0 transfer` 对照。

**Architecture:** 核心层新增 `ConnectionStatsController` 接住 `VpnConnection` 已有的 `bytesChanged` 和新增的 `lastHandshakeChanged` 信号，维护累计值与速率；`ConnectionStatsUiController` 把它包装成 QML 属性并提供复制功能。三端各自补齐"最近握手时间"的上报：桌面走守护进程 status JSON 并增加 1 秒轮询，iOS 在已有的每秒状态查询里多发一个信号，Android 恢复被注释掉的 1 秒统计推送并扩展 `Statistics`。

**Tech Stack:** Qt 6.10 / C++17 / QML（Qt Quick Controls 2）、QtTest、Objective-C++（iOS）、Kotlin + JNI（Android）、CMake + Conan。

**Spec:** `docs/superpowers/specs/2026-10-14-connection-stats-panel-design.md`

## Global Constraints

- 构建：`deploy/build.sh -g Ninja`，单测需 `AMNEZIA_BUILD_TESTS=ON`；测试运行前 `export QT_QPA_PLATFORM=offscreen`。
- 核心层新文件必须登记到 `client/cmake/sources.cmake` 的 HEADERS / SOURCES 列表；`client/ui/controllers/*.{h,cpp}` 由 glob 自动收录（`sources.cmake:234-240`），不用登记。
- 新 QML 文件必须登记到 `client/ui/qml/qml.qrc`。
- `.clang-format`：WebKit 风格、120 列、4 空格缩进、类和函数的大括号独占一行、`PointerAlignment: Right`。`client/mozilla`、`client/daemon`、`client/platforms/*` 在 `.clang-format-ignore` 中，改动这些目录时只改必要的行，保持原有格式，不要重排。
- 界面文案源码用英文并包裹 `tr()` / `qsTr()`，中文译文放 `client/translations/amneziavpn_zh_CN.ts`。
- 速率采样周期 1 秒；握手过期阈值 180 秒（`ConnectionStatsController::handshakeStaleThresholdMs`）。
- 不改 OpenVPN / Xray 协议类；不加设置项；不持久化统计。
- 复制文本的标签用英文字面量（不走翻译），保证格式稳定，便于脚本比对。这是对规范第 3.2 节示例的有意调整。
- 复制文本里服务器只给名称和主机名，不给端口（`ServerDescription` 没有端口字段，按协议分别取会引入无关分支）。这是对规范第 5.2 节"地址:端口"的有意调整。
- 提交信息末尾加一行：`Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`。

## Review Focus

1. **重连后协议对象复用，字节总量回退。** 桌面 `reconnectToVpn` 对同一个 `WireguardProtocol` 调 stop/start，新会话总量从 0 起，而 `VpnProtocol::m_receivedBytes` 仍是旧值，`receivedBytes - m_receivedBytes` 无符号下溢得到天文数字。期望：总量回退时把本次总量当作增量。测试：Task 1 `testProtocolCounterResetOnSmallerTotal`。
2. **两次采样落在同一毫秒。** 速率除以 0 得到 inf。期望：保留上一次速率。测试：Task 1 `testZeroElapsedKeepsPreviousRate`。
3. **握手时间未知（0）。** 不得判为过期，界面显示 `—`。测试：Task 1 `testHandshakeStale` 的 0 分支；Task 2 的 `lastHandshakeText` 分支。
4. **旧守护进程 + 新应用。** 用户更新应用但服务未重启时 status JSON 没有 `lastHandshake` 字段，解析必须按 0 处理而不是报错丢弃整条状态。Task 3 用 `toDouble(0)` 容错，并在 Task 3 的手动验证里覆盖。
5. **状态轮询刷日志。** 1 秒一次的状态查询会让守护进程和客户端日志每秒多写两行。期望：开启日志时日志文件不会因轮询膨胀。Task 3 删除这两行 debug 输出，并在手动验证里检查。

---

### Task 1: 核心统计单元 `ConnectionStatsController` 与单元测试

**Files:**
- Create: `client/core/controllers/connectionStatsController.h`
- Create: `client/core/controllers/connectionStatsController.cpp`
- Create: `client/tests/testConnectionStats.cpp`
- Modify: `client/core/protocols/vpnProtocol.cpp:66-75`（`setBytesChanged` 回退保护）
- Modify: `client/cmake/sources.cmake:45`（HEADERS）、`client/cmake/sources.cmake:130`（SOURCES）
- Modify: `client/tests/CMakeLists.txt:125-133`（新增可执行文件）、`client/tests/CMakeLists.txt:396`（`add_test`）

**Interfaces:**
- Consumes: `Vpn::ConnectionState`（`client/core/protocols/vpnProtocol.h`）、`VpnProtocol::setBytesChanged(quint64 receivedBytes, quint64 sentBytes)`。
- Produces:
  - `class ConnectionStatsController : public QObject`
  - 槽：`void onBytesChanged(quint64 rxDelta, quint64 txDelta)`、`void onHandshakeTimeChanged(qint64 msSinceEpoch)`、`void onConnectionStateChanged(Vpn::ConnectionState state)`
  - 信号：`void statsChanged()`
  - 访问器：`quint64 rxTotal() const`、`quint64 txTotal() const`、`double rxRate() const`、`double txRate() const`（字节/秒）、`qint64 lastHandshakeMs() const`（0 = 未知）、`bool isHandshakeStale() const`
  - 静态：`static QString formatBytes(quint64 bytes)`、`static QString formatRate(double bytesPerSec)`
  - 常量：`static constexpr qint64 handshakeStaleThresholdMs = 180 * 1000`
  - 受保护虚函数：`virtual qint64 nowMs() const`（测试用假时钟）

- [ ] **Step 1: 写失败的测试文件**

创建 `client/tests/testConnectionStats.cpp`：

```cpp
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include "core/controllers/connectionStatsController.h"
#include "core/protocols/vpnProtocol.h"

namespace
{
    class FakeClockStats : public ConnectionStatsController
    {
    public:
        qint64 fakeNowMs = 1'700'000'000'000;

    protected:
        qint64 nowMs() const override
        {
            return fakeNowMs;
        }
    };

    class StubProtocol : public VpnProtocol
    {
    public:
        StubProtocol() : VpnProtocol(QJsonObject())
        {
        }
        amnezia::ErrorCode start() override
        {
            return amnezia::ErrorCode::NoError;
        }
        void stop() override
        {
        }
    };
} // namespace

class TestConnectionStats : public QObject
{
    Q_OBJECT

private slots:
    void testConnectedStartsFromZero();
    void testTotalsAndRate();
    void testZeroElapsedKeepsPreviousRate();
    void testReconnectingKeepsTotalsAndZeroesRate();
    void testDisconnectedResets();
    void testReconnectThenConnectedResets();
    void testFormatBytesBoundaries();
    void testFormatRate();
    void testHandshakeStale();
    void testHandshakeSignalOnlyOnChange();
    void testProtocolCounterResetOnSmallerTotal();
};

void TestConnectionStats::testConnectedStartsFromZero()
{
    FakeClockStats stats;
    stats.onBytesChanged(500, 50);
    stats.onConnectionStateChanged(Vpn::ConnectionState::Connected);

    QCOMPARE(stats.rxTotal(), quint64(0));
    QCOMPARE(stats.txTotal(), quint64(0));
    QCOMPARE(stats.rxRate(), 0.0);
    QCOMPARE(stats.txRate(), 0.0);
    QCOMPARE(stats.lastHandshakeMs(), qint64(0));
}

void TestConnectionStats::testTotalsAndRate()
{
    FakeClockStats stats;
    QSignalSpy spy(&stats, &ConnectionStatsController::statsChanged);

    stats.onConnectionStateChanged(Vpn::ConnectionState::Connected);
    stats.fakeNowMs += 1000;
    stats.onBytesChanged(1000, 100);
    stats.fakeNowMs += 500;
    stats.onBytesChanged(2000, 500);

    QCOMPARE(stats.rxTotal(), quint64(3000));
    QCOMPARE(stats.txTotal(), quint64(600));
    QCOMPARE(stats.rxRate(), 4000.0);
    QCOMPARE(stats.txRate(), 1000.0);
    QCOMPARE(spy.count(), 3);
}

void TestConnectionStats::testZeroElapsedKeepsPreviousRate()
{
    FakeClockStats stats;
    stats.onConnectionStateChanged(Vpn::ConnectionState::Connected);
    stats.fakeNowMs += 1000;
    stats.onBytesChanged(1000, 0);
    stats.onBytesChanged(9000, 0);

    QCOMPARE(stats.rxRate(), 1000.0);
    QCOMPARE(stats.rxTotal(), quint64(10000));
}

void TestConnectionStats::testReconnectingKeepsTotalsAndZeroesRate()
{
    FakeClockStats stats;
    stats.onConnectionStateChanged(Vpn::ConnectionState::Connected);
    stats.fakeNowMs += 1000;
    stats.onBytesChanged(1000, 100);
    stats.onConnectionStateChanged(Vpn::ConnectionState::Reconnecting);

    QCOMPARE(stats.rxTotal(), quint64(1000));
    QCOMPARE(stats.txTotal(), quint64(100));
    QCOMPARE(stats.rxRate(), 0.0);
    QCOMPARE(stats.txRate(), 0.0);
}

void TestConnectionStats::testDisconnectedResets()
{
    FakeClockStats stats;
    stats.onConnectionStateChanged(Vpn::ConnectionState::Connected);
    stats.fakeNowMs += 1000;
    stats.onBytesChanged(1000, 100);
    stats.onHandshakeTimeChanged(stats.fakeNowMs);
    stats.onConnectionStateChanged(Vpn::ConnectionState::Disconnected);

    QCOMPARE(stats.rxTotal(), quint64(0));
    QCOMPARE(stats.txTotal(), quint64(0));
    QCOMPARE(stats.rxRate(), 0.0);
    QCOMPARE(stats.lastHandshakeMs(), qint64(0));
}

void TestConnectionStats::testReconnectThenConnectedResets()
{
    FakeClockStats stats;
    stats.onConnectionStateChanged(Vpn::ConnectionState::Connected);
    stats.fakeNowMs += 1000;
    stats.onBytesChanged(1000, 100);
    stats.onConnectionStateChanged(Vpn::ConnectionState::Reconnecting);
    stats.onConnectionStateChanged(Vpn::ConnectionState::Connected);

    QCOMPARE(stats.rxTotal(), quint64(0));
    QCOMPARE(stats.txTotal(), quint64(0));
}

void TestConnectionStats::testFormatBytesBoundaries()
{
    QCOMPARE(ConnectionStatsController::formatBytes(0), QStringLiteral("0 B"));
    QCOMPARE(ConnectionStatsController::formatBytes(999), QStringLiteral("999 B"));
    QCOMPARE(ConnectionStatsController::formatBytes(1024), QStringLiteral("1.0 KB"));
    QCOMPARE(ConnectionStatsController::formatBytes(1536), QStringLiteral("1.5 KB"));
    QCOMPARE(ConnectionStatsController::formatBytes(1024ull * 1024), QStringLiteral("1.0 MB"));
    QCOMPARE(ConnectionStatsController::formatBytes(1024ull * 1024 * 1024), QStringLiteral("1.0 GB"));
}

void TestConnectionStats::testFormatRate()
{
    QCOMPARE(ConnectionStatsController::formatRate(0.0), QStringLiteral("0 B/s"));
    QCOMPARE(ConnectionStatsController::formatRate(1024.0), QStringLiteral("1.0 KB/s"));
    QCOMPARE(ConnectionStatsController::formatRate(1258291.0), QStringLiteral("1.2 MB/s"));
    QCOMPARE(ConnectionStatsController::formatRate(-5.0), QStringLiteral("0 B/s"));
}

void TestConnectionStats::testHandshakeStale()
{
    FakeClockStats stats;
    stats.onConnectionStateChanged(Vpn::ConnectionState::Connected);

    QVERIFY(!stats.isHandshakeStale());

    stats.onHandshakeTimeChanged(stats.fakeNowMs - 179 * 1000);
    QVERIFY(!stats.isHandshakeStale());

    stats.onHandshakeTimeChanged(stats.fakeNowMs - 181 * 1000);
    QVERIFY(stats.isHandshakeStale());

    stats.onHandshakeTimeChanged(0);
    QVERIFY(!stats.isHandshakeStale());
}

void TestConnectionStats::testHandshakeSignalOnlyOnChange()
{
    FakeClockStats stats;
    QSignalSpy spy(&stats, &ConnectionStatsController::statsChanged);

    stats.onHandshakeTimeChanged(123456);
    stats.onHandshakeTimeChanged(123456);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(stats.lastHandshakeMs(), qint64(123456));
}

void TestConnectionStats::testProtocolCounterResetOnSmallerTotal()
{
    StubProtocol protocol;
    QSignalSpy spy(&protocol, &VpnProtocol::bytesChanged);

    protocol.setBytesChanged(1000, 100);
    protocol.setBytesChanged(1500, 150);
    protocol.setBytesChanged(200, 20);

    QCOMPARE(spy.count(), 3);
    QCOMPARE(spy.at(1).at(0).toULongLong(), quint64(500));
    QCOMPARE(spy.at(1).at(1).toULongLong(), quint64(50));
    QCOMPARE(spy.at(2).at(0).toULongLong(), quint64(200));
    QCOMPARE(spy.at(2).at(1).toULongLong(), quint64(20));
}

QTEST_MAIN(TestConnectionStats)
#include "testConnectionStats.moc"
```

- [ ] **Step 2: 登记测试可执行文件**

在 `client/tests/CMakeLists.txt` 的 `add_executable(test_xray_serialization ...)` 块之前（约第 134 行）加入：

```cmake
    add_executable(test_connection_stats
        testConnectionStats.cpp
    )

    target_link_libraries(test_connection_stats PRIVATE
        Qt6::Test
        test_common
    )
```

在同文件 `add_test(NAME MultipleImportsTest ...)`（约第 396 行）那一组里加一行：

```cmake
    add_test(NAME ConnectionStatsTest              COMMAND test_connection_stats)
```

- [ ] **Step 3: 配置并确认测试编译失败**

运行（首次配置较慢，可在 10 分钟以上）：

```bash
cd /Users/zhangwt/cc_project/amnezia-client/amnezia-client
AMNEZIA_BUILD_TESTS=ON deploy/build.sh -g Ninja 2>&1 | tail -5
cmake --build deploy/build --target test_connection_stats 2>&1 | tail -5
```

Expected: 编译失败，错误信息包含 `connectionStatsController.h: No such file or directory`。

- [ ] **Step 4: 写头文件**

创建 `client/core/controllers/connectionStatsController.h`：

```cpp
#ifndef CONNECTIONSTATSCONTROLLER_H
#define CONNECTIONSTATSCONTROLLER_H

#include <QObject>
#include <QString>

#include "core/protocols/vpnProtocol.h"

class ConnectionStatsController : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 handshakeStaleThresholdMs = 180 * 1000;

    explicit ConnectionStatsController(QObject *parent = nullptr);
    ~ConnectionStatsController() override = default;

    quint64 rxTotal() const;
    quint64 txTotal() const;
    double rxRate() const;
    double txRate() const;
    qint64 lastHandshakeMs() const;
    bool isHandshakeStale() const;

    static QString formatBytes(quint64 bytes);
    static QString formatRate(double bytesPerSec);

public slots:
    void onBytesChanged(quint64 rxDelta, quint64 txDelta);
    void onHandshakeTimeChanged(qint64 msSinceEpoch);
    void onConnectionStateChanged(Vpn::ConnectionState state);

signals:
    void statsChanged();

protected:
    virtual qint64 nowMs() const;

private:
    void resetAll();

    quint64 m_rxTotal = 0;
    quint64 m_txTotal = 0;
    double m_rxRate = 0.0;
    double m_txRate = 0.0;
    qint64 m_lastHandshakeMs = 0;
    qint64 m_lastSampleMs = -1;
};

#endif // CONNECTIONSTATSCONTROLLER_H
```

- [ ] **Step 5: 写实现**

创建 `client/core/controllers/connectionStatsController.cpp`：

```cpp
#include "connectionStatsController.h"

#include <QDateTime>

ConnectionStatsController::ConnectionStatsController(QObject *parent) : QObject(parent)
{
}

quint64 ConnectionStatsController::rxTotal() const
{
    return m_rxTotal;
}

quint64 ConnectionStatsController::txTotal() const
{
    return m_txTotal;
}

double ConnectionStatsController::rxRate() const
{
    return m_rxRate;
}

double ConnectionStatsController::txRate() const
{
    return m_txRate;
}

qint64 ConnectionStatsController::lastHandshakeMs() const
{
    return m_lastHandshakeMs;
}

bool ConnectionStatsController::isHandshakeStale() const
{
    if (m_lastHandshakeMs <= 0) {
        return false;
    }
    return nowMs() - m_lastHandshakeMs > handshakeStaleThresholdMs;
}

qint64 ConnectionStatsController::nowMs() const
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString ConnectionStatsController::formatBytes(quint64 bytes)
{
    constexpr double kib = 1024.0;
    const double value = static_cast<double>(bytes);
    if (value < kib) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (value < kib * kib) {
        return QStringLiteral("%1 KB").arg(value / kib, 0, 'f', 1);
    }
    if (value < kib * kib * kib) {
        return QStringLiteral("%1 MB").arg(value / (kib * kib), 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(value / (kib * kib * kib), 0, 'f', 1);
}

QString ConnectionStatsController::formatRate(double bytesPerSec)
{
    if (bytesPerSec < 0.0) {
        bytesPerSec = 0.0;
    }
    return formatBytes(static_cast<quint64>(bytesPerSec + 0.5)) + QStringLiteral("/s");
}

void ConnectionStatsController::onBytesChanged(quint64 rxDelta, quint64 txDelta)
{
    m_rxTotal += rxDelta;
    m_txTotal += txDelta;

    const qint64 now = nowMs();
    if (m_lastSampleMs >= 0) {
        const qint64 elapsedMs = now - m_lastSampleMs;
        if (elapsedMs > 0) {
            m_rxRate = static_cast<double>(rxDelta) * 1000.0 / static_cast<double>(elapsedMs);
            m_txRate = static_cast<double>(txDelta) * 1000.0 / static_cast<double>(elapsedMs);
            m_lastSampleMs = now;
        }
    } else {
        m_lastSampleMs = now;
    }

    emit statsChanged();
}

void ConnectionStatsController::onHandshakeTimeChanged(qint64 msSinceEpoch)
{
    if (m_lastHandshakeMs == msSinceEpoch) {
        return;
    }
    m_lastHandshakeMs = msSinceEpoch;
    emit statsChanged();
}

void ConnectionStatsController::onConnectionStateChanged(Vpn::ConnectionState state)
{
    switch (state) {
    case Vpn::ConnectionState::Connected:
        resetAll();
        m_lastSampleMs = nowMs();
        break;
    case Vpn::ConnectionState::Reconnecting:
        m_rxRate = 0.0;
        m_txRate = 0.0;
        m_lastSampleMs = -1;
        break;
    case Vpn::ConnectionState::Disconnected:
    case Vpn::ConnectionState::Error:
        resetAll();
        break;
    default:
        return;
    }
    emit statsChanged();
}

void ConnectionStatsController::resetAll()
{
    m_rxTotal = 0;
    m_txTotal = 0;
    m_rxRate = 0.0;
    m_txRate = 0.0;
    m_lastHandshakeMs = 0;
    m_lastSampleMs = -1;
}
```

说明：规范第 3.1 节写的是 `QElapsedTimer`，这里改用可被测试覆盖的 `nowMs()` 虚函数加 `m_lastSampleMs`，行为相同，但速率计算可以用假时钟做确定性测试。`testZeroElapsedKeepsPreviousRate` 要求同一毫秒内的第二次采样不更新 `m_lastSampleMs`，否则下一次采样的时间基准会漂移，上面 `elapsedMs > 0` 分支内才写回 `m_lastSampleMs` 就是为此。

- [ ] **Step 6: 给 `VpnProtocol::setBytesChanged` 加回退保护**

把 `client/core/protocols/vpnProtocol.cpp:66-75` 改为：

```cpp
void VpnProtocol::setBytesChanged(quint64 receivedBytes, quint64 sentBytes)
{
    // Counters restart from zero when the tunnel is re-created on reconnect;
    // treat a smaller total as a fresh session instead of underflowing.
    quint64 rxDiff = receivedBytes >= m_receivedBytes ? receivedBytes - m_receivedBytes : receivedBytes;
    quint64 txDiff = sentBytes >= m_sentBytes ? sentBytes - m_sentBytes : sentBytes;

    emit bytesChanged(rxDiff, txDiff);

    m_receivedBytes = receivedBytes;
    m_sentBytes = sentBytes;
}
```

- [ ] **Step 7: 登记源文件**

`client/cmake/sources.cmake` 第 45 行 `connectionController.h` 之后加：

```cmake
    ${CLIENT_ROOT_DIR}/core/controllers/connectionStatsController.h
```

第 130 行 `connectionController.cpp` 之后加：

```cmake
    ${CLIENT_ROOT_DIR}/core/controllers/connectionStatsController.cpp
```

- [ ] **Step 8: 编译并运行测试**

```bash
cd /Users/zhangwt/cc_project/amnezia-client/amnezia-client
cmake --build deploy/build --target test_connection_stats 2>&1 | tail -3
export QT_QPA_PLATFORM=offscreen
ctest --test-dir deploy/build/client/tests -R '^ConnectionStatsTest$' --output-on-failure
```

Expected: `100% tests passed`，11 个用例全部 PASS。

- [ ] **Step 9: 提交**

```bash
git add client/core/controllers/connectionStatsController.h client/core/controllers/connectionStatsController.cpp \
        client/core/protocols/vpnProtocol.cpp client/cmake/sources.cmake \
        client/tests/testConnectionStats.cpp client/tests/CMakeLists.txt
git commit -m "feat: add ConnectionStatsController with session totals, rates and handshake age

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: 握手时间信号链路与 QML 包装 `ConnectionStatsUiController`

**Files:**
- Modify: `client/core/protocols/vpnProtocol.h:75-88`（新增信号与槽）
- Modify: `client/core/protocols/vpnProtocol.cpp`（新增槽实现）
- Modify: `client/vpnConnection.h:58-63`（新增信号）
- Modify: `client/vpnConnection.cpp:380-391`（`createProtocolConnections` 转发）
- Create: `client/ui/controllers/connectionStatsUiController.h`
- Create: `client/ui/controllers/connectionStatsUiController.cpp`
- Modify: `client/core/controllers/coreController.h:16,44,165,199`（包含、成员）
- Modify: `client/core/controllers/coreController.cpp:155-178`（创建、接线、注册）

**Interfaces:**
- Consumes: Task 1 的 `ConnectionStatsController` 全部接口；`ServersController::getDefaultServerId()`、`ServersController::buildServerDescriptions(bool)`（返回 `QVector<ServerDescription>`，字段 `serverId`、`serverName`、`hostName`、`defaultContainer`）；`ContainerUtils::containerHumanNames()`；`VpnConnection::vpnProtocol()`（返回 `QSharedPointer<VpnProtocol>`）、`VpnProtocol::vpnLocalAddress()`。
- Produces:
  - `VpnProtocol` 信号 `void handshakeTimeChanged(qint64 msSinceEpoch)` 与公共槽 `void setHandshakeTime(qint64 msSinceEpoch)`（直接发出该信号）。后续 Task 3、5 调用这个槽。
  - `VpnConnection` 信号 `void lastHandshakeChanged(qint64 msSinceEpoch)`。Task 4 直接连接到它。
  - `class ConnectionStatsUiController : public QObject`，QML 上下文属性名 `ConnectionStatsController`，只读属性 `summaryText`、`rxTotalText`、`txTotalText`、`rxRateText`、`txRateText`、`lastHandshakeText`（均 `QString`）、`isHandshakeStale`（`bool`），信号 `statsChanged()`，`Q_INVOKABLE void copyReport()`，公共方法 `QString buildReport() const`。Task 6 的 QML 使用这些名字。

- [ ] **Step 1: `VpnProtocol` 增加握手信号与槽**

`client/core/protocols/vpnProtocol.h` 的 `signals:` 块（第 75 行起）增加一行：

```cpp
    void handshakeTimeChanged(qint64 msSinceEpoch);
```

`public slots:` 块中 `setBytesChanged` 之后增加：

```cpp
    void setHandshakeTime(qint64 msSinceEpoch);
```

`client/core/protocols/vpnProtocol.cpp` 中 `setBytesChanged` 实现之后增加：

```cpp
void VpnProtocol::setHandshakeTime(qint64 msSinceEpoch)
{
    emit handshakeTimeChanged(msSinceEpoch);
}
```

- [ ] **Step 2: `VpnConnection` 增加信号并转发**

`client/vpnConnection.h` 的 `signals:` 块中 `bytesChanged` 之后增加：

```cpp
    void lastHandshakeChanged(qint64 msSinceEpoch);
```

`client/vpnConnection.cpp` 的 `createProtocolConnections()`（第 380 行起）中，`bytesChanged` 那一行 connect 之后增加：

```cpp
    connect(m_vpnProtocol.data(), &VpnProtocol::handshakeTimeChanged, this, &VpnConnection::lastHandshakeChanged);
```

- [ ] **Step 3: 写 UI 控制器头文件**

创建 `client/ui/controllers/connectionStatsUiController.h`：

```cpp
#ifndef CONNECTIONSTATSUICONTROLLER_H
#define CONNECTIONSTATSUICONTROLLER_H

#include <QObject>
#include <QString>

#include "core/controllers/connectionStatsController.h"
#include "core/controllers/serversController.h"
#include "core/models/serverDescription.h"
#include "vpnConnection.h"

class ConnectionStatsUiController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString summaryText READ summaryText NOTIFY statsChanged)
    Q_PROPERTY(QString rxTotalText READ rxTotalText NOTIFY statsChanged)
    Q_PROPERTY(QString txTotalText READ txTotalText NOTIFY statsChanged)
    Q_PROPERTY(QString rxRateText READ rxRateText NOTIFY statsChanged)
    Q_PROPERTY(QString txRateText READ txRateText NOTIFY statsChanged)
    Q_PROPERTY(QString lastHandshakeText READ lastHandshakeText NOTIFY statsChanged)
    Q_PROPERTY(bool isHandshakeStale READ isHandshakeStale NOTIFY statsChanged)

public:
    explicit ConnectionStatsUiController(ConnectionStatsController *statsController, ServersController *serversController,
                                         VpnConnection *vpnConnection, QObject *parent = nullptr);
    ~ConnectionStatsUiController() override = default;

    QString summaryText() const;
    QString rxTotalText() const;
    QString txTotalText() const;
    QString rxRateText() const;
    QString txRateText() const;
    QString lastHandshakeText() const;
    bool isHandshakeStale() const;

    QString buildReport() const;

    Q_INVOKABLE void copyReport();

signals:
    void statsChanged();

private:
    QString handshakeAgeText() const;
    QString tunnelAddress() const;
    ServerDescription defaultServerDescription() const;

    ConnectionStatsController *m_stats;
    ServersController *m_serversController;
    VpnConnection *m_vpnConnection;
};

#endif // CONNECTIONSTATSUICONTROLLER_H
```

- [ ] **Step 4: 写 UI 控制器实现**

创建 `client/ui/controllers/connectionStatsUiController.cpp`：

```cpp
#include "connectionStatsUiController.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QStringList>

#include "core/utils/containers/containerUtils.h"

ConnectionStatsUiController::ConnectionStatsUiController(ConnectionStatsController *statsController,
                                                         ServersController *serversController,
                                                         VpnConnection *vpnConnection, QObject *parent)
    : QObject(parent), m_stats(statsController), m_serversController(serversController), m_vpnConnection(vpnConnection)
{
    connect(m_stats, &ConnectionStatsController::statsChanged, this, &ConnectionStatsUiController::statsChanged);
}

QString ConnectionStatsUiController::rxTotalText() const
{
    return ConnectionStatsController::formatBytes(m_stats->rxTotal());
}

QString ConnectionStatsUiController::txTotalText() const
{
    return ConnectionStatsController::formatBytes(m_stats->txTotal());
}

QString ConnectionStatsUiController::rxRateText() const
{
    return ConnectionStatsController::formatRate(m_stats->rxRate());
}

QString ConnectionStatsUiController::txRateText() const
{
    return ConnectionStatsController::formatRate(m_stats->txRate());
}

bool ConnectionStatsUiController::isHandshakeStale() const
{
    return m_stats->isHandshakeStale();
}

QString ConnectionStatsUiController::handshakeAgeText() const
{
    const qint64 handshakeMs = m_stats->lastHandshakeMs();
    if (handshakeMs <= 0) {
        return QStringLiteral("—");
    }
    const qint64 ageSec = qMax<qint64>(0, (QDateTime::currentMSecsSinceEpoch() - handshakeMs) / 1000);
    if (ageSec < 60) {
        return tr("%1 s ago").arg(ageSec);
    }
    return tr("%1 min %2 s ago").arg(ageSec / 60).arg(ageSec % 60);
}

QString ConnectionStatsUiController::lastHandshakeText() const
{
    const qint64 handshakeMs = m_stats->lastHandshakeMs();
    if (handshakeMs <= 0) {
        return QStringLiteral("—");
    }
    const QString clock = QDateTime::fromMSecsSinceEpoch(handshakeMs).toString(QStringLiteral("HH:mm:ss"));
    return QStringLiteral("%1 (%2)").arg(handshakeAgeText(), clock);
}

QString ConnectionStatsUiController::summaryText() const
{
    QString text = QStringLiteral("↓ %1  ↑ %2 · %3 %4")
                           .arg(rxRateText(), txRateText(), tr("handshake"), handshakeAgeText());
    if (m_stats->isHandshakeStale()) {
        text += tr(" (connection may be dead)");
    }
    return text;
}

ServerDescription ConnectionStatsUiController::defaultServerDescription() const
{
    const QString serverId = m_serversController->getDefaultServerId();
    const auto descriptions = m_serversController->buildServerDescriptions(false);
    for (const ServerDescription &description : descriptions) {
        if (description.serverId == serverId) {
            return description;
        }
    }
    return ServerDescription();
}

QString ConnectionStatsUiController::tunnelAddress() const
{
    if (m_vpnConnection && m_vpnConnection->vpnProtocol()) {
        const QString address = m_vpnConnection->vpnProtocol()->vpnLocalAddress();
        if (!address.isEmpty()) {
            return address;
        }
    }
    return QStringLiteral("—");
}

QString ConnectionStatsUiController::buildReport() const
{
    const ServerDescription server = defaultServerDescription();
    const QString protocolName = ContainerUtils::containerHumanNames().value(server.defaultContainer);

    const QDateTime now = QDateTime::currentDateTime();
    const QString nowText = now.toOffsetFromUtc(now.offsetFromUtc()).toString(Qt::ISODate);

    const qint64 handshakeMs = m_stats->lastHandshakeMs();
    const QString handshakeLine = handshakeMs > 0
            ? QStringLiteral("%1 (%2)")
                      .arg(QDateTime::fromMSecsSinceEpoch(handshakeMs).toString(Qt::ISODate), handshakeAgeText())
            : QStringLiteral("—");

    QStringList lines;
    lines << QStringLiteral("AmneziaVPN connection diagnostics") << QStringLiteral("time: %1").arg(nowText)
          << QStringLiteral("server: %1 %2").arg(server.serverName, server.hostName)
          << QStringLiteral("protocol: %1").arg(protocolName) << QStringLiteral("tunnel ip: %1").arg(tunnelAddress())
          << QStringLiteral("last handshake: %1").arg(handshakeLine)
          << QStringLiteral("session rx bytes: %1").arg(m_stats->rxTotal())
          << QStringLiteral("session tx bytes: %1").arg(m_stats->txTotal())
          << QStringLiteral("rate: rx %1 B/s, tx %2 B/s")
                     .arg(qRound64(m_stats->rxRate()))
                     .arg(qRound64(m_stats->txRate()));
    return lines.join(QLatin1Char('\n'));
}

void ConnectionStatsUiController::copyReport()
{
    QGuiApplication::clipboard()->setText(buildReport());
}
```

- [ ] **Step 5: 在 `CoreController` 创建、接线、注册**

`client/core/controllers/coreController.h`：

第 16 行 `#include "ui/controllers/connectionUiController.h"` 之后加：

```cpp
#include "ui/controllers/connectionStatsUiController.h"
```

第 44 行 `#include "core/controllers/connectionController.h"` 之后加：

```cpp
#include "core/controllers/connectionStatsController.h"
```

第 165 行 `ConnectionUiController* m_connectionUiController;` 之后加：

```cpp
    ConnectionStatsUiController* m_connectionStatsUiController;
```

第 199 行 `ConnectionController* m_connectionController;` 之后加：

```cpp
    ConnectionStatsController* m_connectionStatsController;
```

`client/core/controllers/coreController.cpp`：

`initCoreControllers()` 中第 170 行创建 `m_connectionController` 之后加：

```cpp
    m_connectionStatsController = new ConnectionStatsController(this);
    if (m_vpnConnection) {
        connect(m_vpnConnection.get(), &VpnConnection::bytesChanged, m_connectionStatsController,
                &ConnectionStatsController::onBytesChanged);
        connect(m_vpnConnection.get(), &VpnConnection::lastHandshakeChanged, m_connectionStatsController,
                &ConnectionStatsController::onHandshakeTimeChanged);
        connect(m_vpnConnection.get(), &VpnConnection::connectionStateChanged, m_connectionStatsController,
                &ConnectionStatsController::onConnectionStateChanged);
    }
```

`initControllers()` 中第 177 行 `setQmlContextProperty("ConnectionController", ...)` 之后加：

```cpp
    m_connectionStatsUiController = new ConnectionStatsUiController(m_connectionStatsController, m_serversController,
                                                                    m_vpnConnection.get(), this);
    setQmlContextProperty("ConnectionStatsController", m_connectionStatsUiController);
```

- [ ] **Step 6: 编译客户端与测试**

```bash
cd /Users/zhangwt/cc_project/amnezia-client/amnezia-client
cmake --build deploy/build 2>&1 | grep -E "error|warning: unused|Linking CXX executable" | head -20
export QT_QPA_PLATFORM=offscreen
ctest --test-dir deploy/build/client/tests -R '^ConnectionStatsTest$' --output-on-failure | tail -3
```

Expected: 无 `error`；`ConnectionStatsTest` 仍然 PASS。

- [ ] **Step 7: 提交**

```bash
git add client/core/protocols/vpnProtocol.h client/core/protocols/vpnProtocol.cpp \
        client/vpnConnection.h client/vpnConnection.cpp \
        client/ui/controllers/connectionStatsUiController.h client/ui/controllers/connectionStatsUiController.cpp \
        client/core/controllers/coreController.h client/core/controllers/coreController.cpp
git commit -m "feat: expose connection stats to QML and add handshake time signal chain

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: 桌面端数据源（守护进程握手时间 + 1 秒轮询）

**Files:**
- Modify: `client/daemon/daemon.cpp:586-607`（`getStatus` 增加字段，删除一行 debug）
- Modify: `client/mozilla/controllerimpl.h:84-91`（`statusUpdated` 签名）
- Modify: `client/mozilla/localsocketcontroller.cpp:282-292,418-441,448-463`（解析、发出、删除一行 debug）
- Modify: `client/core/protocols/wireGuardProtocol.h`（定时器成员）
- Modify: `client/core/protocols/wireGuardProtocol.cpp:11-55`（定时器、回调）

**Interfaces:**
- Consumes: Task 2 的 `VpnProtocol::setHandshakeTime(qint64)`；`VpnProtocol::setBytesChanged(quint64 receivedBytes, quint64 sentBytes)`（注意参数顺序是 received 在前）。
- Produces: status JSON 新字段 `lastHandshake`（double，毫秒纪元，0 = 未知）；`ControllerImpl::statusUpdated(const QString& serverIpv4Gateway, const QString& deviceIpv4Address, uint64_t txBytes, uint64_t rxBytes, qint64 lastHandshakeMs)`。

- [ ] **Step 1: 确认 `statusUpdated` 只有一个发出方和一个接收方**

```bash
cd /Users/zhangwt/cc_project/amnezia-client/amnezia-client
grep -rn "statusUpdated" client/ service/ --include='*.cpp' --include='*.h' --include='*.mm'
```

Expected: 只出现在 `client/mozilla/controllerimpl.h`、`client/mozilla/localsocketcontroller.cpp`（两处 emit）、`client/core/protocols/wireGuardProtocol.cpp`（一处 connect）。若有其他位置，一并按新签名修改。

- [ ] **Step 2: 守护进程状态增加握手时间**

`client/daemon/daemon.cpp` 的 `Daemon::getStatus()`：

删除第 589 行：

```cpp
  logger.debug() << "Status request";
```

在 `json.insert("rxBytes", QJsonValue(status.m_rxBytes));` 之后加：

```cpp
    json.insert("lastHandshake", QJsonValue(static_cast<double>(status.m_handshake)));
```

（`status.m_handshake` 已是毫秒纪元，见 `client/platforms/macos/daemon/wireguardutilsmacos.cpp:333-336`，Windows/Linux 同样。）

- [ ] **Step 3: 改 `ControllerImpl::statusUpdated` 签名**

`client/mozilla/controllerimpl.h` 第 84-91 行改为：

```cpp
  // This method should be emitted after a checkStatus() call.
  // "serverIpv4Gateway" is the current VPN tunnel gateway.
  // "deviceIpv4Address" is the address of the VPN client.
  // "txBytes" and "rxBytes" contain the total number of transmitted and
  // received bytes for the current tunnel.
  // "lastHandshakeMs" is the last successful handshake time in milliseconds
  // since epoch, or 0 when unknown.
  void statusUpdated(const QString& serverIpv4Gateway,
                     const QString& deviceIpv4Address, uint64_t txBytes,
                     uint64_t rxBytes, qint64 lastHandshakeMs);
```

- [ ] **Step 4: `LocalSocketController` 解析并发出**

`client/mozilla/localsocketcontroller.cpp`：

`checkStatus()`（第 282 行起）删除这一行：

```cpp
  logger.debug() << "Check status";
```

`type == "status"` 分支（第 418 行起）里，`rxBytes` 校验之后、`emit statusUpdated(...)` 之前加：

```cpp
    const qint64 lastHandshakeMs =
        static_cast<qint64>(obj.value("lastHandshake").toDouble(0));
```

并把 emit 改为：

```cpp
    emit statusUpdated(serverIpv4Gateway.toString(),
                       deviceIpv4Address.toString(), txBytes.toDouble(),
                       rxBytes.toDouble(), lastHandshakeMs);
```

`type == "connected"` 分支（第 448 行起）里的 `emit statusUpdated("", m_deviceIpv4, 0, 0);` 改为：

```cpp
    emit statusUpdated("", m_deviceIpv4, 0, 0, 0);
```

- [ ] **Step 5: `WireguardProtocol` 增加 1 秒轮询并消费状态**

`client/core/protocols/wireGuardProtocol.h` 的 `private:` 块中 `QScopedPointer<ControllerImpl> m_impl;` 之后加：

```cpp
    QTimer m_statusTimer;
```

（头文件已经 `#include <QTimer>`。）

`client/core/protocols/wireGuardProtocol.cpp` 的构造函数（第 11-42 行）改为：

```cpp
WireguardProtocol::WireguardProtocol(const QJsonObject &configuration, QObject *parent)
    : VpnProtocol(configuration, parent)
{
    m_impl.reset(new LocalSocketController());

    m_statusTimer.setInterval(1000);
    connect(&m_statusTimer, &QTimer::timeout, this, [this]() { m_impl->checkStatus(); });

    connect(m_impl.get(), &ControllerImpl::connected, this,
            [this](const QString &pubkey, const QDateTime &connectionTimestamp) {
                setConnectionState(Vpn::ConnectionState::Connected);
                m_statusTimer.start();
            });
    connect(m_impl.get(), &ControllerImpl::statusUpdated, this,
            [this](const QString& serverIpv4Gateway,
                   const QString& deviceIpv4Address, uint64_t txBytes,
                   uint64_t rxBytes, qint64 lastHandshakeMs) {
                const QString previousGateway = m_vpnGateway;
                const QString previousLocal = m_vpnLocalAddress;

                if (!serverIpv4Gateway.isEmpty()) {
                    m_vpnGateway = serverIpv4Gateway;
                }
                if (!deviceIpv4Address.isEmpty()) {
                    m_vpnLocalAddress = deviceIpv4Address;
                }

                if ((!m_vpnGateway.isEmpty() && m_vpnGateway != previousGateway) ||
                    (!m_vpnLocalAddress.isEmpty() && m_vpnLocalAddress != previousLocal)) {
                    emit tunnelAddressesUpdated(m_vpnGateway, m_vpnLocalAddress);
                }

                if (txBytes != 0 || rxBytes != 0) {
                    setBytesChanged(rxBytes, txBytes);
                }
                if (lastHandshakeMs > 0) {
                    setHandshakeTime(lastHandshakeMs);
                }
            });

    connect(m_impl.get(), &ControllerImpl::disconnected, this, [this]() {
        m_statusTimer.stop();
        setConnectionState(Vpn::ConnectionState::Disconnected);
    });
    m_impl->initialize(nullptr, nullptr);
}
```

`WireguardProtocol::stop()`（第 51-55 行）改为：

```cpp
void WireguardProtocol::stop()
{
    m_statusTimer.stop();
    stopMzImpl();
    return;
}
```

- [ ] **Step 6: 编译客户端与服务**

```bash
cd /Users/zhangwt/cc_project/amnezia-client/amnezia-client
cmake --build deploy/build 2>&1 | grep -E "error|Linking CXX executable" | head -20
```

Expected: 无 `error`；`AmneziaVPN` 和 `AmneziaVPN-service` 都重新链接。

- [ ] **Step 7: 手动验证（macOS）**

1. 安装或替换本机的 `AmneziaVPN-service`，重启服务后连接 AWG 服务器。
2. 在应用里开启日志，连接 60 秒后查看 `~/Library/Application Support/AmneziaVPN.ORG/AmneziaVPN/log/AmneziaVPN.log`，确认没有每秒出现的 `Check status` / `Status request` 行。
3. 终端执行：

```bash
sudo python3 -c '
import socket,glob
for p in glob.glob("/var/run/amneziawg/*.sock"):
    s=socket.socket(socket.AF_UNIX); s.connect(p); s.sendall(b"get=1\n\n"); d=b""
    while b"errno=" not in d: d+=s.recv(65536)
    for l in d.decode().splitlines():
        if l.split("=")[0] in ("rx_bytes","tx_bytes","last_handshake_time_sec"): print(l)
'
```

在 Task 6 完成后对照界面数值；在 Task 6 之前，可临时在 `ConnectionStatsController::onBytesChanged` 加 `qDebug()` 观察增量是否每秒到达一次，验证完删除。

4. 不重启服务、只替换应用（模拟旧守护进程）：连接后应用不得报 `Unexpected` 解析错误，握手显示为未知。

- [ ] **Step 8: 提交**

```bash
git add client/daemon/daemon.cpp client/mozilla/controllerimpl.h client/mozilla/localsocketcontroller.cpp \
        client/core/protocols/wireGuardProtocol.h client/core/protocols/wireGuardProtocol.cpp
git commit -m "feat(desktop): poll tunnel status every second and report last handshake time

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: iOS / macOS NE 数据源

**Files:**
- Modify: `client/platforms/ios/ios_controller.h:105-107`（新增信号）
- Modify: `client/platforms/ios/ios_controller.mm:381-383`（发出信号）
- Modify: `client/vpnConnection.cpp:46-50`（构造函数接线）

**Interfaces:**
- Consumes: Task 2 的 `VpnConnection::lastHandshakeChanged(qint64)`。
- Produces: `IosController` 信号 `void handshakeTimeChanged(qint64 msSinceEpoch)`。

- [ ] **Step 1: 新增信号**

`client/platforms/ios/ios_controller.h` 的 `signals:` 块中 `bytesChanged` 之后加：

```cpp
    void handshakeTimeChanged(qint64 msSinceEpoch);
```

- [ ] **Step 2: 在状态回调里发出**

`client/platforms/ios/ios_controller.mm` 的 `checkStatus()` 中，`emit bytesChanged(rxBytes - m_rxBytes, txBytes - m_txBytes);`（第 381 行）之前加：

```cpp
            emit handshakeTimeChanged(last_handshake_time_sec > 0
                                              ? static_cast<qint64>(last_handshake_time_sec) * 1000
                                              : 0);
```

- [ ] **Step 3: 接线到 `VpnConnection`**

`client/vpnConnection.cpp` 构造函数中 `#if defined(Q_OS_IOS) || defined(MACOS_NE)` 块内，`bytesChanged` 那行 connect 之后加：

```cpp
    connect(IosController::Instance(), &IosController::handshakeTimeChanged, this, &VpnConnection::lastHandshakeChanged);
```

- [ ] **Step 4: 编译 iOS**

本机已有可用的 iOS 工程配置（`build/ios`，2026-10-08 用 Xcode 生成器、自动签名配置成功；团队、Bundle ID 和自定义 entitlements 见 `CLAUDE.md` 的 iOS 段落）。若 `build/ios` 不存在，先按下面命令配置（把 `<TEAM>`、`<APP_ID>`、`<GROUP_ID>` 和两个 entitlements 路径换成本机的值）：

```bash
cd /Users/zhangwt/cc_project/amnezia-client/amnezia-client
cmake -S . -B build/ios -G Xcode -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/Qt/6.10.1/ios/lib/cmake/Qt6/qt.toolchain.cmake -DQT_HOST_PATH=$HOME/Qt/6.10.1/macos \
  -DBUILD_VPN_DEVELOPMENT_TEAM=<TEAM> -DBUILD_IOS_APP_IDENTIFIER=<APP_ID> -DBUILD_IOS_GROUP_IDENTIFIER=<GROUP_ID> \
  -DCLIENT_IOS_APP_ENTITLEMENTS_PATH=build/ios-branding/app.entitlements \
  -DCLIENT_IOS_NE_ENTITLEMENTS_PATH=build/ios-branding/ne.entitlements
```

然后编译：

```bash
xcodebuild -project build/ios/AmneziaVPN.xcodeproj -scheme AmneziaVPN -configuration Debug -sdk iphoneos \
  -destination generic/platform=iOS -allowProvisioningUpdates build 2>&1 | grep -E "error:|BUILD" | head
```

Expected: `BUILD SUCCEEDED`，产物在 `build/ios/client/Debug-iphoneos/AmneziaVPN.app`。

- [ ] **Step 5: 手动验证（真机）**

连接后在 Xcode 控制台过滤 `IosController::checkStatus`，确认每秒一次；Task 6 完成后在界面上看握手行应在连接后几秒内从 `—` 变成秒数。

- [ ] **Step 6: 提交**

```bash
git add client/platforms/ios/ios_controller.h client/platforms/ios/ios_controller.mm client/vpnConnection.cpp
git commit -m "feat(ios): report last handshake time from tunnel status

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Android 数据源

**Files:**
- Modify: `client/android/protocolApi/src/main/kotlin/Statistics.kt`
- Modify: `client/android/wireguard/src/main/kotlin/org/amnezia/vpn/protocol/wireguard/Wireguard.kt:36-53`
- Modify: `client/android/src/org/amnezia/vpn/AmneziaVpnService.kt:75,103,148,154,384,391,403,413-432`
- Modify: `client/android/src/org/amnezia/vpn/AmneziaActivity.kt:124-128`
- Modify: `client/android/src/org/amnezia/vpn/qt/QtAndroidController.kt:22`
- Modify: `client/platforms/android/android_controller.h:76,109`
- Modify: `client/platforms/android/android_controller.cpp:98,550-556`
- Modify: `client/vpnConnection.cpp:536-543`

**Interfaces:**
- Consumes: Task 2 的 `VpnProtocol::setHandshakeTime(qint64)`、`VpnProtocol::setBytesChanged(quint64 receivedBytes, quint64 sentBytes)`。
- Produces: `Statistics.lastHandshake: Long`（毫秒纪元）；JNI `onStatisticsUpdate(JJJ)V`；`AndroidController::statisticsUpdated(quint64 rxBytes, quint64 txBytes, qint64 lastHandshakeMs)`。

- [ ] **Step 1: `Statistics` 增加握手时间**

把 `client/android/protocolApi/src/main/kotlin/Statistics.kt` 整体改为：

```kotlin
package org.amnezia.vpn.protocol

import android.os.Bundle

private const val RX_BYTES_KEY = "rxBytes"
private const val TX_BYTES_KEY = "txBytes"
private const val LAST_HANDSHAKE_KEY = "lastHandshake"

@Suppress("DataClassPrivateConstructor")
data class Statistics private constructor(
    val rxBytes: Long = 0L,
    val txBytes: Long = 0L,
    val lastHandshake: Long = 0L
) {

    private constructor(builder: Builder) : this(builder.rxBytes, builder.txBytes, builder.lastHandshake)

    @Suppress("SuspiciousEqualsCombination")
    fun isEmpty(): Boolean = this === EMPTY_STATISTICS || this == EMPTY_STATISTICS

    class Builder {
        var rxBytes: Long = 0L
            private set

        var txBytes: Long = 0L
            private set

        var lastHandshake: Long = 0L
            private set

        fun setRxBytes(rxBytes: Long) = apply { this.rxBytes = rxBytes }
        fun setTxBytes(txBytes: Long) = apply { this.txBytes = txBytes }
        fun setLastHandshake(lastHandshake: Long) = apply { this.lastHandshake = lastHandshake }

        fun build(): Statistics =
            if (rxBytes + txBytes == 0L) EMPTY_STATISTICS
            else Statistics(this)
    }

    companion object {
        val EMPTY_STATISTICS: Statistics = Statistics()

        inline fun build(block: Builder.() -> Unit): Statistics = Builder().apply(block).build()
    }
}

fun Bundle.putStatistics(statistics: Statistics) {
    putLong(RX_BYTES_KEY, statistics.rxBytes)
    putLong(TX_BYTES_KEY, statistics.txBytes)
    putLong(LAST_HANDSHAKE_KEY, statistics.lastHandshake)
}

fun Bundle.getStatistics(): Statistics =
    Statistics.build {
        setRxBytes(getLong(RX_BYTES_KEY))
        setTxBytes(getLong(TX_BYTES_KEY))
        setLastHandshake(getLong(LAST_HANDSHAKE_KEY))
    }
```

- [ ] **Step 2: `Wireguard.kt` 解析握手时间**

`client/android/wireguard/src/main/kotlin/org/amnezia/vpn/protocol/wireguard/Wireguard.kt` 的 `statistics` getter（第 36-53 行）改为：

```kotlin
    override val statistics: Statistics
        get() {
            if (tunnelHandle == -1) return Statistics.EMPTY_STATISTICS
            val config = GoBackend.awgGetConfig(tunnelHandle) ?: return Statistics.EMPTY_STATISTICS
            return Statistics.build {
                var optsCount = 0
                config.splitToSequence("\n").forEach { line ->
                    with(line) {
                        when {
                            startsWith("rx_bytes=") -> setRxBytes(substring(9).toLong()).also { ++optsCount }
                            startsWith("tx_bytes=") -> setTxBytes(substring(9).toLong()).also { ++optsCount }
                            startsWith("last_handshake_time_sec=") ->
                                setLastHandshake(substring(24).toLong() * 1000).also { ++optsCount }
                            else -> {}
                        }
                    }
                    if (optsCount == 3) return@forEach
                }
            }
        }
```

（`"last_handshake_time_sec="` 长度为 24。）

- [ ] **Step 3: 服务侧恢复 1 秒统计推送**

`client/android/src/org/amnezia/vpn/AmneziaVpnService.kt`：

第 75 行取消注释：

```kotlin
private const val STATISTICS_SENDING_TIMEOUT = 1000L
```

第 103 行取消注释：

```kotlin
    private var statisticsSendingJob: Job? = null
```

第 148 行取消注释：

```kotlin
                        if (clientName == ACTIVITY_MESSENGER_NAME && isConnected) launchSendingStatistics()
```

第 154 行取消注释：

```kotlin
                            if (it.name == ACTIVITY_MESSENGER_NAME) stopSendingStatistics()
```

第 384 行（`CONNECTED` 分支）取消注释：

```kotlin
                        if (isActivityConnected) launchSendingStatistics()
```

第 391 行（`DISCONNECTED` 分支）和第 403 行（`RECONNECTING` 分支）取消注释：

```kotlin
                        stopSendingStatistics()
```

第 413-432 行被 `/* */` 包裹的两个函数改为（去掉注释包裹，`clientMessenger` 改为 `clientMessengers`，加重复启动保护）：

```kotlin
    @MainThread
    private fun launchSendingStatistics() {
        if (statisticsSendingJob?.isActive == true) return
        if (isServiceBound && isConnected) {
            statisticsSendingJob = mainScope.launch {
                while (true) {
                    clientMessengers.send {
                        ServiceEvent.STATISTICS_UPDATE.packToMessage {
                            putStatistics(protocol?.statistics ?: Statistics.EMPTY_STATISTICS)
                        }
                    }
                    delay(STATISTICS_SENDING_TIMEOUT)
                }
            }
        }
    }

    @MainThread
    private fun stopSendingStatistics() {
        statisticsSendingJob?.cancel()
        statisticsSendingJob = null
    }
```

确认文件顶部已 `import org.amnezia.vpn.protocol.putStatistics` 和 `import org.amnezia.vpn.protocol.Statistics`（取消注释前这两个 import 可能已被 IDE 清理，缺失就补上）。

- [ ] **Step 4: 应用侧转发三个值**

`client/android/src/org/amnezia/vpn/AmneziaActivity.kt` 第 124-128 行改为：

```kotlin
                    ServiceEvent.STATISTICS_UPDATE -> {
                        msg.data?.getStatistics()?.let { (rxBytes, txBytes, lastHandshake) ->
                            QtAndroidController.onStatisticsUpdate(rxBytes, txBytes, lastHandshake)
                        }
                    }
```

`client/android/src/org/amnezia/vpn/qt/QtAndroidController.kt` 第 22 行改为：

```kotlin
    external fun onStatisticsUpdate(rxBytes: Long, txBytes: Long, lastHandshake: Long)
```

- [ ] **Step 5: JNI 与 C++ 侧**

`client/platforms/android/android_controller.h`：

第 76 行改为：

```cpp
    void statisticsUpdated(quint64 rxBytes, quint64 txBytes, qint64 lastHandshakeMs);
```

第 109 行改为：

```cpp
    static void onStatisticsUpdate(JNIEnv *env, jobject thiz, jlong rxBytes, jlong txBytes, jlong lastHandshake);
```

`client/platforms/android/android_controller.cpp`：

第 98 行改为：

```cpp
        {"onStatisticsUpdate", "(JJJ)V", reinterpret_cast<void *>(onStatisticsUpdate)},
```

第 550-556 行改为：

```cpp
void AndroidController::onStatisticsUpdate(JNIEnv *env, jobject thiz, jlong rxBytes, jlong txBytes, jlong lastHandshake)
{
    Q_UNUSED(env);
    Q_UNUSED(thiz);

    emit AndroidController::instance()->statisticsUpdated((quint64) rxBytes, (quint64) txBytes, (qint64) lastHandshake);
}
```

`client/vpnConnection.cpp` 的 `createAndroidConnections()`（第 536-543 行）里第 542 行的 connect 改为：

```cpp
    connect(AndroidController::instance(), &AndroidController::statisticsUpdated, androidVpnProtocol,
            [protocol = androidVpnProtocol](quint64 rxBytes, quint64 txBytes, qint64 lastHandshakeMs) {
                protocol->setBytesChanged(rxBytes, txBytes);
                if (lastHandshakeMs > 0) {
                    protocol->setHandshakeTime(lastHandshakeMs);
                }
            });
```

`androidVpnProtocol` 在第 364 和 531 行被 `m_vpnProtocol.reset(androidVpnProtocol)` 接管，`createProtocolConnections()` 随后把 `handshakeTimeChanged` 转发为 `lastHandshakeChanged`，无需再接线。

- [ ] **Step 6: 编译 Android**

```bash
cd /Users/zhangwt/cc_project/amnezia-client/amnezia-client
deploy/build.sh -t android 2>&1 | grep -E "error:|e: |BUILD|FAILED" | head
```

Expected: 无 Kotlin `e:` 错误、无 C++ `error:`，产物生成。

- [ ] **Step 7: 手动验证（真机）**

安装后连接 AWG，`adb logcat | grep -i "statistics\|AmneziaVpnService"` 应看到服务每秒发送一次 `STATISTICS_UPDATE`；锁屏 1 分钟再解锁，统计不应中断。Task 6 完成后界面累计值在锁屏前后连续。

- [ ] **Step 8: 提交**

```bash
git add client/android/protocolApi/src/main/kotlin/Statistics.kt \
        client/android/wireguard/src/main/kotlin/org/amnezia/vpn/protocol/wireguard/Wireguard.kt \
        client/android/src/org/amnezia/vpn/AmneziaVpnService.kt client/android/src/org/amnezia/vpn/AmneziaActivity.kt \
        client/android/src/org/amnezia/vpn/qt/QtAndroidController.kt \
        client/platforms/android/android_controller.h client/platforms/android/android_controller.cpp \
        client/vpnConnection.cpp
git commit -m "feat(android): send tunnel statistics with last handshake time every second

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: 主页摘要行与详情抽屉（QML）

**Files:**
- Create: `client/ui/qml/Components/ConnectionStatsDrawer.qml`
- Modify: `client/ui/qml/Pages2/PageHome.qml:159-166`（`ConnectButton` 之后插入）
- Modify: `client/ui/qml/qml.qrc:9`（登记新组件）

**Interfaces:**
- Consumes: Task 2 的上下文属性 `ConnectionStatsController`（`summaryText`、`rxTotalText`、`txTotalText`、`rxRateText`、`txRateText`、`lastHandshakeText`、`isHandshakeStale`、`copyReport()`）；现有 `ConnectionController.isConnected`、`ServersUiController.defaultServerName`、`ServersUiController.defaultServerDefaultContainerName`、`PageController.showNotificationMessage(text)`；控件 `DrawerType2`、`Header2Type`、`LabelWithButtonType`、`DividerType`、`BasicButtonType`。
- Produces: 组件 `ConnectionStatsDrawer`（`DrawerType2` 派生，信号 `openTriggered()` / `closeTriggered()` 继承自基类）。

- [ ] **Step 1: 写抽屉组件**

创建 `client/ui/qml/Components/ConnectionStatsDrawer.qml`：

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../Controls2"
import "../Controls2/TextTypes"
import "../Config"

DrawerType2 {
    id: root

    anchors.fill: parent
    expandedHeight: parent.height * 0.9

    expandedStateContent: ColumnLayout {
        id: content

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        Header2Type {
            Layout.fillWidth: true
            Layout.topMargin: 24
            Layout.rightMargin: 16
            Layout.leftMargin: 16
            Layout.bottomMargin: 16

            headerText: qsTr("Connection details")
            descriptionText: qsTr("Live statistics of the current tunnel")
        }

        LabelWithButtonType {
            Layout.fillWidth: true

            text: qsTr("Server")
            descriptionText: ServersUiController.defaultServerName
        }

        DividerType {
        }

        LabelWithButtonType {
            Layout.fillWidth: true

            text: qsTr("Protocol")
            descriptionText: ServersUiController.defaultServerDefaultContainerName
        }

        DividerType {
        }

        LabelWithButtonType {
            Layout.fillWidth: true

            text: qsTr("Last handshake")
            descriptionText: ConnectionStatsController.lastHandshakeText
            descriptionColor: ConnectionStatsController.isHandshakeStale ? AmneziaStyle.color.goldenApricot
                                                                         : AmneziaStyle.color.mutedGray
        }

        DividerType {
        }

        LabelWithButtonType {
            Layout.fillWidth: true

            text: qsTr("This session")
            descriptionText: qsTr("Received %1 · Sent %2").arg(ConnectionStatsController.rxTotalText)
                                                           .arg(ConnectionStatsController.txTotalText)
        }

        DividerType {
        }

        LabelWithButtonType {
            Layout.fillWidth: true

            text: qsTr("Current speed")
            descriptionText: "↓ " + ConnectionStatsController.rxRateText + "  ↑ " + ConnectionStatsController.txRateText
        }

        DividerType {
        }

        BasicButtonType {
            id: copyButton

            Layout.fillWidth: true
            Layout.topMargin: 24
            Layout.bottomMargin: 24
            Layout.leftMargin: 16
            Layout.rightMargin: 16

            text: qsTr("Copy diagnostics")

            Keys.onEnterPressed: copyButton.clicked()
            Keys.onReturnPressed: copyButton.clicked()

            clickedFunc: function() {
                ConnectionStatsController.copyReport()
                PageController.showNotificationMessage(qsTr("Copied"))
            }
        }
    }
}
```

规范第 5.2 节里"隧道内 IP"这一行只有桌面能拿到值（来自守护进程 status），iOS 和 Android 始终是 `—`。为避免三端里两端常年显示 `—`，隧道 IP 不单独成行，只保留在复制文本中。这是对规范的有意调整。

- [ ] **Step 2: 登记到 qrc**

`client/ui/qml/qml.qrc` 第 9 行 `<file>Components/HomeSplitTunnelingDrawer.qml</file>` 之前加：

```xml
        <file>Components/ConnectionStatsDrawer.qml</file>
```

- [ ] **Step 3: 主页加摘要行**

`client/ui/qml/Pages2/PageHome.qml` 中 `ConnectButton { ... }` 块（第 159-166 行）之后、`splitTunnelingButton` 之前插入：

```qml
            BasicButtonType {
                id: connectionStatsButton
                objectName: "connectionStatsButton"

                Layout.alignment: Qt.AlignHCenter
                leftPadding: 16
                rightPadding: 16

                implicitHeight: 36

                defaultColor: AmneziaStyle.color.transparent
                hoveredColor: AmneziaStyle.color.translucentWhite
                pressedColor: AmneziaStyle.color.sheerWhite
                disabledColor: AmneziaStyle.color.mutedGray
                textColor: ConnectionStatsController.isHandshakeStale ? AmneziaStyle.color.goldenApricot
                                                                      : AmneziaStyle.color.mutedGray
                borderWidth: 0

                buttonTextLabel.lineHeight: 20
                buttonTextLabel.font.pixelSize: 14
                buttonTextLabel.font.weight: 500

                visible: ConnectionController.isConnected
                text: ConnectionStatsController.summaryText
                rightImageSource: "qrc:/images/controls/chevron-down.svg"

                Keys.onEnterPressed: this.clicked()
                Keys.onReturnPressed: this.clicked()

                onClicked: {
                    connectionStatsDrawer.openTriggered()
                }

                ConnectionStatsDrawer {
                    id: connectionStatsDrawer
                    objectName: "connectionStatsDrawer"

                    parent: root
                }
            }
```

- [ ] **Step 4: 编译并做 QML 静态检查**

```bash
cd /Users/zhangwt/cc_project/amnezia-client/amnezia-client
cmake --build deploy/build 2>&1 | grep -E "error|Linking CXX executable" | head
QT_DIR=$(ls -d ~/Qt/6.*/macos 2>/dev/null | tail -1); "$QT_DIR/bin/qmllint" -I client/ui/qml client/ui/qml/Components/ConnectionStatsDrawer.qml 2>&1 | grep -v "unqualified\|Warning: " | head
```

Expected: 构建无 `error`；qmllint 没有语法错误（关于上下文属性未限定的 warning 与仓库其他文件一致，可忽略）。

- [ ] **Step 5: 手动验证（macOS 桌面）**

1. 启动应用，未连接时主页没有摘要行。
2. 连接 AWG，几秒内出现 `↓ … ↑ … · handshake N s ago`，数值每秒变化。
3. 点开抽屉，六行信息齐全；点"Copy diagnostics"后弹"Copied"，粘贴到文本编辑器得到九行文本，`session rx bytes` 为原始整数。
4. 与 Task 3 Step 7 的脚本输出对比，`rx_bytes` 与 `session rx bytes` 差距应在一秒流量以内。
5. 切到一台 OpenVPN 或 Xray 服务器连接，摘要行不显示，控制台无 QML 错误。
6. 断开，摘要行消失；再连接，累计从 0 开始。

- [ ] **Step 6: 提交**

```bash
git add client/ui/qml/Components/ConnectionStatsDrawer.qml client/ui/qml/Pages2/PageHome.qml client/ui/qml/qml.qrc
git commit -m "feat(ui): show live connection stats on home page with details drawer

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: 中文译文

**Files:**
- Modify: `client/translations/amneziavpn_zh_CN.ts`

**Interfaces:**
- Consumes: Task 2 与 Task 6 引入的全部 `tr()` / `qsTr()` 字符串。

- [ ] **Step 1: 用 lupdate 收集新字符串**

```bash
cd /Users/zhangwt/cc_project/amnezia-client/amnezia-client
QT_DIR=$(ls -d ~/Qt/6.*/macos 2>/dev/null | tail -1)
"$QT_DIR/bin/lupdate" -no-obsolete -locations none \
    client/ui/controllers/connectionStatsUiController.cpp \
    client/ui/qml/Components/ConnectionStatsDrawer.qml \
    -ts client/translations/amneziavpn_zh_CN.ts 2>&1 | tail -3
git diff --stat client/translations/amneziavpn_zh_CN.ts
```

Expected: 输出类似 `Found N source text(s)`，diff 只新增 `ConnectionStatsUiController` 和 `ConnectionStatsDrawer` 两个 `<context>`。如果 diff 删除了其他上下文的条目，放弃改动（`git checkout client/translations/amneziavpn_zh_CN.ts`），改为按 Step 2 手动追加。

- [ ] **Step 2: 填写译文**

在 `amneziavpn_zh_CN.ts` 中把以下 `<translation type="unfinished"></translation>` 填为对应译文（若 Step 1 失败，则在根元素 `</TS>` 之前手动追加这两个 context 块）：

```xml
<context>
    <name>ConnectionStatsUiController</name>
    <message>
        <source>%1 s ago</source>
        <translation>%1 秒前</translation>
    </message>
    <message>
        <source>%1 min %2 s ago</source>
        <translation>%1 分 %2 秒前</translation>
    </message>
    <message>
        <source>handshake</source>
        <translation>握手</translation>
    </message>
    <message>
        <source> (connection may be dead)</source>
        <translation>（可能已断）</translation>
    </message>
</context>
<context>
    <name>ConnectionStatsDrawer</name>
    <message>
        <source>Connection details</source>
        <translation>连接详情</translation>
    </message>
    <message>
        <source>Live statistics of the current tunnel</source>
        <translation>当前隧道的实时统计</translation>
    </message>
    <message>
        <source>Server</source>
        <translation>服务器</translation>
    </message>
    <message>
        <source>Protocol</source>
        <translation>协议</translation>
    </message>
    <message>
        <source>Last handshake</source>
        <translation>最近握手</translation>
    </message>
    <message>
        <source>This session</source>
        <translation>本次会话</translation>
    </message>
    <message>
        <source>Received %1 · Sent %2</source>
        <translation>接收 %1 · 发送 %2</translation>
    </message>
    <message>
        <source>Current speed</source>
        <translation>当前速率</translation>
    </message>
    <message>
        <source>Copy diagnostics</source>
        <translation>复制诊断信息</translation>
    </message>
    <message>
        <source>Copied</source>
        <translation>已复制</translation>
    </message>
</context>
```

- [ ] **Step 3: 编译并在中文界面下检查**

```bash
cd /Users/zhangwt/cc_project/amnezia-client/amnezia-client
cmake --build deploy/build 2>&1 | grep -E "error|lrelease" | head
```

启动应用，语言为简体中文时主页摘要行显示 `↓ … ↑ … · 握手 43 秒前`，抽屉各行标题为中文。

- [ ] **Step 4: 提交**

```bash
git add client/translations/amneziavpn_zh_CN.ts
git commit -m "i18n: add zh_CN strings for connection stats panel

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## 完成标准

- `ctest -R ConnectionStatsTest` 通过。
- macOS 桌面、iOS、Android 三端连接 AWG 后主页出现摘要行，抽屉数值每秒刷新，复制文本可与服务器 `awg show awg0 transfer` 对照。
- OpenVPN / Xray 连接下摘要行隐藏且无 QML 错误。
- 日志开启时不因状态轮询每秒增长。
