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
#include <QDir>
#include <QFileDialog>
#include <QChart>
#include <QChartView>
#include <QLineSeries>

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
struct speeds_data;

//---------------------------------------------------------------

extern bool g_pause;
extern bool g_stop_run;
extern bool g_was_change_in_use_modify_funscript_functions;

extern std::mutex g_stop_mutex;
extern std::mutex g_change_in_use_modify_funscript_functions_mutex;
extern std::condition_variable g_stop_cvar;

extern int g_max_allowed_hismith_speed;
extern int g_min_funscript_relative_move;

extern bool g_modify_funscript;
extern QString g_modify_funscript_function_move_variants;
extern QString g_modify_funscript_function_move_in_out_variants;
extern int g_functions_move_in_out_variant;

extern QString g_req_webcam_name;
extern QString g_hismith_device_name;
extern std::vector<DeviceClass> g_myDevices;

extern bool g_work_in_progress;

extern QString g_hotkey_stop;
extern QString g_hotkey_pause;
extern QString g_hotkey_resume;
extern QString g_hotkey_use_modify_funscript_functions;

//---------------------------------------------------------------

void show_msg(QString msg, int timeout = 5000, bool always = false, bool drow_modify_funscript_functions = false);
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
void get_statistics_with_hismith();
bool get_parsed_funscript_data(QString funscript_fname, std::vector<QPair<int, int>>& funscript_data_maped, speeds_data& all_speeds_data, QString* p_res_details = NULL);
bool get_speed_statistics_data(speeds_data& all_speeds_data);

//---------------------------------------------------------------

struct statistics_data
{
    int dpos;
    int dt_video;
    int dt_gtc;
    int avg_cur_speed;
};

struct speed_data
{
    int total_average_speed;
    double average_rate_of_change_of_speed;
    int time_delay;
    std::vector<statistics_data> speed_statistics_data;
};

struct speeds_data
{
    std::vector<speed_data> speed_data_vector;
    double min_average_rate_of_change_of_speed;
    double max_average_rate_of_change_of_speed;

    speeds_data() : speed_data_vector(100) {}
};

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

    bool eventFilter(QObject* obj, QEvent* event);
    void RegisterHotKeys();

private slots:
    void handleStartButton();
    void handleTestButton();
    void handleGetPerformance();
    void handleGetStatistics();
    void handleTestCameraButton();
    void handleStopStart();
    void handlePauseStart();
    void handleResumeStart();
    void handleUseModifyFunscriptFunctions();
    void handleTrayExit();
    void handleRefreshDevicesButton();
    void handleSaveSettings();
    void handleExit();
    void handleSpeedLimitChanged();
    void handleMinRelativeMoveChanged();
    void handleOpenFunscript();
    void handleCheckFunscript();
    void handleModifyFunscriptChanged();
    void handleFunctionsMoveVariantsChanged(const QString& str);
    void handleFunctionsMoveVariantsContextMenuRequested(QPoint pos);
    void handleFunctionsMoveVariantsEditingFinished();
    void handleFunctionsMoveInOutVariantsChanged(const QString& str);
    void handleFunctionsMoveInOutVariantsContextMenuRequested(QPoint pos);
    void handleFunctionsMoveInOutVariantsEditingFinished();

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result);

public:
    QString funscript;
    Ui::MainWindow *ui;
    QAction* stopStartAction;
    QAction* pauseStartAction;
    QAction* resumeStartAction;
    QAction* useModifyFunscriptFunctionsAction;
    QAction* exitAction;
    QMenu* trayIconMenu;
    QChart* chart;
};
#endif // MAINWINDOW_H
