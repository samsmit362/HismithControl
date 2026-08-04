#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "ThreadedCapture.h"
#include "HighPrecisionTimerGuard.h"

#include <QOpenGLWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QKeyEvent>
#include <opencv2/core.hpp>

extern cv::VideoCapture* g_pCapture;
extern __int64 g_delta_frame_read_time_vs_video_time;

class FastLatencyWindow : public QOpenGLWidget {
    Q_OBJECT

private:
    LARGE_INTEGER m_frequency;

    QImage m_camera_qimage;
    cv::Mat m_bgr_frame;
    __int64 m_msec_pos_output_frame;
    __int64 m_frame_current_time_ms;
    bool m_show_camera = true;
    bool test_started = false;
    int m_paint_cnt = 0;
    int m_current_hz = 60;
    __int64 m_pc_current_time_ms = 0;

    QTimer* m_refresh_timer;

protected:
    // Intercept keyboard events
    void keyPressEvent(QKeyEvent* event) override {
        int key = event->key();

       if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            m_show_camera = false;
            m_paint_cnt = 0;
            test_started = true;
            update(); // Instant re-render on click
        }
        // Esc to close the window
        else if (key == Qt::Key_Escape) {
            this->close();
        }
    }

    // Main drawing loop (Called on every VSync frame)
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        LARGE_INTEGER frame_read_time;

        // Enable antialiasing for font clarity
        painter.setRenderHint(QPainter::TextAntialiasing);

        if (test_started)
        {
            if (m_paint_cnt < m_current_hz)
            {
                m_paint_cnt++;
            }
            else if (m_paint_cnt == m_current_hz)
            {
                m_paint_cnt++;
                g_threaded_capture.wait_and_get_fresh_frame(m_bgr_frame, m_msec_pos_output_frame, frame_read_time);
                m_frame_current_time_ms = g_delta_frame_read_time_vs_video_time + m_msec_pos_output_frame;
            }

            if (m_paint_cnt >= m_current_hz)
            {
                painter.fillRect(this->rect(), Qt::black);

                QFont font_info("Consolas", 24, QFont::Bold);
                QFont font_time("Consolas", 48, QFont::Bold);
                painter.setPen(Qt::white);

                int window_width = this->width();
                int window_height = this->height();

                QString info_str = QString(
                    "Webcam latency measurement finished.\n"
                    "You need to manually subtract [frame_current_time_ms] - [time shown by webcam / [current_time_ms]]\n"
                    "to get the [webcam_end_to_end_latency] value, then manually save it in settings.xml.\n"
                    "Press: \"Enter\" to start again measuring webcam latency or \"Esc\" to exit."
                );
                QRect info_str_rect(window_width / 40, window_height / 40, window_width, (window_height * 45) / 100);

                painter.setFont(font_info);
                painter.drawText(info_str_rect, Qt::AlignLeft | Qt::AlignTop, info_str);

                // Format the time string
                QString time_str = QString("[frame_current_time_ms]: %1").arg(m_frame_current_time_ms);
                QRect time_str_rect(0, 0, window_width, window_height / 2);

                painter.setFont(font_time);
                painter.drawText(time_str_rect, Qt::AlignHCenter | Qt::AlignBottom, time_str);

                if (!m_bgr_frame.empty())
                {
                    // Convert cv::Mat (BGR) to QImage (RGB) without unnecessary memory reallocation if the sizes match
                    QImage img(m_bgr_frame.data, m_bgr_frame.cols, m_bgr_frame.rows, m_bgr_frame.step, QImage::Format_RGB888);

                    // Since OpenCV stores BGR, and Qt requires RGB, we implement fast channel mirroring.
                    m_camera_qimage = img.rgbSwapped().copy();

                    int cam_w = window_width / 2;
                    int cam_h = window_height / 2;
                    int cam_x = 0;
                    int cam_y = window_height - cam_h;

                    QRect cam_target_rect(cam_x, cam_y, cam_w, cam_h);
                    painter.drawImage(cam_target_rect, m_camera_qimage);
                }

                return;
            }
        }

        // 1. Fill the background completely black (to minimize matrix latency)
        painter.fillRect(this->rect(), Qt::black);

        // 2. Calculate the current time m_pc_current_time_ms using QPC
        LARGE_INTEGER cur_time;
        QueryPerformanceCounter(&cur_time);
        m_pc_current_time_ms = (cur_time.QuadPart * 1000) / m_frequency.QuadPart;

        // 3. Setting the font
        QFont font_time("Consolas", 48, QFont::Bold);
        painter.setPen(Qt::white);

        int window_width = this->width();
        int window_height = this->height();

        // Format the time string
        QString time_str = QString("[current_time_ms]: %1").arg(m_pc_current_time_ms);
        QRect time_str_rect(0, 0, window_width, window_height / 2);

        painter.setFont(font_time);
        painter.drawText(time_str_rect, Qt::AlignHCenter | Qt::AlignBottom, time_str);

        if (!test_started)
        {
            QFont font_info("Consolas", 24, QFont::Bold);

            QString info_str = QString(
                "You need to align the webcam so that it captures the current time in milliseconds shown on the monitor.\n"
                "It is highly recommended to set a static webcam focus and exposure; you can do this in \"Test Webcam\".\n"
                "This is required to minimize end-to-end latency and to measure it correctly.\n"
                "Press: \"Enter\" to start measuring webcam latency or \"Esc\" to exit."
            );
            QRect info_str_rect(window_width / 40, window_height / 40, window_width, (window_height * 45) / 100);

            painter.setFont(font_info);
            painter.drawText(info_str_rect, Qt::AlignLeft | Qt::AlignTop, info_str);

            // 5. Display the camera in the lower left corner 1/2
            if (m_show_camera && g_threaded_capture.is_running) {
                g_threaded_capture.wait_and_get_fresh_frame(m_bgr_frame, m_msec_pos_output_frame, frame_read_time);

                if (!m_bgr_frame.empty())
                {
                    // Convert cv::Mat (BGR) to QImage (RGB) without unnecessary memory reallocation if the sizes match
                    QImage img(m_bgr_frame.data, m_bgr_frame.cols, m_bgr_frame.rows, m_bgr_frame.step, QImage::Format_RGB888);

                    // Since OpenCV stores BGR, and Qt requires RGB, we implement fast channel mirroring.
                    m_camera_qimage = img.rgbSwapped().copy();

                    int cam_w = window_width / 2;
                    int cam_h = window_height / 2;
                    int cam_x = 0;
                    int cam_y = window_height - cam_h;

                    QRect cam_target_rect(cam_x, cam_y, cam_w, cam_h);
                    painter.drawImage(cam_target_rect, m_camera_qimage);
                }
            }
        }
    }

