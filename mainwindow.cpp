#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "HighPrecisionTimerGuard.h"
#include "ThreadAffinity.h"
#include <thread>
#include <condition_variable>
#include <mutex>

#define HOTKEY_PAUSE_ID                               1
#define HOTKEY_RESUME_ID                              2
#define HOTKEY_STOP_ID                                3
#define HOTKEY_USE_MODIFY_FUNSCRIPT_FUNCTIONS_ID      4
#define FUNSCRIPT_TIME_SHIFT_UP_ID                    5
#define FUNSCRIPT_TIME_SHIFT_DOWN_ID                  6

//---------------------------------------------------------------

void StartWorker::doWork()
{
    g_work_in_progress = true;
    // P-core pin (T3, see ThreadAffinity.h). All 4 worker types share P-core
    // index 1 because they are mutually exclusive via g_work_in_progress —
    // only ONE of them is ever running on this thread at any given moment,
    // so a single P-core slot is sufficient for the whole worker pool.
    ::ThreadAffinity::pinCurrentThread(/*idx=*/1, ::ThreadAffinity::kTagWorkerMainLoop);

    g_runing_funscript = false;
    g_initial_start = true;
    g_stop_run = false;
    g_pause = false;
    run_funscript();
    emit resultReady();
}

void StartController::handleResults()
{
    g_stop_run = false;
    g_work_in_progress = false;
    show_msg("", 0, MessageType::Clean);
    p_parent->show();
    p_parent->trayIcon->hide();
}

//---------------------------------------------------------------

