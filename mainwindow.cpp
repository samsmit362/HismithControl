#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <windows.h>
#include <thread>
#include <QPushButton>
#include <QLineEdit>
#include <condition_variable>
#include <mutex>

#define HOTKEY_PAUSE_ID     1
#define HOTKEY_RESUME_ID    2
#define HOTKEY_STOP_ID      3

void Controller::handleResults()
{
    g_stop_run = false;
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

    trayIcon = new QSystemTrayIcon(this);
    trayIcon->setContextMenu(trayIconMenu);

    QIcon icon(":/images/icon.png");
    trayIcon->setIcon(icon);
    setWindowIcon(icon);

    trayIcon->setToolTip("Hismith Control With Funscript");
    setWindowTitle("Hismith Control With Funscript");
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

void MainWindow::handleTestCameraButton()
{
    test_camera();
}

void MainWindow::handleStopStart()
{
    std::lock_guard lk(g_stop_mutex);
    show_msg("Stop pressed", 1000);
    g_stop_run = true;
    g_stop_cvar.notify_all();
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

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_HOTKEY) {
        WPARAM wp = msg->wParam;
        
        {
            std::lock_guard lk(g_stop_mutex);
            if (wp == HOTKEY_PAUSE_ID)
            {
                show_msg("Pause pressed", 1000);
                g_pause = true;
            }
            else if (wp == HOTKEY_RESUME_ID)
            {
                show_msg("Resume pressed", 1000);
                g_pause = false;
            }
            else if (wp == HOTKEY_STOP_ID)
            {
                show_msg("Stop pressed", 1000);
                g_stop_run = true;
            }
            g_stop_cvar.notify_all();
        }
        
        return(true);

    }

    return(false);
}
