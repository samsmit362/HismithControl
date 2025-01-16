#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <windows.h>
#include <thread>
#include <condition_variable>
#include <mutex>

#define HOTKEY_PAUSE_ID     1
#define HOTKEY_RESUME_ID    2
#define HOTKEY_STOP_ID      3
#define HOTKEY_USE_MODIFY_FUNSCRIPT_FUNCTIONS_ID      4

//---------------------------------------------------------------

void StartWorker::doWork()
{
    g_work_in_progress = true;
    run_funscript();
    emit resultReady();
}

void StartController::handleResults()
{
    g_stop_run = false;
    g_work_in_progress = false;
    p_parent->show();
    p_parent->trayIcon->hide();
}

//---------------------------------------------------------------

void GetStatisticsWorker::doWork()
{
    g_work_in_progress = true;
    p_parent->ui->getStatistics->setText(QString("Stop Get Hismith Statistics Data"));
    get_statistics_with_hismith(p_parent->ui->startSpeed->text().toInt(), p_parent->ui->endSpeed->text().toInt());
    emit resultReady();
}

void GetStatisticsController::handleResults()
{
    g_stop_run = false;
    g_work_in_progress = false;
    p_parent->ui->getStatistics->setText(QString("Get Hismith Statistics Data"));
}

//---------------------------------------------------------------

UINT get_key_mod(QString &ks)
{
    UINT res = 0; 
    for (QString &str: ks.split("+"))
    {
        if(str == "Ctrl")
        {
            res |= MOD_CONTROL;
        } 
        else if(str == "Alt")
        {
            res |= MOD_ALT;
        } 
        else if(str == "Shift")
        {
            res |= MOD_SHIFT;
        }
    }
    return res;
}

UINT get_key(QString &ks)
{
    return QKeySequence(ks.split("+").last())[0];
}

void MainWindow::RegisterHotKeys()
{
    RegisterHotKey(
        (HWND)this->window()->winId(),
        HOTKEY_STOP_ID,
        get_key_mod(g_hotkey_stop) | MOD_NOREPEAT,
        get_key(g_hotkey_stop));

    RegisterHotKey(
        (HWND)this->window()->winId(),
        HOTKEY_PAUSE_ID,
        get_key_mod(g_hotkey_pause) | MOD_NOREPEAT,
        get_key(g_hotkey_pause));

    RegisterHotKey(
        (HWND)this->window()->winId(),
        HOTKEY_RESUME_ID,
        get_key_mod(g_hotkey_resume) | MOD_NOREPEAT,
        get_key(g_hotkey_resume));

    RegisterHotKey(
        (HWND)this->window()->winId(),
        HOTKEY_USE_MODIFY_FUNSCRIPT_FUNCTIONS_ID,
        get_key_mod(g_hotkey_use_modify_funscript_functions) | MOD_NOREPEAT,
        get_key(g_hotkey_use_modify_funscript_functions));

    stopStartAction->setText(tr("Stop Run\t") + g_hotkey_stop);
    pauseStartAction->setText(tr("Pause Run\t") + g_hotkey_pause);
    resumeStartAction->setText(tr("Resume Run\t") + g_hotkey_resume);
    useModifyFunscriptFunctionsAction->setText(tr("Change Use Modify Funscript Functions\t") + g_hotkey_use_modify_funscript_functions);
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ctrlStart(this), ctrlGetStatistics(this)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

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

    connect(ui->functionsMoveInOutVariants, SIGNAL(editTextChanged(const QString&)), this, SLOT(handleFunctionsMoveInOutVariantsChanged(const QString&)));
    connect(ui->functionsMoveInOutVariants, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(handleFunctionsMoveInOutVariantsContextMenuRequested(QPoint)));
    connect(ui->functionsMoveInOutVariants->lineEdit(), SIGNAL(editingFinished()), SLOT(handleFunctionsMoveInOutVariantsEditingFinished()));
    ui->functionsMoveInOutVariants->installEventFilter(this);

    //-------------------

    trayIconMenu = new QMenu(this);

    stopStartAction = new QAction(tr("Stop Run"), this);
    connect(stopStartAction, &QAction::triggered, this, &MainWindow::handleStopStart);
    trayIconMenu->addAction(stopStartAction);

    pauseStartAction = new QAction(tr("Pause Run"), this);
    connect(pauseStartAction, &QAction::triggered, this, &MainWindow::handlePauseStart);
    trayIconMenu->addAction(pauseStartAction);

    resumeStartAction = new QAction(tr("Resume Run"), this);
    connect(resumeStartAction, &QAction::triggered, this, &MainWindow::handleResumeStart);
    trayIconMenu->addAction(resumeStartAction);

    useModifyFunscriptFunctionsAction = new QAction(tr("Change Use Modify Funscript Functions"), this);
    connect(useModifyFunscriptFunctionsAction, &QAction::triggered, this, &MainWindow::handleUseModifyFunscriptFunctions);
    trayIconMenu->addAction(useModifyFunscriptFunctionsAction);

    exitAction = new QAction(tr("Exit"), this);
    connect(exitAction, &QAction::triggered, this, &MainWindow::handleTrayExit);
    trayIconMenu->addAction(exitAction);

    trayIcon = new QSystemTrayIcon(this);
    trayIcon->setContextMenu(trayIconMenu);

    QIcon icon(":/images/icon.png");
    trayIcon->setIcon(icon);
    setWindowIcon(icon);

    trayIcon->setToolTip(QString("Hismith Control With Funscript v") + g_cur_version);
    setWindowTitle(QString("Hismith Control With Funscript v") + g_cur_version);

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
        QFileInfo info(funscript);
        QString fname = info.fileName();
        QString fext = info.suffix();

        if (fext != QString("funscript"))
        {
            funscript = QDir::toNativeSeparators(info.path() + "/" + info.completeBaseName() + ".funscript");
        }

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
    emit ctrlStart.operate();
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
    if (!g_work_in_progress)
    {
        emit ctrlGetStatistics.operate();
    }
    else if (!g_stop_run)
    {
        show_msg("Stop pressed", 1000);
        g_stop_run = true;
    }
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
    else if (obj == ui->functionsMoveInOutVariants && event->type() == QEvent::KeyPress)
    {
        QKeyEvent* key = static_cast<QKeyEvent*>(event);
        int ikey = key->key();

        if ((ikey == Qt::Key_Up) || (ikey == Qt::Key_Down))
        {
            handleFunctionsMoveInOutVariantsEditingFinished();
        }
    }

    return QObject::eventFilter(obj, event);
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

    chart->setTitle(QString("Modify Funscript Function [id: %1]").arg(cur_id+1));
}