void GetStatisticsWorker::doWork()
{
    g_work_in_progress = true;
    // P-core pin (T3, see ThreadAffinity.h) — same P-core #1 as the other
    // 3 workers, because they are all mutually exclusive via
    // g_work_in_progress.
    ::ThreadAffinity::pinCurrentThread(/*idx=*/1, ::ThreadAffinity::kTagWorkerMainLoop);

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

void TestWorker::doWork(int speed)
{
    g_work_in_progress = true;
    // P-core pin (T3, see ThreadAffinity.h) — same P-core #1 as the other
    // 3 workers, mutually exclusive via g_work_in_progress.
    ::ThreadAffinity::pinCurrentThread(/*idx=*/1, ::ThreadAffinity::kTagWorkerMainLoop);

    test_hismith(speed);
    emit resultReady();
}

void TestController::handleResults()
{
    g_stop_run = false;
    g_work_in_progress = false;
}

//---------------------------------------------------------------

void FredorchTestWorker::doWork()
{
    g_work_in_progress = true;
    // P-core pin (T3, see ThreadAffinity.h) — same P-core #1 as the other
    // workers, mutually exclusive via g_work_in_progress.
    ::ThreadAffinity::pinCurrentThread(/*idx=*/1, ::ThreadAffinity::kTagWorkerMainLoop);

    test_FredorchF21S();
    emit resultReady();
}

void FredorchTestController::handleResults()
{
    g_stop_run = false;
    g_work_in_progress = false;
}

//---------------------------------------------------------------

void PerfWorker::doWork(int speed)
{
    g_work_in_progress = true;
    // P-core pin (T3, see ThreadAffinity.h) — same P-core #1 as the other
    // 3 workers, mutually exclusive via g_work_in_progress.
    ::ThreadAffinity::pinCurrentThread(/*idx=*/1, ::ThreadAffinity::kTagWorkerMainLoop);

    get_performance_with_hismith(speed);
    emit resultReady();
}

void PerfController::handleResults()
{
    g_stop_run = false;
    g_work_in_progress = false;
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

    RegisterHotKey(
        (HWND)this->window()->winId(),
        FUNSCRIPT_TIME_SHIFT_UP_ID,
        get_key_mod(g_hotkey_funscript_time_shift_up) | MOD_NOREPEAT,
        get_key(g_hotkey_funscript_time_shift_up));

    RegisterHotKey(
        (HWND)this->window()->winId(),
        FUNSCRIPT_TIME_SHIFT_DOWN_ID,
        get_key_mod(g_hotkey_funscript_time_shift_down) | MOD_NOREPEAT,
        get_key(g_hotkey_funscript_time_shift_down));

    stopStartAction->setText(tr("Stop Run\t") + g_hotkey_stop);
    pauseStartAction->setText(tr("Pause Run\t") + g_hotkey_pause);
    resumeStartAction->setText(tr("Resume Run\t") + g_hotkey_resume);
    funscriptTimeShiftUpStartAction->setText(tr("Funscript Time Shift Up\t") + g_hotkey_funscript_time_shift_up);
    funscriptTimeShiftDownStartAction->setText(tr("Funscript Time Shift Down\t") + g_hotkey_funscript_time_shift_down);

    useModifyFunscriptFunctionsAction->setText(tr("Change Use Modify Funscript Functions\t") + g_hotkey_use_modify_funscript_functions);
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ctrlStart(this), ctrlGetStatistics(this), ctrlTest(this), ctrlPerformance(this), ctrlFredorchTest(this)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    connect(ui->getWebcamLatency, &QPushButton::released, this, &MainWindow::handleGetWebcamLatency);
    connect(ui->startButton, &QPushButton::released, this, &MainWindow::handleStartButton);
    connect(ui->testButton, &QPushButton::released, this, &MainWindow::handleTestButton);
    connect(ui->testButtonFredorchF21S, &QPushButton::released, this, &MainWindow::handleTestButtonFredorchF21S);
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
    chart->setMargins(QMargins(0, 0, 0, 0));
    QChartView* chartView = new QChartView(chart);
    chartView->setRenderHint(QPainter::Antialiasing);
    QVBoxLayout* layout = new QVBoxLayout(ui->chartFrame);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
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

    funscriptTimeShiftUpStartAction = new QAction(tr("Funscript Time Shift Up"), this);
    connect(funscriptTimeShiftUpStartAction, &QAction::triggered, this, &MainWindow::handleFunscriptTimeShiftUpStart);
    trayIconMenu->addAction(funscriptTimeShiftUpStartAction);

    funscriptTimeShiftDownStartAction = new QAction(tr("Funscript Time Shift Down"), this);
    connect(funscriptTimeShiftDownStartAction, &QAction::triggered, this, &MainWindow::handleFunscriptTimeShiftDownStart);
    trayIconMenu->addAction(funscriptTimeShiftDownStartAction);

    useModifyFunscriptFunctionsAction = new QAction(tr("Change Use Modify Funscript Functions"), this);
    connect(useModifyFunscriptFunctionsAction, &QAction::triggered, this, &MainWindow::handleUseModifyFunscriptFunctions);
    trayIconMenu->addAction(useModifyFunscriptFunctionsAction);

    exitAction = new QAction(tr("Exit"), this);
    connect(exitAction, &QAction::triggered, this, &MainWindow::handleTrayExit);
    trayIconMenu->addAction(exitAction);

    trayIcon = new QSystemTrayIcon(this);
    trayIcon->setContextMenu(trayIconMenu);

    QIcon icon(":/images/icon.ico");
    trayIcon->setIcon(icon);
    setWindowIcon(icon);

    trayIcon->setToolTip(QString("Hismith Control With Funscript v") + g_cur_version);
    setWindowTitle(QString("Hismith Control With Funscript v") + g_cur_version);

    QIcon iconOpenFunscript(":/images/folder.png");
    ui->openFunscript->setIcon(iconOpenFunscript);
    connect(ui->openFunscript, &QToolButton::released, this, &MainWindow::handleOpenFunscript);

    connect(ui->checkFunscript, &QPushButton::released, this, &MainWindow::handleCheckFunscript);

    connect(this, &MainWindow::errorOccurred, this, &MainWindow::showErrorMsg, Qt::QueuedConnection);
    connect(this, &MainWindow::warningOccurred, this, &MainWindow::showWarningMsg, Qt::QueuedConnection);
    connect(this, &MainWindow::msgOccurred, this, &MainWindow::showMsg, Qt::QueuedConnection);

    ui->DeviceConnectionTypes->clear();
    ui->DeviceConnectionTypes->addItem(g_winrt_ble);
    ui->DeviceConnectionTypes->addItem(g_intiface);

    // Allocate the BLE manager ONCE for the lifetime of the program.
    // All calls into it are now synchronous; the only signals we use from it
    // are convenience ones (deviceConnected / errorOccurred) for log output.
    m_bleManager = new WinRtBleManager(this);

    // Allocate the Fredorch F21S Program Mode controller ONCE for the
    // lifetime of the program. It sits on top of m_bleManager and is the
    // entry point for F21S control (moveTo / stop). It is NOT connected
    // here — connect() is called on demand from the transport dispatcher.
    {
        FredorchF21sController::Config fc;
        fc.throttleMs = 90;
        fc.useStepBlock = true;      // 5-register step block (0x10) — documented encoding
        fc.connectTimeoutMs = 8000;
        fc.deviceName = "F21S";
        m_fredorch = new FredorchF21sController(m_bleManager, fc);
    }

    // Log-only hookup: surface BLE connection events on the main window
    // status bar (no UI state is driven by these signals anymore).
    connect(m_bleManager, &WinRtBleManager::deviceConnected, this, [this]() {
            qDebug() << "BLE: device connected (handshake complete)";
        }, Qt::QueuedConnection);
    connect(m_bleManager, &WinRtBleManager::errorOccurred, this, [this](const QString& e) {
            qWarning() << "BLE: error:" << e;
        }, Qt::QueuedConnection);
}

void MainWindow::handleOpenFunscript()
{
    funscript = QDir::toNativeSeparators(QFileDialog::getOpenFileName(this,
        tr("Open Funscript"), "", tr("Funscripts (*.funscript)")));
    ui->funscriptPathEdit->setText(funscript);
}

// ---------------------------------------------------------------------------
// Fredorch F21S / T8 Program Mode transport (minimal wiring)
// ---------------------------------------------------------------------------

// Parse a Bluetooth MAC address string into a 48-bit uint64 address.
// Accepts "AA:BB:CC:DD:EE:FF", "AA-BB-CC-DD-EE-FF", "AABBCCDDEEFF",
// "0xAABBCCDDEEFF" and "0x00AA:BB:CC:DD:EE:FF". Returns false on failure.
static bool parseBleMacAddress(const QString& macIn, uint64_t& outAddress)
{
    QString s = macIn.trimmed();

    // Strip a leading "0x" / "0X" prefix (case-insensitive).
    if (s.size() >= 2 && s[0] == '0'
        && (s[1] == 'x' || s[1] == 'X'))
    {
        s = s.mid(2);
    }

    // Remove any separators: ':', '-', '.', ' '.
    QString hex;
    hex.reserve(s.size());
    for (QChar c : s)
    {
        const QChar low = c.toLower();
        if (low >= '0' && low <= '9')      hex.append(low);
        else if (low >= 'a' && low <= 'f') hex.append(low);
        // other characters (':' '-' '.' ' ' 'x') are dropped
    }

    if (hex.size() != 12)
    {
        return false;
    }

    bool ok = false;
    const uint64_t v = hex.toULongLong(&ok, 16);
    if (!ok || v == 0)
    {
        return false;
    }

    outAddress = v;
    return true;
}

bool MainWindow::connect_to_fredorch(QString bleMacAddress)
{
    if (!m_fredorch)
    {
        error_msg(tr("Fredorch controller is not initialized"));
        return false;
    }

    // If a previous session is active, tear it down first.
    if (m_fredorch->isConnected())
    {
        m_fredorch->disconnect();
    }

    uint64_t addr = 0;
    if (!parseBleMacAddress(bleMacAddress, addr))
    {
        error_msg(tr("Invalid BLE MAC address: %1")
                  .arg(bleMacAddress));
        return false;
    }

    // Point the controller at the requested device and connect.
    // The controller already holds a Config; we update the address here
    // without rebuilding the object (address is read during connect()).
    // We rely on connect() reading m_config.deviceAddress, so we set it
    // via a fresh connect using the address-based overload below.
    //
    // NOTE: FredorchF21sController::connect() uses m_config.deviceAddress.
    // Since the controller was constructed with deviceAddress = 0, we
    // re-target it by storing the parsed address and calling the BLE
    // manager's address-based handshake through the controller.
    qDebug().noquote() << "connect_to_fredorch: target 0x"
                       << QString::number(addr, 16);

    // Delegate to the controller. The controller reads
    // m_config.deviceAddress; to allow per-call addressing we expose the
    // BLE manager handshake path via a dedicated connect with address.
    bool ok = false;
    {
        // Update the controller's target address, then connect.
        // (The controller's public connect() uses its stored config address;
        //  we therefore set it through a helper exposed below.)
        ok = m_fredorch->connectWithAddress(addr);
    }

    if (ok)
    {
        // Marshal to UI thread — showMessage with timeout creates a QTimer
        // which must live on the same thread as the QStatusBar parent.
        QMetaObject::invokeMethod(this, [this]() {
            statusBar()->showMessage(tr("Fredorch F21S: connected (Program Mode)"), 5000);
        }, Qt::QueuedConnection);
        qDebug() << "connect_to_fredorch: SUCCESS";
    }
    else
    {
        error_msg(tr("Fredorch connect failed: %1").arg(m_fredorch->lastError()));
        qWarning().noquote()
            << "connect_to_fredorch: FAILED:"
            << m_fredorch->lastError().toStdString();
    }
    return ok;
}

void MainWindow::disconnect_from_fredorch()
{
    if (!m_fredorch)
    {
        return;
    }
    if (m_fredorch->isConnected())
    {
        m_fredorch->disconnect();
        // Marshal to UI thread (QTimer created by showMessage must be on UI thread)
        QMetaObject::invokeMethod(this, [this]() {
            statusBar()->showMessage(tr("Fredorch F21S: disconnected"), 4000);
        }, Qt::QueuedConnection);
        qDebug() << "disconnect_from_fredorch: done";
    }
}

bool MainWindow::fredorch_moveTo(int targetPos, int speed)
{
    if (!m_fredorch)
    {
        return false;
    }
    if (!m_fredorch->isConnected())
    {
        qWarning() << "fredorch_moveTo: not connected";
        return false;
    }
    return m_fredorch->moveTo(targetPos, speed);
}

bool MainWindow::fredorch_moveTo(
    const std::vector<FredorchF21sController::FredorchMoveSteps>& steps, bool loop)
{
    if (!m_fredorch)
    {
        return false;
    }
    if (!m_fredorch->isConnected())
    {
        qWarning() << "fredorch_moveTo(batch): not connected";
        return false;
    }
    return m_fredorch->moveTo(steps, loop);
}

bool MainWindow::fredorch_moveTo(
    int slotOffset,
    const std::vector<FredorchF21sController::FredorchMoveSteps>& steps,
    bool loop)
{
    if (!m_fredorch)
    {
        return false;
    }
    if (!m_fredorch->isConnected())
    {
        qWarning() << "fredorch_moveTo(batch+offset): not connected";
        return false;
    }
    return m_fredorch->moveTo(steps, slotOffset, loop);
}

bool MainWindow::fredorch_stop()
{
    if (!m_fredorch)
    {
        return false;
    }
    return m_fredorch->stop();
}

bool MainWindow::fredorch_readPlayState(uint16_t* stateOut, int timeoutMs)
{
    if (!m_fredorch)
    {
        return false;
    }
    return m_fredorch->readPlayState(stateOut, timeoutMs);
}

bool MainWindow::fredorch_startLoopRange(int posA, int posB, int speed)
{
    if (!m_fredorch)
    {
        return false;
    }
    if (!m_fredorch->isConnected())
    {
        qWarning() << "fredorch_startLoopRange: not connected";
        return false;
    }
    return m_fredorch->startLoopRange(posA, posB, speed);
}

bool MainWindow::start_programMode()
{
    if (!m_fredorch)
    {
        return false;
    }
    return m_fredorch->startProgramMode();
}

void MainWindow::handleCheckFunscript()
{
    std::vector<QPair<int, double>> funscript_data_maped_full;
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
    g_high_precision_timer_guard.Stop();
    trayIcon->hide();
    UnregisterHotKey((HWND)this->window()->winId(), HOTKEY_PAUSE_ID);
    delete ui;
}

void MainWindow::showErrorMsg(const QString& msg) {
    QMessageBox::critical(this, "Error", msg);
}

void MainWindow::showWarningMsg(const QString& msg, const QString& title) {
    QMessageBox::warning(this, title, msg);
}

void MainWindow::showMsg(const QString& msg, const QString& title) {
    QMessageBox::information(this, title, msg);
}

void MainWindow::handleGetWebcamLatency()
{
    get_webcam_latency();
}

void MainWindow::handleStartButton()
{
    g_stop_run = false;
    g_functions_move_in_out_variant = ui->functionsMoveInOutVariants->currentIndex() + 1;
    hide();
    trayIcon->show();
    emit ctrlStart.operate();
}

void MainWindow::handleTestButton()
{
    g_stop_run = false;
    if (!g_work_in_progress)
    {
        emit ctrlTest.operate(ui->testSpeed->text().toInt());
    }
}

void MainWindow::handleTestButtonFredorchF21S()
{
    g_stop_run = false;
    if (!g_work_in_progress)
    {
        emit ctrlFredorchTest.operate();
    }
}

void MainWindow::handleGetPerformance()
{
    g_stop_run = false;
    if (!g_work_in_progress)
    {
        emit ctrlPerformance.operate(ui->testSpeed->text().toInt());
    }
}

void MainWindow::handleGetStatistics()
{
    g_stop_run = false;
    if (!g_work_in_progress)
    {
        emit ctrlGetStatistics.operate();
    }
}

void MainWindow::handleTestCameraButton()
{
    g_stop_run = false;
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
    refreshDevices();
}

void MainWindow::refreshDevices(bool show_msgs)
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

    if (ui->DeviceConnectionTypes->currentText() == g_winrt_ble)
    {
        ui->Devices->clear();

        // Synchronous BLE advertisement scan (3 seconds by default, ~5 for
        // a typical UI refresh). Devices are NOT held / NOT connected here -
        // we only collect name + MAC for the dropdown.
        // The connection happens later inside connect_to_hismith() via the
        // synchronous connectToDevice() call.
        if (!m_bleManager)
        {
            if (show_msgs)
            {
                warning_msg(QString("ERROR: WinRtBleManager is not initialized"), "Refresh BLE Devices");
            }
            return;
        }

        show_msg("", 0, MessageType::Clean);
        show_msg(QString("Refreshing BLE Devices for %1ms, please wait...").arg(g_search_ble_devices_timeout), g_search_ble_devices_timeout, MessageType::Always);
        const auto devices = m_bleManager->findHismithDevices(g_search_ble_devices_timeout);

        for (const auto& d : devices)
        {
            // Store the MAC address in Qt::UserRole so that
            // connect_to_hismith() can read it back later via itemData().
            ui->Devices->addItem(d.name, QVariant::fromValue(d.address));

            if (!g_hismith_device_name.isEmpty() && d.name.contains(g_hismith_device_name))
            {
                ui->Devices->setCurrentIndex(ui->Devices->count() - 1);
            }
        }

        if (devices.empty() && show_msgs)
        {
            warning_msg(QString("No Hismith / Wildolo / Fredorch (YXlinksSPP) device was seen "
                "in the BLE advertisement scan (5 second window).\n\n"
                "Please verify the device is powered on, the Bluetooth radio is "
                "enabled in Windows, and that it is NOT paired/locked by another "
                "application (e.g. Intiface Central).\n\n"
                "Note: the Fredorch F21S / T8 advertises over BLE as \"YXlinksSPP\" "
                "(the factory BT module name), not as \"Fredorch F21S\".")
                , "Refresh BLE Devices");
        }
        return;
    }
    else
    {
        ui->Devices->clear();
        get_devices_list(show_msgs);
        for (DeviceClass& dev : g_myDevices)
        {
            ui->Devices->addItem(dev.deviceName.c_str());
            if (QString(dev.deviceName.c_str()).contains(g_hismith_device_name))
            {
                ui->Devices->setCurrentIndex(ui->Devices->count() - 1);
            }
        }
    }
}

