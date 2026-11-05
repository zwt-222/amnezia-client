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
