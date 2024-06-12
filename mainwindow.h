#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QSystemTrayIcon>
#include <QFileInfo>
#include <QTimer>
#include <QRegularExpression>
#include <QFile>
#include <QDomDocument>
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QLineEdit>

#include <iostream>
#include <map>
#include <ppl.h>

#include <opencv2/core.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs/legacy/constants_c.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>

#include "3rdParty/buttplugCpp/include/buttplugclient.h"
#include "3rdParty/OpenCVDeviceEnumerator/DeviceEnumerator.h"
#include "DataTypes.h"
#include "MyClosedFigure.h"

class MainWindow;

//---------------------------------------------------------------

extern bool g_pause;
extern bool g_stop_run;

extern std::mutex g_stop_mutex;
extern std::condition_variable g_stop_cvar;

extern int g_max_allowed_hismith_speed;
extern int g_min_funscript_relative_move;

extern QString g_req_webcam_name;
extern QString g_hismith_device_name;
extern std::vector<DeviceClass> g_myDevices;

extern bool g_work_in_progress;

//---------------------------------------------------------------

void show_msg(QString msg, int timeout = 5000);
void run_funscript();
void test_hismith(int hismith_speed);
void get_performance_with_hismith(int hismith_speed);
void test_camera();
void disconnect_from_hismith();
bool connect_to_hismith();
void error_msg(QString msg, cv::Mat* p_frame = NULL, cv::Mat* p_frame_upd = NULL, cv::Mat* p_prev_frame = NULL, int x1 = -1, int y1 = -1, int x2 = -1, int y2 = -1);
int make_vlc_status_request(QNetworkAccessManager* manager, QNetworkRequest& req, bool& is_paused, QString& video_filename);
QByteArray get_vlc_reply(QNetworkAccessManager* manager, QNetworkRequest& req, QString ReqUrl);
bool get_devices_list();
void SaveSettings();

//---------------------------------------------------------------

class Worker : public QObject
{
    Q_OBJECT

public slots:
    void doWork() {
        g_work_in_progress = true;
        run_funscript();
        emit resultReady();
    }

signals:
    void resultReady();
};

class Controller : public QObject
{
    Q_OBJECT
        QThread workerThread;
        MainWindow* p_parent;
public:
    Controller(MainWindow *parent) : p_parent(parent) {
        Worker* worker = new Worker;
        worker->moveToThread(&workerThread);
        connect(&workerThread, &QThread::finished, worker, &QObject::deleteLater);
        connect(this, &Controller::operate, worker, &Worker::doWork);
        connect(worker, &Worker::resultReady, this, &Controller::handleResults);
        workerThread.start();
    }
    ~Controller() {
        workerThread.quit();
        workerThread.wait();
    }

public slots:
    void handleResults();
signals:
    void operate();
};

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();    
    
    Controller ctrl;
    QSystemTrayIcon* trayIcon;

private slots:
    void handleStartButton();
    void handleTestButton();
    void handleGetPerformance();
    void handleTestCameraButton();
    void handleStopStart();
    void handlePauseStart();
    void handleResumeStart();
    void handleRefreshDevicesButton();
    void handleSaveSettings();
    void handleSpeedLimitChanged();
    void handleMinRelativeMoveChanged();

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result);

public:
    Ui::MainWindow *ui;
    QAction* stopStartAction;
    QAction* pauseStartAction;
    QAction* resumeStartAction;
    QMenu* trayIconMenu;    
};
#endif // MAINWINDOW_H