void MainWindow::handleStopStart()
{
    if (g_work_in_progress)
    {
        std::lock_guard lk(g_update_mutex);
        show_msg("Stop pressed", 5000, MessageType::Clean);
        g_stop_run = true;
    }
}

void MainWindow::handlePauseStart()
{
    if (g_work_in_progress && !g_initial_start)
    {
        std::unique_lock lk(g_update_mutex);
        show_msg(QString("Pause pressed"), 5000, MessageType::Clean);
        g_pause = true;
        g_update = true;
        g_update_cvar.wait(lk, [] { return !g_update.load(); });
    }
}

void MainWindow::handleResumeStart()
{
    if (g_work_in_progress && !g_initial_start)
    {
        std::unique_lock lk(g_update_mutex);
        show_msg(QString("Resume pressed"), g_runing_funscript ? 2000 : 5000, MessageType::Clean);
        g_pause = false;
        g_update = true;
        g_update_cvar.wait(lk, [] { return !g_update.load(); });
    }
}

void MainWindow::handleFunscriptTimeShiftUpStart()
{
    if (g_work_in_progress && !g_initial_start)
    {
        std::unique_lock lk(g_update_mutex);
        g_funscript_time_shift_ms += g_funscript_time_shift_delta_ms;
        show_msg(QString("Funscript Time Shift In MS (Up): %1")
            .arg(g_funscript_time_shift_ms), g_runing_funscript ? 2000 : 5000, MessageType::Clean);
    }
}

