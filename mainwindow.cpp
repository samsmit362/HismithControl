#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <windows.h>
#include <thread>
#include <condition_variable>
#include <mutex>

#define HOTKEY_PAUSE_ID     1
#define HOTKEY_RESUME_ID    2
#define HOTKEY_STOP_ID      3

void Controller::handleResults()
{
    g_stop_run = false;
    g_work_in_progress = false;
    p_parent->show();
    p_parent->trayIcon->hide();
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ctrl(this)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    bool result = RegisterHotKey(
        (HWND)this->window()->winId(),
        HOTKEY_PAUSE_ID,
        MOD_ALT | MOD_NOREPEAT,
        Qt::Key_B);


    result &= RegisterHotKey(
        (HWND)this->window()->winId(),
        HOTKEY_RESUME_ID,
        MOD_ALT | MOD_NOREPEAT,
        Qt::Key_N);

    result &= RegisterHotKey(
        (HWND)this->window()->winId(),
        HOTKEY_STOP_ID,
        MOD_ALT | MOD_NOREPEAT,
        Qt::Key_Q);

    connect(ui->startButton, &QPushButton::released, this, &MainWindow::handleStartButton);
    connect(ui->testButton, &QPushButton::released, this, &MainWindow::handleTestButton);
    connect(ui->testCameraButton, &QPushButton::released, this, &MainWindow::handleTestCameraButton);
    connect(ui->refreshDevices, &QPushButton::released, this, &MainWindow::handleRefreshDevicesButton);
    connect(ui->getPerformance, &QPushButton::released, this, &MainWindow::handleGetPerformance);
    connect(ui->getStatistics, &QPushButton::released, this, &MainWindow::handleGetStatistics);

    connect(ui->actionSave_Settings, &QAction::triggered, this, &MainWindow::handleSaveSettings);
    connect(ui->actionExit, &QAction::triggered, this, &MainWindow::handleExit);

    connect(ui->speedLimit, SIGNAL(textChanged(const QString&)), this, SLOT(handleSpeedLimitChanged()));
    connect(ui->minRelativeMove, SIGNAL(textChanged(const QString&)), this, SLOT(handleMinRelativeMoveChanged()));

    //-------------------

    trayIconMenu = new QMenu(this);

    stopStartAction = new QAction(tr("Stop Run\tAlt+Q"), this);
    connect(stopStartAction, &QAction::triggered, this, &MainWindow::handleStopStart);
    trayIconMenu->addAction(stopStartAction);

    pauseStartAction = new QAction(tr("Pause Run\tAlt+B"), this);
    connect(pauseStartAction, &QAction::triggered, this, &MainWindow::handlePauseStart);
    trayIconMenu->addAction(pauseStartAction);

    resumeStartAction = new QAction(tr("Resume Run\tAlt+N"), this);
    connect(resumeStartAction, &QAction::triggered, this, &MainWindow::handleResumeStart);
    trayIconMenu->addAction(resumeStartAction);

    exitAction = new QAction(tr("Exit"), this);
    connect(exitAction, &QAction::triggered, this, &MainWindow::handleTrayExit);
    trayIconMenu->addAction(exitAction);

    trayIcon = new QSystemTrayIcon(this);
    trayIcon->setContextMenu(trayIconMenu);

    QIcon icon(":/images/icon.png");
    trayIcon->setIcon(icon);
    setWindowIcon(icon);

    trayIcon->setToolTip("Hismith Control With Funscript");
    setWindowTitle("Hismith Control With Funscript");

    QIcon iconOpenFunscript(":/images/folder.png");
    ui->openFunscript->setIcon(iconOpenFunscript);
    connect(ui->openFunscript, &QToolButton::released, this, &MainWindow::handleOpenFunscript);

    connect(ui->checkFunscript, &QPushButton::released, this, &MainWindow::handleCheckFunscript);
}

void MainWindow::handleOpenFunscript()
{
    funscript = QDir::toNativeSeparators(QFileDialog::getOpenFileName(this,
        tr("Open Funscript"), "", tr("Funscripts (*.funscript)")));
    ui->funscriptPathEdit->setText(funscript);
}

