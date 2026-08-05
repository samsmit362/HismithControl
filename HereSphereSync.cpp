#include "herespheresync.h"
#include <QDataStream>
#include <QDebug>

HereSphereSync::HereSphereSync(QObject* parent) : QObject(parent) {}

HereSphereSync::~HereSphereSync() {
    stop();
}

void HereSphereSync::start(const QString& host, quint16 port) {
    stop();

    m_host = host;
    m_port = port;
    m_isRunning = true;

    m_netThread = QThread::create([this]() {
        //SetThreadAffinityMask(GetCurrentThread(), 0x02);

        m_socket = new QTcpSocket();
        m_reconnectTimer = new QTimer();
        m_reconnectTimer->setInterval(2000);

        this->moveToThread(QThread::currentThread());

        connect(m_socket, &QTcpSocket::connected, this, &HereSphereSync::onConnected, Qt::DirectConnection);
        connect(m_socket, &QTcpSocket::disconnected, this, &HereSphereSync::onDisconnected, Qt::DirectConnection);
        connect(m_socket, &QTcpSocket::readyRead, this, &HereSphereSync::onReadyRead, Qt::DirectConnection);
        connect(m_socket, &QTcpSocket::errorOccurred, this, &HereSphereSync::onErrorOccurred, Qt::DirectConnection);
        connect(m_reconnectTimer, &QTimer::timeout, this, &HereSphereSync::onReconnectTimer, Qt::DirectConnection);

        m_reconnectTimer->start();
        onReconnectTimer();

        QEventLoop loop;
        connect(m_netThread, &QThread::finished, &loop, &QEventLoop::quit);
        loop.exec();

        this->moveToThread(nullptr);

        m_reconnectTimer->stop();
        delete m_reconnectTimer;
        m_reconnectTimer = nullptr;

        if (m_socket->state() != QAbstractSocket::UnconnectedState) {
            m_socket->disconnectFromHost();
        }
        delete m_socket;
        m_socket = nullptr;
        });

    m_netThread->start();
}

void HereSphereSync::stop() {
    if (!m_isRunning) return;
    m_isRunning = false;

    if (m_netThread && m_netThread->isRunning()) {
        m_netThread->quit();
        m_netThread->wait();
        delete m_netThread;
        m_netThread = nullptr;
    }

    QMutexLocker locker(&m_mutex);
    m_currentData.gotData = false;
}

void HereSphereSync::onReconnectTimer() {
    if (!m_isRunning || !m_socket) return;
    if (m_socket->state() == QAbstractSocket::UnconnectedState) {

        QString cleanHost = m_host.trimmed();

        QHostAddress address;

        if (cleanHost.toLower() == "localhost") {
            address = QHostAddress::LocalHost;
        }
        else {
            address.setAddress(cleanHost);
        }

        if (address.isNull()) {
            address = QHostAddress::LocalHost;
        }

        m_socket->connectToHost(address, m_port);
    }
}


void HereSphereSync::onConnected() {
    m_buffer.clear();

    QJsonObject hello;
    sendJsonCommand(hello);
}

void HereSphereSync::onDisconnected() {
    QMutexLocker locker(&m_mutex);
    m_currentData.gotData = false;
}

void HereSphereSync::onErrorOccurred(QAbstractSocket::SocketError error) {
    if (m_socket) {
        qDebug() << "[HereSphere Network Error] Soccet issue:" << m_socket->errorString() << " SocketError:" << error;
    }
}

void HereSphereSync::onReadyRead() {
    static PlayerData local_currentData;
    if (!m_socket) return;

    LARGE_INTEGER cur_time;
    QueryPerformanceCounter(&cur_time);

    m_buffer.append(m_socket->readAll());

    while (m_buffer.size() >= 4) {
        quint32 packetLength = 0;
        QDataStream stream(m_buffer.left(4));
        stream.setByteOrder(QDataStream::LittleEndian);
        stream >> packetLength;

        if (m_buffer.size() < static_cast<int>(4 + packetLength)) {
            break;
        }

        QByteArray jsonBytes = m_buffer.mid(4, packetLength);
        m_buffer.remove(0, 4 + packetLength);

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject json = doc.object();

            // qDebug() << QJsonDocument(json).toJson(QJsonDocument::Indented).constData();
            /*{
                "currentTime": 98.53166961669922,
                "duration" : 1035.690673828125,
                "identifier" : "%file_name%",
                "path" : "%file_path%",
                "playbackSpeed" : 1,
                "playerState" : 1 (paused),
                "resource" : "file://%file_path%"
            }*/

            if (json.contains("currentTime"))   local_currentData.videoPos = json["currentTime"].toDouble();
            if (json.contains("playerState"))   local_currentData.isPaused = (json["playerState"].toInt() == 1);
            if (json.contains("path"))          local_currentData.videoFilePath = json["path"].toString();
            if (json.contains("playbackSpeed")) local_currentData.videoRate = json["playbackSpeed"].toDouble();

            local_currentData.cur_time = cur_time;
            local_currentData.gotData = true;

            {
                QMutexLocker locker(&m_mutex);
                m_currentData = local_currentData;
            }
        }
    }
}

void HereSphereSync::sendJsonCommand(const QJsonObject& json) {
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) return;

    QByteArray jsonBytes = QJsonDocument(json).toJson(QJsonDocument::Compact);
    quint32 length = jsonBytes.size();

    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << length;
    packet.append(jsonBytes);

    m_socket->write(packet);
    m_socket->flush();
}

void HereSphereSync::getLatestData(PlayerData& data) {
    QMutexLocker locker(&m_mutex);
    data = m_currentData;
}

void HereSphereSync::seekToPosition(double seconds) { sendJsonCommand(QJsonObject{ {"currentTime", seconds} }); }
void HereSphereSync::setPlaying(bool play) { sendJsonCommand(QJsonObject{ {"playerState", play ? 0 : 1} }); }
