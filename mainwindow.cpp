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

    connect(ui->modifyFunscript, SIGNAL(stateChanged(int)), this, SLOT(handleModifyFunscriptChanged()));
    connect(ui->functionsMoveVariants, SIGNAL(editTextChanged(const QString&)), this, SLOT(handleFunctionsMoveVariantsChanged(const QString&)));
    connect(ui->functionsMoveVariants, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(handleFunctionsMoveVariantsContextMenuRequested(QPoint)));
    connect(ui->functionsMoveVariants->lineEdit(), SIGNAL(editingFinished()), SLOT(handleFunctionsMoveVariantsEditingFinished()));
    ui->functionsMoveVariants->installEventFilter(this);

    chart = new QChart();
    chart->legend()->hide();    
    chart->setTitle("Modify Funscript Function");
    QChartView* chartView = new QChartView(chart);
    chartView->setRenderHint(QPainter::Antialiasing);
    QVBoxLayout* layout = new QVBoxLayout(ui->chartFrame);
    layout->addWidget(chartView);

    connect(ui->functionsMoveIn, SIGNAL(textChanged(const QString&)), this, SLOT(handleFunctionsMoveInChanged()));
    connect(ui->functionsMoveOut, SIGNAL(textChanged(const QString&)), this, SLOT(handleFunctionsMoveOutChanged()));

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

    funscript = ui->funscriptPathEdit->text();

    if (funscript.size() > 0)
    {
        speeds_data all_speeds_data;
        get_speed_statistics_data(all_speeds_data);

        if (get_parsed_funscript_data(funscript, funscript_data_maped_full, all_speeds_data, &result_details))
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

void MainWindow::handleModifyFunscriptChanged()
{
    g_modify_funscript = ui->modifyFunscript->isChecked();
}

void MainWindow::handleFunctionsMoveVariantsContextMenuRequested(QPoint pos)
{
    QMenu menu;
    menu.addAction(QString("delete current item"));
    menu.addAction(QString("add item before current item"));
    menu.addAction(QString("add item after current item"));
    QAction *pAction = menu.exec(ui->functionsMoveVariants->mapToGlobal(pos));

    if (pAction)
    {
        if (pAction->text() == QString("delete current item"))
        {
            int cur_id = ui->functionsMoveVariants->currentIndex();

            if (cur_id != -1)
            {
                ui->functionsMoveVariants->removeItem(cur_id);
            }
        }
        else if (pAction->text() == QString("add item before current item"))
        {
            int cur_id = ui->functionsMoveVariants->currentIndex();
            int insert_id = (cur_id == -1) ? 0 : cur_id;
            ui->functionsMoveVariants->insertItem(insert_id, QString("[]"));
            handleFunctionsMoveVariantsChanged(ui->functionsMoveVariants->itemText(ui->functionsMoveVariants->currentIndex()));
        }
        else if (pAction->text() == QString("add item after current item"))
        {
            int cur_id = ui->functionsMoveVariants->currentIndex();
            int insert_id = (cur_id == -1) ? 0 : cur_id + 1;
            ui->functionsMoveVariants->insertItem(insert_id, QString("[]"));
            handleFunctionsMoveVariantsChanged(ui->functionsMoveVariants->itemText(ui->functionsMoveVariants->currentIndex()));
        }
    }
}

void MainWindow::handleFunctionsMoveVariantsEditingFinished()
{
    int cur_id = ui->functionsMoveVariants->currentIndex();
    ui->functionsMoveVariants->setItemText(cur_id, ui->functionsMoveVariants->lineEdit()->text());
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == ui->functionsMoveVariants && event->type() == QEvent::KeyPress)
    {
        QKeyEvent* key = static_cast<QKeyEvent*>(event);
        int ikey = key->key();
        
        if ((ikey == Qt::Key_Up) || (ikey == Qt::Key_Down))
        {
            handleFunctionsMoveVariantsEditingFinished();
        }
    }

    return QObject::eventFilter(obj, event);
}

void MainWindow::handleFunctionsMoveVariantsChanged(const QString& str)
{
    int cur_id = ui->functionsMoveVariants->currentIndex();

    g_modify_funscript_function_move_variants = "";    
    for (int id = 0; id < ui->functionsMoveVariants->count(); id++)
    {
        if (id > 0)
        {
            g_modify_funscript_function_move_variants += ",";
        }

        if (id == cur_id)
        {
            g_modify_funscript_function_move_variants += str;
        }
        else
        {
            g_modify_funscript_function_move_variants += ui->functionsMoveVariants->itemText(id);
        }
    }

    std::vector<QPair<double, double>> modify_funscript_move_function;
    bool pars_passed = true;

    if (str != QString("[unchanged]"))
    {
        QStringList modify_funscript_function_move_variant_actions = str.mid(1, str.size() - 2).split("|");
        for (QString& modify_funscript_function_move_variant_action : modify_funscript_function_move_variant_actions)
        {
            QRegularExpression re_modify_funscript_function_move_variant_action("^([\\d\\.]+):([\\d\\.]+)$");
            QRegularExpressionMatch match;

            match = re_modify_funscript_function_move_variant_action.match(modify_funscript_function_move_variant_action);
            if (!match.hasMatch())
            {
                pars_passed = false;
                break;
            }
            else
            {
                double ddt = match.captured(1).toDouble();
                double ddpos = match.captured(2).toDouble();

                if (!((ddt > 0) && (ddt < 1)))
                {
                    pars_passed = false;
                    break;
                }

                if (!((ddpos >= 0) && (ddpos <= 1)))
                {
                    pars_passed = false;
                    break;
                }

                modify_funscript_move_function.push_back(QPair<double, double>(ddt, ddpos));
            }
        }
    }

    chart->removeAllSeries();

    if (pars_passed)
    {
        QLineSeries* series = new QLineSeries();
        series->append(0, 0);
        for (QPair<double, double>& pair : modify_funscript_move_function)
        {
            // ddt, ddpos
            series->append(pair.first, pair.second);
        }
        series->append(1, 1);


        chart->addSeries(series);

        chart->createDefaultAxes();

        chart->axisX()->setTitleText("ddt");
        chart->axisY()->setTitleText("ddpos");
    }   
}

void MainWindow::handleFunctionsMoveInChanged()
{
    g_modify_funscript_function_move_in_variants = ui->functionsMoveIn->text();
}

void MainWindow::handleFunctionsMoveOutChanged()
{
    g_modify_funscript_function_move_out_variants = ui->functionsMoveOut->text();
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