void MainWindow::handleCheckFunscript()
{
    std::vector<QPair<int, int>> funscript_data_maped_full;
    QString result_details;

    if (funscript.size() > 0)
    {
        if (get_parsed_funscript_data(funscript, funscript_data_maped_full, &result_details))
        {
            if (result_details.size() > 0)
            {
                result_details = QString("File path: ") + funscript + "\n----------\n" + result_details + "\n----------\n";                
            }
            else
            {
                result_details = QString("File path: ") + funscript + "\n----------\nLoaded successfully without changes\n----------\n";
            }

            QMessageBox msgBox;
            msgBox.setText(result_details);
            msgBox.exec();
        }
    }
}

MainWindow::~MainWindow()
{
    trayIcon->hide();
    UnregisterHotKey((HWND)this->window()->winId(), HOTKEY_PAUSE_ID);
    delete ui;
}

void MainWindow::handleStartButton()
{
    hide();
    trayIcon->show();
    emit ctrl.operate();
}

void MainWindow::handleTestButton()
{
    test_hismith(ui->testSpeed->text().toInt());
}

void MainWindow::handleGetPerformance()
{
    get_performance_with_hismith(ui->testSpeed->text().toInt());
}

void MainWindow::handleGetStatistics()
{
    get_statistics_with_hismith();
}

void MainWindow::handleTestCameraButton()
{
    test_camera();
}

void MainWindow::handleSaveSettings()
{
    SaveSettings();
}

void MainWindow::handleExit()
{
    QApplication::quit();
}

void MainWindow::handleSpeedLimitChanged()
{
    g_max_allowed_hismith_speed = ui->speedLimit->text().toInt();
}

void MainWindow::handleMinRelativeMoveChanged()
{
    g_min_funscript_relative_move = ui->minRelativeMove->text().toInt();
}

void MainWindow::handleRefreshDevicesButton()
{
    ui->Webcams->clear();
    DeviceEnumerator de;
    std::map<int, InputDevice> devices = de.getVideoDevicesMap();
    for (auto const& device : devices) {
        ui->Webcams->addItem(device.second.deviceName.c_str());
        if (QString(device.second.deviceName.c_str()).contains(g_req_webcam_name))
        {
            ui->Webcams->setCurrentIndex(ui->Webcams->count() - 1);
        }
    }

    //--------------------

    ui->Devices->clear();
    get_devices_list();
    for (DeviceClass& dev : g_myDevices)
    {
        ui->Devices->addItem(dev.deviceName.c_str());
        if (QString(dev.deviceName.c_str()).contains(g_hismith_device_name))
        {
            ui->Devices->setCurrentIndex(ui->Devices->count() - 1);
        }
    }
}

void MainWindow::handleStopStart()
{
    if (g_work_in_progress)
    {
        std::lock_guard lk(g_stop_mutex);
        show_msg("Stop pressed", 1000);
        g_stop_run = true;
        g_stop_cvar.notify_all();
    }
}

void MainWindow::handlePauseStart()
{
    std::lock_guard lk(g_stop_mutex);
    show_msg("Pause pressed", 1000);
    g_pause = true;
    g_stop_cvar.notify_all();
}

void MainWindow::handleResumeStart()
{
    std::lock_guard lk(g_stop_mutex);
    show_msg("Resume pressed", 1000);
    g_pause = false;
    g_stop_cvar.notify_all();
}

void MainWindow::handleTrayExit()
{
    handleStopStart();
    handleExit();
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_HOTKEY) {
        WPARAM wp = msg->wParam;
        
        {
            if (wp == HOTKEY_PAUSE_ID)
            {
                handlePauseStart();
            }
            else if (wp == HOTKEY_RESUME_ID)
            {
                handleResumeStart();
            }
            else if (wp == HOTKEY_STOP_ID)
            {
                handleStopStart();
            }
        }
        
        return(true);

    }

    return(false);
}
