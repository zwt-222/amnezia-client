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