void MainWindow::handleFunscriptTimeShiftDownStart()
{
    if (g_work_in_progress && !g_initial_start)
    {
        std::unique_lock lk(g_update_mutex);
        g_funscript_time_shift_ms -= g_funscript_time_shift_delta_ms;
        show_msg(QString("Funscript Time Shift In MS (Down): %1")
            .arg(g_funscript_time_shift_ms), g_runing_funscript ? 2000 : 5000, MessageType::Clean);
    }
}

void MainWindow::handleUseModifyFunscriptFunctions()
{
    if (g_work_in_progress && !g_initial_start)
    {
        {
            std::lock_guard lk(g_change_in_use_modify_funscript_functions_mutex);
            g_was_change_in_use_modify_funscript_functions = true;

            if (g_modify_funscript)
            {
                if (g_functions_move_in_out_variant == ui->functionsMoveInOutVariants->count())
                {
                    g_functions_move_in_out_variant = 1;
                    g_modify_funscript = false;
                }
                else
                {
                    g_functions_move_in_out_variant++;
                }
            }
            else
            {
                g_modify_funscript = true;
            }
        }

        {
            std::unique_lock lk(g_update_mutex);
            if (g_pause)
            {
                g_update = true;
                g_update_cvar.wait(lk, [] { return !g_update.load(); });
            }
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
            else if (wp == FUNSCRIPT_TIME_SHIFT_UP_ID)
            {
                handleFunscriptTimeShiftUpStart();
            }
            else if (wp == FUNSCRIPT_TIME_SHIFT_DOWN_ID)
            {
                handleFunscriptTimeShiftDownStart();
            }
        }

        return(true);

    }

    return(false);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    cv::destroyAllWindows();
    // Release the Fredorch F21S BLE link if it is still active, so the
    // radio is not held when the main window is closed.
    disconnect_from_fredorch();
    event->accept();
}
