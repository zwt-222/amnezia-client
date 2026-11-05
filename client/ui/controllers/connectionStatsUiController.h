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
