#include "connectionStatsUiController.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QStringList>

#include "core/utils/containers/containerUtils.h"

ConnectionStatsUiController::ConnectionStatsUiController(ConnectionStatsController *statsController,
                                                         ServersController *serversController, QObject *parent)
    : QObject(parent), m_stats(statsController), m_serversController(serversController)
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

bool ConnectionStatsUiController::hasStats() const
{
    return m_stats->hasHandshake();
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
    const QString address = m_stats->tunnelAddress();
    return address.isEmpty() ? QStringLiteral("—") : address;
}

QString ConnectionStatsUiController::buildReport() const
{
    const ServerDescription server = defaultServerDescription();
    const QString protocolName = ContainerUtils::containerHumanNames().value(server.defaultContainer);

    const QDateTime now = QDateTime::currentDateTime();
    const QString nowText = now.toOffsetFromUtc(now.offsetFromUtc()).toString(Qt::ISODate);

    const qint64 handshakeMs = m_stats->lastHandshakeMs();
    QString handshakeLine = QStringLiteral("—");
    if (handshakeMs > 0) {
        const QDateTime handshake = QDateTime::fromMSecsSinceEpoch(handshakeMs);
        const qint64 ageSec = qMax<qint64>(0, (QDateTime::currentMSecsSinceEpoch() - handshakeMs) / 1000);
        handshakeLine = QStringLiteral("%1 (%2 s ago)")
                                .arg(handshake.toOffsetFromUtc(handshake.offsetFromUtc()).toString(Qt::ISODate))
                                .arg(ageSec);
    }

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
