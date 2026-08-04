#pragma once

#include <QObject>
#include <QThread>
#include <QTcpSocket>
#include <QMutex>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTimer>
#include <QEventLoop>
#include <windows.h>

struct PlayerData {
    QString videoFilePath = "";
    double videoPos = 0.0;
    double videoRate = 1.0;
    bool isPaused = true;
    LARGE_INTEGER cur_time;
    bool gotData = false;
};

class HereSphereSync : public QObject {
    Q_OBJECT
public:
    explicit HereSphereSync(QObject* parent = nullptr);
    ~HereSphereSync();

    void start(const QString& host = "127.0.0.1", quint16 port = 23554);
    void stop();
    void getLatestData(PlayerData& data);

    void seekToPosition(double seconds);
    void setPlaying(bool play);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onReconnectTimer();
    void onErrorOccurred(QAbstractSocket::SocketError error);

private:
    QThread* m_netThread = nullptr;

    QTcpSocket* m_socket = nullptr;
    QTimer* m_reconnectTimer = nullptr;
    QByteArray m_buffer;

    QMutex m_mutex;
    PlayerData m_currentData;

    QString m_host;
    quint16 m_port;
    bool m_isRunning = false;

    void sendJsonCommand(const QJsonObject& json);
};