void MainWindow::handleFunctionsMoveInOutVariantsContextMenuRequested(QPoint pos)
{
    QMenu menu;
    menu.addAction(QString("delete current item"));
    menu.addAction(QString("add item before current item"));
    menu.addAction(QString("add item after current item"));
    QAction* pAction = menu.exec(ui->functionsMoveInOutVariants->mapToGlobal(pos));

    if (pAction)
    {
        if (pAction->text() == QString("delete current item"))
        {
            int cur_id = ui->functionsMoveInOutVariants->currentIndex();

            if (cur_id != -1)
            {
                ui->functionsMoveInOutVariants->removeItem(cur_id);
            }
        }
        else if (pAction->text() == QString("add item before current item"))
        {
            int cur_id = ui->functionsMoveInOutVariants->currentIndex();
            int insert_id = (cur_id == -1) ? 0 : cur_id;
            ui->functionsMoveInOutVariants->insertItem(insert_id, QString("[]"));
            handleFunctionsMoveInOutVariantsChanged(ui->functionsMoveInOutVariants->itemText(ui->functionsMoveInOutVariants->currentIndex()));
        }
        else if (pAction->text() == QString("add item after current item"))
        {
            int cur_id = ui->functionsMoveInOutVariants->currentIndex();
            int insert_id = (cur_id == -1) ? 0 : cur_id + 1;
            ui->functionsMoveInOutVariants->insertItem(insert_id, QString("[]"));
            handleFunctionsMoveInOutVariantsChanged(ui->functionsMoveInOutVariants->itemText(ui->functionsMoveInOutVariants->currentIndex()));
        }
    }
}

void MainWindow::handleFunctionsMoveInOutVariantsEditingFinished()
{
    int cur_id = ui->functionsMoveInOutVariants->currentIndex();
    ui->functionsMoveInOutVariants->setItemText(cur_id, ui->functionsMoveInOutVariants->lineEdit()->text());
}

void MainWindow::handleFunctionsMoveInOutVariantsChanged(const QString& str)
{
    int cur_id = ui->functionsMoveInOutVariants->currentIndex();

    g_modify_funscript_function_move_in_out_variants = "";
    for (int id = 0; id < ui->functionsMoveInOutVariants->count(); id++)
    {
        if (id > 0)
        {
            g_modify_funscript_function_move_in_out_variants += ";";
        }

        if (id == cur_id)
        {
            g_modify_funscript_function_move_in_out_variants += str;
        }
        else
        {
            g_modify_funscript_function_move_in_out_variants += ui->functionsMoveInOutVariants->itemText(id);
        }
    }
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
    if (g_work_in_progress)
    {
        std::lock_guard lk(g_stop_mutex);
        show_msg("Pause pressed", 2000, true, g_modify_funscript);
        g_pause = true;
        g_stop_cvar.notify_all();
    }
}

void MainWindow::handleResumeStart()
{
    if (g_work_in_progress)
    {
        std::lock_guard lk(g_stop_mutex);
        show_msg("Resume pressed", 2000, true, g_modify_funscript);
        g_pause = false;
        g_stop_cvar.notify_all();
    }
}

void MainWindow::handleUseModifyFunscriptFunctions()
{
    if (g_work_in_progress)
    {
        std::lock_guard lk(g_change_in_use_modify_funscript_functions_mutex);
        g_was_change_in_use_modify_funscript_functions = true;

        if (g_modify_funscript)
        {
            if (g_functions_move_in_out_variant == ui->functionsMoveInOutVariants->count())
            {
                g_functions_move_in_out_variant = 1;
                g_modify_funscript = false;
                show_msg("Turn Off Use Modify Funscript Functions", 4000, true);
            }
            else
            {
                g_functions_move_in_out_variant++;
                show_msg(QString("Change Use Modify Funscript Functions to\nvariant %1/%2 : %3").arg(g_functions_move_in_out_variant).arg(ui->functionsMoveInOutVariants->count()).arg(ui->functionsMoveInOutVariants->itemText(g_functions_move_in_out_variant - 1)), 4000, true, true);
            }
        }
        else
        {
            g_modify_funscript = true;
            show_msg(QString("Turn On Use Modify Funscript Functions to\nvariant %1/%2 : %3").arg(g_functions_move_in_out_variant).arg(ui->functionsMoveInOutVariants->count()).arg(ui->functionsMoveInOutVariants->itemText(g_functions_move_in_out_variant - 1)), 4000, true, true);
        }
    }
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
            else if (wp == HOTKEY_USE_MODIFY_FUNSCRIPT_FUNCTIONS_ID)
            {
                handleUseModifyFunscriptFunctions();
            }
        }
        
        return(true);

    }

    return(false);
}