public:

    FastLatencyWindow(QWidget* parent = nullptr) : QOpenGLWidget(parent) {
        // Automatically free up heap memory when the user closes the window (via Esc)
        this->setAttribute(Qt::WA_DeleteOnClose);

        // Initialize the frequency of the Windows system timer
        QueryPerformanceFrequency(&m_frequency);

        // Allow the window to receive keyboard focus
        setFocusPolicy(Qt::StrongFocus);

        QScreen* current_screen = this->screen(); // Получаем экран самого виджета
        if (current_screen) {
            m_current_hz = current_screen->refreshRate();

            if (m_current_hz < 60)
            {
                m_current_hz = 60;
            }
        }
        else
        {
            m_current_hz = 60;
        }

        // Set up the update timer.
        // An interval of 0 means that Qt will call update() as soon as
        // the operating system and video card are ready to process frames (based on VSync).
        m_refresh_timer = new QTimer(this);
        connect(m_refresh_timer, &QTimer::timeout, this, QOverload<>::of(&FastLatencyWindow::update));
        m_refresh_timer->start(0);
    }

    ~FastLatencyWindow()
    {
        if (g_pCapture)
        {
            g_threaded_capture.stop();
            g_high_precision_timer_guard.Stop();
            g_pCapture->release();
            delete g_pCapture;
            g_pCapture = NULL;
        }
    }
};
