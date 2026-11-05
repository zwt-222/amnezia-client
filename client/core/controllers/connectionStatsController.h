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
    bool hasHandshake() const;
    QString tunnelAddress() const;

    static QString formatBytes(quint64 bytes);
    static QString formatRate(double bytesPerSec);

public slots:
    void onBytesChanged(quint64 rxDelta, quint64 txDelta);
    void onHandshakeTimeChanged(qint64 msSinceEpoch);
    void onConnectionStateChanged(Vpn::ConnectionState state);
    void onTunnelAddressChanged(const QString &localAddress);

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
    QString m_tunnelAddress;
    Vpn::ConnectionState m_lastState = Vpn::ConnectionState::Unknown;
};

#endif // CONNECTIONSTATSCONTROLLER_H
