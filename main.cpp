
#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <gdiplus.h>
#include <sys/timeb.h>
#include <eh.h> // required for _set_se_translator
#include <stdexcept>
#include <stacktrace> // Requires C++23
#include <deque>
#include <memory_resource>
#include <algorithm>

using namespace Gdiplus; // Required for Graphics in WndProc
#pragma comment (lib,"Gdiplus.lib")

#pragma comment(lib, "winmm.lib") // Required for timeBeginPeriod

//---------------------------------------------------------------

QString g_cur_version = "8.00";

//---------------------------------------------------------------

int g_max_allowed_hismith_speed;
int g_min_funscript_relative_move;

int g_dt_for_get_cur_speed;
const int g_min_dt_for_set_hismith_speed = 20;

int g_speed_change_delay;
int g_min_dt_between_speed_changes_on_slow_moves;
int g_min_dt_between_speed_changes_on_fast_moves;
int g_fast_move_min_hismith_speed_for_switch_min_dt_between_speed_changes;
int g_min_dt_start_for_speed_40;
int g_cpu_freezes_timeout;

double g_video_cur_rate = 1.0;
int g_actual_video_pos = 0;
int g_video_cur_actions_end_time = 0;
int g_video_cur_actions_start_pos = 0;
bool g_initial_start = true;
int g_d_from_search_start_pos = -360;

//YUV:
int g_B_range[3][2];

//YUV:
int g_G_range[3][2];

double g_max_telescopic_motor_rocker_arm_proportions;
double g_min_telescopic_motor_rocker_arm_center_x_proportions;
double g_max_telescopic_motor_rocker_arm_center_x_proportions;

cv::VideoCapture *g_pCapture = NULL;
QString g_req_webcam_name;
int g_webcam_frame_width;
int g_webcam_frame_height;
double g_webcam_fps;
int g_webcam_focus;
int g_webcam_end_to_end_latency = 0;
bool g_webcam_msmf_supported = false;

std::string g_intiface_central_client_url;
int g_intiface_central_client_port;
QString g_hismith_device_name;

QString g_vlc_url;
int g_vlc_port;
QString g_vlc_password;

QString g_hotkey_stop;
QString g_hotkey_pause;
QString g_hotkey_resume;
QString g_hotkey_use_modify_funscript_functions;

//---------------------------------------------------------------

QString g_root_dir;
QString g_results_file_path;
QString g_results_file_data;

Client* g_pClient = NULL;
std::vector<DeviceClass> g_myDevices;
DeviceClass* g_pMyDevice = NULL;

QNetworkAccessManager* g_pNetworkAccessManager = NULL;
QNetworkRequest g_NetworkRequest;

bool g_stop_run = false;
bool g_pause = false;
bool g_update = false;
bool g_msg_created = false;
bool g_was_change_in_use_modify_funscript_functions = false;
bool g_video_freezed = false;

// g_ccxlcx_lh_ratio = (double)(c_cx - l_cx) / (double)max(l_h, l_w)
double g_ccxlcx_lh_ratio = -1.0;
double g_max_ccxlcx_lh_ratio_prev_to_cur_dif = -1.0;

MainWindow* g_pW = NULL;

std::mutex g_update_mutex;
std::mutex g_change_in_use_modify_funscript_functions_mutex;
std::condition_variable g_update_cvar;

bool g_work_in_progress = false;
bool g_runing_funscript = false;

int g_avg_time_delay = 0;

int g_save_images = true;

bool g_modify_funscript = false;

// "[0.25:0.38|0.75:0.62],[0.25:0.12|0.75:0.87],[unchanged]"; // [fast:slow:fast],[slow:fast:slow],[unchanged]
QString g_modify_funscript_function_move_variants;

//"[0-200:1],[200-maximum:2];[0-200:1|2],[200-maximum:2];[0-200:random],[200-maximum:2];[0-maximum:random]";
QString g_modify_funscript_function_move_in_out_variants;

// selected variant in g_modify_funscript_function_move_in_out_variants
// it's value should be from 1 to num in g_modify_funscript_function_move_in_out_variants
int g_functions_move_in_out_variant = 1;

int g_hismith_speed_for_set_initial_pos = 5;
const int g_min_search_pos_dif = -10;
const int g_max_search_pos_dif = 70;

//---------------------------------------------------------------

HighPrecisionTimerGuard g_high_precision_timer_guard;
__int64 g_delta_cur_vs_video_time = -1;

//---------------------------------------------------------------

ThreadedCapture g_threaded_capture;

//---------------------------------------------------------------

#define time_diff_in_milliseconds(a, b, c) (((a.QuadPart - b.QuadPart)*(__int64)1000)/c.QuadPart)

//---------------------------------------------------------------

void save_BGR_image(cv::Mat &frame, QString fpath)
{
	if (g_save_images && (!frame.empty()))
	{
		std::vector<uchar> write_data;
		cv::imencode(".bmp", frame, write_data);

		QFile f(fpath);
		if (!f.open(QFile::WriteOnly))
		{
			f.close();
			show_msg(QString("ERROR: failed to open file: %1").arg(fpath));
		}
		QDataStream fs(&f);
		fs.writeRawData((char*)(write_data.data()), write_data.size());
		f.flush();
		f.close();
	}
}

void save_text_to_file(QString fpath, QString text, QFlags<QIODeviceBase::OpenModeFlag> flags)
{
	QFile file(fpath);

	if (!file.open(flags))
	{
		error_msg(QString("ERROR: file [%1] already opened for write or there is another issue").arg(fpath));
	}
	else
	{
		QTextStream ts(&file);

		ts << text;
		file.flush();
		file.close();
	}
}

QString get_cur_time_str()
{
	auto t = std::time(nullptr);
	auto tm = *std::localtime(&t);
	std::ostringstream oss;
	oss << std::put_time(&tm, "%Y.%m.%d_%H.%M.%S");
	QString time_str = oss.str().c_str();
	return time_str;
}

void get_new_camera_frame(cv::VideoCapture &capture, cv::Mat& frame, __int64& msec_frame_cur_pos)
{
	static __int64 msec_frame_prev_pos = -1;

	if (g_threaded_capture.is_running)
	{
		g_threaded_capture.wait_and_get_fresh_frame(frame, msec_frame_cur_pos);
	}
	else
	{
		do
		{
			if (!capture.read(frame))
			{
				error_msg(QString("ERROR: capture.read(frame) failed"));
				break;
			}
			msec_frame_cur_pos = capture.get(cv::CAP_PROP_POS_MSEC);
		} while ((msec_frame_cur_pos == msec_frame_prev_pos) &&
			!g_stop_run &&
			!g_pause &&
			!g_was_change_in_use_modify_funscript_functions);
		msec_frame_prev_pos = msec_frame_cur_pos;
	}
}

int _tmp_cur_hismith_speed_int = 0;
double set_hismith_speed(double speed)
{
	double res_speed = -1.0;
	if (g_pClient && g_pMyDevice)
	{
		int speed_int = speed > 0 ? speed * 100.0 : 0;
		if (speed_int > g_max_allowed_hismith_speed)
		{
			speed_int = g_max_allowed_hismith_speed;
		}
		_tmp_cur_hismith_speed_int = speed_int;
		res_speed = (double)speed_int / 100.0;
		g_pClient->sendScalar(*g_pMyDevice, res_speed);
	}

	return res_speed;
}

void draw_text(QString text, cv::Mat &frame, int x1 = -1, int y1 = -1, int x2 = -1, int y2 = -1)
{
	int fontFace = cv::FONT_HERSHEY_SIMPLEX;
	double fontScale = 1;
	int thickness = 4;
	int width = frame.cols;
	int height = frame.rows;
	int y_offset = 5;

	for (QString& line : text.split('\n'))
	{
		int baseline = 0;
		cv::Size textSize = cv::getTextSize(line.toStdString(), fontFace,
			fontScale, thickness, &baseline);
		baseline += thickness;

		// center the text
		cv::Point textOrg((width - textSize.width) / 2, y_offset + textSize.height + thickness);

		cv::putText(frame, line.toStdString(), textOrg, fontFace, fontScale,
			cv::Scalar(0, 0, 0), thickness, cv::LINE_AA);
		cv::putText(frame, line.toStdString(), textOrg, fontFace, fontScale,
			cv::Scalar(255, 255, 255), thickness - 2, cv::LINE_AA);

		y_offset += textSize.height + (2*thickness) + 5;
	}

	if (x1 != -1)
	{
		cv::rectangle(frame, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(0, 0, 255));
	}
}

void show_frame_in_cv_window(cv::String wname, cv::Mat &frame)
{
	cv::namedWindow(wname, cv::WINDOW_NORMAL);
	cv::setWindowProperty(wname, cv::WND_PROP_TOPMOST, 1);
	cv::imshow(wname, frame);
	cv::Size s = frame.size();
	cv::resizeWindow(wname, s);
	int sw = (int)GetSystemMetrics(SM_CXSCREEN);
	int sh = (int)GetSystemMetrics(SM_CYSCREEN);
	cv::moveWindow(wname, (sw - s.width) / 2, (sh - s.height) / 2);
}

void error_msg(QString msg, cv::Mat* p_frame, cv::Mat* p_frame_upd, cv::Mat *p_prev_frame, int x1, int y1, int x2, int y2)
{
	g_stop_run = true;

	show_msg("", 0, MessageType::Clean);

	g_ccxlcx_lh_ratio = -1.0;
	g_max_ccxlcx_lh_ratio_prev_to_cur_dif = -1.0;

	disconnect_from_hismith();

	g_results_file_data += QString("\nerror_msg:\n") + msg + QString("\n");

	auto t = std::time(nullptr);
	auto tm = *std ::localtime(&t);
	std::ostringstream oss;
	oss << std::put_time(&tm, "%Y.%m.%d_%H.%M.%S");
	QString time_str = oss.str().c_str();
	cv::Mat* p_draw_frame = p_frame_upd ? p_frame_upd : p_frame;

	if (p_frame)
	{
		save_BGR_image(*p_frame, g_root_dir + "\\error_data\\" + time_str + "_frame_orig.bmp");
	}

	if (p_prev_frame)
	{
		save_BGR_image(*p_prev_frame, g_root_dir + "\\error_data\\" + time_str + "_frame_prev.bmp");
	}

	if (p_draw_frame)
	{
		draw_text(msg, *p_draw_frame, x1, y1, x2, y2);
		save_BGR_image(*p_draw_frame, g_root_dir + "\\error_data\\" + time_str + "_frame_draw.bmp");

		show_frame_in_cv_window("Error", *p_draw_frame);
		cv::waitKey(0);
	}
	else
	{
		emit g_pW->errorOccurred(msg);
	}
	cv::destroyAllWindows();
}

void warning_msg(QString msg, QString title = "")
{
	g_results_file_data += QString("\nwarning_msg:\n") + msg + QString("\n");
	emit g_pW->warningOccurred(msg, title);
}

void show_msg(QString msg, QString title)
{
	emit g_pW->msgOccurred(msg, title);
}

//---------------------------------------------------------------

int get_video_dev_id()
{
	DeviceEnumerator de;

	// Video Devices
	std::map<int, InputDevice> devices = de.getVideoDevicesMap();
	int video_dev_id = -1;

	QString selected_webcam = g_pW->ui->Webcams->itemText(g_pW->ui->Webcams->currentIndex());

	// Print information about the devices
	for (auto const& device : devices) {
		if (selected_webcam == device.second.deviceName.c_str())
		{
			video_dev_id = device.first;
			break;
		}
	}

	if (video_dev_id == -1)
	{
		error_msg(QString("ERROR: Selected webcam is not currently present"));
	}

	return video_dev_id;
}

bool _tmp_got_client_msg;
void callbackFunction(const mhl::Messages msg) {
	_tmp_got_client_msg = true;

	if (msg.messageType == mhl::MessageTypes::DeviceList) {
		std::cout << "Device List callback" << std::endl;
	}
	if (msg.messageType == mhl::MessageTypes::DeviceAdded) {
		std::cout << "Device Added callback" << std::endl;
	}
	if (msg.messageType == mhl::MessageTypes::ServerInfo) {
		std::cout << "Server Info callback" << std::endl;
	}
	if (msg.messageType == mhl::MessageTypes::DeviceRemoved) {
		std::cout << "Device Removed callback" << std::endl;
	}
	if (msg.messageType == mhl::MessageTypes::SensorReading) {
		std::cout << "Sensor Reading callback" << std::endl;
	}
}

inline int pow2(int x)
{
	return x * x;
}

void GreyscaleImageToMat(simple_buffer<u8>& ImGR, int w, int h, cv::Mat& res)
{
	res = cv::Mat(h, w, CV_8UC1);
	custom_assert(w * h <= ImGR.m_size, "GreyscaleImageToMat(simple_buffer<u8>& ImGR, int w, int h, cv::Mat& res)\nnot: w * h <= ImGR.m_size");
	memcpy(res.data, ImGR.m_pData, w * h);
}

void GreyscaleMatToImage(cv::Mat& ImGR, int w, int h, simple_buffer<u8>& res)
{
	res.copy_data(ImGR.data, w * h);
}

cv::Mat GetFigureMask(CMyClosedFigure* pFigure, int w, int h)
{
	int l, ii;
	cv::Mat res(cv::Size(w, h), CV_8UC1, cv::Scalar(0));

	for (l = 0; l < pFigure->m_PointsArray.m_size; l++)
	{
		ii = pFigure->m_PointsArray[l];
		res.data[ii] = (u8)255;
	}

	return res;
}

void get_binary_image(cv::Mat &img, int (&range)[3][2], cv::Mat& img_res, int erosion_size)
{
	cv::inRange(img, cv::Scalar( range[0][0], range[1][0], range[2][0] ), cv::Scalar(range[0][1], range[1][1], range[2][1] ), img_res);

	if (erosion_size > 0)
	{
		cv::Mat element = cv::getStructuringElement(cv::MORPH_RECT,
			cv::Size(2 * erosion_size + 1, 2 * erosion_size + 1),
			cv::Point(erosion_size, erosion_size));
		cv::erode(img_res, img_res, element);
	}
}

// return pos in range: [0;360]
bool get_hismith_pos_by_image(cv::Mat& frame, int& pos, bool ignore_error = false, bool show_results = false, cv::Mat *p_res_frame = NULL, double *p_cur_speed = NULL, cv::String title = "Get Hismith Pos By Image", QString add_data = QString())
{
	static cv::Mat prev_frame;

	bool res = true;
	cv::Mat img, img_b, img_g, img_right;
	custom_buffer<CMyClosedFigure> figures_b;
	simple_buffer<CMyClosedFigure*> p_figures_b;
	int figures_b_N;
	custom_buffer<CMyClosedFigure> figures_g;

	LARGE_INTEGER start_time, t1, t2, Frequency;
	QueryPerformanceFrequency(&Frequency);

	if (show_results)
	{
		QueryPerformanceCounter(&start_time);
	}

	cv::cvtColor(frame, img, cv::COLOR_BGR2YUV);
	int width = img.cols;
	int height = img.rows;

	simple_buffer<u8> Im_b(width * height);
	simple_buffer<u8> Im_g(width * height);

	concurrency::parallel_invoke(
		[&img, &img_b, &Im_b, width, height] {
			get_binary_image(img, g_B_range, img_b, 3);
			GreyscaleMatToImage(img_b, width, height, Im_b);
		},
		[&img, &img_g, &Im_g, width, height] {
			get_binary_image(img, g_G_range, img_g, 3);
			GreyscaleMatToImage(img_g, width, height, Im_g);
		}
	);

	concurrency::parallel_invoke(
		[&Im_b, &Im_g, &figures_b, &p_figures_b, &figures_b_N, width, height] {

			// intersection points be treated as related to green for get device position
			for (int i = 0; i < width * height; i++)
			{
				if (Im_g[i])
				{
					Im_b[i] = 0;
				}
			}

			SearchClosedFigures(Im_b, width, height, (u8)255, figures_b);

			figures_b_N = figures_b.size();

			if (figures_b_N > 0)
			{
				p_figures_b.set_size(figures_b_N);
				for (int i = 0; i < figures_b_N; i++)
				{
					p_figures_b[i] = &(figures_b[i]);
				}
			}
		},
		[&Im_g, &figures_g, width, height] {
			SearchClosedFigures(Im_g, width, height, (u8)255, figures_g);
		}
	);

	if (show_results)
	{
		QueryPerformanceCounter(&t1);
	}

	CMyClosedFigure* p_best_match_c_figure = NULL;
	int c_x = -1, c_y = -1, c_w = -1, c_h = -1;
	int max_size_c = 0;
	CMyClosedFigure* p_best_match_l_figure = NULL;
	int l_x = -1, l_y = -1, l_w = -1, l_h = -1, l_cx = -1, l_cy = -1;
	int max_size_l = 0;
	CMyClosedFigure* p_best_match_r_figure = NULL;
	int r_x = -1, r_y = -1, r_w = -1, r_h = -1;
	int max_size_r = 0;
	CMyClosedFigure* p_best_match_g_figure = NULL;
	int g_x = -1, g_y = -1, g_w = -1, g_h = -1;
	int max_size_g = 0;

	for (int id = 0; id < figures_b_N; id++)
	{
		CMyClosedFigure* pFigure = p_figures_b[id];
		int x = pFigure->m_minX, y = pFigure->m_minY, w = pFigure->width(), h = pFigure->height();
		int size = pFigure->m_PointsArray.m_size;

		if (((x + w) < (2 * width) / 3) && (x < width / 2) && (y > height / 30) && ((y + h / 2) < (3 * height) / 4))
		{
			if (size > max_size_l)
			{
				max_size_l = size;
				p_best_match_l_figure = pFigure;
				l_x = x;
				l_y = y;
				l_w = w;
				l_h = h;
			}
		}
	}

	if (p_best_match_l_figure)
	{
		int l, i, ii, x, y, n, min_y = -1, max_y = -1;
		int size = p_best_match_l_figure->m_PointsArray.m_size;
		int w = p_best_match_l_figure->width(), h = p_best_match_l_figure->height();
		int* fmask = new int[w * h];
		memset(fmask, 0, w * h);

		for (l = 0; l < size; l++)
		{
			i = p_best_match_l_figure->m_PointsArray[l];
			x = (i % width) - l_x;
			y = (i / width) - l_y;
			ii = (y * w) + x;
			fmask[ii] = 255;
		}

		for (y = 0, ii = 0; y < h; y++)
		{
			n = 0;
			for (x = 0; x < w; x++, ii++)
			{
				if (fmask[ii]) n++;
			}

			if (n >= w / 4)
			{
				if (min_y == -1) min_y = y;
				max_y = y;
			}
		}

		delete[] fmask;

		if (min_y == -1)
		{
			p_best_match_l_figure = NULL;
		}
		else
		{
			l_y += min_y;
			l_h = max_y - min_y + 1;
			l_cx = l_x + (l_w / 2);
			l_cy = l_y + (l_h / 2);
		}
	}

	if (!p_best_match_l_figure)
	{
		if (!ignore_error)
		{
			cv::Mat frame_upd;
			frame.copyTo(frame_upd);
			frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
			frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
			cv::rectangle(frame_upd, cv::Point(0, 0), cv::Point(max(5, ((2 * height) / 15) / 10), (2 * height) / 15), cv::Scalar(0, 255, 0));
			error_msg("ERROR: Failed to find big left vertical border blue color figure\n(min search size in top left green rectangle)", &frame, &frame_upd, NULL, 0, 0, (2 * width) / 3, (3 * height) / 4);
		}
		res = false;
	}
	else
	{
		// sorting b figures by m_minX in order from min to max
		for (int id1 = 0; id1 < figures_b_N - 1; id1++)
		{
			for (int id2 = id1 + 1; id2 < figures_b_N; id2++)
			{
				if (p_figures_b[id2]->m_minX < p_figures_b[id1]->m_minX)
				{
					CMyClosedFigure* pFigure = p_figures_b[id1];
					p_figures_b[id1] = p_figures_b[id2];
					p_figures_b[id2] = pFigure;
				}
			}
		}

		for (int id = 0; id < figures_b_N; id++)
		{
			CMyClosedFigure* pFigure = p_figures_b[id];
			int x = pFigure->m_minX, y = pFigure->m_minY, w = pFigure->width(), h = pFigure->height();
			int rcx = x + (w / 2);
			double wh_ratio = (double)w / (double)h;
			double rcxlcx_lh_ratio = (double)(rcx - l_cx) / (double)max(l_h, l_w);
			int size = pFigure->m_PointsArray.m_size;

			if (pFigure == p_best_match_l_figure)
			{
				continue;
			}

			if ( (x > l_x + l_w) && (y > height / 30) && (y + h < height - (height / 30)) && wh_ratio > 0.5 && wh_ratio < 2.0 &&
				rcxlcx_lh_ratio > g_max_telescopic_motor_rocker_arm_center_x_proportions)
			{
				if ( (x > r_x) && (size > (2*max_size_r)/3) )
				{
					p_best_match_r_figure = pFigure;
					r_x = x;
					r_y = y;
					r_w = w;
					r_h = h;
					if (size > max_size_r)
					{
						max_size_r = size;
					}
				}
			}
		}

		if (!p_best_match_r_figure)
		{
			if (!ignore_error)
			{
				cv::Mat frame_upd;
				frame.copyTo(frame_upd);
				frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
				frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
				int min_rcx = l_cx + (int)(g_max_telescopic_motor_rocker_arm_center_x_proportions * (double)max(l_h, l_w));
				cv::line(frame_upd, cv::Point(min_rcx, 0), cv::Point(min_rcx, height - 1), cv::Scalar(0, 0, 255), 5);

				error_msg(QString("ERROR: Failed to find big right blue color figure\n"
					"It should be located righter then red vertical line\n"
					"according settings max_telescopic_motor_rocker_arm_center_x_proportions:%3\n")
					.arg(g_max_telescopic_motor_rocker_arm_center_x_proportions),
					&frame, &frame_upd, NULL, l_x + ((3 * l_h) / 2), l_y - (l_h / 2), width, l_y + ((4 * l_h) / 2));
			}
			res = false;
		}
		else
		{
			int min_cw = 5;
			int min_sy = min(l_y, r_y);
			int max_sy = max(l_y + l_h, r_y + r_h);

			for (int id = 0; id < figures_b_N; id++)
			{
				CMyClosedFigure* pFigure = p_figures_b[id];
				int x = pFigure->m_minX, y = pFigure->m_minY, w = pFigure->width(), h = pFigure->height();
				int size = pFigure->m_PointsArray.m_size;

				if ((pFigure == p_best_match_l_figure) || (pFigure == p_best_match_r_figure))
				{
					continue;
				}

				if ( (x > l_x + l_w) && (x + w < r_x) && (y + h >= min_sy) && (y <= max_sy) && (w >= min_cw) )
				{
					if (size > max_size_c)
					{
						max_size_c = size;
						p_best_match_c_figure = pFigure;
					}
				}
			}

			int r_cx = r_x + (r_w / 2);
			int r_cy = r_y + (r_h / 2);
			int c_cx;
			int c_cy;

			double ccxlcx_lh_ratio_prev = g_ccxlcx_lh_ratio;

			if (r_cx == l_cx)
			{
				if (!ignore_error)
				{
					cv::Mat frame_upd;
					frame.copyTo(frame_upd);
					frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
					frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
					cv::rectangle(frame_upd, cv::Rect(l_x, l_y, l_w, l_h), cv::Scalar(0, 0, 255), 3);
					cv::circle(frame_upd, cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), int(max(r_w / 2, r_h / 2)), cv::Scalar(0, 0, 255), 3);
					error_msg(QString("ERROR: unexpected issue, centers of left and right blue figures are same\n"), &frame, &frame_upd);
				}
				res = false;
			}
			else
			{
				if (p_best_match_c_figure == NULL)
				{
					if (g_ccxlcx_lh_ratio > 0)
					{
						add_data = QString("[CENTER_POSITION_WAS_RESTORED_BY_PREVIOUS_DATA]\n") + add_data;

						c_cx = l_cx + (int)(g_ccxlcx_lh_ratio * (double)l_h);
						c_w = max(r_w, r_h);
						c_h = c_w;
						c_x = c_cx - (c_w / 2);
						c_cy = l_cy + (((r_cy - l_cy) * (c_cx - l_cx)) / (r_cx - l_cx));
						c_y = c_cy - (c_h / 2);
					}
					else
					{
						if (!ignore_error)
						{
							cv::Mat frame_upd;
							frame.copyTo(frame_upd);
							frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
							frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
							cv::rectangle(frame_upd, cv::Rect(l_x, l_y, l_w, l_h), cv::Scalar(0, 0, 255), 3);
							cv::circle(frame_upd, cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), int(max(r_w / 2, r_h / 2)), cv::Scalar(0, 0, 255), 3);
							error_msg(QString("ERROR: failed to get telescopic motor rocker arm center x position\n"), &frame, &frame_upd);
						}
						res = false;
					}
				}
				else
				{
					// get vector between centers of the left and right blue figures
					int v1_x = r_cx - l_cx;
					int v1_y = r_cy - l_cy;
					int l, ii, x, y, v2_x, v2_y, x_projection;
					int min_x_projection = width, max_x_projection = -1;
					int v1_vec_len_pow2 = (v1_x * v1_x) + (v1_y * v1_y);

					for (l = 0; l < p_best_match_c_figure->m_PointsArray.m_size; l++)
					{
						ii = p_best_match_c_figure->m_PointsArray[l];

						// getting c_figure point position
						x = ii % width;
						y = ii / width;

						// get v2 vector between center of the left blue figure and c_figure point position
						v2_x = x - l_cx;
						v2_y = y - l_cy;

						// getting x coordinate of projection of v2 vector on v1 vector by using scalar product of v1 and v2 vectors
						// it should be == l_cx + (v1_x*(v2_vec,v1_vec)/(v1_vec_len_pow2))
						x_projection = l_cx + ((v1_x * ((v1_x * v2_x) + (v1_y * v2_y))) / v1_vec_len_pow2);

						if (x_projection < min_x_projection)
						{
							min_x_projection = x_projection;
						}

						if (x_projection > max_x_projection)
						{
							max_x_projection = x_projection;
						}
					}

					c_cx = (min_x_projection + max_x_projection) / 2;
					c_cy = l_cy + (((r_cy - l_cy) * (c_cx - l_cx)) / (r_cx - l_cx));
					c_w = c_h = max(r_w, r_h);
					c_x = c_cx - (c_w / 2);
					c_y = c_cy - (c_h / 2);

					g_ccxlcx_lh_ratio = (double)(c_cx - l_cx) / (double)max(l_h, l_w);

					if ((g_ccxlcx_lh_ratio < g_min_telescopic_motor_rocker_arm_center_x_proportions) ||
						(g_ccxlcx_lh_ratio > g_max_telescopic_motor_rocker_arm_center_x_proportions))
					{
						if (!ignore_error)
						{
							cv::Mat img_res;
							frame.copyTo(img_res);

							img_res.setTo(cv::Scalar(255, 0, 0), GetFigureMask(p_best_match_l_figure, width, height));
							img_res.setTo(cv::Scalar(255, 255, 0), GetFigureMask(p_best_match_r_figure, width, height));
							img_res.setTo(cv::Scalar(255, 0, 255), GetFigureMask(p_best_match_c_figure, width, height));

							cv::rectangle(img_res, cv::Rect(l_x, l_y, l_w, l_h), cv::Scalar(0, 0, 255), 3);
							cv::circle(img_res, cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), int(max(r_w / 2, r_h / 2)), cv::Scalar(0, 0, 255), 3);
							cv::circle(img_res, cv::Point(c_x + int(c_w / 2), c_y + int(c_h / 2)), int(max(c_w / 2, c_h / 2)), cv::Scalar(0, 0, 255), 3);

							cv::line(img_res, cv::Point(c_cx, c_cy - int(c_h / 4)), cv::Point(c_cx, c_cy + int(c_h / 4)), cv::Scalar(0, 170, 0), 5);
							cv::line(img_res, cv::Point(l_x + int(l_w / 2), l_y + int(l_h / 2)), cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), cv::Scalar(0, 170, 0), 5);

							error_msg(QString("ERROR: got strange center position:\n"
								"telescopic_motor_rocker_arm_center_x_proportions:%1\n"
								"min_telescopic_motor_rocker_arm_center_x_proportions:%2\n"
								"max_telescopic_motor_rocker_arm_center_x_proportions:%3\n")
								.arg(g_ccxlcx_lh_ratio)
								.arg(g_min_telescopic_motor_rocker_arm_center_x_proportions)
								.arg(g_max_telescopic_motor_rocker_arm_center_x_proportions),
								&frame, &img_res);
						}
						res = false;
					}
					else
					{
						if (ccxlcx_lh_ratio_prev > 0)
						{
							double dif = (max(g_ccxlcx_lh_ratio, ccxlcx_lh_ratio_prev) / min(g_ccxlcx_lh_ratio, ccxlcx_lh_ratio_prev) - 1.0)*100.0;

							if (dif > g_max_ccxlcx_lh_ratio_prev_to_cur_dif)
							{
								g_max_ccxlcx_lh_ratio_prev_to_cur_dif = dif;
							}

							// more then 10%
							if (dif > 10.0)
							{
								if (!ignore_error)
								{
									cv::Mat img_res;
									frame.copyTo(img_res);

									img_res.setTo(cv::Scalar(255, 0, 0), GetFigureMask(p_best_match_l_figure, width, height));
									img_res.setTo(cv::Scalar(255, 255, 0), GetFigureMask(p_best_match_r_figure, width, height));
									img_res.setTo(cv::Scalar(255, 0, 255), GetFigureMask(p_best_match_c_figure, width, height));

									{
										int c_cx_exp = l_cx + (int)(ccxlcx_lh_ratio_prev * (double)l_h);
										int c_w_exp = max(r_w, r_h);
										int c_h_exp = c_w_exp;
										int c_x_exp = c_cx_exp - (c_w_exp / 2);
										int c_cy_exp = l_cy + (((r_cy - l_cy) * (c_cx_exp - l_cx)) / (r_cx - l_cx));
										int c_y_exp = c_cy_exp - (c_h_exp / 2);
										cv::circle(img_res, cv::Point(c_x_exp + int(c_w_exp / 2), c_y_exp + int(c_h_exp / 2)), int(max(c_w_exp / 2, c_h_exp / 2)), cv::Scalar(0, 255, 255), 2);
										cv::line(img_res, cv::Point(c_cx_exp, c_cy_exp - int(c_h_exp / 4)), cv::Point(c_cx_exp, c_cy_exp + int(c_h_exp / 4)), cv::Scalar(0, 255, 255), 2);
									}

									cv::rectangle(img_res, cv::Rect(l_x, l_y, l_w, l_h), cv::Scalar(0, 0, 255), 3);
									cv::circle(img_res, cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), int(max(r_w / 2, r_h / 2)), cv::Scalar(0, 0, 255), 3);
									cv::circle(img_res, cv::Point(c_x + int(c_w / 2), c_y + int(c_h / 2)), int(max(c_w / 2, c_h / 2)), cv::Scalar(0, 0, 255), 3);

									cv::line(img_res, cv::Point(c_cx, c_cy - int(c_h / 4)), cv::Point(c_cx, c_cy + int(c_h / 4)), cv::Scalar(0, 170, 0), 5);
									cv::line(img_res, cv::Point(l_x + int(l_w / 2), l_y + int(l_h / 2)), cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), cv::Scalar(0, 170, 0), 5);

									error_msg(QString("ERROR: got strange center position of rocker arm rotation:\n"
										"center position is different from previously obtained with diff: %1\%\n")
										.arg(dif),
										&frame, &img_res, &prev_frame);
								}
								res = false;
							}
						}

						frame.copyTo(prev_frame);
					}
				}

				if (res)
				{
					for (int id = 0; id < figures_g.m_size; id++)
					{
						CMyClosedFigure* pFigure = &(figures_g[id]);
						int x = pFigure->m_minX, y = pFigure->m_minY, w = pFigure->width(), h = pFigure->height();
						int size = pFigure->m_PointsArray.m_size;

						int g_to_c_distance_pow2 = pow2((x + (w / 2)) - c_cx) + pow2((y + (h / 2)) - c_cy);
						int g_to_r_distance_pow2 = pow2((x + (w / 2)) - r_cx) + pow2((y + (h / 2)) - r_cy);

						if (g_to_r_distance_pow2 > 0)
						{
							if ((double)g_to_c_distance_pow2 / g_to_r_distance_pow2 <= g_max_telescopic_motor_rocker_arm_proportions * g_max_telescopic_motor_rocker_arm_proportions)
							{
								if (size > max_size_g)
								{
									max_size_g = size;
									p_best_match_g_figure = pFigure;
									g_x = x;
									g_y = y;
									g_w = w;
									g_h = h;
								}
							}
						}
					}

					if (!p_best_match_g_figure)
					{
						if (!ignore_error)
						{
							cv::Mat frame_upd;
							frame.copyTo(frame_upd);
							frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
							frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
							error_msg("ERROR: Failed to find green color figure", &frame, &frame_upd, NULL, l_x, min(l_y, r_y), r_x, min(l_y + l_h, r_y + r_h));
						}
						res = false;
					}
					else
					{
						int g_cx = g_x + (g_w / 2), g_cy = g_y + (g_h / 2);

						double g_to_c_distance = sqrt((double)(pow2(g_cx - c_cx) + pow2(g_cy - c_cy)));
						double r_to_c_distance = sqrt((double)(pow2(r_cx - c_cx) + pow2(r_cy - c_cy)));

						if (g_to_c_distance * r_to_c_distance == 0)
						{
							if (!ignore_error)
							{
								cv::Mat frame_upd;
								frame.copyTo(frame_upd);
								frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
								frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
								error_msg("ERROR: unexpected issue: g_to_c_distance * r_to_c_distance == 0", &frame, &frame_upd);
							}
							res = false;
						}
						else
						{
							int g_to_c_cx = g_cx - c_cx;
							int g_to_c_cy_inv = -(g_cy - c_cy);

							int r_to_c_cx = r_cx - c_cx;
							int r_to_c_cy_inv = -(r_cy - c_cy);

							// From scalar vector multiplication a_vec * b_vac
							// cos(alpha) = (a_x*b_x + a_y*b_y)/|a|*|b|
							// std::acos return: [0, M_PI] aacording https://en.cppreference.com/w/cpp/numeric/math/acos
							double alpha = std::acos((double)((g_to_c_cx * r_to_c_cx) + (g_to_c_cy_inv * r_to_c_cy_inv)) / (g_to_c_distance * r_to_c_distance));

							// From cross product (vector product of vectors) [a_vec * b_vac]
							// [a_vec * b_vac] = ax*by-ay*bx
							int cross_product = g_to_c_cx * r_to_c_cy_inv - g_to_c_cy_inv * r_to_c_cx;

							if (cross_product >= 0)
							{
								pos = (double)((M_PI - alpha) * 180.0) / M_PI;
							}
							else
							{
								pos = (double)((M_PI + alpha) * 180.0) / M_PI;
							}

							res = true;

							if (show_results)
							{
								QueryPerformanceCounter(&t2);
								int dt = time_diff_in_milliseconds(t2, start_time, Frequency);
								int dt1 = time_diff_in_milliseconds(t1, start_time, Frequency);

								cv::Mat img_res = frame.clone();

								img_res.setTo(cv::Scalar(255, 0, 0), GetFigureMask(p_best_match_l_figure, width, height));
								img_res.setTo(cv::Scalar(255, 0, 0), GetFigureMask(p_best_match_r_figure, width, height));
								img_res.setTo(cv::Scalar(255, 0, 0), GetFigureMask(p_best_match_c_figure, width, height));
								img_res.setTo(cv::Scalar(0, 255, 0), GetFigureMask(p_best_match_g_figure, width, height));

								if (ccxlcx_lh_ratio_prev > 0)
								{
									int c_cx_exp = l_cx + (int)(ccxlcx_lh_ratio_prev * (double)l_h);
									int c_w_exp = max(r_w, r_h);
									int c_h_exp = c_w_exp;
									int c_x_exp = c_cx_exp - (c_w_exp / 2);
									int c_cy_exp = l_cy + (((r_cy - l_cy) * (c_cx_exp - l_cx)) / (r_cx - l_cx));
									int c_y_exp = c_cy_exp - (c_h_exp / 2);
									cv::rectangle(img_res, cv::Rect(c_x_exp, c_y_exp, c_w_exp, c_h_exp), cv::Scalar(0, 255, 255), 1);
								}

								cv::rectangle(img_res, cv::Rect(l_x, l_y, l_w, l_h), cv::Scalar(0, 0, 255), 3);
								cv::circle(img_res, cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), int(max(r_w / 2, r_h / 2)), cv::Scalar(0, 0, 255), 3);
								cv::circle(img_res, cv::Point(c_x + int(c_w / 2), c_y + int(c_h / 2)), int(max(c_w / 2, c_h / 2)), cv::Scalar(0, 0, 255), 3);
								cv::circle(img_res, cv::Point(g_x + int(g_w / 2), g_y + int(g_h / 2)), int(max(g_w / 2, g_h / 2)), cv::Scalar(0, 0, 255), 3);

								cv::line(img_res, cv::Point(c_x + int(c_w / 2), c_y + int(c_h / 2)), cv::Point(g_x + int(g_w / 2), g_y + int(g_h / 2)), cv::Scalar(0, 170, 0), 5);
								cv::line(img_res, cv::Point(l_x + int(l_w / 2), l_y + int(l_h / 2)), cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), cv::Scalar(0, 170, 0), 5);

								int cur_speed = -1;
								int dt_get_speed = -1;
								if (p_cur_speed)
								{
									cur_speed = *p_cur_speed;
								}

								double g_to_c_distance = (int)sqrt((double)(pow2(g_cx - c_cx) + pow2(g_cy - c_cy)));
								double g_to_r_distance = (int)sqrt((double)(pow2(g_cx - r_cx) + pow2(g_cy - r_cy)));
								double telescopic_motor_rocker_arm_proportions = g_to_r_distance > 0 ? g_to_c_distance / g_to_r_distance : 0;

								QString text = QString::asprintf("%s" "pos: %d cur_speed: %d\ntelescopic_motor_rocker_arm_proportions: %.03f\ntelescopic_motor_rocker_arm_center_x_proportions: %.03f max_prev_to_cur_dif: %.02f\%\nperformance data: dt_get_pos_total: %03d dt_get_pos_conversion: %03d", add_data.toStdString().c_str(), pos, cur_speed, telescopic_motor_rocker_arm_proportions, g_ccxlcx_lh_ratio, g_max_ccxlcx_lh_ratio_prev_to_cur_dif, dt, dt1);

								draw_text(text, img_res);

								if (p_res_frame)
								{
									img_res.copyTo(*p_res_frame);
								}

								show_frame_in_cv_window(title, img_res);
							}
						}
					}
				}
			}
		}
	}

	return res;
}

cv::String VideoTimeToStr(__int64 pos)
{
	cv::String str;
	int hour, min, sec, msec, val;

	val = (int)(pos / 1000); // seconds
	msec = pos - ((__int64)val * (__int64)1000);
	hour = val / 3600;
	val -= hour * 3600;
	min = val / 60;
	val -= min * 60;
	sec = val;

	str = cv::format("%d:%02d:%02d:%03d", hour, min, sec, msec);

	return str;
}

int get_loc(int pos)
{
	int loc = 0;

	if (pos < 0)
	{
		pos += 360;
	}

	if ((pos >= 45) && (pos < (90 + 45)))
	{
		loc = 1;
	}
	else if ((pos >= (90 + 45)) && (pos < (180 + 45)))
	{
		loc = 2;
	}
	else if ((pos >= (180 + 45)) && (pos < (270 + 45)))
	{
		loc = 3;
	}

	return loc;
}

void test_err_frame(QString fpath)
{
	cv::Mat data;
	QFile f(fpath);
	if (!f.open(QIODevice::ReadOnly)) return;
	QByteArray blob = f.readAll();
	size_t size = blob.size();
	data.reserveBuffer(size);
	memcpy(data.data, blob.data(), size);
	cv::Mat frame = cv::imdecode(data, cv::IMREAD_COLOR); // load in BGR format

	g_save_images = false;

	int pos;
	get_hismith_pos_by_image(frame, pos, false, true);

	cv::String title("Test Error Frame");
	cv::namedWindow(title, 1);
	cv::setWindowProperty(title, cv::WND_PROP_TOPMOST, 1);
	int sw = (int)GetSystemMetrics(SM_CXSCREEN);
	int sh = (int)GetSystemMetrics(SM_CYSCREEN);
	cv::moveWindow(title, (sw - g_webcam_frame_width) / 2, (sh - g_webcam_frame_height) / 2);

	int B_range[3][2];
	int G_range[3][2];
	std::copy(&g_B_range[0][0], &g_B_range[0][0] + 3 * 2, &B_range[0][0]);
	std::copy(&g_G_range[0][0], &g_G_range[0][0] + 3 * 2, &G_range[0][0]);

	while (1)
	{
		int width = frame.cols;
		int height = frame.rows;

		cv::Mat img, img_b, img_g, img_intersection;
		cv::cvtColor(frame, img, cv::COLOR_BGR2YUV);

		concurrency::parallel_invoke(
			[&img, &img_b, &B_range, width, height] {
				get_binary_image(img, B_range, img_b, 3);
			},
			[&img, &img_g, &G_range, width, height] {
				get_binary_image(img, G_range, img_g, 3);
			}
			);

		frame.copyTo(img);

		img.setTo(cv::Scalar(255, 0, 0), img_b);
		img.setTo(cv::Scalar(0, 255, 0), img_g);

		cv::bitwise_and(img_b, img_g, img_intersection);
		img.setTo(cv::Scalar(0, 0, 255), img_intersection);

		draw_text(QString(
			"NOTE: intersection points shown as Red and will be treated as related to green for get device position\n"
			"Press 'Enter' for use current colors as original colors. Current vs original colors:\n"
			"b[%1(%2)-%3(%4)][%5(%6)-%7(%8)][%9(%10)-%11(%12)]\n"
			"g[%13(%14)-%15(%16)][%17(%18)-%19(%20)][%21(%22)-%23(%24)]\n"
			"Press b[q/w/e/r][a/s/d/f][z/x/c/v] | g[t/y/u/i][g/h/j/k][b/n/m/,] for change colors")
			.arg(B_range[0][0])
			.arg(g_B_range[0][0])
			.arg(B_range[0][1])
			.arg(g_B_range[0][1])
			.arg(B_range[1][0])
			.arg(g_B_range[1][0])
			.arg(B_range[1][1])
			.arg(g_B_range[1][1])
			.arg(B_range[2][0])
			.arg(g_B_range[2][0])
			.arg(B_range[2][1])
			.arg(g_B_range[2][1])
			.arg(G_range[0][0])
			.arg(g_G_range[0][0])
			.arg(G_range[0][1])
			.arg(g_G_range[0][1])
			.arg(G_range[1][0])
			.arg(g_G_range[1][0])
			.arg(G_range[1][1])
			.arg(g_G_range[1][1])
			.arg(G_range[2][0])
			.arg(g_G_range[2][0])
			.arg(G_range[2][1])
			.arg(g_G_range[2][1])
			, img);
		show_frame_in_cv_window(title, img);

		int key = cv::waitKey(0);

		if ((key == 27 /* Esc key */) ||
			((key == -1) && (cv::getWindowProperty(title, cv::WND_PROP_VISIBLE) != 1.0)))
		{
			break;
		}

		else if (key == 13) // Enter key
		{
			std::copy(&B_range[0][0], &B_range[0][0] + 3 * 2, &g_B_range[0][0]);
			std::copy(&G_range[0][0], &G_range[0][0] + 3 * 2, &g_G_range[0][0]);
		}

		else if (key == 'w')
		{
			B_range[0][0] = min(B_range[0][0] + 1, 255);
		}
		else if (key == 'q')
		{
			B_range[0][0] = max(B_range[0][0] - 1, 0);
		}
		else if (key == 'r')
		{
			B_range[0][1] = min(B_range[0][1] + 1, 255);
		}
		else if (key == 'e')
		{
			B_range[0][1] = max(B_range[0][1] - 1, 0);
		}

		else if (key == 's')
		{
			B_range[1][0] = min(B_range[1][0] + 1, 255);
		}
		else if (key == 'a')
		{
			B_range[1][0] = max(B_range[1][0] - 1, 0);
		}
		else if (key == 'f')
		{
			B_range[1][1] = min(B_range[1][1] + 1, 255);
		}
		else if (key == 'd')
		{
			B_range[1][1] = max(B_range[1][1] - 1, 0);
		}

		else if (key == 'x')
		{
			B_range[2][0] = min(B_range[2][0] + 1, 255);
		}
		else if (key == 'z')
		{
			B_range[2][0] = max(B_range[2][0] - 1, 0);
		}
		else if (key == 'v')
		{
			B_range[2][1] = min(B_range[2][1] + 1, 255);
		}
		else if (key == 'c')
		{
			B_range[2][1] = max(B_range[2][1] - 1, 0);
		}

		//-----------------------

		else if (key == 'y')
		{
			G_range[0][0] = min(G_range[0][0] + 1, 255);
		}
		else if (key == 't')
		{
			G_range[0][0] = max(G_range[0][0] - 1, 0);
		}
		else if (key == 'i')
		{
			G_range[0][1] = min(G_range[0][1] + 1, 255);
		}
		else if (key == 'u')
		{
			G_range[0][1] = max(G_range[0][1] - 1, 0);
		}

		else if (key == 'h')
		{
			G_range[1][0] = min(G_range[1][0] + 1, 255);
		}
		else if (key == 'g')
		{
			G_range[1][0] = max(G_range[1][0] - 1, 0);
		}
		else if (key == 'k')
		{
			G_range[1][1] = min(G_range[1][1] + 1, 255);
		}
		else if (key == 'j')
		{
			G_range[1][1] = max(G_range[1][1] - 1, 0);
		}

		else if (key == 'n')
		{
			G_range[2][0] = min(G_range[2][0] + 1, 255);
		}
		else if (key == 'b')
		{
			G_range[2][0] = max(G_range[2][0] - 1, 0);
		}
		else if (key == ',')
		{
			G_range[2][1] = min(G_range[2][1] + 1, 255);
		}
		else if (key == 'm')
		{
			G_range[2][1] = max(G_range[2][1] - 1, 0);
		}
	}

	g_save_images = true;
}

void ThreadedCapture::start(cv::VideoCapture* p_capture) {
	stop();

	std::lock_guard<std::mutex> lock(cap_mutex);
	p_cap = p_capture;
	if (p_cap && p_cap->isOpened())
	{
		is_running = true;
		capture_thread = std::thread(&ThreadedCapture::capture_loop, this);
	}
}

void ThreadedCapture::stop() {
	is_running = false;
	if (capture_thread.joinable()) {
		capture_thread.join();
	}
}

void ThreadedCapture::capture_loop() {
	cv::Mat local_frame;
	__int64 msec_frame_prev_pos = -1;
	__int64 msec_frame_cur_pos;

	while (is_running)
	{
		if (p_cap->read(local_frame) && !g_stop_run)
		{
			msec_frame_cur_pos = p_cap->get(cv::CAP_PROP_POS_MSEC);

			if (msec_frame_cur_pos != msec_frame_prev_pos && !local_frame.empty())
			{
				msec_frame_prev_pos = msec_frame_cur_pos;

				{
					std::lock_guard<std::mutex> lock(cap_mutex);
					local_frame.copyTo(latest_frame);
					msec_pos_latest_frame = msec_frame_cur_pos;
					has_new_frame = true;
					cvar.notify_one();
				}
			}
		}
		else {
			std::lock_guard<std::mutex> lock(cap_mutex);
			has_new_frame = false;
			is_running = false;
			cvar.notify_all();
			break;
		}

		std::this_thread::yield();
	}
}

bool ThreadedCapture::wait_and_get_fresh_frame(cv::Mat& output_frame, __int64& msec_pos_output_frame) {
	if (!is_running) return false;

	std::unique_lock<std::mutex> lock(cap_mutex);

	cvar.wait(lock, [this] { return this->has_new_frame || !this->is_running; });

	if (!is_running) return false;

	latest_frame.copyTo(output_frame);
	msec_pos_output_frame = msec_pos_latest_frame;
	has_new_frame = false;

	return true;
}

void set_webcam_fps(cv::VideoCapture& capture)
{
	if (g_webcam_fps > 0)
	{
		capture.set(cv::CAP_PROP_FPS, g_webcam_fps);
	}

	cv::Mat frame;
	capture.read(frame);

	double cur_webcam_fps = capture.get(cv::CAP_PROP_FPS);
	if (cur_webcam_fps < 10)
	{
		error_msg(QString("ERROR: got strange webcam fps:%1 from capture.get(cv::CAP_PROP_FPS) please try manually set webcam_fps in settings.xml to 30, 60, etc").arg(g_webcam_fps));
	}

	if ((g_webcam_fps > 0) && (cur_webcam_fps != g_webcam_fps))
	{
		show_msg(QString("Camera doesn't support %1 fps, and currently use %2 fps")
			.arg(g_webcam_fps)
			.arg(cur_webcam_fps),
			5000, MessageType::Always);
	}

	g_webcam_fps = cur_webcam_fps;
}

enum StandardCaptureModes {
	CAP_MODE_BGR = 0,
	CAP_MODE_RGB = 1,
	CAP_MODE_GRAY = 2,
	CAP_MODE_YUYV = 22,
	CAP_MODE_YV12 = 23,
	CAP_MODE_I420 = 24,
	CAP_MODE_NV12 = 25,
	CAP_MODE_MJPEG = 26
};

class CaptureModeUtils {
public:
	static QString to_string(int mode) {
		switch (mode) {
		case StandardCaptureModes::CAP_MODE_BGR:   return "Uncompressed BGR (Native OpenCV)";
		case StandardCaptureModes::CAP_MODE_RGB:   return "Uncompressed RGB";
		case StandardCaptureModes::CAP_MODE_GRAY:  return "Grayscale (8-bit)";
		case StandardCaptureModes::CAP_MODE_YUYV:  return "YUY2 / YUYV (Raw Uncompressed Stream)";
		case StandardCaptureModes::CAP_MODE_YV12:  return "YV12 (YUV 4:2:0 Planar)";
		case StandardCaptureModes::CAP_MODE_I420:  return "I420 / IYUV (YUV 4:2:0)";
		case StandardCaptureModes::CAP_MODE_NV12:  return "NV12 (Bi-Planar YUV 4:2:0)";
		case StandardCaptureModes::CAP_MODE_MJPEG: return "MJPEG (Motion JPEG Compressed)";
		default:                                   return QString("UNKNOWN FORMAT (%1)").arg(mode);
		}
	}
};

void set_camera_settings(cv::VideoCapture& capture)
{
	int fourcc_yuy2 = cv::VideoWriter::fourcc('Y', 'U', 'Y', '2');
	int fourcc_mjpg = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
	int cur_fourcc = static_cast<int>(capture.get(cv::CAP_PROP_FOURCC));

	if (cur_fourcc != fourcc_yuy2 && cur_fourcc != StandardCaptureModes::CAP_MODE_YUYV)
	{
		capture.set(cv::CAP_PROP_FOURCC, fourcc_yuy2);
		cur_fourcc = static_cast<int>(capture.get(cv::CAP_PROP_FOURCC));

		if (cur_fourcc != fourcc_yuy2 && cur_fourcc != StandardCaptureModes::CAP_MODE_YUYV)
		{
			show_msg(QString("fourcc:YUY2 is not supported by Webcam\n"
				"fourcc:%1 currently is used")
				.arg(CaptureModeUtils::to_string(cur_fourcc)), 5000, MessageType::Always);
		}
	}

	capture.set(cv::CAP_PROP_FRAME_WIDTH, g_webcam_frame_width);
	capture.set(cv::CAP_PROP_FRAME_HEIGHT, g_webcam_frame_height);

	set_webcam_fps(capture);

	if (g_webcam_focus >= 0)
	{
		capture.set(cv::CAP_PROP_AUTOFOCUS, 0);
		capture.set(cv::CAP_PROP_FOCUS, g_webcam_focus);
	}

	capture.set(cv::CAP_PROP_BUFFERSIZE, 1);

	int webcam_frame_width = capture.get(cv::CAP_PROP_FRAME_WIDTH);
	int webcam_frame_height = capture.get(cv::CAP_PROP_FRAME_HEIGHT);

	if ((webcam_frame_width != g_webcam_frame_width) || (webcam_frame_height != g_webcam_frame_height))
	{
		show_msg(QString("%1:%2 resolution is not supported by Webcam\n"
			"%3:%4 resolution is currently used by Webcam")
			.arg(g_webcam_frame_width)
			.arg(g_webcam_frame_height)
			.arg(webcam_frame_width)
			.arg(webcam_frame_height), 5000, MessageType::Always);
		g_webcam_frame_width = webcam_frame_width;
		g_webcam_frame_height = webcam_frame_height;
	}
}

bool init_camera(cv::VideoCapture &capture)
{
	bool res = false;
	g_webcam_msmf_supported = false;

	int video_dev_id = get_video_dev_id();
	if (video_dev_id == -1)
	{
		return false;
	}

	show_msg("", 0, MessageType::Clean);
	show_msg("Connecting to Webcam with API: CAP_MSMF ...\n", 120000, MessageType::Always);

	capture.open(video_dev_id, cv::CAP_MSMF);

	if (capture.isOpened())
	{
		set_camera_settings(capture);

		cv::Mat frame;
		for (int i = 0; i < 10; i++)
		{
			capture.read(frame);
		}

		double msec = capture.get(cv::CAP_PROP_POS_MSEC);

		if (msec <= 0.0) {
			show_msg(QString("API: CAP_MSMF is not supported by Webcam, returned CAP_PROP_POS_MSEC:%1 <= 0.0").arg(msec), 5000, MessageType::Always);
			capture.release();
		}
		else
		{
			g_webcam_msmf_supported = true;
			res = true;
		}
	}
	else
	{
		show_msg("API: CAP_MSMF is not supported by Webcam, failed to start CAP_MSMF", 5000, MessageType::Always);
	}

	if (!g_webcam_msmf_supported)
	{
		show_msg("Connecting to Webcam with default API ...\n", 120000, MessageType::Always);
		capture.open(video_dev_id);

		if (capture.isOpened())
		{
			g_high_precision_timer_guard.Start();

			set_camera_settings(capture);

			res = true;
		}
		else
		{
			error_msg("ERROR: Failed to start Webcam video capture");
		}
	}

	if (res)
	{
		cv::Mat frame;
		__int64 msec_video_cur_pos;

		LARGE_INTEGER cur_time, frequency;
		__int64 pc_current_time_ms;
		QueryPerformanceFrequency(&frequency);
		bool delta_cur_vs_video_time_was_init = false;
		g_delta_cur_vs_video_time = -1;

		show_msg("Geting first ~5 secons Webcam frames for get better Webcam focus and sync...\n", 5000, MessageType::Always);

		// geting first 5 * g_webcam_fps (~5 seconds) frames for get better camera focus and correct g_delta_cur_vs_video_time
		for (int i = 0; i < 4 * g_webcam_fps; i++)
		{
			get_new_camera_frame(capture, frame, msec_video_cur_pos);
		}
		for (int i = 0; i < g_webcam_fps; i++)
		{
			get_new_camera_frame(capture, frame, msec_video_cur_pos);
			QueryPerformanceCounter(&cur_time);
			pc_current_time_ms = (cur_time.QuadPart * (__int64)1000) / frequency.QuadPart;
			if (delta_cur_vs_video_time_was_init)
			{
				g_delta_cur_vs_video_time = min(g_delta_cur_vs_video_time, pc_current_time_ms - msec_video_cur_pos);
			}
			else
			{
				g_delta_cur_vs_video_time = pc_current_time_ms - msec_video_cur_pos;
				delta_cur_vs_video_time_was_init = true;
			}
		}

		show_msg("", 0, MessageType::Clean);

		if (msec_video_cur_pos <= 0.0) {
			g_high_precision_timer_guard.Stop();
			capture.release();
			error_msg(QString("ERROR: CAP_PROP_POS_MSEC:%1 <= 0.0").arg(msec_video_cur_pos));
			res = false;
		}
	}

	return res;
}

struct SelectionState {
	bool is_drawing = false;
	bool is_finished = false;
	cv::Point start_point;
	cv::Point end_point;
	cv::Rect selection_rect;
	bool ctrl_pressed = false;
	bool shift_pressed = false;
};

SelectionState g_selection;

void on_mouse_click(int event, int x, int y, int flags, void* userdata) {
	SelectionState* state = reinterpret_cast<SelectionState*>(userdata);

	state->ctrl_pressed = (flags & cv::EVENT_FLAG_CTRLKEY) != 0;
	state->shift_pressed = (flags & cv::EVENT_FLAG_SHIFTKEY) != 0;

	if (event == cv::EVENT_LBUTTONDOWN) {
		if (state->ctrl_pressed || state->shift_pressed) {
			state->is_drawing = true;
			state->start_point = cv::Point(x, y);
			state->end_point = cv::Point(x, y);
			state->selection_rect = cv::Rect();
		}
	}

	else if (event == cv::EVENT_MOUSEMOVE && state->is_drawing) {
		state->end_point = cv::Point(x, y);
		state->selection_rect = cv::Rect(state->start_point, state->end_point);
	}

	else if (event == cv::EVENT_LBUTTONUP && state->is_drawing) {
		state->is_drawing = false;
		state->is_finished = true;
		state->end_point = cv::Point(x, y);
		state->selection_rect = cv::Rect(state->start_point, state->end_point);
	}
}

void calculate_yuv_range(const cv::Mat& yuv_img, cv::Rect selection_rect, int(&range)[3][2])
{
	range[0][0] = 0; range[0][1] = 255; // Y
	range[1][0] = 0; range[1][1] = 255; // U
	range[2][0] = 0; range[2][1] = 255; // V

	cv::Rect safe_rect = selection_rect & cv::Rect(0, 0, yuv_img.cols, yuv_img.rows);

	cv::Mat roi = yuv_img(safe_rect);

	cv::Scalar mean, stddev;
	cv::meanStdDev(roi, mean, stddev);

	double sigma_multiplier = 2.0;

	for (int i = 0; i < 3; ++i) {
		double min_val = mean[i] - max(sigma_multiplier * stddev[i], 30);
		double max_val = mean[i] + max(sigma_multiplier * stddev[i], 30);
		range[i][0] = max(0, min_val);
		range[i][1] = min(255, max_val);
	}
}

void test_camera()
{
	cv::VideoCapture capture;

	if (init_camera(capture))
	{
		cv::Mat frame;
		int B_range[3][2];
		int G_range[3][2];
		cv::String title("Test Webcam");

		std::copy(&g_B_range[0][0], &g_B_range[0][0] + 3 * 2, &B_range[0][0]);
		std::copy(&g_G_range[0][0], &g_G_range[0][0] + 3 * 2, &G_range[0][0]);

		cv::namedWindow(title, 1);
		cv::setWindowProperty(title, cv::WND_PROP_TOPMOST, 1);
		int sw = (int)GetSystemMetrics(SM_CXSCREEN);
		int sh = (int)GetSystemMetrics(SM_CYSCREEN);
		cv::moveWindow(title, (sw - g_webcam_frame_width) / 2, (sh - g_webcam_frame_height) / 2);

		cv::setMouseCallback(title, on_mouse_click, &g_selection);

		while (capture.read(frame))
		{
			int width = frame.cols;
			int height = frame.rows;

			cv::Mat img, img_b, img_g, img_intersection;
			bool is_ctrl_held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
			bool is_shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

			if (!is_ctrl_held && !is_shift_held)
			{
				cv::cvtColor(frame, img, cv::COLOR_BGR2YUV);

				if (g_selection.is_finished)
				{
					if (g_selection.ctrl_pressed || g_selection.shift_pressed)
					{
						if (g_selection.ctrl_pressed)
						{
							calculate_yuv_range(img, g_selection.selection_rect, B_range);
						}
						else
						{
							calculate_yuv_range(img, g_selection.selection_rect, G_range);
						}
					}
					g_selection.is_finished = false;
					g_selection.ctrl_pressed = false;
					g_selection.shift_pressed = false;
				}

				concurrency::parallel_invoke(
					[&img, &img_b, &B_range, width, height] {
						get_binary_image(img, B_range, img_b, 3);
					},
					[&img, &img_g, &G_range, width, height] {
						get_binary_image(img, G_range, img_g, 3);
					}
				);

				frame.copyTo(img);

				img.setTo(cv::Scalar(255, 0, 0), img_b);
				img.setTo(cv::Scalar(0, 255, 0), img_g);

				cv::bitwise_and(img_b, img_g, img_intersection);
				img.setTo(cv::Scalar(0, 0, 255), img_intersection);
			}
			else
			{
				frame.copyTo(img);
			}

			draw_text(QString(
				"NOTE: intersection points shown as Red and will be treated as related to green for get device position\n"
				"Press 'Enter' for use current colors as original colors. Current vs original colors:\n"
				"b[%1(%2)-%3(%4)][%5(%6)-%7(%8)][%9(%10)-%11(%12)]\n"
				"g[%13(%14)-%15(%16)][%17(%18)-%19(%20)][%21(%22)-%23(%24)]\n"
				"Press b[q/w/e/r][a/s/d/f][z/x/c/v] | g[t/y/u/i][g/h/j/k][b/n/m/,] for change colors\n"
				"Or hold Ctrl or Shift with select area by left mouse button for auto detect color\n"
				"fps: %25 focus: %26 press '[' or ']' for change focus")
				.arg(B_range[0][0])
				.arg(g_B_range[0][0])
				.arg(B_range[0][1])
				.arg(g_B_range[0][1])
				.arg(B_range[1][0])
				.arg(g_B_range[1][0])
				.arg(B_range[1][1])
				.arg(g_B_range[1][1])
				.arg(B_range[2][0])
				.arg(g_B_range[2][0])
				.arg(B_range[2][1])
				.arg(g_B_range[2][1])
				.arg(G_range[0][0])
				.arg(g_G_range[0][0])
				.arg(G_range[0][1])
				.arg(g_G_range[0][1])
				.arg(G_range[1][0])
				.arg(g_G_range[1][0])
				.arg(G_range[1][1])
				.arg(g_G_range[1][1])
				.arg(G_range[2][0])
				.arg(g_G_range[2][0])
				.arg(G_range[2][1])
				.arg(g_G_range[2][1])
				.arg(g_webcam_fps)
				.arg(g_webcam_focus)
				, img);

			if (g_selection.is_drawing) {
				cv::Scalar color = g_selection.ctrl_pressed ? cv::Scalar(255, 0, 0) : cv::Scalar(0, 255, 0);
				cv::rectangle(img, g_selection.selection_rect, cv::Scalar(0, 0, 0), 4, cv::LINE_AA);
				cv::rectangle(img, g_selection.selection_rect, color, 1, cv::LINE_AA);
			}

			show_frame_in_cv_window(title, img);

			int key = cv::waitKey(1);

			if ((key == 27 /* Esc key */) ||
				((key == -1) && (cv::getWindowProperty(title, cv::WND_PROP_VISIBLE) != 1.0)))
			{
				break;
			}

			else if (key == 13) // Enter key
			{
				std::copy(&B_range[0][0], &B_range[0][0] + 3 * 2, &g_B_range[0][0]);
				std::copy(&G_range[0][0], &G_range[0][0] + 3 * 2, &g_G_range[0][0]);
			}

			else if (key == 'w')
			{
				B_range[0][0] = min(B_range[0][0] + 1, 255);
			}
			else if (key == 'q')
			{
				B_range[0][0] = max(B_range[0][0] - 1, 0);
			}
			else if (key == 'r')
			{
				B_range[0][1] = min(B_range[0][1] + 1, 255);
			}
			else if (key == 'e')
			{
				B_range[0][1] = max(B_range[0][1] - 1, 0);
			}

			else if (key == 's')
			{
				B_range[1][0] = min(B_range[1][0] + 1, 255);
			}
			else if (key == 'a')
			{
				B_range[1][0] = max(B_range[1][0] - 1, 0);
			}
			else if (key == 'f')
			{
				B_range[1][1] = min(B_range[1][1] + 1, 255);
			}
			else if (key == 'd')
			{
				B_range[1][1] = max(B_range[1][1] - 1, 0);
			}

			else if (key == 'x')
			{
				B_range[2][0] = min(B_range[2][0] + 1, 255);
			}
			else if (key == 'z')
			{
				B_range[2][0] = max(B_range[2][0] - 1, 0);
			}
			else if (key == 'v')
			{
				B_range[2][1] = min(B_range[2][1] + 1, 255);
			}
			else if (key == 'c')
			{
				B_range[2][1] = max(B_range[2][1] - 1, 0);
			}

			//-----------------------

			else if (key == 'y')
			{
				G_range[0][0] = min(G_range[0][0] + 1, 255);
			}
			else if (key == 't')
			{
				G_range[0][0] = max(G_range[0][0] - 1, 0);
			}
			else if (key == 'i')
			{
				G_range[0][1] = min(G_range[0][1] + 1, 255);
			}
			else if (key == 'u')
			{
				G_range[0][1] = max(G_range[0][1] - 1, 0);
			}

			else if (key == 'h')
			{
				G_range[1][0] = min(G_range[1][0] + 1, 255);
			}
			else if (key == 'g')
			{
				G_range[1][0] = max(G_range[1][0] - 1, 0);
			}
			else if (key == 'k')
			{
				G_range[1][1] = min(G_range[1][1] + 1, 255);
			}
			else if (key == 'j')
			{
				G_range[1][1] = max(G_range[1][1] - 1, 0);
			}

			else if (key == 'n')
			{
				G_range[2][0] = min(G_range[2][0] + 1, 255);
			}
			else if (key == 'b')
			{
				G_range[2][0] = max(G_range[2][0] - 1, 0);
			}
			else if (key == ',')
			{
				G_range[2][1] = min(G_range[2][1] + 1, 255);
			}
			else if (key == 'm')
			{
				G_range[2][1] = max(G_range[2][1] - 1, 0);
			}

			//-----------------------

			else if (key == '[')
			{
				g_webcam_focus--;
				if (g_webcam_focus < -1)
				{
					g_webcam_focus = -1;
				}

				if (g_webcam_focus >= 0)
				{
					capture.set(cv::CAP_PROP_AUTOFOCUS, 0);
					capture.set(cv::CAP_PROP_FOCUS, g_webcam_focus);
				}
				else
				{
					capture.set(cv::CAP_PROP_AUTOFOCUS, 1);
				}
			}
			else if (key == ']')
			{
				g_webcam_focus++;

				if (g_webcam_focus >= 0)
				{
					capture.set(cv::CAP_PROP_AUTOFOCUS, 0);
					capture.set(cv::CAP_PROP_FOCUS, g_webcam_focus);
				}
				else
				{
					capture.set(cv::CAP_PROP_AUTOFOCUS, 1);
				}
			}
		}

		g_high_precision_timer_guard.Stop();
		capture.release();
	}
	else
	{
		show_msg("", 0, MessageType::Clean);
	}

	cv::destroyAllWindows();
}

int rel_move_to_dpos(double rel_move)
{
	if ((rel_move < 0) || (rel_move > 100))
	{
		error_msg(QString("ERROR: rel_move_to_dpos !(rel_move < 0) || (rel_move > 100) rel_move: %1").arg(rel_move));
	}

	double a = abs(rel_move - 50.0);
	double b = 50.0;
	int dpos = (double)(std::acos(a / b) * 180.0) / M_PI;

	if (rel_move > b)
	{
		dpos = 180 - dpos;
	}

	return dpos;
}

bool get_modify_funscript_move_in_out_functions(std::vector<std::vector<QPair<double, double>>>& modify_funscript_move_functions, std::vector<QPair<QPair<int, int>, std::vector<QPair<std::vector<int>, std::vector<int>>>>>& modify_funscript_move_in_out_functions)
{
	bool res = false;
	modify_funscript_move_functions.clear();
	modify_funscript_move_in_out_functions.clear();

	// "[0.25:0.38|0.75:0.62],[0.25:0.12|0.75:0.87],[unchanged]"; // [fast:slow:fast],[slow:fast:slow],[unchanged]
	QStringList modify_funscript_function_move_variants = g_modify_funscript_function_move_variants.mid(1, g_modify_funscript_function_move_variants.size() - 2).split("],[");
	for (QString& modify_funscript_function_move_variant : modify_funscript_function_move_variants)
	{
		std::vector<QPair<double, double>> modify_funscript_move_function;

		if (modify_funscript_function_move_variant == QString("unchanged"))
		{
			modify_funscript_move_functions.push_back(modify_funscript_move_function);
		}
		else
		{
			QStringList modify_funscript_function_move_variant_actions = modify_funscript_function_move_variant.split("|");
			for (QString& modify_funscript_function_move_variant_action : modify_funscript_function_move_variant_actions)
			{
				QRegularExpression re_modify_funscript_function_move_variant_action("^([\\d\\.]+):([\\d\\.]+)$");
				QRegularExpressionMatch match;

				match = re_modify_funscript_function_move_variant_action.match(modify_funscript_function_move_variant_action);
				if (!match.hasMatch())
				{
					error_msg(QString("ERROR: incorrect format of modify_funscript_function_move_variant_action: %1, it should be ddt(0.0-1.0):ddpos[0.0-1.0] or 'unchanged'").arg(modify_funscript_function_move_variant_action));
					return res;
				}
				else
				{
					double ddt = match.captured(1).toDouble();
					double ddpos = match.captured(2).toDouble();

					if (!((ddt > 0) && (ddt < 1)))
					{
						error_msg(QString("ERROR: incorrect format of ddt: %1 in modify_funscript_function_move_variant_action: %2, ddt should be: (ddt > 0) && (ddt < 1)").arg(ddt).arg(modify_funscript_function_move_variant_action));
						return res;
					}

					if (!((ddpos >= 0) && (ddpos <= 1)))
					{
						error_msg(QString("ERROR: incorrect format of ddpos: %1 in modify_funscript_function_move_variant_action: %2, ddpos should be: (ddt > 0) && (ddt < 1)").arg(ddpos).arg(modify_funscript_function_move_variant_action));
						return res;
					}

					modify_funscript_move_function.push_back(QPair<double, double>(ddt, ddpos));
				}
			}

			modify_funscript_move_functions.push_back(modify_funscript_move_function);
		}
	}

	// [0-200:1/2|2/1|random/random],[200-maximum:0/0]
	QString modify_funscript_function_move_in_out_variant = g_pW->ui->functionsMoveInOutVariants->itemText(g_functions_move_in_out_variant - 1);
	QStringList modify_funscript_function_move_in_out_variants = modify_funscript_function_move_in_out_variant.mid(1, modify_funscript_function_move_in_out_variant.size() - 2).split("],[");
	for (QString& modify_funscript_function_move_in_out_sub_variant : modify_funscript_function_move_in_out_variants)
	{
		QRegularExpression re_modify_funscript_move_in_out_function("^(\\d+)\\-(\\d+|maximum):([^:]+)$");
		QRegularExpressionMatch match;

		bool sub_res = false;

		match = re_modify_funscript_move_in_out_function.match(modify_funscript_function_move_in_out_sub_variant);
		if (match.hasMatch())
		{
			QString val;
			int min_speed = match.captured(1).toInt();

			val = match.captured(2);
			int max_speed = (val == "maximum") ? -1 : val.toInt();

			if ((max_speed != -1) && (max_speed <= min_speed))
			{
				error_msg(QString("ERROR: incorrect format of max_speed: %1 in modify_funscript_function_move_in_out_sub_variant: %2, max_speed should be > min_speed").arg(max_speed).arg(modify_funscript_function_move_in_out_sub_variant));
				return res;
			}

			val = match.captured(3);

			std::vector<QPair<std::vector<int>, std::vector<int>>> variants_pairs;

			QStringList _variants = val.split("|");
			for (QString& variant_pair : _variants)
			{
				QStringList vars_in_out = variant_pair.split("/");
				std::vector<int> variants_in;
				std::vector<int> variants_out;

				if (vars_in_out.size() == 2)
				{
					auto get_variants = [&modify_funscript_move_functions, &variant_pair, &modify_funscript_function_move_in_out_sub_variant](QString variant_str, QString move_type, std::vector<int>& variants)
					{
						if (variant_str == QString("random"))
						{
							for (int variant = 0; variant < modify_funscript_move_functions.size(); variant++)
							{
								variants.push_back(variant);
							}
						}
						else
						{
							int variant = variant_str.toInt() - 1;

							if (!((variant >= 0) && (variant < modify_funscript_move_functions.size())))
							{
								error_msg(QString("ERROR: incorrect format of variant_%1: %2 in variant_pair: %3 in modify_funscript_function_move_in_out_sub_variant: %4, it should be >= 1, <= modify_funscript_move_functions.size() (%4) or 'random'").arg(move_type).arg(variant_str).arg(variant_pair).arg(modify_funscript_function_move_in_out_sub_variant).arg(modify_funscript_move_functions.size()));
								return false;
							}

							variants.push_back(variant);
						}

						return true;
					};

					res = get_variants(vars_in_out[0], "in", variants_in);
					if (!res)
					{
						return res;
					}

					res = get_variants(vars_in_out[1], "out", variants_out);
					if (!res)
					{
						return res;
					}

					variants_pairs.push_back(QPair<std::vector<int>, std::vector<int>>(variants_in, variants_out));
				}
			}

			modify_funscript_move_in_out_functions.push_back(QPair<QPair<int, int>, std::vector<QPair<std::vector<int>, std::vector<int>>>>(QPair<int, int>(min_speed, max_speed), variants_pairs));
			sub_res = true;
		}

		if (!sub_res)
		{
			error_msg(QString("ERROR: incorrect format of modify_funscript_function_move_in_out_sub_variant: %2,\nit should be :\n"\
				"[min_speed_in_rpm_1-max_speed_in_rpm_1:move_in_variant_id_1_1/move_out_variant_id_1_1|(or)move_in_variant_id_1_2/move_out_variant_id_1_2|...]\n"\
				"For example:\n" \
				"[0-200:2/3|3/2|random/random]\n"\
				"[200-maximum:1/1]").arg(modify_funscript_function_move_in_out_sub_variant));
			return res;
		}
	}

	return true;
}

bool get_parsed_funscript_data(QString funscript_fname, std::vector<QPair<int, int>>& funscript_data_maped, speeds_data &all_speeds_data, QString *p_res_details)
{
	bool res = false;

	QString result_details;
	QFile file(funscript_fname);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		show_msg(QString("ERROR: Failed to open funscript file for read: %1").arg(funscript_fname));
		return res;
	}
	QTextStream in(&file);
	QString line = in.readAll();
	file.close();

	line.replace(QRegularExpression("[\\n\\r\\s]+"), "");

	QRegularExpression re_actions("\"actions\":\\[([^\\]]+)\\]");
	QRegularExpression re_at_action("at\\D*(\\d+)");
	QRegularExpression re_pos_action("pos\\D*(\\d+)");
	QRegularExpressionMatch match;

	match = re_actions.match(line);
	if (!match.hasMatch())
	{
		show_msg(QString("ERROR: actions not found in file: %1").arg(funscript_fname));
		return res;
	}

	QStringList actions = match.captured(1).split("},{");
	int at, pos;
	int size = actions.size();
	std::vector<QPair<int, int>> funscript_data(size);

	int id = 0, last_set_id_data = -1;
	for (QString& action : actions)
	{
		match = re_at_action.match(action);
		if (!match.hasMatch())
		{
			show_msg(QString("ERROR: at action data [%1] not match regex in file: %2").arg(action).arg(funscript_fname));
			return res;
		}
		at = match.captured(1).toInt();

		match = re_pos_action.match(action);
		if (!match.hasMatch())
		{
			show_msg(QString("ERROR: pos action data [%1] not match regex in file: %2").arg(action).arg(funscript_fname));
			return res;
		}
		pos = match.captured(1).toInt();

		if (!((pos >= 0) && (pos <= 100)))
		{
			show_msg(QString("ERROR: Strange pose:%1 in funscript file: %2 it should be in range 0-100").arg(pos).arg(funscript_fname));
			return res;
		}

		funscript_data[id].first = at;
		funscript_data[id].second = 100 - pos;

		id++;
	}

	int cur_move_dirrection = 0, prev_move_dirrection = 0; // 1 - move up, -1 - move down
	int prev_top_end_id = -1;
	int found_first_move;

	id = 1;
	found_first_move = 0;
	while (id < size)
	{
		if (funscript_data[id].second != funscript_data[id - 1].second)
		{
			if (!found_first_move)
			{
				found_first_move = 1;

				if (funscript_data[id].second > funscript_data[id - 1].second)
				{
					prev_move_dirrection = 1;
					prev_top_end_id = id - 1;
				}
				else
				{
					prev_move_dirrection = -1;
					prev_top_end_id = id - 1;
				}
			}
			else
			{
				if (funscript_data[id].second > funscript_data[id - 1].second)
				{
					cur_move_dirrection = 1;
				}
				else
				{
					cur_move_dirrection = -1;
				}

				if (cur_move_dirrection != prev_move_dirrection)
				{
					int min_pos, max_pos, min_id, max_id;

					if (funscript_data[prev_top_end_id].second > funscript_data[id - 1].second)
					{
						max_pos = funscript_data[prev_top_end_id].second;
						max_id = prev_top_end_id;
						min_pos = funscript_data[id - 1].second;
						min_id = id - 1;
					}
					else
					{
						max_pos = funscript_data[id - 1].second;
						max_id = id - 1;
						min_pos = funscript_data[prev_top_end_id].second;
						min_id = prev_top_end_id;
					}

					if (max_pos - min_pos < g_min_funscript_relative_move)
					{
						int next_id, next_move_dirrection, prev_next_move_dirrection = cur_move_dirrection;

						next_id = id + 1;
						while (next_id < size)
						{
							if (funscript_data[next_id].second != funscript_data[next_id - 1].second)
							{
								if (funscript_data[next_id].second > funscript_data[next_id - 1].second)
								{
									next_move_dirrection = 1;
								}
								else
								{
									next_move_dirrection = -1;
								}

								if (next_move_dirrection != prev_next_move_dirrection)
								{
									if (funscript_data[next_id - 1].second > max_pos)
									{
										max_pos = funscript_data[next_id - 1].second;
										max_id = next_id - 1;
									}
									else if (funscript_data[next_id - 1].second < min_pos)
									{
										min_pos = funscript_data[next_id - 1].second;
										min_id = next_id - 1;
									}

									if (max_pos - min_pos >= g_min_funscript_relative_move)
									{
										break;
									}

									prev_next_move_dirrection = next_move_dirrection;
								}
							}

							next_id++;
						}

						if (result_details.size() > 0)
						{
							result_details += "\n+\n";
						}

						int id1 = prev_top_end_id;
						int id2 = max(min_id, max_id);

						if (id2 != id1)
						{
							result_details += QString("Averaged %1 positions\n").arg(id2 - id1);

							int total_move = 0;
							std::vector<int> i_move(id2 - id1);
							for (int i = id1 + 1; i <= id2; i++)
							{
								total_move += abs(funscript_data[i].second - funscript_data[i - 1].second);
								i_move[i - (id1 + 1)] = total_move;
							}
							int rel_move = funscript_data[id2].second - funscript_data[id1].second;

							if (total_move == 0)
							{
								show_msg(QString("ERROR: got total_move == 0 after time: %1 in file: %2").arg(funscript_data[id1].first).arg(funscript_fname));
								return res;
							}

							for (int i = id1; i <= id2; i++)
							{
								result_details += QString("%1").arg(100 - funscript_data[i].second);
								if (i < id2)
								{
									result_details += " | ";
								}
							}

							result_details += QString("\n%1 | ").arg(100 - funscript_data[id1].second);
							for (int i = id1 + 1; i <= id2; i++)
							{
								int res_inv_pos = funscript_data[id1].second + ((i_move[i - (id1 + 1)] * rel_move) / total_move);
								result_details += QString("%1").arg(100 - res_inv_pos);
								if (i < id2)
								{
									result_details += " | ";
								}
							}

							result_details += QString("\n[at:%1(%2)][pos:%3] | ").arg(funscript_data[id1].first).arg(VideoTimeToStr(funscript_data[id1].first).c_str()).arg(100 - funscript_data[id1].second);
							for (int i = id1 + 1; i <= id2; i++)
							{
								int res_inv_pos = funscript_data[id1].second + ((i_move[i - (id1 + 1)] * rel_move) / total_move);

								if ((res_inv_pos > 100) || (res_inv_pos < 0))
								{
									show_msg("ERROR: unexpected case (res_inv_pos > 100) || (res_inv_pos < 0)");
									return res;
								}

								result_details += QString("[at:%1(%2)][pos:%3][res_pos:%4]").arg(funscript_data[i].first).arg(VideoTimeToStr(funscript_data[i].first).c_str()).arg(100 - funscript_data[i].second).arg(100 - res_inv_pos);
								if (i < id2)
								{
									result_details += " | ";
								}

								funscript_data[i].second = res_inv_pos;
							}
						}

						if (next_id == size)
						{
							break;
						}
						else
						{
							id = 0;
							found_first_move = 0;
						}
					}
					else
					{
						prev_move_dirrection = cur_move_dirrection;
						prev_top_end_id = id - 1;
					}
				}
			}
		}

		id++;
	}



	if (g_modify_funscript)
	{
		std::srand(std::time(nullptr)); // use current time as seed for random generator

		std::vector<std::vector<QPair<double, double>>> modify_funscript_move_functions;
		std::vector<QPair<QPair<int, int>, std::vector<QPair<std::vector<int>, std::vector<int>>>>> modify_funscript_move_in_out_functions;
		res = get_modify_funscript_move_in_out_functions(modify_funscript_move_functions, modify_funscript_move_in_out_functions);

		if (!res)
		{
			return res;
		}

		std::list<QPair<int, int>> funscript_data_list;
		int prev_move_in_out_id = -1;
		int prev_variants_pair_id = -1;

		id = 1;
		found_first_move = 0;
		while (id < size)
		{
			if (funscript_data[id].second != funscript_data[id - 1].second)
			{
				if (!found_first_move)
				{
					found_first_move = 1;

					if (funscript_data[id].second > funscript_data[id - 1].second)
					{
						prev_move_dirrection = 1;
						prev_top_end_id = id - 1;
					}
					else
					{
						prev_move_dirrection = -1;
						prev_top_end_id = id - 1;
					}

					funscript_data_list.push_back(funscript_data[id - 1]);
				}
				else
				{
					if (funscript_data[id].second > funscript_data[id - 1].second)
					{
						cur_move_dirrection = 1;
					}
					else
					{
						cur_move_dirrection = -1;
					}

					if (cur_move_dirrection != prev_move_dirrection)
					{
						int min_pos, max_pos, min_id, max_id;
						int move_in_out_id = -1;
						int variants_pair_id = -1;

						if (funscript_data[prev_top_end_id].second > funscript_data[id - 1].second)
						{
							max_pos = funscript_data[prev_top_end_id].second;
							max_id = prev_top_end_id;
							min_pos = funscript_data[id - 1].second;
							min_id = id - 1;
						}
						else
						{
							max_pos = funscript_data[id - 1].second;
							max_id = id - 1;
							min_pos = funscript_data[prev_top_end_id].second;
							min_id = prev_top_end_id;
						}

						if (prev_top_end_id == id - 2)
						{
							int dpos = 180;
							int dt = funscript_data[id - 1].first - funscript_data[prev_top_end_id].first;

							if (dt == 0)
							{
								show_msg(QString("ERROR: got dt == 0 after time: %1 in file: %2").arg(funscript_data[prev_top_end_id].first).arg(funscript_fname));
								return res;
							}

							int avg_cur_speed_in_rpm = (dpos * 1000 * 60) / (dt * 360);

							move_in_out_id = 0;
							while (move_in_out_id < modify_funscript_move_in_out_functions.size())
							{
								if ((avg_cur_speed_in_rpm >= modify_funscript_move_in_out_functions[move_in_out_id].first.first) &&
									((modify_funscript_move_in_out_functions[move_in_out_id].first.second == -1) ? true : (avg_cur_speed_in_rpm <= modify_funscript_move_in_out_functions[move_in_out_id].first.second)))
								{

									int variants_pairs_size = modify_funscript_move_in_out_functions[move_in_out_id].second.size();

									if (prev_move_dirrection == 1) // move in
									{
										variants_pair_id = (variants_pairs_size == 1) ? 0 : (std::rand() % variants_pairs_size);
									}
									else
									{
										if ((prev_move_in_out_id == move_in_out_id) && (prev_variants_pair_id != -1))
										{
											variants_pair_id = prev_variants_pair_id;
										}
										else
										{
											variants_pair_id = (variants_pairs_size == 1) ? 0 : (std::rand() % variants_pairs_size);
										}
									}

									std::vector<int>& variats_by_cur_move_type = (prev_move_dirrection == 1) ? modify_funscript_move_in_out_functions[move_in_out_id].second[variants_pair_id].first : modify_funscript_move_in_out_functions[move_in_out_id].second[variants_pair_id].second;
									int variats_by_cur_move_type_size = variats_by_cur_move_type.size();
									int variant_id = (variats_by_cur_move_type_size == 1) ? variats_by_cur_move_type[0] : variats_by_cur_move_type[std::rand() % variats_by_cur_move_type_size];

									int total_move = funscript_data[id - 1].second - funscript_data[prev_top_end_id].second;
									std::vector<QPair<double, double>>& modify_funscript_move_function = modify_funscript_move_functions[variant_id];

									int ddt, ddmove, ddt_prev = 0;

									for (QPair<double, double>& pair : modify_funscript_move_function)
									{
										ddt = (int)((double)dt * pair.first);
										ddmove = (int)((double)total_move * pair.second);

										if ((ddt > ddt_prev) && (ddt < dt))
										{
											funscript_data_list.push_back(QPair<double, double>(funscript_data[prev_top_end_id].first + ddt, funscript_data[prev_top_end_id].second + ddmove));
										}

										ddt_prev = ddt;
									}

									break;
								}

								move_in_out_id++;
							}

							funscript_data_list.push_back(funscript_data[id - 1]);
						}
						else
						{
							funscript_data_list.push_back(funscript_data[id - 1]);
						}

						prev_move_in_out_id = move_in_out_id;
						prev_variants_pair_id = variants_pair_id;
						prev_move_dirrection = cur_move_dirrection;
						prev_top_end_id = id - 1;
					}
					else
					{
						funscript_data_list.push_back(funscript_data[id - 1]);
					}
				}
			}
			else
			{
				funscript_data_list.push_back(funscript_data[id - 1]);
			}

			id++;
		}

		funscript_data_list.push_back(funscript_data[id - 1]);

		size = funscript_data_list.size();
		funscript_data.clear();
		funscript_data.reserve(size);
		std::copy(std::begin(funscript_data_list), std::end(funscript_data_list), std::back_inserter(funscript_data));
	}

	funscript_data_maped.resize(size);

	id = 1;
	found_first_move = 0;
	while (id < size)
	{
		if (funscript_data[id].second != funscript_data[id - 1].second)
		{
			if (!found_first_move)
			{
				found_first_move = 1;

				if (funscript_data[id].second > funscript_data[id - 1].second)
				{
					for (int i = 0; i <= id - 1; i++)
					{
						funscript_data_maped[i].first = funscript_data[i].first;
						funscript_data_maped[i].second = 0;
					}

					last_set_id_data = id - 1;
					prev_move_dirrection = 1;
					prev_top_end_id = id - 1;
				}
				else
				{
					for (int i = 0; i <= id - 1; i++)
					{
						funscript_data_maped[i].first = funscript_data[i].first;
						funscript_data_maped[i].second = 180;
					}

					last_set_id_data = id - 1;
					prev_move_dirrection = -1;
					prev_top_end_id = id - 1;
				}
			}
			else
			{
				if (funscript_data[id].second > funscript_data[id - 1].second)
				{
					cur_move_dirrection = 1;
				}
				else
				{
					cur_move_dirrection = -1;
				}

				if (cur_move_dirrection != prev_move_dirrection)
				{
					int min_pos, max_pos, min_id, max_id;

					if (funscript_data[prev_top_end_id].second > funscript_data[id - 1].second)
					{
						max_pos = funscript_data[prev_top_end_id].second;
						max_id = prev_top_end_id;
						min_pos = funscript_data[id - 1].second;
						min_id = id - 1;
					}
					else
					{
						max_pos = funscript_data[id - 1].second;
						max_id = id - 1;
						min_pos = funscript_data[prev_top_end_id].second;
						min_id = prev_top_end_id;
					}

					if (max_pos - min_pos == 0)
					{
						show_msg(QString("ERROR: got max_pos - min_pos == 0 after time: %1 in file: %2").arg(funscript_data[prev_top_end_id].first).arg(funscript_fname));
						return res;
					}

					if (prev_move_dirrection == -1)
					{
						for (int i = prev_top_end_id + 1; i <= id - 1; i++)
						{
							double rel_move = ((double)(max_pos - funscript_data[i].second) * 100.0) / (double)(max_pos - min_pos);
							int dpos = rel_move_to_dpos(rel_move);
							funscript_data_maped[i].first = funscript_data[i].first;
							funscript_data_maped[i].second = funscript_data_maped[prev_top_end_id].second + dpos;
						}

						last_set_id_data = id - 1;
						prev_move_dirrection = cur_move_dirrection;
						prev_top_end_id = id - 1;
					}
					else
					{
						for (int i = prev_top_end_id + 1; i <= id - 1; i++)
						{
							double rel_move = ((double)(funscript_data[i].second - min_pos) * 100.0) / (double)(max_pos - min_pos);
							int dpos = rel_move_to_dpos(rel_move);
							funscript_data_maped[i].first = funscript_data[i].first;
							funscript_data_maped[i].second = funscript_data_maped[prev_top_end_id].second + dpos;
						}

						last_set_id_data = id - 1;
						prev_move_dirrection = cur_move_dirrection;
						prev_top_end_id = id - 1;
					}
				}
			}
		}

		id++;
	}

	if (last_set_id_data < size - 1)
	{
		id = size - 1;
		if (cur_move_dirrection == -1)
		{
			int max_pos = funscript_data[prev_top_end_id].second;
			int min_pos = funscript_data[id].second;

			if (max_pos - min_pos == 0)
			{
				show_msg(QString("ERROR: got max_pos - min_pos == 0 after time: %1 in file: %2").arg(funscript_data[prev_top_end_id].first).arg(funscript_fname));
				return res;
			}

			for (int i = prev_top_end_id + 1; i <= id; i++)
			{
				double rel_move = ((double)(max_pos - funscript_data[i].second) * 100.0) / (double)(max_pos - min_pos);
				int dpos = rel_move_to_dpos(rel_move);
				funscript_data_maped[i].first = funscript_data[i].first;
				funscript_data_maped[i].second = funscript_data_maped[prev_top_end_id].second + dpos;
			}
		}
		else
		{
			int max_pos = funscript_data[id].second;
			int min_pos = funscript_data[prev_top_end_id].second;

			if (max_pos - min_pos == 0)
			{
				show_msg(QString("ERROR: got max_pos - min_pos == 0 after time: %1 in file: %2").arg(funscript_data[prev_top_end_id].first).arg(funscript_fname));
				return res;
			}

			for (int i = prev_top_end_id + 1; i <= id; i++)
			{
				double rel_move = ((double)(funscript_data[i].second - min_pos) * 100.0) / (double)(max_pos - min_pos);
				int dpos = rel_move_to_dpos(rel_move);
				funscript_data_maped[i].first = funscript_data[i].first;
				funscript_data_maped[i].second = funscript_data_maped[prev_top_end_id].second + dpos;
			}
		}
	}

	QString cur_time_str = get_cur_time_str();

	if (p_res_details)
	{
		*p_res_details = result_details;
	}

	bool save_res_funscript = g_modify_funscript ? true : false;

	if (result_details.size() > 0)
	{
		save_res_funscript = true;

		save_text_to_file(g_root_dir + "\\res_data\\!results_for_get_parsed_funscript_data.txt",
			cur_time_str + "\nFile path: " + funscript_fname + "\n----------\n" + result_details + "\n----------\n\n",
			QFile::WriteOnly | QFile::Append | QFile::Text);
	}
	else
	{
		save_text_to_file(g_root_dir + "\\res_data\\!results_for_get_parsed_funscript_data.txt",
			cur_time_str + "\nFile path: " + funscript_fname + "\n----------\nLoaded successfully without changes\n----------\n\n",
			QFile::WriteOnly | QFile::Append | QFile::Text);
	}

	if (save_res_funscript)
	{
		QString result_funscript = "{\"actions\":[";
		for (id = 0; id < size; id++)
		{
			result_funscript += QString("{\"at\":%1,\"pos\":%2}").arg(funscript_data[id].first).arg(100 - funscript_data[id].second);
			if (id < size - 1)
			{
				result_funscript += ",";
			}
		}
		result_funscript += "]}";

		QFileInfo info(funscript_fname);
		QString fname = info.fileName();

		save_text_to_file(g_root_dir + "\\res_data\\" + fname, result_funscript, QFile::WriteOnly | QFile::Text);
	}

	res = true;
	return res;
}

bool get_speed_statistics_data(speeds_data &all_speeds_data)
{
	bool res = false;
	statistics_data sub_data{0, 0, 0, -1};

	g_avg_time_delay = 0;
	for (int speed = 1; speed <= 100; speed++)
	{
		speed_data &speed_data = all_speeds_data.speed_data_vector[speed - 1];
		QDomDocument doc("data");
		QString fpath = g_root_dir + QString("\\data\\speed_statistics_data_%1.txt").arg(speed);
		QFile xmlFile(fpath);
		if (!xmlFile.open(QIODevice::ReadOnly))
		{
			error_msg(QString("Missed required speed statistics file: %1").arg(fpath));
			return res;
		}
		if (!doc.setContent(&xmlFile))
		{
			xmlFile.close();
			error_msg(QString("Incorrect xml data in file: %1").arg(fpath));
			return res;
		}
		xmlFile.close();

		QDomElement docElem = doc.documentElement();

		QDomNode n = docElem.firstChild();
		while (!n.isNull()) {
			QDomElement e = n.toElement(); // try to convert the node to an element.
			if (!e.isNull()) {
				QString tag_name = e.tagName();

				if (tag_name.contains("sub_data_"))
				{
					// <sub_data_0 dt_video="66" dt_gtc="31" dpos="0"/>
					sub_data.dpos = e.attribute("dpos").toInt();
					sub_data.dt_video = e.attribute("dt_video").toInt();
					sub_data.dt_gtc = e.attribute("dt_gtc").toInt();
					speed_data.speed_statistics_data.push_back(sub_data);
				}
			}
			n = n.nextSibling();
		}

		int dpos;
		int dt_gtc;
		int end_i = -1;

		// skip first 2 seconds (treating that 2 seconds is enough for accelerate to full speed)
		dt_gtc = 0;
		for (int i = 0; i < speed_data.speed_statistics_data.size() - 1; i++)
		{
			dt_gtc += speed_data.speed_statistics_data[i].dt_gtc;
			if (dt_gtc >= 2000)
			{
				end_i = i;
				break;
			}
		}

		if (end_i == -1)
		{
			error_msg(QString("ERROR: got end_i == -1 in file: %1").arg(fpath));
			return res;
		}

		dpos = 0;
		dt_gtc = 0;
		for (int i = speed_data.speed_statistics_data.size() - 1; i > end_i; i--)
		{
			dt_gtc += speed_data.speed_statistics_data[i].dt_gtc;
			dpos += speed_data.speed_statistics_data[i].dpos;
			if (dt_gtc > 5000)
			{
				break;
			}
		}

		if (dt_gtc == 0)
		{
			error_msg(QString("ERROR: got dt_gtc == 0 (1) in file: %1").arg(fpath));
			return res;
		}

		speed_data.total_average_speed = (dpos * 1000) / dt_gtc;

		dt_gtc = 0;
		for (int i = 0; i < speed_data.speed_statistics_data.size()-3; i++)
		{
			if ((speed_data.speed_statistics_data[i].dpos == 0) || ((speed_data.speed_statistics_data[i + 1].dpos == 0) && (speed_data.speed_statistics_data[i + 2].dpos == 0)))
			{
				dt_gtc += speed_data.speed_statistics_data[i].dt_gtc;
			}
			else
			{
				dt_gtc += speed_data.speed_statistics_data[i].dt_gtc;
				break;
			}
		}

		speed_data.time_delay = dt_gtc;
		g_avg_time_delay += speed_data.time_delay;

		for (int i = 0; i < speed_data.speed_statistics_data.size(); i++)
		{
			dpos = 0;
			dt_gtc = 0;
			for (int j = i; j >= 0; j--)
			{
				dt_gtc += speed_data.speed_statistics_data[j].dt_gtc;
				dpos += speed_data.speed_statistics_data[j].dpos;
				if (dt_gtc >= g_dt_for_get_cur_speed)
				{
					break;
				}
			}

			if (dt_gtc < g_dt_for_get_cur_speed)
			{
				dt_gtc = g_dt_for_get_cur_speed;
			}

			if (dt_gtc == 0)
			{
				error_msg(QString("ERROR: got dt_gtc == 0 (2) in file: %1").arg(fpath));
				return res;
			}

			speed_data.speed_statistics_data[i].avg_cur_speed = (dpos * 1000) / dt_gtc;
		}

		{
			int n = 0;
			int avg_speed = 0;
			int s = 0;

			int dt_gtc = 0;
			for (int i = speed_data.speed_statistics_data.size() - 1; i > end_i; i--)
			{
				avg_speed += speed_data.speed_statistics_data[i].avg_cur_speed;
				dt_gtc += speed_data.speed_statistics_data[i].dt_gtc;
				n++;
				if (dt_gtc > 5000)
				{
					break;
				}
			}

			if (n == 0)
			{
				error_msg(QString("ERROR: got n == 0 in file: %1").arg(fpath));
				return res;
			}

			avg_speed = avg_speed / n;

			int cnt = 0;
			for (int i = speed_data.speed_statistics_data.size() - 1; i > speed_data.speed_statistics_data.size() - 1 - n; i--)
			{
				cnt++;
				s += pow2(speed_data.speed_statistics_data[i].avg_cur_speed - avg_speed);
			}
			s = sqrt(s / n);

			int min_avg_speed = avg_speed - s;

			dt_gtc = 0;
			int i = 0;
			while (i < speed_data.speed_statistics_data.size() - 1)
			{
				if ((speed_data.speed_statistics_data[i].dpos != 0) && (speed_data.speed_statistics_data[i + 1].dpos != 0))
				{
					break;
				}
				i++;
			}

			if (speed_data.speed_statistics_data[i].dpos < speed_data.speed_statistics_data[i + 1].dpos)
			{
				if (speed_data.speed_statistics_data[i + 1].dpos == 0)
				{
					error_msg(QString("ERROR: got speed_data.speed_statistics_data[i + 1].dpos == 0 in file: %1").arg(fpath));
					return res;
				}

				int dt = ((speed_data.speed_statistics_data[i + 1].dt_gtc * speed_data.speed_statistics_data[i].dpos) / speed_data.speed_statistics_data[i + 1].dpos);
				if (dt < speed_data.speed_statistics_data[i].dt_gtc)
				{
					dt_gtc += dt;
				}
				else
				{
					dt_gtc += speed_data.speed_statistics_data[i].dt_gtc;
				}
			}
			else
			{
				dt_gtc += speed_data.speed_statistics_data[i].dt_gtc;
			}
			i++;

			while (i < speed_data.speed_statistics_data.size())
			{
				dt_gtc += speed_data.speed_statistics_data[i].dt_gtc;

				if (speed_data.speed_statistics_data[i].avg_cur_speed >= min_avg_speed)
				{
					break;
				}
				i++;
			}

			if (dt_gtc == 0)
			{
				error_msg(QString("ERROR: got dt_gtc == 0 (3) in file: %1").arg(fpath));
				return res;
			}

			speed_data.average_rate_of_change_of_speed = (double)(speed_data.speed_statistics_data[i].avg_cur_speed) / (double)dt_gtc;
		}
	}

	g_avg_time_delay = g_avg_time_delay / 100;

	int speed = 1;
	while (speed <= 100 - 1)
	{
		if (all_speeds_data.speed_data_vector[speed - 1].total_average_speed > all_speeds_data.speed_data_vector[speed].total_average_speed)
		{
			int tmp = all_speeds_data.speed_data_vector[speed].total_average_speed;
			all_speeds_data.speed_data_vector[speed].total_average_speed = all_speeds_data.speed_data_vector[speed - 1].total_average_speed;
			all_speeds_data.speed_data_vector[speed - 1].total_average_speed = tmp;
			speed = 0;
		}
		speed++;
	}

	all_speeds_data.min_average_rate_of_change_of_speed = all_speeds_data.speed_data_vector[0].average_rate_of_change_of_speed;
	all_speeds_data.max_average_rate_of_change_of_speed = all_speeds_data.speed_data_vector[0].average_rate_of_change_of_speed;
	for (int speed = 2; speed <= 100; speed++)
	{
		speed_data& speed_data = all_speeds_data.speed_data_vector[speed - 1];

		if (speed_data.average_rate_of_change_of_speed < all_speeds_data.min_average_rate_of_change_of_speed)
		{
			all_speeds_data.min_average_rate_of_change_of_speed = speed_data.average_rate_of_change_of_speed;
		}

		if (speed_data.average_rate_of_change_of_speed > all_speeds_data.max_average_rate_of_change_of_speed)
		{
			all_speeds_data.max_average_rate_of_change_of_speed = speed_data.average_rate_of_change_of_speed;
		}
	}

	for (int speed = 1; speed <= 100; speed++)
	{
		speed_data& speed_data = all_speeds_data.speed_data_vector[speed - 1];
		speed_data.average_rate_of_change_of_speed = all_speeds_data.min_average_rate_of_change_of_speed + (((double)(speed - 1)*(all_speeds_data.max_average_rate_of_change_of_speed - all_speeds_data.min_average_rate_of_change_of_speed)) / 99.0);
	}

	res = true;
	return res;
}

int get_avg_hismith_speed(speeds_data& all_speeds_data, int speed)
{
	int h_speed = 0;

	if (speed > 0)
	{
		h_speed = 1;
		while (h_speed <= 100)
		{
			if (speed <= all_speeds_data.speed_data_vector[h_speed - 1].total_average_speed)
			{
				break;
			}
			h_speed++;
		}
	}

	return h_speed;
}

int get_optimal_hismith_speed(speeds_data& all_speeds_data, int cur_h_speed, int cur_speed, int dpos, int dt)
{
	if (dt < g_min_dt_for_set_hismith_speed)
	{
		error_msg(QString("ERROR: got error in get_optimal_hismith_speed: dt(%1) < g_min_dt_for_set_hismith_speed(%2)").arg(dt).arg(g_min_dt_for_set_hismith_speed));
		return 0;
	}

	int h_speed = 0;
	int avg_req_speed = (dpos * 1000) / dt;

	if (avg_req_speed > 0)
	{
		h_speed = 1;
		while (h_speed <= 100)
		{
			if (avg_req_speed <= all_speeds_data.speed_data_vector[h_speed - 1].total_average_speed)
			{
				break;
			}
			h_speed++;
		}

		if (h_speed > g_max_allowed_hismith_speed)
		{
			h_speed = g_max_allowed_hismith_speed;
		}
	}

	return h_speed;
}

// return value in range: target_pos + [-90;270)
int get_abs_to_target_pos(int cur_pos, int target_pos)
{
	int dif = (cur_pos % 360) - (target_pos % 360);
	if (dif >= 270)
	{
		dif -= 360;
	}
	else if (dif < -90)
	{
		dif += 360;
	}
	return target_pos + dif;
}

// return value in range: (-360 + max_search_dif; max_search_dif]
int get_d_in_front_from_poses(int abs_pos1, int pos2, int max_search_dif)
{
	abs_pos1 %= 360;
	pos2 %= 360;
	int dif = abs_pos1 - pos2;
	if (dif > max_search_dif)
	{
		dif -= 360;
	}
	else if (dif < 0)
	{
		if (dif + 360 <= max_search_dif)
		{
			dif += 360;
		}
	}
	return dif;
}

int update_abs_pos(const int &cur_pos, const int &prev_pos, int &abs_cur_pos, cv::Mat& frame, double prev_speed)
{
	int dpos = 0;

	if (cur_pos < prev_pos)
	{
		if (cur_pos + 360 - prev_pos < 180)
		{
			dpos = cur_pos + 360 - prev_pos;
		}
		else if (prev_pos - cur_pos < 45)
		{
			dpos = -(prev_pos - cur_pos);
		}
		else
		{
			dpos = cur_pos + 360 - prev_pos;
		}
	}
	if (cur_pos > prev_pos)
	{
		if (cur_pos - prev_pos < 180)
		{
			dpos = cur_pos - prev_pos;
		}
		else if (prev_pos + 360 - cur_pos < 45)
		{
			dpos = -(prev_pos + 360 - cur_pos);
		}
		else
		{
			dpos = cur_pos - prev_pos;
		}
	}

	abs_cur_pos += dpos;

	return dpos;
}

//------------------------------------------

const int _tmp_prev_poss_max_N = 100;
__int64 _tmp_msec_video_prev_poss[_tmp_prev_poss_max_N];
int _tmp_abs_prev_poss[_tmp_prev_poss_max_N];
int _num_prev_poss = 0;

bool _tmp_start_check_abs_cur_pos = false;
int _tmp_prev_start_abs_cur_pos = 0;
LARGE_INTEGER _tmp_prev_start_time;

void shift_get_next_frame_and_cur_speed_data(int dpos)
{
	for (int i = 0; i < _num_prev_poss; i++)
	{
		_tmp_abs_prev_poss[i] += dpos;
	}
	_tmp_prev_start_abs_cur_pos += dpos;
}

bool get_next_frame_and_cur_speed(cv::VideoCapture& capture, cv::Mat& frame,
	int& abs_cur_pos, int& cur_pos, __int64& msec_video_cur_pos, double& cur_speed,
	__int64& msec_video_prev_pos, int& abs_prev_pos, bool show_results = false, cv::Mat* p_res_frame = NULL, cv::String title = "", QString add_data = QString())
{
	int prev_pos, dpos;
	bool res = false;

	prev_pos = cur_pos;

	LARGE_INTEGER start_time, cur_time, Frequency;
	int dt = 0;
	QueryPerformanceFrequency(&Frequency);
	QueryPerformanceCounter(&start_time);

	while (!res && dt < 1000)
	{
		get_new_camera_frame(capture, frame, msec_video_cur_pos);

		if (msec_video_prev_pos != -1)
		{
			msec_video_prev_pos = _tmp_msec_video_prev_poss[0];
			abs_prev_pos = _tmp_abs_prev_poss[0];
		}

		if (get_hismith_pos_by_image(frame, cur_pos, true, show_results, p_res_frame, &cur_speed, title, add_data))
		{
			dpos = update_abs_pos(cur_pos, prev_pos, abs_cur_pos, frame, cur_speed);

			if (msec_video_prev_pos != -1)
			{
				int dt = (int)(msec_video_cur_pos - msec_video_prev_pos);

				_tmp_msec_video_prev_poss[_num_prev_poss] = msec_video_cur_pos;
				_tmp_abs_prev_poss[_num_prev_poss] = abs_cur_pos;
				_num_prev_poss++;

				if (dt > 0)
				{
					cur_speed = (double)((abs_cur_pos - abs_prev_pos) * 1000.0) / (double)dt;
				}

				if (dt >= g_dt_for_get_cur_speed)
				{
					for (int i = 0; i < _num_prev_poss - 1; i++)
					{
						_tmp_msec_video_prev_poss[i] = _tmp_msec_video_prev_poss[i + 1];
						_tmp_abs_prev_poss[i] = _tmp_abs_prev_poss[i + 1];
					}
					_num_prev_poss--;
				}

				if (_num_prev_poss == _tmp_prev_poss_max_N)
				{
					for (int i = 0; i < _num_prev_poss - 1; i++)
					{
						_tmp_msec_video_prev_poss[i] = _tmp_msec_video_prev_poss[i + 1];
						_tmp_abs_prev_poss[i] = _tmp_abs_prev_poss[i + 1];
					}
					_num_prev_poss--;
				}
			}
			else
			{
				msec_video_prev_pos = msec_video_cur_pos;
				abs_prev_pos = abs_cur_pos;
				_num_prev_poss = 0;
				_tmp_start_check_abs_cur_pos = false;

				_tmp_msec_video_prev_poss[_num_prev_poss] = msec_video_cur_pos;
				_tmp_abs_prev_poss[_num_prev_poss] = abs_cur_pos;
				_num_prev_poss++;
			}

			res = true;
		}
		else
		{
			show_msg(QString("WARNING: Failed to get hismith position."), 2000, MessageType::Always, false, 0.1);
			QueryPerformanceCounter(&cur_time);
			dt = time_diff_in_milliseconds(cur_time, start_time, Frequency);
		}
	}

	if (res)
	{
		if (_tmp_cur_hismith_speed_int >= g_hismith_speed_for_set_initial_pos)
		{
			if (_tmp_start_check_abs_cur_pos)
			{
				if (time_diff_in_milliseconds(start_time, _tmp_prev_start_time, Frequency) >= 2000)
				{
					if (std::abs(abs_cur_pos - _tmp_prev_start_abs_cur_pos) < 30)
					{
						error_msg(QString("ERROR: It looks that Hismith control has been lost.\n"
						"dpos for 2 second: %1 < 30 with speed: %2")
						.arg(std::abs(abs_cur_pos - _tmp_prev_start_abs_cur_pos))
						.arg(_tmp_cur_hismith_speed_int));
					}

					_tmp_prev_start_abs_cur_pos = abs_cur_pos;
					_tmp_prev_start_time = start_time;
				}
			}
			else
			{
				_tmp_start_check_abs_cur_pos = true;
				_tmp_prev_start_abs_cur_pos = abs_cur_pos;
				_tmp_prev_start_time = start_time;
			}
		}
		else
		{
			_tmp_start_check_abs_cur_pos = false;
		}
	}
	else
	{
		// remove showed messages
		show_msg("", 0, MessageType::Clean);
		g_ccxlcx_lh_ratio = -1.0;
		g_max_ccxlcx_lh_ratio_prev_to_cur_dif = -1.0;
	}

	return res;
}

QByteArray get_vlc_reply(QNetworkAccessManager* manager, QNetworkRequest& req, QString ReqUrl)
{
	QByteArray reply_res;
	bool res = false;
	bool show_warning = true;

	while (!res && !g_stop_run)
	{
		req.setUrl(QUrl(ReqUrl));
		QNetworkReply* rep = manager->get(req);
		QObject::connect(manager, &QNetworkAccessManager::finished, rep, &QNetworkReply::deleteLater);


		QEventLoop loop;
		QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();

		if (rep->isFinished() == true)
		{
			if (rep->error())
			{
				if (show_warning)
				{
					show_msg("Waiting for VLC is starded");
					set_hismith_speed(0.0);
					std::this_thread::sleep_for(std::chrono::milliseconds(1000));
					show_warning = false;
				}
			}
			else
			{
				reply_res = rep->readAll();
				res = true;
			}
		}

		if (!res && !g_stop_run)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
	}

	return reply_res;
}

void make_vlc_status_request(QNetworkAccessManager *manager, QNetworkRequest &req, bool &is_paused, QString &video_filename, bool &is_vlc_time_in_milliseconds, int &video_pos, __int64 &vlc_sys_time, double& rate, QString command)
{
	bool res = false;
	video_pos = -1;
	vlc_sys_time = -1;
	is_paused = true;
	is_vlc_time_in_milliseconds = false;
	rate = 1;
	video_filename.clear();
	int length;
	double position;
	bool show_warning_vlc_is_not_started = true;
	bool show_warning_no_video_selected = true;
	QString sync_data;
	QString ReqUrl(g_vlc_url + ":" + QString::number(g_vlc_port) + "/requests/status.xml" + command);

	do
	{
		req.setUrl(QUrl(ReqUrl));
		QNetworkReply* rep = manager->get(req);
		QObject::connect(manager, &QNetworkAccessManager::finished, rep, &QNetworkReply::deleteLater);
		QByteArray reply_res;

		QEventLoop loop;
		QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		int loop_res = loop.exec();

		if (rep->isFinished() == true)
		{
			if (rep->error())
			{
				if (show_warning_vlc_is_not_started)
				{
					show_msg("Can't get info from VLC: waiting for VLC is started or respond");
					show_warning_vlc_is_not_started = false;
					show_warning_no_video_selected = true;
				}
			}
			else
			{
				reply_res = rep->readAll();
				res = true;
			}
		}

		if (res)
		{
			QDomDocument doc("data");
			doc.setContent(reply_res);

			QDomElement docElem = doc.documentElement();

			bool got_sync_data = false;
			QDomNode n = docElem.firstChild();
			while (!n.isNull())
			{
				QDomElement e = n.toElement(); // try to convert the node to an element.
				if (!e.isNull())
				{
					QString tag_name = e.tagName();
					if (tag_name == "state")
					{
						is_paused = (e.text() == "paused") ? true : false;
					}
					else if (tag_name == "time")
					{
						video_pos = e.text().toInt();
					}
					else if (tag_name == "sync_data")
					{
						sync_data = e.text();
						got_sync_data = true;
					}
					else if (tag_name == "length")
					{
						length = e.text().toInt();
					}
					else if (tag_name == "position")
					{
						position = e.text().toDouble();
					}
					else if (tag_name == "rate")
					{
						rate = e.text().toDouble();
					}
					else if (tag_name == "information")
					{
						QDomNode in = e.firstChild();
						while (!in.isNull())
						{
							QDomElement ie = in.toElement(); // try to convert the node to an element.
							if (!ie.isNull())
							{
								QString itag_name = ie.tagName();

								if (ie.attribute("name") == "meta")
								{
									QDomNode mn = ie.firstChild();

									while (!mn.isNull())
									{
										if (!mn.isNull())
										{
											QDomElement me = mn.toElement();
											if (!me.isNull())
											{
												if (me.attribute("name") == "filename")
												{
													video_filename = me.text();
													video_filename = QUrl::fromPercentEncoding(video_filename.toUtf8());
													break;
												}
											}
										}

										mn = mn.nextSibling();
									}
								}
							}

							if (video_filename.size() > 0)
							{
								break;
							}

							in = in.nextSibling();
						}
					}

				}
				n = n.nextSibling();
			}

			if (video_filename.size() == 0)
			{
				if (show_warning_no_video_selected)
				{
					show_msg("Video file is not selected in playlist: waiting for video file selection");
					show_warning_no_video_selected = false;
					show_warning_vlc_is_not_started = true;
				}
				res = false;
			}
			else
			{
				if (!got_sync_data)
				{
					// in case of old VLC extended version with milliseconds support but without sync_data support
					if (video_pos < (int)((double)(length * 100) * position))
					{
						is_vlc_time_in_milliseconds = false;
						double video_pos_alt = length * position;

						if (video_pos_alt < video_pos + 1)
						{
							video_pos = max(video_pos * 1000, (int)(video_pos_alt * 1000.0));
						}
						else
						{
							video_pos *= 1000;
						}
					}
					else
					{
						is_vlc_time_in_milliseconds = true;
					}
				}
				else if (sync_data.size() == 0)
				{
					res = false;
					is_paused = true;
					is_vlc_time_in_milliseconds = true;
				}
				else
				{
					QRegularExpression re_sync_data("^video_time:(\\d+):sys_time:(\\d+):paused:(\\d+)$");
					QRegularExpressionMatch match;

					match = re_sync_data.match(sync_data);
					if (!match.hasMatch())
					{
						error_msg(QString("Got wrong sync_data format from VLC: %1").arg(sync_data));
					}
					else
					{
						video_pos = match.captured(1).toLongLong();
						vlc_sys_time = match.captured(2).toLongLong();
						is_paused = is_paused || (match.captured(3).toInt() == 1 ? true : false);
					}

					is_vlc_time_in_milliseconds = true;
				}
			}
		}

		if (!res && !g_stop_run)
		{
			video_pos = -1;
			vlc_sys_time = -1;
			is_paused = true;
			is_vlc_time_in_milliseconds = false;
			video_filename.clear();
			if (g_pMyDevice)
				set_hismith_speed(0.0);
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
	} while (!res && !g_stop_run);
}

void get_cur_video_pos(bool is_paused, int video_pos, __int64 vlc_sys_time, double rate, LARGE_INTEGER &cur_time, int &cur_video_pos, bool show_waring = true)
{
	if ((vlc_sys_time > 0) && !is_paused)
	{
		struct _timeb timebuffer;
		_ftime(&timebuffer);
		QueryPerformanceCounter(&cur_time);
		__int64 cur_sys_time = (((__int64)timebuffer.time) * 1000) + timebuffer.millitm;
		__int64 d_time = cur_sys_time - vlc_sys_time;

		if ((d_time < 0) || (d_time > 2000))
		{
			g_video_freezed = true;
			if (show_waring)
			{
				show_msg(QString("video freezed on %1 seconds").arg((double)d_time / 1000.0), 3000, MessageType::Always);
			}
		}
		else
		{
			g_video_freezed = false;
		}

		cur_video_pos = video_pos + (int)((double)d_time * rate);
	}
	else
	{
		cur_video_pos = video_pos;
		QueryPerformanceCounter(&cur_time);
	}
}

//---------------------------------------------------------------
// NOTE: QT Doesn't allow to create GUI in a non-main GUI thread
// like QMessageBox for example, so using Win API
//---------------------------------------------------------------
bool  _tmp_always;
QString  _tmp_msg_always;
QString  _tmp_msg;
int  _tmp_timeout;
const wchar_t _tmp_CLASS_NAME[] = L"MSG Window Class";
std::mutex _tmp_create_msg_mutex;
std::condition_variable _tmp_create_msg_cvar;
HWND _tmp_hwnd = NULL;
std::thread* _tmp_p_msg_thr = NULL;
bool _tmp_stop_msg = false;
bool _tmp_drow_modify_funscript_functions = true;
double _tmp_wnd_y_offset_from_top = 0.5;

LRESULT CALLBACK WndProc(HWND hwnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	switch (Msg)
	{
	case WM_CLOSE:
		DestroyWindow(hwnd);
		break;
	case WM_DESTROY:
		PostQuitMessage(WM_QUIT);
		break;
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC         hdc;
		RECT        rc, rw, rt;
		hdc = BeginPaint(hwnd, &ps);

		GetClientRect(hwnd, &rc);
		GetWindowRect(hwnd, &rw);

		SetTextColor(hdc, 0);
		SetBkMode(hdc, TRANSPARENT);

		//--------------

		HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
		LOGFONT logfont;
		GetObject(hFont, sizeof(LOGFONT), &logfont);

		int dpi = GetDeviceCaps(hdc, LOGPIXELSY);

		// Scale the font(s)
		constexpr UINT font_size{ 12 };
		logfont.lfHeight = -((font_size * dpi) / 72);

		HFONT hNewFont = CreateFontIndirect(&logfont);
		HFONT hOldFont = (HFONT)SelectObject(hdc, hNewFont);

		SIZE total_text_size {0, 0}, text_size;
		QStringList lines = _tmp_msg.split(QChar('\n'));
		for (QString& line : lines)
		{
			GetTextExtentPoint32(hdc, line.toStdWString().c_str(), wcslen(line.toStdWString().c_str()), &text_size);

			total_text_size.cx = max(text_size.cx, total_text_size.cx);
			total_text_size.cy += text_size.cy + 10;
		}

		int draw_h = 100;
		std::vector<std::vector<QPair<double, double>>> modify_funscript_move_functions;
		std::vector<QPair<QPair<int, int>, std::vector<QPair<std::vector<int>, std::vector<int>>>>> modify_funscript_move_in_out_functions;

		if (_tmp_drow_modify_funscript_functions)
		{
			if (get_modify_funscript_move_in_out_functions(modify_funscript_move_functions, modify_funscript_move_in_out_functions))
			{
				total_text_size.cy += modify_funscript_move_in_out_functions.size() * (draw_h + 10);

				for (int move_in_out_id = 0; move_in_out_id < modify_funscript_move_in_out_functions.size(); move_in_out_id++)
				{
					total_text_size.cx = max(total_text_size.cx, (20 * (modify_funscript_move_in_out_functions[move_in_out_id].second.size() - 1)) +
						(draw_h * 2 * modify_funscript_move_in_out_functions[move_in_out_id].second.size()));
				}
			}
			else
			{
				_tmp_drow_modify_funscript_functions = false;
			}
		}

		int screen_w = GetSystemMetrics(SM_CXSCREEN);
		int screen_h = GetSystemMetrics(SM_CYSCREEN);

		SetWindowPos(hwnd, NULL, (screen_w - (total_text_size.cx + 20))/2,
			max((int)((double)screen_h * _tmp_wnd_y_offset_from_top) - (total_text_size.cy + 10)/2, 0),
			total_text_size.cx + 20, total_text_size.cy + 10, SWP_NOREDRAW);
		GetClientRect(hwnd, &rc);

		HBRUSH brush;
		int thickness = 2;

		brush = CreateSolidBrush(RGB(0, 0, 0));
		FrameRect(hdc, &rc, brush);
		DeleteObject(brush);

		rc.left += thickness;
		rc.top += thickness;
		rc.right -= thickness;
		rc.bottom -= thickness;
		brush = CreateSolidBrush(RGB(255, 222, 108));
		FillRect(hdc, &rc, brush);
		DeleteObject(brush);

		rt.left = 0;
		rt.right = total_text_size.cx + 20;
		rt.top = 0;
		for (QString& line : lines)
		{
			GetTextExtentPoint32(hdc, line.toStdWString().c_str(), wcslen(line.toStdWString().c_str()), &text_size);
			rt.top += 10;
			rt.bottom = rt.top + text_size.cy;
			DrawText(hdc, line.toStdWString().c_str(), -1, &rt, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
			rt.top = rt.bottom;
		}

		int bottom = rt.bottom + 10;

		if (_tmp_drow_modify_funscript_functions)
		{
			Graphics graphics(hdc);
			Pen      pen(Color(255, 0, 0, 0), 5.0);

			for (int move_in_out_id = 0; move_in_out_id < modify_funscript_move_in_out_functions.size(); move_in_out_id++)
			{
				int left = ( (total_text_size.cx + 20) -
					(20 * (modify_funscript_move_in_out_functions[move_in_out_id].second.size() - 1)) -
					(draw_h * 2 * modify_funscript_move_in_out_functions[move_in_out_id].second.size()) ) / 2;

				for (int variants_pair_id = 0; variants_pair_id < modify_funscript_move_in_out_functions[move_in_out_id].second.size(); variants_pair_id++)
				{
					std::vector<int> variant_ids;

					if (modify_funscript_move_in_out_functions[move_in_out_id].second[variants_pair_id].first.size() == 1)
					{
						variant_ids.push_back(modify_funscript_move_in_out_functions[move_in_out_id].second[variants_pair_id].first[0]);
					}
					else
					{
						variant_ids.push_back(-1); // random
					}

					if (modify_funscript_move_in_out_functions[move_in_out_id].second[variants_pair_id].second.size() == 1)
					{
						variant_ids.push_back(modify_funscript_move_in_out_functions[move_in_out_id].second[variants_pair_id].second[0]);
					}
					else
					{
						variant_ids.push_back(-1); // random
					}

					for (int direction_id = 0; direction_id < 2; direction_id++)
					{
						int variant_id = variant_ids[direction_id];

						if (variant_id == -1) // random
						{
							if (direction_id == 0)
							{
								rt.left = left;
								rt.right = left + draw_h;
								rt.top = bottom;
								rt.bottom = bottom + draw_h;
							}
							else
							{
								rt.left = left + draw_h;
								rt.right = left + (draw_h * 2);
								rt.top = bottom;
								rt.bottom = bottom + draw_h;
							}
							DrawText(hdc, L"RND", -1, &rt, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
						}
						else
						{
							std::vector<QPair<double, double>> modify_funscript_move_function = modify_funscript_move_functions[variant_id];

							int ddt, ddmove, ddt_prev = 0;

							modify_funscript_move_function.insert(modify_funscript_move_function.begin(), QPair<double, double>(0.0, 0.0));
							modify_funscript_move_function.push_back(QPair<double, double>(1.0, 1.0));
							for (int mov_id = 1; mov_id < modify_funscript_move_function.size(); mov_id++)
							{
								int ddt1, ddmove1, ddt2, ddmove2;
								int x1, y1, x2, y2;

								ddt1 = (int)((double)draw_h * modify_funscript_move_function[mov_id - 1].first);
								ddmove1 = (int)((double)draw_h * modify_funscript_move_function[mov_id - 1].second);

								ddt2 = (int)((double)draw_h * modify_funscript_move_function[mov_id].first);
								ddmove2 = (int)((double)draw_h * modify_funscript_move_function[mov_id].second);

								if (direction_id == 0)
								{
									x1 = left + ddt1;
									x2 = left + ddt2;
									y1 = bottom + draw_h - ddmove1;
									y2 = bottom + draw_h - ddmove2;
								}
								else
								{
									x1 = left + draw_h + ddt1;
									x2 = left + draw_h + ddt2;

									y1 = bottom + ddmove1;
									y2 = bottom + ddmove2;
								}

								graphics.DrawLine(&pen, x1, y1, x2, y2);
							}
						}
					}

					left += (draw_h * 2) + 20;
				}

				bottom += draw_h + 10;
			}
		}

		// Always select the old font back into the DC
		SelectObject(hdc, hOldFont);
		DeleteObject(hNewFont);

		EndPaint(hwnd, &ps);
		break;
	}
	break;
	default:
		return DefWindowProc(hwnd, Msg, wParam, lParam);
	}
	return 0;
}

// NOT: if message is always (always == true):
// then this message will replace all previous messages (no matter always or not)
// and only last next new message which is not "always" will be accumulated with it
void show_msg(QString msg, int timeout, MessageType msg_type, bool drow_modify_funscript_functions, double msg_wnd_y_offset_from_top)
{
	if (msg_type == MessageType::Clean)
	{
		_tmp_msg_always.clear();
		_tmp_always = false;
	}
	else if (msg_type == MessageType::Always)
	{
		if (_tmp_hwnd && _tmp_always)
		{
			msg = _tmp_msg_always + QString("\n") + msg;
		}
		_tmp_msg_always = msg;
		_tmp_always = false;
	}

	if (_tmp_p_msg_thr)
	{
		if (_tmp_hwnd)
		{
			if (_tmp_always)
			{
				msg = _tmp_msg_always + QString("\n") + msg;
				msg_type = MessageType::Always;
				drow_modify_funscript_functions = drow_modify_funscript_functions || _tmp_drow_modify_funscript_functions;

				if (timeout < _tmp_timeout)
				{
					timeout = _tmp_timeout;
				}
			}
			_tmp_stop_msg = true;
			SendMessage(_tmp_hwnd, WM_CLOSE, 0, 0);
		}
		_tmp_p_msg_thr->join();
		delete _tmp_p_msg_thr;
		_tmp_p_msg_thr = NULL;
		_tmp_stop_msg = false;
	}

	_tmp_msg = msg;
	_tmp_always = (msg_type == MessageType::Always) ? true : false;
	_tmp_timeout = timeout;
	_tmp_drow_modify_funscript_functions = drow_modify_funscript_functions;
	_tmp_wnd_y_offset_from_top = msg_wnd_y_offset_from_top;

	if (timeout > 0)
	{
		std::unique_lock lk(_tmp_create_msg_mutex);
		g_msg_created = false;

		_tmp_p_msg_thr = new std::thread([msg, timeout] {
			HINSTANCE hInstance = (HINSTANCE)::GetModuleHandle(NULL);
			WNDCLASSEX wx = {};
			wx.cbSize = sizeof(WNDCLASSEX);
			wx.lpfnWndProc = WndProc;
			wx.hInstance = hInstance;
			wx.lpszClassName = _tmp_CLASS_NAME;
			if (RegisterClassEx(&wx))
			{
				_tmp_hwnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_NOACTIVATE,
					_tmp_CLASS_NAME,
					L"Window Title",
					WS_OVERLAPPEDWINDOW,
					0,
					0,
					100,
					50,
					NULL,
					NULL,
					hInstance,
					NULL);

				if (_tmp_hwnd)
				{
					LONG lStyle = GetWindowLong(_tmp_hwnd, GWL_STYLE);
					lStyle &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
					SetWindowLong(_tmp_hwnd, GWL_STYLE, lStyle);

					ShowWindow(_tmp_hwnd, SW_SHOWNOACTIVATE);

					LARGE_INTEGER start_time, cur_time, Frequency;
					QueryPerformanceFrequency(&Frequency);

					QueryPerformanceCounter(&start_time);
					int dt = timeout;

					g_msg_created = true;
					_tmp_create_msg_cvar.notify_all();

					while (!_tmp_stop_msg && dt > 0)
					{
						if (MsgWaitForMultipleObjects(0, NULL, FALSE, dt, QS_ALLINPUT) == WAIT_OBJECT_0)
						{
							MSG msg;
							while (!_tmp_stop_msg && PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
							{
								TranslateMessage(&msg);
								DispatchMessage(&msg);
							}
						}
						QueryPerformanceCounter(&cur_time);
						dt = timeout - (int)(time_diff_in_milliseconds(cur_time, start_time, Frequency));
					}

					DestroyWindow(_tmp_hwnd);
					_tmp_hwnd = NULL;
				}
				else
				{
					g_msg_created = true;
					_tmp_create_msg_cvar.notify_all();
				}

				UnregisterClass(_tmp_CLASS_NAME, hInstance);
			}
			else
			{
				g_msg_created = true;
				_tmp_create_msg_cvar.notify_all();
			}
			});

		_tmp_create_msg_cvar.wait(lk, [] { return g_msg_created; });
		int j = 5;
	}
}

//---------------------------------------------------------------

void save_results_file_data(QString results_file_path, QString results_file_data)
{
	QFile file(results_file_path);
	if (file.open(QFile::Append | QFile::Text))
	{
		QTextStream ts(&file);
		ts << results_file_data << "\n";
		file.flush();
		file.close();
	}
}

//---------------------------------------------------------------

QString get_time_to_cur_actions_end()
{
	int dt = (double)(g_video_cur_actions_end_time - g_actual_video_pos) / g_video_cur_rate;
	int min, sec;
	sec = (int)(dt / 1000);
	min = sec / 60;
	sec = sec % 60;
	QString str = QString("%1:%2")
		.arg(min)
		.arg(sec, 2, 10, QChar('0'));
	return str;
}

//---------------------------------------------------------------

QString get_add_msg_data()
{
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    QString time_str = oss.str().c_str();
	int abs_cur_pos, cur_pos;
	__int64 msec_video_cur_pos;
	cv::Mat frame;
	bool get_res;

	if (g_initial_start)
	{
		return "";
	}

    QString add_msg_data = QString(
        "Current time: %1 (hour:min:sec)\n"
		"Current video actions will end in: %2 (min:sec)\n"
        "Video speed rate: %3%4"
        )
        .arg(time_str)
        .arg(get_time_to_cur_actions_end())
        .arg(g_video_cur_rate)
		.arg(g_runing_funscript ? QString("") : QString("\nDiff start hismith pos: %1").arg(g_d_from_search_start_pos))
		;
    if (g_modify_funscript)
    {
        add_msg_data += QString("\nUse Modify Funscript Functions variant %1/%2 : %3")
			.arg(g_functions_move_in_out_variant)
			.arg(g_pW->ui->functionsMoveInOutVariants->count())
			.arg(g_pW->ui->functionsMoveInOutVariants->itemText(g_functions_move_in_out_variant - 1));
    }
    else
    {
        add_msg_data += QString("\nUse Modify Funscript Functions: Off");
    }

    return add_msg_data;
}

//---------------------------------------------------------------

void show_cur_execution_status(QString base_msg = "", int timeout = 5000)
{
	if (g_update)
	{
		std::lock_guard lk(g_update_mutex);
		show_msg(base_msg + get_add_msg_data(), timeout, MessageType::Always, g_modify_funscript);
		g_update = false;
		g_update_cvar.notify_all();
	}
}

//---------------------------------------------------------------

class HardwareException : public std::runtime_error {
public:
	DWORD code;
	std::string trace_string;

	HardwareException(DWORD exception_code)
		: std::runtime_error("Hardware Exception"), code(exception_code)
	{
		// Automatically captures the full backtrace with resolved symbols (requires .pdb)
		// std::to_string formats it into a clean, human-readable multiline string
		trace_string = std::to_string(std::stacktrace::current());
	}

	QString toQString() const {
		QString error_name;
		switch (code) {
		case STATUS_INTEGER_DIVIDE_BY_ZERO:
			error_name = "Integer Divide by Zero";
			break;
		case STATUS_FLOAT_DIVIDE_BY_ZERO:
			error_name = "Float Divide by Zero";
			break;
		case EXCEPTION_ACCESS_VIOLATION:
			error_name = "Access Violation";
			break;
		case EXCEPTION_STACK_OVERFLOW:
			error_name = "Stack Overflow";
			break;
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			error_name = "Array Bounds Exceeded";
			break;
		case EXCEPTION_ILLEGAL_INSTRUCTION:
			error_name = "Illegal Instruction";
			break;
		case STATUS_DATATYPE_MISALIGNMENT:
			error_name = "Datatype Misalignment";
			break;
		default:
			error_name = QString("Unknown Hardware Exception (Code: 0x%1)").arg(code, 0, 16).toUpper();
			break;
		}

		return QString("Hardware Exception: %1\nStack Trace:\n%2")
			.arg(error_name)
			.arg(QString::fromStdString(trace_string));
	}
};

//---------------------------------------------------------------

void trans_func(unsigned int u, _EXCEPTION_POINTERS* pExp) {
	throw HardwareException(u);
}

//---------------------------------------------------------------

void run_funscript()
{
	g_results_file_path = g_root_dir + "\\res_data\\!results_" + get_cur_time_str() + ".txt";
	g_results_file_data.clear();
	QString funscript_fname, last_load_funscript_fname, last_load_funscript_video_filename;
	int d_cur_from_search_start_pos, d_exp_from_search_start_pos, cur_pos, search_start_pos;
	__int64 msec_video_cur_pos, msec_video_prev_pos;
	double dt = 0, dmove = 0;
	int abs_cur_pos = 0, abs_prev_pos = 0;
	int exp_abs_cur_pos = 0;
	int dpos;
	double cur_speed, prev_cur_speed = 0, action_start_speed;
	int start_video_pos, video_pos, cur_video_pos = 0;

	__int64 vlc_sys_time = -1;
	double prev_rate = 1;
	bool is_video_paused = false;
	QString video_filename, last_play_video_filename;
	std::vector<QPair<int, int>> funscript_data_maped_full;
	bool get_res = true;
	bool is_vlc_time_in_milliseconds = true;
	double cur_set_hismith_speed = 0;
	int res;

	std::thread* p_save_results = NULL;

	LARGE_INTEGER start_time, cur_time, prev_time, set_hismith_speed_time, prev_set_hismith_speed_time, Frequency;
	QueryPerformanceFrequency(&Frequency);

	//-----------------------------------------------------
	// Connecting to Hismith
	// NOTE: At first start: intiface central

	show_msg("Connecting to Hismith...", 120000, MessageType::Clean);

	if (!connect_to_hismith())
	{
		show_msg("", 0, MessageType::Clean);
		return;
	}

	//-----------------------------------------------------
	// Connecting to Webcam

	cv::Mat frame, prev_frame;

	g_pCapture = new cv::VideoCapture;

	if (init_camera(*g_pCapture))
	{
		g_threaded_capture.start(g_pCapture);

		get_new_camera_frame(*g_pCapture, frame, msec_video_cur_pos);
		if (!get_hismith_pos_by_image(frame, cur_pos))
		{
			g_threaded_capture.stop();
			g_high_precision_timer_guard.Stop();
			g_pCapture->release();
			delete g_pCapture;
			g_pCapture = NULL;
			return;
		}
		abs_cur_pos = get_abs_to_target_pos(cur_pos, 0);
	}
	else
	{
		delete g_pCapture;
		g_pCapture = NULL;
		error_msg("ERROR: Failed to connect to Webcam");
		return;
	}

	g_results_file_data += QString(
		"webcam_name:%1\n"
		"webcam_frame_width:%2\n"
		"webcam_frame_height:%3\n"
		"webcam_fps:%4\n"
		"webcam_focus:%5\n"
		"webcam_end_to_end_latency:%6\n")
		.arg(g_pW->ui->Webcams->itemText(g_pW->ui->Webcams->currentIndex()))
		.arg(g_webcam_frame_width)
		.arg(g_webcam_frame_height)
		.arg(g_webcam_fps)
		.arg(g_webcam_focus)
		.arg(g_webcam_end_to_end_latency);

	//-----------------------------------------------------
	// Connecting to VLC player with already opened video

	show_msg("Connecting to VLC player...", 120000, MessageType::Clean);

	g_pNetworkAccessManager = new QNetworkAccessManager();

	QString concatenated = ":" + g_vlc_password; //username:password
	QByteArray data = concatenated.toLocal8Bit().toBase64();
	QString headerData = "Basic " + data;
	g_NetworkRequest.setRawHeader("Authorization", headerData.toLocal8Bit());
	g_NetworkRequest.setTransferTimeout(1000);

	speeds_data all_speeds_data;
	get_speed_statistics_data(all_speeds_data);

	if (g_speed_change_delay == -1)
	{
		g_speed_change_delay = g_avg_time_delay;
	}

	g_results_file_data += QString("min_dt_between_speed_changes_on_slow_moves:%1\n"
		"min_dt_between_speed_changes_on_fast_moves:%2\n"
		"fast_move_min_hismith_speed_for_switch_min_dt_between_speed_changes:%3\n"
		"speed_change_delay:%4\n")
		.arg(g_min_dt_between_speed_changes_on_slow_moves)
		.arg(g_min_dt_between_speed_changes_on_fast_moves)
		.arg(g_fast_move_min_hismith_speed_for_switch_min_dt_between_speed_changes)
		.arg(g_speed_change_delay);
	g_results_file_data += QString("get_speed_statistics_data:\naverage_time_delay:%1\n").arg(g_avg_time_delay);
	for (int h_speed = 1; h_speed <= 100; h_speed++)
	{
		g_results_file_data += QString("h_speed:%1 total_average_speed:%2 time_delay:%3\n")
			.arg(h_speed)
			.arg(all_speeds_data.speed_data_vector[h_speed - 1].total_average_speed)
			.arg(all_speeds_data.speed_data_vector[h_speed - 1].time_delay);
	}
	g_results_file_data += QString("\n");

	cur_set_hismith_speed = set_hismith_speed(0.0);
	QueryPerformanceCounter(&set_hismith_speed_time);
	prev_set_hismith_speed_time = set_hismith_speed_time;
	cur_time = set_hismith_speed_time;
	prev_time = cur_time;

	struct time_statistic
	{
		int dt1;
		int dt2;
		int dt3;
		int dt4;
		int dt5;
		int dt6;
	};
	time_statistic time_stat;

	make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
	get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos, false);
	g_actual_video_pos = cur_video_pos;
	prev_rate = g_video_cur_rate;

	bool clear_msgs = true;

	_set_se_translator(trans_func);

	try
	{
		while (!g_stop_run)
		{
			g_runing_funscript = false;
			g_ccxlcx_lh_ratio = -1.0;

			if (clear_msgs)
			{
				if (!g_pause)
				{
					show_msg("", 0, MessageType::Clean);
				}
			}
			else
			{
				clear_msgs = true;
			}

			QueryPerformanceCounter(&cur_time);
			time_stat.dt6 = time_diff_in_milliseconds(cur_time, prev_time, Frequency);
			time_stat.dt1 = -1;
			time_stat.dt2 = -1;
			time_stat.dt3 = -1;
			time_stat.dt4 = -1;
			time_stat.dt5 = -1;
			prev_time = cur_time;

			if (cur_set_hismith_speed != 0.0)
			{
				set_hismith_speed(0.0);
				prev_set_hismith_speed_time = set_hismith_speed_time;
				QueryPerformanceCounter(&set_hismith_speed_time);
			}

			if (g_pause && !g_update)
			{
				do
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(300));
					make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
					get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos, false);
					g_actual_video_pos = cur_video_pos;
					if (prev_rate != g_video_cur_rate)
					{
						prev_rate = g_video_cur_rate;
						g_update = true;
					}
				} while (!g_stop_run && g_pause && !g_update);

				if (g_stop_run)
					break;
			}

			if (g_video_freezed)
			{
				do
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
					get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos, false);
					g_actual_video_pos = cur_video_pos;
				} while (g_video_freezed && !g_stop_run && !g_pause);
			}
			else
			{
				g_video_freezed = false;
				make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
				get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos);
				g_actual_video_pos = cur_video_pos;

				if (g_video_freezed)
				{
					do
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(100));
						make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
						get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos, false);
						g_actual_video_pos = cur_video_pos;
					} while (g_video_freezed && !g_stop_run && !g_pause);
				}
			}
			last_play_video_filename = video_filename;

			if (g_stop_run || (g_pause && !g_update))
			{
				continue;
			}

			if (last_load_funscript_video_filename != last_play_video_filename)
			{
				funscript_fname.clear();
				QByteArray vlc_reply = get_vlc_reply(g_pNetworkAccessManager, g_NetworkRequest, g_vlc_url + ":" + QString::number(g_vlc_port) + "/requests/playlist.xml");
				QDomDocument doc("data");
				doc.setContent(vlc_reply);
				QDomElement docElem = doc.documentElement();
				QDomNode n = docElem.firstChild().firstChild();
				QString uri, current;
				while (!n.isNull()) {
					QDomElement e = n.toElement(); // try to convert the node to an element.
					if (!e.isNull()) {
						QString tag_name = e.tagName();
						if (tag_name == "leaf")
						{
							uri = e.attribute("uri");
							current = e.attribute("current");

							if (current == "current")
							{
								QString fpath = QUrl(uri).toLocalFile();
								QFileInfo info(fpath);
								QString fname = info.fileName();
								funscript_fname = QDir::toNativeSeparators(info.path() + "/" + info.completeBaseName() + ".funscript");
								break;
							}
						}
					}
					n = n.nextSibling();
				}

				if (!((funscript_fname.size() > 0) && QFile::exists(funscript_fname)))
				{
					if ((funscript_fname.size() > 0) && !QFile::exists(funscript_fname))
					{
						show_msg(QString("WARNING: Funscript not found (for current video): %1").arg(funscript_fname), 5000);
					}

					do
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(100));
						make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
						get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos);
						g_actual_video_pos = cur_video_pos;
					} while (last_play_video_filename == video_filename && !g_stop_run);
					continue;
				}
			}

			QueryPerformanceCounter(&cur_time);
			time_stat.dt1 = time_diff_in_milliseconds(cur_time, prev_time, Frequency);
			prev_time = cur_time;

			//-----------------------------------------------------
			// Load Funscript and Hismith statistical data
			std::vector<QPair<int, int>> funscript_data_maped;

			if ( (last_load_funscript_fname != funscript_fname) || g_was_change_in_use_modify_funscript_functions )
			{
				last_load_funscript_fname = funscript_fname;
				last_load_funscript_video_filename = last_play_video_filename;
				funscript_data_maped_full.clear();

				g_change_in_use_modify_funscript_functions_mutex.lock();
				bool get_data = get_parsed_funscript_data(funscript_fname, funscript_data_maped_full, all_speeds_data);
				g_was_change_in_use_modify_funscript_functions = false;
				g_change_in_use_modify_funscript_functions_mutex.unlock();

				if (!get_data)
				{
					do
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(100));
						make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
						get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos);
						g_actual_video_pos = cur_video_pos;
					} while (last_play_video_filename == video_filename && !g_stop_run);
					continue;
				}
			}

			QueryPerformanceCounter(&cur_time);
			time_stat.dt2 = time_diff_in_milliseconds(cur_time, prev_time, Frequency);
			prev_time = cur_time;

			bool found_start = false;
			int pos_offset;
			int search_video_pos;
			QString start_info;

			make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
			get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos);
			g_actual_video_pos = cur_video_pos;
			search_video_pos = cur_video_pos;
			prev_rate = g_video_cur_rate;

			for (int i = 0; i < funscript_data_maped_full.size(); i++)
			{
				if (!found_start)
				{
					if (funscript_data_maped_full[i].first > search_video_pos)
					{
						found_start = true;

						if ((i > 0) && (funscript_data_maped_full[i].second != funscript_data_maped_full[i - 1].second))
						{
							search_start_pos = funscript_data_maped_full[i - 1].second + ((funscript_data_maped_full[i].second - funscript_data_maped_full[i - 1].second) * (search_video_pos - funscript_data_maped_full[i - 1].first)) / (funscript_data_maped_full[i].first - funscript_data_maped_full[i - 1].first);
							pos_offset = search_start_pos - (search_start_pos % 360);
							search_start_pos = search_start_pos % 360;

							funscript_data_maped.push_back(QPair<int, int>(search_video_pos, search_start_pos));
							funscript_data_maped.push_back(QPair<int, int>(funscript_data_maped_full[i].first, funscript_data_maped_full[i].second - pos_offset));

							start_info += QString("search_video_time_and_exp_pos: [%1, %2] found_action_time_and_pos: [%3, %4] prev_action_time_and_pos: [%5, %6] pos_offset: %7")
								.arg(VideoTimeToStr(search_video_pos).c_str())
								.arg(search_start_pos)
								.arg(VideoTimeToStr(funscript_data_maped_full[i].first).c_str())
								.arg(funscript_data_maped_full[i].second - pos_offset)
								.arg(VideoTimeToStr(funscript_data_maped_full[i - 1].first).c_str())
								.arg(funscript_data_maped_full[i - 1].second - pos_offset)
								.arg(pos_offset);
						}
						else
						{
							search_start_pos = funscript_data_maped_full[i].second % 360;
							pos_offset = funscript_data_maped_full[i].second - search_start_pos;
							funscript_data_maped.push_back(QPair<int, int>(funscript_data_maped_full[i].first, search_start_pos));

							start_info += QString("search_video_time: %1 found_action_time_and_pos: [%2, %3] pos_offset: %4")
								.arg(VideoTimeToStr(search_video_pos).c_str())
								.arg(VideoTimeToStr(funscript_data_maped_full[i].first).c_str())
								.arg(search_start_pos)
								.arg(pos_offset);
						}
					}
				}
				else
				{
					funscript_data_maped.push_back(QPair<int, int>(funscript_data_maped_full[i].first, funscript_data_maped_full[i].second - pos_offset));
				}
			}

			if (funscript_data_maped.size() < 2)
			{
				show_msg(QString("There is not funscript data at this video pos in forward dirrection\nThe first action is at: %1").arg(VideoTimeToStr(funscript_data_maped_full[0].first).c_str()), 5000);
				do
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
					get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos);
					g_actual_video_pos = cur_video_pos;
				} while ((last_play_video_filename == video_filename) && (cur_video_pos >= search_video_pos) && (cur_video_pos <= search_video_pos + 1000) && !g_stop_run);
				continue;
			}
			else
			{
				if ((int)((double)(funscript_data_maped[0].first - search_video_pos) / g_video_cur_rate) > 10000)
				{
					show_msg(QString("The first funscript video action will be afte %1 seconds at pos: %2").arg((int)((double)(funscript_data_maped[0].first - search_video_pos)/(g_video_cur_rate*1000.0))).arg(VideoTimeToStr(funscript_data_maped[0].first).c_str()), 5000, MessageType::Always);
				}
			}

			{
				int last_i = 0;
				for (int i = 1; i < funscript_data_maped.size(); i++)
				{
					if ((funscript_data_maped[i].first - funscript_data_maped[i - 1].first) < 10000)
					{
						last_i = i;
					}
					else
					{
						break;
					}
				}
				g_video_cur_actions_end_time = funscript_data_maped[last_i].first;
				g_video_cur_actions_start_pos = funscript_data_maped[0].second;

				get_new_camera_frame(*g_pCapture, frame, msec_video_cur_pos);
				get_res = get_hismith_pos_by_image(frame, cur_pos);
				if (!get_res)
				{
					show_msg(QString("Failed to get device position accoring webcam frame."));
					g_stop_run = true;
					continue;
				}
				abs_cur_pos = get_abs_to_target_pos(cur_pos, g_video_cur_actions_start_pos);
				g_d_from_search_start_pos = abs_cur_pos - g_video_cur_actions_start_pos;
			}

			if (g_initial_start)
			{
				g_initial_start = false;
				g_pause = true;
				clear_msgs = false;
				show_msg("", 0, MessageType::Clean);
				show_msg(QString("Pausing execution funscript at the begining\n%1").arg(get_add_msg_data()),
					5000, MessageType::Always, g_modify_funscript);
				continue;
			}

			if (g_stop_run || g_pause)
			{
				if (g_update)
				{
					std::lock_guard lk(g_update_mutex);
					show_msg("", 0, MessageType::Clean);
					show_msg(QString("Paused execution funscript.\n%1").arg(get_add_msg_data()), 5000, MessageType::Always, g_modify_funscript);
					g_update = false;
					clear_msgs = false;
					g_update_cvar.notify_all();
				}
				continue;
			}

			QueryPerformanceCounter(&cur_time);
			time_stat.dt3 = time_diff_in_milliseconds(cur_time, prev_time, Frequency);
			prev_time = cur_time;

			int actions_size = funscript_data_maped.size();

			struct results_data
			{
				int avg_req_hismith_speed;
				int min_dt_between_speed_changes;
				int actual_action_id_dif;
				int move_dif;
				int dif_cur_vs_req_action_end_time;
				int dif_cur_vs_req_action_start_time;
				int dif_cur_vs_req_exp_pos = -999;
				int action_length_time;
				int req_dpos;
				int req_dpos_add;
				int req_speed;
				int start_speed;
				int end_speed;
				int hismith_speed_prev;
				int avg_hismith_speed_prev;
				int optimal_hismith_speed;
				int optimal_hismith_start_speed;
				QString action_start_video_time;
				QString hismith_speed_changed;
			};
			std::vector<results_data> results(actions_size);
			int results_size = 0;

			struct FrameData
			{
				int abs_pos;
				int video_pos;
			};

			uint64_t pool_size_bytes = static_cast<uint64_t>(g_webcam_fps *
				((double)(funscript_data_maped[actions_size - 1].first - cur_video_pos) / (g_video_cur_rate * 1000.0)) *
				(double)sizeof(FrameData) * 1.2);
			auto* mem_pool = new std::pmr::monotonic_buffer_resource(pool_size_bytes);
			std::pmr::deque<FrameData> frames_data_history(mem_pool);

			int dtime = 0;

			start_video_pos = cur_video_pos;
			QString start_video_name = video_filename;
			//waiting for video unpaused

			bool position_was_aligned = false;
			bool video_was_paused = is_video_paused;

			if (is_vlc_time_in_milliseconds && is_video_paused)
			{
				get_new_camera_frame(*g_pCapture, frame, msec_video_cur_pos);
				get_res = get_hismith_pos_by_image(frame, cur_pos);
				if (!get_res)
				{
					show_msg(QString("Failed to get device position accoring webcam frame."));
					g_stop_run = true;
					break;
				}
				abs_cur_pos = get_abs_to_target_pos(cur_pos, funscript_data_maped[0].second);
				g_d_from_search_start_pos = abs_cur_pos - funscript_data_maped[0].second;

				if ((g_d_from_search_start_pos < g_min_search_pos_dif) || (g_d_from_search_start_pos > g_max_search_pos_dif))
				{
					dpos = funscript_data_maped[0].second - abs_cur_pos;
					if (dpos < 0) dpos += 360;
					dt = (int)((double)(funscript_data_maped[0].first - cur_video_pos) / g_video_cur_rate) - g_speed_change_delay;

					int hspeed = g_hismith_speed_for_set_initial_pos;

					show_msg(QString("Align device position for run funscript actions..."), 5000, MessageType::Always);

					cur_set_hismith_speed = set_hismith_speed((double)hspeed / 100.0);
					prev_set_hismith_speed_time = set_hismith_speed_time;
					QueryPerformanceCounter(&set_hismith_speed_time);
					msec_video_prev_pos = -1;
					abs_prev_pos = 0;
					cur_speed = 0;
					int exp_abs_pos_before_speed_change_to_target_pos = 0;
					int abs_cur_pos_to_target_pos = 0;

					do
					{
						// run in parallel
						{
							std::thread t1( [&is_video_paused, &video_filename, &is_vlc_time_in_milliseconds, &video_pos, &vlc_sys_time] {
								make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
								} );
							std::thread t2([&get_res, &frame, &abs_cur_pos,
								&cur_pos, &msec_video_cur_pos, &cur_speed, &msec_video_prev_pos, &abs_prev_pos] {
								get_res = get_next_frame_and_cur_speed(*g_pCapture, frame,
									abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
									msec_video_prev_pos, abs_prev_pos);
								} );
							t1.join();
							t2.join();
							get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos);
							g_actual_video_pos = cur_video_pos;
						}

						if (!get_res)
						{
							show_msg("", 0, MessageType::Clean);
							show_msg(QString("Failed to get hismith position.\nPausing video if not and pausing hismith control."), 5000, MessageType::Always);
							g_pause = true;
							clear_msgs = false;
							break;
						}

						QueryPerformanceCounter(&cur_time);

						exp_abs_cur_pos = abs_cur_pos;
						if (cur_speed > 0)
						{
							__int64 video_time_diff = g_webcam_end_to_end_latency + max((cur_time.QuadPart * (__int64)1000) / Frequency.QuadPart - (g_delta_cur_vs_video_time + msec_video_cur_pos), 0);

							if (video_time_diff > 0)
							{
								int exp_abs_cur_dif = ((cur_speed * (double)(video_time_diff)) / 1000.0);
								exp_abs_cur_pos = abs_cur_pos + exp_abs_cur_dif;
							}
						}

						d_cur_from_search_start_pos = get_abs_to_target_pos(exp_abs_cur_pos, funscript_data_maped[0].second) - funscript_data_maped[0].second;
						d_exp_from_search_start_pos = get_abs_to_target_pos(exp_abs_cur_pos + ((cur_speed * (double)g_speed_change_delay) / 1000.0), funscript_data_maped[0].second) - funscript_data_maped[0].second;

						if (g_stop_run || g_pause || g_video_freezed || !is_video_paused || g_was_change_in_use_modify_funscript_functions || (last_play_video_filename != video_filename) || (cur_video_pos > funscript_data_maped[1].first) || (cur_video_pos < search_video_pos) || (prev_rate != g_video_cur_rate))
						{
							break;
						}

					} while ( (d_cur_from_search_start_pos < g_min_search_pos_dif) ||
							(d_cur_from_search_start_pos > g_max_search_pos_dif) ||
							(d_exp_from_search_start_pos < g_min_search_pos_dif) ||
							(d_exp_from_search_start_pos > g_max_search_pos_dif) );

					cur_set_hismith_speed = set_hismith_speed(0.0);
					prev_set_hismith_speed_time = set_hismith_speed_time;
					QueryPerformanceCounter(&set_hismith_speed_time);

					do
					{
						get_res = get_next_frame_and_cur_speed(*g_pCapture, frame,
							abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
							msec_video_prev_pos, abs_prev_pos);
					} while (cur_speed > 0);

					g_d_from_search_start_pos = get_abs_to_target_pos(abs_cur_pos, funscript_data_maped[0].second) - funscript_data_maped[0].second;

					if (g_stop_run || g_pause || g_video_freezed || g_was_change_in_use_modify_funscript_functions || (last_play_video_filename != video_filename) || (cur_video_pos > funscript_data_maped[1].first) || (cur_video_pos < search_video_pos) || (prev_rate != g_video_cur_rate))
					{
						if (g_pause)
						{
							make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
							if (!is_video_paused && (video_filename.size() > 0))
							{
								make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate, QString("?command=pl_pause"));
							}
							if (g_update)
							{
								show_cur_execution_status(QString("Paused execution funscript.\n"), 2000);
							}
						}

						continue;
					}

					if ((g_d_from_search_start_pos < g_min_search_pos_dif) || (g_d_from_search_start_pos > g_max_search_pos_dif))
					{
						position_was_aligned = true;
					}
				}
				else
				{
					position_was_aligned = true;
				}
			}

			make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
			get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos);
			g_actual_video_pos = cur_video_pos;
			start_time = cur_time;
			start_video_pos = cur_video_pos;

			if (g_stop_run || g_pause || g_video_freezed || g_was_change_in_use_modify_funscript_functions || (last_play_video_filename != video_filename) || (cur_video_pos > funscript_data_maped[1].first) || (cur_video_pos < search_video_pos) || (prev_rate != g_video_cur_rate) ||
				(is_vlc_time_in_milliseconds && !video_was_paused && is_video_paused))
			{
				if (g_pause)
				{
					make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
					if (!is_video_paused && (video_filename.size() > 0))
					{
						make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate, QString("?command=pl_pause"));
					}
					if (g_update)
					{
						show_cur_execution_status(QString("Paused execution funscript.\n"), 2000);
					}
				}
				continue;
			}

			if (is_video_paused)
			{
				get_new_camera_frame(*g_pCapture, frame, msec_video_cur_pos);
				get_res = get_hismith_pos_by_image(frame, cur_pos);
				if (!get_res)
				{
					show_msg(QString("Failed to get device position accoring webcam frame."));
					g_stop_run = true;
					break;
				}
				abs_cur_pos = get_abs_to_target_pos(cur_pos, funscript_data_maped[0].second);
				g_d_from_search_start_pos = abs_cur_pos - funscript_data_maped[0].second;

				show_msg(QString("Ready to go!\n%1").arg(get_add_msg_data()), 5000, MessageType::Always, g_modify_funscript);
			}
			else
			{
				show_msg(QString("Runing!\n%1").arg(get_add_msg_data()), 2000, MessageType::Always, g_modify_funscript);
			}

			start_info += QString("\ncur_video_time:%1 before wait for video run").arg(VideoTimeToStr(cur_video_pos).c_str());

			QueryPerformanceCounter(&cur_time);
			time_stat.dt4 = time_diff_in_milliseconds(cur_time, prev_time, Frequency);
			prev_time = cur_time;

			if (g_update)
			{
				std::lock_guard lk(g_update_mutex);
				g_update = false;
				g_update_cvar.notify_all();
			}

			while (
				is_video_paused ||
				( ((int)((double)(funscript_data_maped[0].first - cur_video_pos) / g_video_cur_rate) > g_speed_change_delay + g_min_dt_between_speed_changes_on_fast_moves) && position_was_aligned ) ||
				((int)((double)(funscript_data_maped[0].first - cur_video_pos) / g_video_cur_rate) >= g_speed_change_delay + 2*g_min_dt_between_speed_changes_on_fast_moves)
				)
			{
				make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
				get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos);
				g_actual_video_pos = cur_video_pos;
				start_time = cur_time;
				start_video_pos = cur_video_pos;

				if (g_stop_run || g_pause || g_video_freezed || g_was_change_in_use_modify_funscript_functions || (last_play_video_filename != video_filename) || ((int)((double)(cur_video_pos - funscript_data_maped[1].first) / g_video_cur_rate) > 200) || (cur_video_pos < search_video_pos) || (prev_rate != g_video_cur_rate) ||
					(is_video_paused && g_update) ||
					(is_video_paused && (cur_video_pos - search_video_pos >= 1000)) ||
					(is_vlc_time_in_milliseconds && !video_was_paused && is_video_paused))
				{
					break;
				}

				if (g_update)
				{
					show_cur_execution_status(QString("Ready to go!\n"));
				}
			}

			if ( (is_video_paused && g_update) ||
				(is_vlc_time_in_milliseconds && !video_was_paused && is_video_paused) )
			{
				continue;
			}

			g_runing_funscript = true;

			start_info += QString("\ncur_video_time:%1 after wait for video run").arg(VideoTimeToStr(cur_video_pos).c_str());

			int action_id = 1, action_id_start = 0;

			get_new_camera_frame(*g_pCapture, frame, msec_video_cur_pos);
			get_res = get_hismith_pos_by_image(frame, cur_pos);
			if (!get_res)
			{
				show_msg(QString("Failed to get device position accoring webcam frame."));
				g_stop_run = true;
				break;
			}
			abs_cur_pos = get_abs_to_target_pos(cur_pos, funscript_data_maped[action_id_start].second);
			start_info += QString("\nSetting abs_cur_pos to abs_cur_pos:%1 req_actio_start_pos:%2 cur_pos:%3 with action_id:%4")
				.arg(abs_cur_pos)
				.arg(funscript_data_maped[action_id - 1].second)
				.arg(cur_pos)
				.arg(action_id - 1);

			if (g_stop_run || g_pause || g_video_freezed || g_was_change_in_use_modify_funscript_functions || (last_play_video_filename != video_filename) || ((int)((double)(cur_video_pos - funscript_data_maped[1].first) / g_video_cur_rate) > 200) || (cur_video_pos < search_video_pos) || (prev_rate != g_video_cur_rate) ||
				(is_video_paused && (cur_video_pos - search_video_pos >= 1000)))
			{
				if (g_pause)
				{
					make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
					if (!is_video_paused && (video_filename.size() > 0))
					{
						make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate, QString("?command=pl_pause"));
					}
					if (g_update)
					{
						show_cur_execution_status(QString("Paused execution funscript.\n"), 2000);
					}
				}

				g_results_file_data += QString("video_name:%1 start_t:%2[%3 msec] start_pos:%4 req_pos:%5\nvideo_speed_rate:%6\n%7\n\n")
					.arg(start_video_name)
					.arg(VideoTimeToStr(start_video_pos).c_str())
					.arg(start_video_pos)
					.arg(abs_cur_pos)
					.arg(funscript_data_maped[action_id - 1].second)
					.arg(g_video_cur_rate)
					.arg(start_info);

				continue;
			}

			LARGE_INTEGER action_start_time, speed_change_time, prev_get_speed_time = start_time;
			int start_abs_pos, exp_abs_cur_pos_to_req_time, tmp_val, dif_cur_vs_req_action_start_time, last_set_action_id_for_dif_cur_vs_req_exp_pos;

			int exp_abs_pos_before_speed_change, action_start_abs_pos;
			int req_speed, req_cur_speed, req_new_speed;
			double optimal_hismith_speed = 0, hismith_speed_prev = 0, optimal_hismith_start_speed = 0, avg_hismith_speed_prev = 0;
			QString actions_end_with = "success";
			bool speed_change_was_made = false;
			QString hismith_speed_changed;

			msec_video_prev_pos = -1;
			abs_prev_pos = 0;
			dpos = 0;
			cur_speed = 0;

			LARGE_INTEGER t1, t2, t3, dt_total;

			if (!is_vlc_time_in_milliseconds)
			{
				// for minimize time difference sync waiting for the nearest second change
				int prev_video_pos;
				do
				{
					prev_video_pos = cur_video_pos;
					make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
					get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos);
					g_actual_video_pos = cur_video_pos;
					start_time = cur_time;
					start_video_pos = cur_video_pos;
					if (g_update)
					{
						show_cur_execution_status(QString("Minimize time difference sync with video.\n"));
					}
				} while ((prev_video_pos / 1000 == cur_video_pos / 1000) && !g_stop_run);

				if (g_stop_run)
				{
					break;
				}

				// updating abs_cur_pos due to time freeze (possible device position was changed during it)

				get_new_camera_frame(*g_pCapture, frame, msec_video_cur_pos);
				get_res = get_hismith_pos_by_image(frame, cur_pos);
				if (!get_res)
				{
					show_msg(QString("Failed to get device position accoring webcam frame."));
					g_stop_run = true;
					break;
				}
				abs_cur_pos = get_abs_to_target_pos(cur_pos, funscript_data_maped[action_id - 1].second);
				start_info += QString("\nUpdating abs_cur_pos after wait to abs_cur_pos:%1 req_actio_start_pos:%2 cur_pos:%3 with action_id:%4")
					.arg(abs_cur_pos)
					.arg(funscript_data_maped[action_id - 1].second)
					.arg(cur_pos)
					.arg(action_id - 1);
			}

			while ((action_id < actions_size) && ((int)((double)(funscript_data_maped[action_id - 1].first - cur_video_pos) / g_video_cur_rate) < g_speed_change_delay))
			{
				start_info += QString("\ncur_video_time:%1 > (actio_start_time:%2 - speed_change_delay:%3) with action_id:%4 => action_id++")
					.arg(VideoTimeToStr(cur_video_pos).c_str())
					.arg(VideoTimeToStr(funscript_data_maped[action_id - 1].first).c_str())
					.arg(g_speed_change_delay)
					.arg(action_id);
				action_id++;
			}

			while ( (action_id < actions_size) &&
					( (double)(funscript_data_maped[action_id].second - abs_cur_pos) / (double)(funscript_data_maped[action_id].first - cur_video_pos)
					> 1.2 * ( ((double)(funscript_data_maped[action_id].second - funscript_data_maped[action_id_start].second)) /
				  			(double)(funscript_data_maped[action_id].first - funscript_data_maped[action_id_start].first) ) )
					)
			{
				start_info += QString("\nreq_speed/actions_speed:%1 > 1.2 with action_id:%4 => action_id++")
					.arg(((double)(funscript_data_maped[action_id].second - abs_cur_pos) / (double)(funscript_data_maped[action_id].first - cur_video_pos)) /
						((((double)(funscript_data_maped[action_id].second - funscript_data_maped[action_id_start].second)) / (double)(funscript_data_maped[action_id].first - funscript_data_maped[action_id_start].first))))
					.arg(action_id);
				action_id++;
			}

			int avg_req_speed, avg_req_hismith_speed, req_dt;

			avg_req_speed = (((double)(funscript_data_maped[action_id].second - abs_cur_pos) * 1000.0) /
				((double)(funscript_data_maped[action_id].first - cur_video_pos) / g_video_cur_rate));
			avg_req_hismith_speed = get_avg_hismith_speed(all_speeds_data, avg_req_speed);
			req_dt = (g_min_dt_start_for_speed_40 * avg_req_hismith_speed) / 40;

			while (funscript_data_maped[action_id].first - cur_video_pos < req_dt)
			{
				start_info += QString("\n(action_id_end_time - cur_video_pos):%1 < req_dt:%2 (for avg_req_speed: %3) => action_id++")
					.arg(funscript_data_maped[action_id].first - cur_video_pos)
					.arg(req_dt)
					.arg(avg_req_hismith_speed);

				action_id++;
				avg_req_speed = (((double)(funscript_data_maped[action_id].second - abs_cur_pos) * 1000.0) /
					((double)(funscript_data_maped[action_id].first - cur_video_pos) / g_video_cur_rate));
				avg_req_hismith_speed = get_avg_hismith_speed(all_speeds_data, avg_req_speed);
				req_dt = (g_min_dt_start_for_speed_40 * avg_req_hismith_speed) / 40;
			}

			start_abs_pos = abs_cur_pos;
			last_set_action_id_for_dif_cur_vs_req_exp_pos = action_id - 1;

			int start_video_pos_dif;
			int new_start_video_pos;
			LARGE_INTEGER _tmp_cur_time;
			int _tmp_video_pos = -1;
			__int64 _tmp_vlc_sys_time = -1;
			double _tmp_cur_rate = 1;
			bool _tmp_is_paused = false;
			QString _tmp_video_filename;
			bool _tmp_is_vlc_time_in_milliseconds;
			int prev_cur_video_pos = cur_video_pos;
			std::thread* p_get_vlc_status = NULL;

			QueryPerformanceCounter(&cur_time);
			time_stat.dt5 = time_diff_in_milliseconds(cur_time, prev_time, Frequency);
			prev_time = cur_time;
			// required for check computer freezes
			prev_get_speed_time = cur_time;

			start_info += QString("\n\nInitial target action_id:%1 action_id_start:%2\n"
				"dstart_pos_cur_vs_act_id_0:%3 dstart_time_cur_vs_act_id_0:%4\n"
				"dstart_pos_cur_vs_act_id_start:%5 dstart_time_cur_vs_act_id_start:%6\n"
				"req_speed/actions_speed:%7\n")
				.arg(action_id)
				.arg(action_id_start)
				.arg(abs_cur_pos - funscript_data_maped[0].second)
				.arg((start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate)) - funscript_data_maped[0].first)
				.arg(abs_cur_pos - funscript_data_maped[action_id_start].second)
				.arg((start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate)) - funscript_data_maped[action_id_start].first)
				.arg(( (double)(funscript_data_maped[action_id].second - abs_cur_pos) / (double)(funscript_data_maped[action_id].first - cur_video_pos) ) /
						( (((double)(funscript_data_maped[action_id].second - funscript_data_maped[action_id_start].second)) / (double)(funscript_data_maped[action_id].first - funscript_data_maped[action_id_start].first)) ));

			const __int64 start_time_in_ms = (start_time.QuadPart * (__int64)1000) / Frequency.QuadPart;

			{
				__int64 frame_time_in_ms = g_delta_cur_vs_video_time + msec_video_cur_pos - g_webcam_end_to_end_latency;
				int frame_video_pos = start_video_pos + (int)((double)(frame_time_in_ms - start_time_in_ms) * g_video_cur_rate);
				frames_data_history.push_back({ abs_cur_pos, frame_video_pos });
			}

			while (action_id < actions_size)
			{
				speed_change_was_made = false;
				QueryPerformanceCounter(&cur_time);
				action_start_time = cur_time;

				exp_abs_cur_pos = abs_cur_pos;
				if (cur_speed > 0)
				{
					__int64 video_time_diff = g_webcam_end_to_end_latency + max((cur_time.QuadPart * (__int64)1000) / Frequency.QuadPart - (g_delta_cur_vs_video_time + msec_video_cur_pos), 0);

					if (video_time_diff > 0)
					{
						int exp_abs_cur_dif = ((cur_speed * (double)(video_time_diff)) / 1000.0);
						exp_abs_cur_pos = abs_cur_pos + exp_abs_cur_dif;
						hismith_speed_changed += QString("\n\t[exp_abs_cur_dif:%1 video_time_diff:%2 cur_speed:%3]")
							.arg(exp_abs_cur_dif)
							.arg(video_time_diff)
							.arg(cur_speed);
					}
				}

				hismith_speed_changed.clear();

				if (g_update && !g_pause)
				{
					show_cur_execution_status(QString("Runing!\n"), 2000);
				}

				int actual_action_id = action_id;
				g_actual_video_pos = start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate);
				while (actual_action_id > 1 && (funscript_data_maped[actual_action_id - 1].first > g_actual_video_pos))
				{
					actual_action_id--;
				}

				int dpos_exp = 0;
				if ( (g_actual_video_pos > funscript_data_maped[actual_action_id - 1].first) &&
						(g_actual_video_pos <= funscript_data_maped[actual_action_id].first) )
				{
					dpos_exp = ((funscript_data_maped[actual_action_id].second - funscript_data_maped[actual_action_id - 1].second) *
						(g_actual_video_pos - funscript_data_maped[actual_action_id - 1].first)) /
						(funscript_data_maped[actual_action_id].first - funscript_data_maped[actual_action_id - 1].first);
				}
				int req_dpos = funscript_data_maped[action_id].second - funscript_data_maped[actual_action_id - 1].second - dpos_exp;
				int cur_dpos = funscript_data_maped[action_id].second - exp_abs_cur_pos;

				int move_dif = cur_dpos - req_dpos;

				if (move_dif >= 360)
				{
					move_dif = move_dif - (move_dif % 360);
					hismith_speed_changed += QString("\n\t[skip_part_of_moves_by_shift_abs_cur_pos_on:%1]").arg(move_dif);
					abs_cur_pos += move_dif;
					exp_abs_cur_pos += move_dif;
					shift_get_next_frame_and_cur_speed_data(move_dif);
				}

				// in case if user manually changed speed higher than it should be or for some other reasons
				if (move_dif < -360)
				{
					move_dif = -move_dif;
					move_dif = move_dif - (move_dif % 360);
					move_dif = -move_dif;
					hismith_speed_changed += QString("\n\t[it_looks_user_manually_changed_speed][shift_abs_cur_pos_on:%1]").arg(move_dif);
					abs_cur_pos += move_dif;
					exp_abs_cur_pos += move_dif;
					shift_get_next_frame_and_cur_speed_data(move_dif);
				}

				int fut_action_id = action_id;

				while ( fut_action_id < actions_size - 1 &&
						(funscript_data_maped[fut_action_id].first - funscript_data_maped[actual_action_id - 1].first <= 333) )
				{
					fut_action_id++;
				}

				avg_req_speed = ( ((double)(funscript_data_maped[fut_action_id].second - funscript_data_maped[actual_action_id - 1].second) * 1000.0) /
										((double)(funscript_data_maped[fut_action_id].first - funscript_data_maped[actual_action_id - 1].first) / g_video_cur_rate) );

				avg_req_hismith_speed = get_avg_hismith_speed(all_speeds_data, avg_req_speed);

				int min_dt_between_speed_changes = g_min_dt_between_speed_changes_on_slow_moves;

				if (avg_req_hismith_speed >= g_fast_move_min_hismith_speed_for_switch_min_dt_between_speed_changes)
				{
					min_dt_between_speed_changes = g_min_dt_between_speed_changes_on_fast_moves;
				}

				action_start_abs_pos = exp_abs_cur_pos;
				action_start_speed = cur_speed;
				speed_change_time.QuadPart = -1;

				dif_cur_vs_req_action_start_time = (start_video_pos + (int)((double)(time_diff_in_milliseconds(action_start_time, start_time, Frequency)) * g_video_cur_rate)) - funscript_data_maped[action_id - 1].first;

				dt = ((double)(funscript_data_maped[action_id].first - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate))) / g_video_cur_rate);
				if (dt < g_speed_change_delay + min_dt_between_speed_changes)
				{
					hismith_speed_changed += QString("\n\t[action_id++: dt:(%1) < req:(%2)]").arg(dt).arg(g_speed_change_delay + min_dt_between_speed_changes);

					results[action_id - 1].avg_req_hismith_speed = avg_req_hismith_speed;
					results[action_id - 1].min_dt_between_speed_changes = min_dt_between_speed_changes;
					results[action_id - 1].actual_action_id_dif = actual_action_id - action_id;
					results[action_id - 1].move_dif = move_dif;
					results[action_id - 1].action_start_video_time = VideoTimeToStr(funscript_data_maped[action_id - 1].first).c_str();
					results[action_id - 1].dif_cur_vs_req_action_end_time = (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate)) - funscript_data_maped[action_id].first;
					results[action_id - 1].dif_cur_vs_req_action_start_time = dif_cur_vs_req_action_start_time;
					results[action_id - 1].action_length_time = funscript_data_maped[action_id].first - funscript_data_maped[action_id - 1].first;
					results[action_id - 1].req_dpos = funscript_data_maped[action_id].second - funscript_data_maped[action_id - 1].second;
					results[action_id - 1].req_dpos_add = funscript_data_maped[action_id - 1].second - action_start_abs_pos;
					results[action_id - 1].start_speed = (int)action_start_speed;
					results[action_id - 1].end_speed = (int)cur_speed;
					results[action_id - 1].hismith_speed_changed = hismith_speed_changed;
					action_id++;
					continue;
				}

				if ((int)((double)(start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate) - cur_video_pos) / g_video_cur_rate) >= 250)
				{
					p_get_vlc_status = new std::thread([&_tmp_video_pos, &_tmp_vlc_sys_time, &_tmp_is_paused, &_tmp_video_filename, &_tmp_is_vlc_time_in_milliseconds, &_tmp_cur_time, &_tmp_cur_rate] {
						make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, _tmp_is_paused, _tmp_video_filename, _tmp_is_vlc_time_in_milliseconds, _tmp_video_pos, _tmp_vlc_sys_time, _tmp_cur_rate);
						QueryPerformanceCounter(&_tmp_cur_time);
						}
					);
				}

				hismith_speed_prev = cur_set_hismith_speed;
				avg_hismith_speed_prev = (double)get_avg_hismith_speed(all_speeds_data, action_start_speed) / 100.0;
				exp_abs_pos_before_speed_change = exp_abs_cur_pos + ((cur_speed * min((double)g_speed_change_delay, dt)) / 1000.0);
				dpos = funscript_data_maped[action_id].second - exp_abs_cur_pos;
				req_speed = (dpos * 1000) / (int)dt;


				{
					optimal_hismith_speed = (double)get_optimal_hismith_speed(all_speeds_data, (int)(hismith_speed_prev * 100.0), cur_speed, dpos, dt) / 100.0;

					int optimal_hismith_start_speed_int = (int)(optimal_hismith_speed * 100.0);

					if (action_start_speed <= req_speed)
					{
						if (optimal_hismith_start_speed_int < avg_req_hismith_speed)
						{
							optimal_hismith_start_speed_int = (optimal_hismith_start_speed_int + avg_req_hismith_speed) / 2;
						}
					}
					else
					{
						if (optimal_hismith_start_speed_int > avg_req_hismith_speed)
						{
							optimal_hismith_start_speed_int = (optimal_hismith_start_speed_int + avg_req_hismith_speed) / 2;
						}
					}

					optimal_hismith_start_speed = (double)(optimal_hismith_start_speed_int) / 100.0;
				}

				if ( (optimal_hismith_start_speed != cur_set_hismith_speed) ||
					((cur_set_hismith_speed == 0) && ((int)(time_diff_in_milliseconds(cur_time, set_hismith_speed_time, Frequency)) > 3*min_dt_between_speed_changes)) )
				{
					if ((cur_set_hismith_speed == 0) && (optimal_hismith_start_speed == cur_set_hismith_speed))
					{
						hismith_speed_changed += QString("\n\t[optimal_hismith_start_speed == cur_set_hismith_speed == 0 forcing_to_stop_device: dt:%1]").arg((int)(time_diff_in_milliseconds(cur_time, set_hismith_speed_time, Frequency)));
					}

					optimal_hismith_start_speed = set_hismith_speed(optimal_hismith_start_speed);
					cur_set_hismith_speed = optimal_hismith_start_speed;
					prev_set_hismith_speed_time = set_hismith_speed_time;
					QueryPerformanceCounter(&set_hismith_speed_time);
					speed_change_was_made = true;

					hismith_speed_changed += QString("\n\t[act_start spd_change: new_set_h_spd:%1 opt_h_spd:%2 set_h_spd_dt:%3 tm_ofs_to_end:%4 cur_spd:%5 req_spd:%6 req_pos_vs_cur:%7]")
						.arg((int)(cur_set_hismith_speed * 100.0))
						.arg((int)(optimal_hismith_speed * 100.0))
						.arg((int)(time_diff_in_milliseconds(set_hismith_speed_time, prev_set_hismith_speed_time, Frequency)))
						.arg(funscript_data_maped[action_id].first - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate)))
						.arg(cur_speed)
						.arg(req_speed)
						.arg(funscript_data_maped[action_id].second - exp_abs_cur_pos);
				}

				do
				{
					if (g_stop_run || g_pause || g_video_freezed || g_was_change_in_use_modify_funscript_functions || is_video_paused || ((int)((double)(cur_video_pos - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate))) / g_video_cur_rate) > (is_vlc_time_in_milliseconds ? 300 : 1000)) ||
						(cur_video_pos < prev_cur_video_pos - 300) || (last_play_video_filename != video_filename) || (prev_rate != g_video_cur_rate))
					{
						break;
					}

					get_res = get_next_frame_and_cur_speed(*g_pCapture, frame,
						abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
						msec_video_prev_pos, abs_prev_pos);

					if (!get_res)
					{
						show_msg("", 0, MessageType::Clean);
						show_msg(QString("Failed to get hismith position.\nPausing video if not and pausing hismith control."));
						if (p_get_vlc_status)
						{
							p_get_vlc_status->join();
							delete p_get_vlc_status;
							p_get_vlc_status = NULL;
						}
						g_pause = true;
						clear_msgs = false;
						break;
					}

					QueryPerformanceCounter(&cur_time);

					{
						__int64 frame_time_in_ms = g_delta_cur_vs_video_time + msec_video_cur_pos - g_webcam_end_to_end_latency;
						int frame_video_pos = start_video_pos + (int)((double)(frame_time_in_ms - start_time_in_ms) * g_video_cur_rate);
						frames_data_history.push_back({ abs_cur_pos, frame_video_pos });
					}

					exp_abs_cur_pos = abs_cur_pos;
					if (cur_speed > 0)
					{
						__int64 video_time_diff = g_webcam_end_to_end_latency + max((cur_time.QuadPart * (__int64)1000) / Frequency.QuadPart - (g_delta_cur_vs_video_time + msec_video_cur_pos), 0);

						if (video_time_diff > 0)
						{
							int exp_abs_cur_dif = ((cur_speed * (double)(video_time_diff)) / 1000.0);
							exp_abs_cur_pos = abs_cur_pos + exp_abs_cur_dif;
							hismith_speed_changed += QString("\n\t[exp_abs_cur_dif:%1 video_time_diff:%2 cur_speed:%3]")
								.arg(exp_abs_cur_dif)
								.arg(video_time_diff)
								.arg(cur_speed);
						}
					}

					if ((int)(time_diff_in_milliseconds(cur_time, prev_get_speed_time, Frequency)) > g_cpu_freezes_timeout)
					{
						break;
					}

					g_actual_video_pos = start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate);

					if ((int)((double)(funscript_data_maped[action_id].first - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate))) / g_video_cur_rate) > 1000)
					{
						if ((int)((double)(start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate) - cur_video_pos) / g_video_cur_rate) >= 1000)
						{
							if (p_get_vlc_status)
							{
								p_get_vlc_status->join();
								delete p_get_vlc_status;
								p_get_vlc_status = NULL;
								prev_cur_video_pos = cur_video_pos;
								is_video_paused = _tmp_is_paused;
								video_pos = _tmp_video_pos;
								vlc_sys_time = _tmp_vlc_sys_time;
								video_filename = _tmp_video_filename;
								is_vlc_time_in_milliseconds = _tmp_is_vlc_time_in_milliseconds;
								g_video_cur_rate = _tmp_cur_rate;
								get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos);
								g_actual_video_pos = cur_video_pos;

								if (g_stop_run || g_pause || g_video_freezed || g_was_change_in_use_modify_funscript_functions || is_video_paused || ((int)((double)(cur_video_pos - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate))) / g_video_cur_rate) > (is_vlc_time_in_milliseconds ? 300 : 1000)) ||
									(cur_video_pos < prev_cur_video_pos - 300) || (last_play_video_filename != video_filename) || (prev_rate != g_video_cur_rate))
								{
									break;
								}

								if (is_vlc_time_in_milliseconds)
								{
									if (vlc_sys_time > 0)
									{
										new_start_video_pos = cur_video_pos - (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate);
										start_video_pos_dif = new_start_video_pos - start_video_pos;
										start_video_pos = new_start_video_pos;
										hismith_speed_changed += QString("\n\t[get_vlc_status: start_video_pos_dif:%1 new_start_video_pos:%2]").arg(start_video_pos_dif).arg(new_start_video_pos);
									}
									else
									{
										new_start_video_pos = cur_video_pos - (int)((double)(time_diff_in_milliseconds(_tmp_cur_time, start_time, Frequency)) * g_video_cur_rate);
										start_video_pos_dif = new_start_video_pos - start_video_pos;
										start_video_pos = new_start_video_pos;
										hismith_speed_changed += QString("\n\t[get_vlc_status: start_video_pos_dif:%1 new_start_video_pos:%2]").arg(start_video_pos_dif).arg(new_start_video_pos);
									}
								}
							}

							if ((int)((double)(start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate) - cur_video_pos) / g_video_cur_rate) >= 1000)
							{
								p_get_vlc_status = new std::thread([&_tmp_video_pos, &_tmp_vlc_sys_time, &_tmp_is_paused, &_tmp_video_filename, &_tmp_is_vlc_time_in_milliseconds, &_tmp_cur_time, &_tmp_cur_rate] {
									make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, _tmp_is_paused, _tmp_video_filename, _tmp_is_vlc_time_in_milliseconds, _tmp_video_pos, _tmp_vlc_sys_time, _tmp_cur_rate);
									QueryPerformanceCounter(&_tmp_cur_time);
									}
								);
							}
						}
					}

					if (g_update && !g_pause)
					{
						show_cur_execution_status(QString("Runing!\n"), 2000);
					}

					{
						prev_get_speed_time = cur_time;
						prev_cur_speed = cur_speed;
					}

					dt = (double)(funscript_data_maped[action_id].first - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate))) / g_video_cur_rate;
					dpos = funscript_data_maped[action_id].second - exp_abs_cur_pos;
					exp_abs_pos_before_speed_change = exp_abs_cur_pos + ((cur_speed * min((double)g_speed_change_delay, dt)) / 1000.0);

					if ( ((int)(time_diff_in_milliseconds(cur_time, set_hismith_speed_time, Frequency)) >= min_dt_between_speed_changes) &&
							(dt >= g_speed_change_delay + min_dt_between_speed_changes) )
					{
						req_cur_speed = max((dpos * 1000) / (int)dt, 0);

						if (((cur_speed - req_cur_speed) > req_cur_speed / 10) && (exp_abs_pos_before_speed_change >= funscript_data_maped[action_id].second))
						{
							if ( (cur_set_hismith_speed > 0) ||
								((cur_set_hismith_speed == 0) && ((int)(time_diff_in_milliseconds(cur_time, set_hismith_speed_time, Frequency)) > 3*min_dt_between_speed_changes)) )
							{
								optimal_hismith_speed = (double)get_optimal_hismith_speed(all_speeds_data, (int)(cur_set_hismith_speed * 100.0), cur_speed, dpos, dt) / 100.0;
								double optimal_speed = optimal_hismith_speed;

								if (optimal_speed > (double)avg_req_hismith_speed / 100.0)
								{
									optimal_speed = (optimal_hismith_speed + ((double)avg_req_hismith_speed / 100.0)) / 2.0;
								}

								if (cur_set_hismith_speed != optimal_speed)
								{
									cur_set_hismith_speed = set_hismith_speed(optimal_speed);
									prev_set_hismith_speed_time = set_hismith_speed_time;
									QueryPerformanceCounter(&set_hismith_speed_time);
									hismith_speed_changed += QString("\n\t[force_stop spd_change: new_set_h_spd:%1 opt_h_spd:%2 set_h_spd_dt:%3 tm_ofs_to_end:%4 cur_spd:%5 req_cur_spd:%6 req_pos_vs_cur:%7]")
										.arg((int)(cur_set_hismith_speed * 100.0))
										.arg((int)(optimal_hismith_speed * 100.0))
										.arg((int)(time_diff_in_milliseconds(set_hismith_speed_time, prev_set_hismith_speed_time, Frequency)))
										.arg(funscript_data_maped[action_id].first - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate)))
										.arg(cur_speed)
										.arg(req_cur_speed)
										.arg(funscript_data_maped[action_id].second - exp_abs_cur_pos);
									speed_change_was_made = true;
								}
							}
						}
						else
						{
							{
								optimal_hismith_speed = (double)get_optimal_hismith_speed(all_speeds_data, (int)(cur_set_hismith_speed * 100.0), cur_speed, dpos, dt) / 100.0;
								double optimal_speed = optimal_hismith_speed;
								{
									int optimal_hismith_speed_int = (int)(optimal_speed * 100.0);

									if (cur_speed <= req_speed)
									{
										if (optimal_hismith_speed_int < avg_req_hismith_speed)
										{
											optimal_hismith_speed_int = (optimal_hismith_speed_int + avg_req_hismith_speed) / 2;
										}
									}
									else
									{
										if (optimal_hismith_speed_int > avg_req_hismith_speed)
										{
											optimal_hismith_speed_int = (optimal_hismith_speed_int + avg_req_hismith_speed) / 2;
										}
									}

									optimal_speed = (double)(optimal_hismith_speed_int) / 100.0;
								}

								if (cur_set_hismith_speed != optimal_speed)
								{
									if ( (!((optimal_speed < cur_set_hismith_speed) && (cur_speed < req_cur_speed))) &&
											(!((optimal_speed > cur_set_hismith_speed) && (cur_speed > req_cur_speed))) )
									{
										cur_set_hismith_speed = set_hismith_speed(optimal_speed);
										prev_set_hismith_speed_time = set_hismith_speed_time;
										QueryPerformanceCounter(&set_hismith_speed_time);

										if (!speed_change_was_made)
										{
											speed_change_was_made = true;
											hismith_speed_changed += QString("\n\t[spd_change: new_set_h_spd:%1 opt_h_spd:%2 set_h_spd_dt:%3 tm_ofs_to_end:%4 cur_spd:%5 req_cur_spd:%6 req_pos_vs_cur:%7]")
												.arg((int)(cur_set_hismith_speed * 100.0))
												.arg((int)(optimal_hismith_speed * 100.0))
												.arg((int)(time_diff_in_milliseconds(set_hismith_speed_time, prev_set_hismith_speed_time, Frequency)))
												.arg(funscript_data_maped[action_id].first - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate)))
												.arg(cur_speed)
												.arg(req_cur_speed)
												.arg(funscript_data_maped[action_id].second - exp_abs_cur_pos);
										}
										else
										{
											hismith_speed_changed += QString("\n\t[add spd_change: new_set_h_spd:%1 opt_h_spd:%2 set_h_spd_dt:%3 tm_ofs_to_end:%4 cur_spd:%5 req_cur_spd:%6 req_pos_vs_cur:%7]")
												.arg((int)(cur_set_hismith_speed * 100.0))
												.arg((int)(optimal_hismith_speed * 100.0))
												.arg((int)(time_diff_in_milliseconds(set_hismith_speed_time, prev_set_hismith_speed_time, Frequency)))
												.arg(funscript_data_maped[action_id].first - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate)))
												.arg(cur_speed)
												.arg(req_cur_speed)
												.arg(funscript_data_maped[action_id].second - exp_abs_cur_pos);
										}
									}
								}
							}
						}
					}

					dt = (double)(funscript_data_maped[action_id].first - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate))) / g_video_cur_rate;
					dtime = (action_id < actions_size - 1) ? g_speed_change_delay : 0;

					if ( (action_id + 1 < actions_size - 1) &&
						((int)(time_diff_in_milliseconds(cur_time, set_hismith_speed_time, Frequency)) >= min_dt_between_speed_changes) )
					{
						double dt2 = (double)(funscript_data_maped[action_id + 1].first - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate))) / g_video_cur_rate;
						if (dt2 < 50 + g_speed_change_delay + min_dt_between_speed_changes)
						{
							break;
						}
					}

				} while (dt > dtime);

				if (p_get_vlc_status)
				{
					p_get_vlc_status->join();
					delete p_get_vlc_status;
					p_get_vlc_status = NULL;
					prev_cur_video_pos = cur_video_pos;
					is_video_paused = _tmp_is_paused;
					video_pos = _tmp_video_pos;
					vlc_sys_time = _tmp_vlc_sys_time;
					video_filename = _tmp_video_filename;
					is_vlc_time_in_milliseconds = _tmp_is_vlc_time_in_milliseconds;
					g_video_cur_rate = _tmp_cur_rate;
					get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, g_video_cur_rate, cur_time, cur_video_pos);
					g_actual_video_pos = cur_video_pos;

					if (g_stop_run || g_pause || g_video_freezed || g_was_change_in_use_modify_funscript_functions || is_video_paused || ((int)((double)(cur_video_pos - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate))) / g_video_cur_rate) > (is_vlc_time_in_milliseconds ? 300 : 1000)) ||
						(cur_video_pos < prev_cur_video_pos - 300) || (last_play_video_filename != video_filename) || (prev_rate != g_video_cur_rate))
					{
						// need to stop run
					}
					else if (is_vlc_time_in_milliseconds)
					{
						if (vlc_sys_time > 0)
						{
							new_start_video_pos = cur_video_pos - (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate);
							start_video_pos_dif = new_start_video_pos - start_video_pos;
							start_video_pos = new_start_video_pos;
							hismith_speed_changed += QString("\n\t[get_vlc_status: start_video_pos_dif:%1 new_start_video_pos:%2]").arg(start_video_pos_dif).arg(new_start_video_pos);
						}
						else
						{
							new_start_video_pos = cur_video_pos - (int)((double)(time_diff_in_milliseconds(_tmp_cur_time, start_time, Frequency)) * g_video_cur_rate);
							start_video_pos_dif = new_start_video_pos - start_video_pos;
							start_video_pos = new_start_video_pos;
							hismith_speed_changed += QString("\n\t[get_vlc_status: start_video_pos_dif:%1 new_start_video_pos:%2]").arg(start_video_pos_dif).arg(new_start_video_pos);
						}
					}
				}

				results[action_id - 1].avg_req_hismith_speed = avg_req_hismith_speed;
				results[action_id - 1].min_dt_between_speed_changes = min_dt_between_speed_changes;
				results[action_id - 1].actual_action_id_dif = actual_action_id - action_id;
				results[action_id - 1].move_dif = move_dif;
				results[action_id - 1].action_start_video_time = VideoTimeToStr(funscript_data_maped[action_id - 1].first).c_str();
				results[action_id - 1].dif_cur_vs_req_action_end_time = (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate)) - funscript_data_maped[action_id].first;
				results[action_id - 1].dif_cur_vs_req_action_start_time = dif_cur_vs_req_action_start_time;
				results[action_id - 1].action_length_time = funscript_data_maped[action_id].first - funscript_data_maped[action_id - 1].first;
				results[action_id - 1].req_dpos = funscript_data_maped[action_id].second - funscript_data_maped[action_id - 1].second;
				results[action_id - 1].req_dpos_add = funscript_data_maped[action_id - 1].second - action_start_abs_pos;
				results[action_id - 1].req_speed = req_speed;
				results[action_id - 1].start_speed = (int)action_start_speed;
				results[action_id - 1].end_speed = (int)cur_speed;
				results[action_id - 1].hismith_speed_prev = (int)(hismith_speed_prev * 100.0);
				results[action_id - 1].avg_hismith_speed_prev = (int)(avg_hismith_speed_prev * 100.0);
				results[action_id - 1].optimal_hismith_speed = (int)(optimal_hismith_speed * 100.0);
				results[action_id - 1].optimal_hismith_start_speed = (int)(optimal_hismith_start_speed * 100.0);
				results[action_id - 1].hismith_speed_changed = hismith_speed_changed;

				if ((int)(time_diff_in_milliseconds(cur_time, prev_get_speed_time, Frequency)) > g_cpu_freezes_timeout)
				{
					actions_end_with = QString("It looks you get CPU freezes now, restarting all actions.");
					show_msg(actions_end_with);
					actions_size = action_id;
					break;
				}

				if (g_stop_run || g_pause || g_video_freezed || g_was_change_in_use_modify_funscript_functions || is_video_paused ||
					((int)((double)(cur_video_pos - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate))) / g_video_cur_rate) > (is_vlc_time_in_milliseconds ? 300 : 1000)) ||
					(cur_video_pos < prev_cur_video_pos - 300) || (last_play_video_filename != video_filename) || (prev_rate != g_video_cur_rate))
				{
					if (last_play_video_filename != video_filename)
					{
						show_msg("Played video was changed");
						actions_end_with = QString("last_play_video_filename (%1) != video_filename (%2)").arg(last_play_video_filename).arg(video_filename);
					}
					else if ((int)((double)(cur_video_pos - (start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate))) / g_video_cur_rate) > (is_vlc_time_in_milliseconds ? 300 : 1000))
					{
						show_msg("Video time was jumped forward");
						actions_end_with = QString("cur_video_pos (%1) > (start_video_pos + (int)(cur_time - start_time))(%2) + %3").arg(cur_video_pos).arg(start_video_pos + (int)((double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) * g_video_cur_rate)).arg(is_vlc_time_in_milliseconds ? 300 : 1000);
					}
					else if ((action_id > 1) && (cur_video_pos < prev_cur_video_pos - 300))
					{
						show_msg("Video time was jumped backward");
						actions_end_with = QString("cur_video_pos (%1) < prev_cur_video_pos (%2) - 300").arg(cur_video_pos).arg(prev_cur_video_pos);
					}
					else if (prev_rate != g_video_cur_rate)
					{
						show_msg(QString("Video speed rate was changed to: %1").arg(g_video_cur_rate));
						actions_end_with = QString("prev_rate != g_video_cur_rate");
					}
					else if (g_pause)
					{
						actions_end_with = QString("g_pause");
						make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
						if (!is_video_paused && (video_filename.size() > 0))
						{
							make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate, QString("?command=pl_pause"));
						}
						if (g_update)
						{
							show_cur_execution_status(QString("Paused execution funscript.\n"), 2000);
						}
					}
					else if (g_stop_run)
					{
						actions_end_with = QString("g_stop_run");
					}
					else if (g_video_freezed)
					{
						actions_end_with = QString("g_video_freezed");
					}
					else if (g_was_change_in_use_modify_funscript_functions)
					{
						actions_end_with = QString("g_was_change_in_use_modify_funscript_functions");
					}
					else if (is_video_paused)
					{
						actions_end_with = QString("is_video_paused");
					}
					else
					{
						actions_end_with = QString("unknown reason");
					}

					actions_size = action_id;
					break;
				}

				action_id++;
			}

			QueryPerformanceCounter(&cur_time);
			prev_time = cur_time;
			int avg_dif_cur_vs_req_exp_pos = -999;
			int deviation_dif_cur_vs_req_exp_pos = -999;

			if (actions_size > 2)
			{
				int i = 0;
				int _id = 1;
				__int64 cnt = 0;
				__int64 sum_dif_cur_vs_req_exp_pos = 0;
				__int64 sum_sq_dif_cur_vs_req_exp_pos = 0;
				FrameData frame_data_prev;

				for (const FrameData& frame_data : frames_data_history)
				{
					if (i == 0)
					{
						frame_data_prev = frame_data;
					}

					if (frame_data.video_pos >= funscript_data_maped[_id].first)
					{
						if (frame_data_prev.video_pos <= funscript_data_maped[_id].first)
						{
							double exp_abs_cur_pos_to_req_time =
								(double)frame_data_prev.abs_pos +
								(((double)(funscript_data_maped[_id].first - frame_data_prev.video_pos) /
								(double)(frame_data.video_pos - frame_data_prev.video_pos)) *
								(double)(frame_data.abs_pos - frame_data_prev.abs_pos));
							results[_id - 1].dif_cur_vs_req_exp_pos = exp_abs_cur_pos_to_req_time - funscript_data_maped[_id].second;
							sum_dif_cur_vs_req_exp_pos += results[_id - 1].dif_cur_vs_req_exp_pos;
							sum_sq_dif_cur_vs_req_exp_pos += results[_id - 1].dif_cur_vs_req_exp_pos * results[_id - 1].dif_cur_vs_req_exp_pos;
							cnt++;
						}

						_id++;

						if (_id > actions_size - 1)
						{
							break;
						}
					}

					frame_data_prev = frame_data;
					i++;
				}

				if (cnt > 0)
				{
					avg_dif_cur_vs_req_exp_pos = sum_dif_cur_vs_req_exp_pos / cnt;
					deviation_dif_cur_vs_req_exp_pos = std::sqrt(sum_sq_dif_cur_vs_req_exp_pos / cnt);
				}
			}

			QString result_str = start_info + QString("\n");
			for (int i = 0; i < actions_size; i++)
			{
				result_str += QString("dif_end_pos:%1 start_t:%2 len:%3 req_dpos:%4+(%5) dif_start_t:%6 dif_end_t:%7 "
									"actual_action_id_dif:%8 move_dif:%9 avg_req_hismith_speed:%10 min_dt_between_speed_changes:%11 "
									"start_spd:%12 end_spd:%13 req_avg_speed:%14 prev_set_end_h_spd:%15 prev_real_end_h_spd:%16 "
									"opt_avg_h_spd:%17 set_start_h_spd:%18 add_info:%19\n")
					.arg(results[i].dif_cur_vs_req_exp_pos != -999 ? QString::number(results[i].dif_cur_vs_req_exp_pos) : "unknown")
					.arg(results[i].action_start_video_time)
					.arg(results[i].action_length_time)
					.arg(results[i].req_dpos)
					.arg(results[i].req_dpos_add)
					.arg(results[i].dif_cur_vs_req_action_start_time)
					.arg(results[i].dif_cur_vs_req_action_end_time)
					.arg(results[i].actual_action_id_dif)
					.arg(results[i].move_dif)
					.arg(results[i].avg_req_hismith_speed)
					.arg(results[i].min_dt_between_speed_changes)
					.arg(results[i].start_speed)
					.arg(results[i].end_speed)
					.arg(results[i].req_speed)
					.arg(results[i].hismith_speed_prev)
					.arg(results[i].avg_hismith_speed_prev)
					.arg(results[i].optimal_hismith_speed)
					.arg(results[i].optimal_hismith_start_speed)
					.arg(results[i].hismith_speed_changed);
			}
			result_str += QString(
				"actions_end_with: %1\n"
				"average_dif_end_pos: %2 deviation_dif_end_pos: %3\n"
				"\n")
				.arg(actions_end_with)
				.arg(avg_dif_cur_vs_req_exp_pos != -999 ? QString::number(avg_dif_cur_vs_req_exp_pos) : "unknown")
				.arg(deviation_dif_cur_vs_req_exp_pos != -999 ? QString::number(deviation_dif_cur_vs_req_exp_pos) : "unknown");

			g_results_file_data += QString(	"time_statistic: dt1:%1 dt2:%2 dt3:%3 dt4:%4 dt5:%5 dt6:%6\n"
							"video_name:%7 start_t:%8[%9 msec] start_pos:%10 req_pos:%11\n"
							"video_speed_rate:%12\n"
							"%13")
				.arg(time_stat.dt1)
				.arg(time_stat.dt2)
				.arg(time_stat.dt3)
				.arg(time_stat.dt4)
				.arg(time_stat.dt5)
				.arg(time_stat.dt6)
				.arg(start_video_name)
				.arg(VideoTimeToStr(start_video_pos).c_str())
				.arg(start_video_pos)
				.arg(start_abs_pos)
				.arg(funscript_data_maped[0].second)
				.arg(g_video_cur_rate)
				.arg(result_str);

			cur_set_hismith_speed = set_hismith_speed(0.0);
			prev_set_hismith_speed_time = set_hismith_speed_time;
			QueryPerformanceCounter(&set_hismith_speed_time);

			if (p_save_results)
			{
				p_save_results->join();
				delete p_save_results;
				p_save_results = NULL;
			}

			p_save_results = new std::thread([results_file_path = g_results_file_path, results_file_data = g_results_file_data] {
				save_results_file_data(results_file_path, results_file_data);
			} );

			g_results_file_data.clear();
		}
	}
	catch (const HardwareException& e) {
		error_msg(QString("Caught %1").arg(e.toQString()));
	}
	catch (const std::exception& e) {
		error_msg(QString("Caught C++ Exception: %1").arg(e.what()));
	}
	catch (...) {
		error_msg(QString("Caught Unknown Exception"));
	}

	if (p_save_results)
	{
		p_save_results->join();
		delete p_save_results;
		p_save_results = NULL;
	}

	if (g_results_file_data.length() > 0)
	{
		save_results_file_data(g_results_file_path, g_results_file_data);
	}

	if (g_update)
	{
		std::lock_guard lk(g_update_mutex);
		g_update = false;
		g_update_cvar.notify_all();
	}

	disconnect_from_hismith();

	g_threaded_capture.stop();
	g_high_precision_timer_guard.Stop();
	g_pCapture->release();
	delete g_pCapture;
	g_pCapture = NULL;

	make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate);
	if ((video_filename.size() != 0) && (!is_video_paused))
	{
		make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, g_video_cur_rate, QString("?command=pl_pause"));
	}

	g_pNetworkAccessManager->deleteLater();
	g_pNetworkAccessManager = NULL;
}

void disconnect_from_hismith()
{
	if (g_pMyDevice)
	{
		g_pClient->stopAllDevices();
		g_myDevices.clear();
		delete g_pClient;
		g_pClient = NULL;
		g_pMyDevice = NULL;
	}
}

bool get_devices_list(bool show_msgs)
{
	bool res = false;
	//-----------------------------------------------------
	// Connecting to Hismith
	// NOTE: At first start: intiface central

	if (!g_pClient)
	{
		g_pClient = new Client(g_intiface_central_client_url, g_intiface_central_client_port);
		g_pClient->connect(callbackFunction);
	}

	_tmp_got_client_msg = false;
	g_pClient->requestDeviceList();
	g_pClient->startScan();
	while (1) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		g_pClient->stopScan();
		break;
	}

	g_myDevices = g_pClient->getDevices();

	if (show_msgs && (g_myDevices.size() == 0))
	{
		if (!_tmp_got_client_msg)
		{
			warning_msg(QString("It looks \"Intiface Central\" not started\nor has another client URL: %1 or port: %2\nPlease start and \"Refresh Devices List\"").arg(g_intiface_central_client_url.c_str()).arg(g_intiface_central_client_port), "Getting Devices List");
		}
		else
		{
			warning_msg("It Looks no any device connected to \"Intiface Central\".\nPlease connect and \"Refresh Devices List\"", "Getting Devices List");
		}
	}
	else
	{
		res = true;
	}

	return res;
}

bool connect_to_hismith()
{
	bool res = false;
	//-----------------------------------------------------
	// Connecting to Hismith
	// NOTE: At first start: intiface central

	if (!g_pClient)
	{
		g_pClient = new Client(g_intiface_central_client_url, g_intiface_central_client_port);
		g_pClient->connect(callbackFunction);
	}

	_tmp_got_client_msg = false;
	g_pClient->requestDeviceList();
	g_pClient->startScan();
	while (1) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		g_pClient->stopScan();
		break;
	}

	g_myDevices = g_pClient->getDevices();

	g_pMyDevice = NULL;
	QString selected_device = g_pW->ui->Devices->itemText(g_pW->ui->Devices->currentIndex());
	for (DeviceClass& dev : g_myDevices)
	{
		if (selected_device == dev.deviceName.c_str())
		{
			g_pMyDevice = &dev;
			res = true;
			break;
		}
	}

	if (g_myDevices.size() == 0)
	{
		if (!_tmp_got_client_msg)
		{
			error_msg(QString("It looks \"Intiface Central\" not started\nor has another client URL: %1 or port: %2\nPlease start and \"Refresh Devices List\"").arg(g_intiface_central_client_url.c_str()).arg(g_intiface_central_client_port));
		}
		else
		{
			error_msg("It Looks no any device connected to \"Intiface Central\".\nPlease connect and \"Refresh Devices List\"");
		}
	}
	else if (!g_pMyDevice)
	{
		error_msg(QString("ERROR: Selected Hismith device is not currently present"));
	}

	return res;
}

void get_performance_with_hismith(int hismith_speed)
{
	int cur_pos, res;
	__int64 msec_video_cur_pos = -1, last_msec_video_prev_pos;
	int abs_cur_pos, abs_prev_pos = 0;
	__int64 msec_video_prev_pos = -1, msec_video_start_pos;
	int dpos = 0;
	double cur_speed = -1;
	int max_dt_according_webcam_to_get_new_frame_and_speed = 0;
	int max_dt_according_GetTickCount = 0;

	LARGE_INTEGER start_time, cur_time, prev_time, Frequency;
	QueryPerformanceFrequency(&Frequency);

	//-----------------------------------------------------
	// Connecting to Hismith
	// NOTE: At first start: intiface central

	show_msg("Connecting to Hismith...", 120000, MessageType::Clean);

	if (!connect_to_hismith())
	{
		show_msg("", 0, MessageType::Clean);
		return;
	}

	//-----------------------------------------------------
	// Connecting to Webcam

	cv::Mat frame, bad_frame, prev_frame, res_frame;
	cv::VideoCapture capture;

	if (init_camera(capture))
	{
		show_msg("Getting performance data.\n"
				"It will takes about 5 seconds, please wait...", 120000, MessageType::Clean);

		g_threaded_capture.start(&capture);

		get_new_camera_frame(capture, frame, msec_video_cur_pos);
		last_msec_video_prev_pos = msec_video_cur_pos;
		if (!get_hismith_pos_by_image(frame, cur_pos))
		{
			show_msg("", 0, MessageType::Clean);
			g_threaded_capture.stop();
			g_high_precision_timer_guard.Stop();
			capture.release();
			return;
		}
		abs_cur_pos = get_abs_to_target_pos(cur_pos, 0);

		hismith_speed = (int)(set_hismith_speed((double)hismith_speed / 100.0) * 100.0);

		get_next_frame_and_cur_speed(capture, frame,
			abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
			msec_video_prev_pos, abs_prev_pos);

		QueryPerformanceCounter(&cur_time);
		start_time = cur_time;
		msec_video_start_pos = msec_video_cur_pos;

		int num_frames = 0, num_frame_video, num_frame_gtc;
		bool last_get_frame_status = false;
		__int64 max_webcam_time_diff = 0;

		while (1)
		{
			last_msec_video_prev_pos = msec_video_cur_pos;
			last_get_frame_status = get_next_frame_and_cur_speed(capture, frame,
				abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
				msec_video_prev_pos, abs_prev_pos);
			if (!last_get_frame_status)
			{
				break;
			}
			prev_time = cur_time;
			QueryPerformanceCounter(&cur_time);
			num_frames++;

			max_webcam_time_diff = max((cur_time.QuadPart * (__int64)1000) / Frequency.QuadPart - (g_delta_cur_vs_video_time + msec_video_cur_pos), max_webcam_time_diff);

			if (msec_video_cur_pos - last_msec_video_prev_pos > max_dt_according_webcam_to_get_new_frame_and_speed)
			{
				max_dt_according_webcam_to_get_new_frame_and_speed = msec_video_cur_pos - last_msec_video_prev_pos;
				num_frame_video = num_frames;
			}

			if (time_diff_in_milliseconds(cur_time, prev_time, Frequency) > max_dt_according_GetTickCount)
			{
				max_dt_according_GetTickCount = time_diff_in_milliseconds(cur_time, prev_time, Frequency);
				num_frame_gtc = num_frames;
			}

			if (time_diff_in_milliseconds(cur_time, start_time, Frequency) > 5000)
			{
				break;
			}
		}

		g_threaded_capture.stop();
		g_high_precision_timer_guard.Stop();
		capture.release();

		show_msg("", 0, MessageType::Clean);

		set_hismith_speed(0.0);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));

		if (num_frames > 0)
		{
			show_msg(QString("last_get_frame_status: %1\n"
				"max_dt_according_webcam_for_get_new_frame_and_speed:%2 frame_number:%3\n"
				"max_dt_according_QueryPerformanceCounter_for_get_new_frame_and_speed:%4 frame_number:%5\n"
				"avg_dt_according_webcam_for_get_new_frame_and_speed:%6\n"
				"avg_dt_according_QueryPerformanceCounter_for_get_new_frame_and_speed:%7\n"
				"avg_fps_for_get_new_frame_and_speed:%8\n"
				"max_webcam_time_diff:%9")
				.arg(last_get_frame_status)
				.arg(max_dt_according_webcam_to_get_new_frame_and_speed)
				.arg(num_frame_video)
				.arg(max_dt_according_GetTickCount)
				.arg(num_frame_gtc)
				.arg((int)(msec_video_cur_pos - msec_video_start_pos) / num_frames)
				.arg((int)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) / num_frames)
				.arg(((double)num_frames*1000.0)/(double)(time_diff_in_milliseconds(cur_time, start_time, Frequency)))
				.arg(max_webcam_time_diff)
				,
				"Performance Results");
		}
	}
	else
	{
		show_msg("", 0, MessageType::Clean);
	}

	disconnect_from_hismith();
}

void get_statistics_with_hismith(int start_speed, int end_speed)
{
	int cur_pos, res;
	__int64 msec_video_cur_pos = -1, msec_video_prev_pos = -1, last_msec_video_prev_pos, msec_video_start_pos;
	int abs_cur_pos, abs_prev_pos = 0, last_abs_prev_pos;
	int dpos = 0;
	double cur_speed = -1;
	int max_dt_according_webcam_to_get_new_frame_and_speed = 0;
	int max_dt_according_GetTickCount = 0;
	int hismith_speed;

	if (g_stop_run || start_speed < 1 || start_speed > end_speed || end_speed > 100)
	{
		return;
	}

	LARGE_INTEGER get_statistics_start_time, start_time, cur_time, prev_time, Frequency;
	QueryPerformanceFrequency(&Frequency);

	QueryPerformanceCounter(&get_statistics_start_time);

	show_msg(QString("It can takes ~%1 minutes, please wait...\nIt will get data for speed from range: %2-%3")
		.arg(((20*(end_speed-start_speed+1)) + 99)/100)
		.arg(start_speed)
		.arg(end_speed), 5000);

	//-----------------------------------------------------
	// Connecting to Hismith
	// NOTE: At first start: intiface central

	if (!connect_to_hismith())
	{
		return;
	}

	//-----------------------------------------------------
	// Connecting to Webcam

	cv::Mat frame, bad_frame, prev_frame, res_frame;
	cv::VideoCapture capture;

	if (init_camera(capture))
	{
		show_msg("", 0, MessageType::Clean);

		if (g_stop_run)
		{
			return;
		}

		get_new_camera_frame(capture, frame, msec_video_cur_pos);
		if (!get_hismith_pos_by_image(frame, cur_pos))
		{
			g_high_precision_timer_guard.Stop();
			capture.release();
			return;
		}
		abs_cur_pos = get_abs_to_target_pos(cur_pos, 0);

		int results_size = g_webcam_fps * 100, result_id;
		std::vector<statistics_data> results(results_size);

		QueryPerformanceCounter(&cur_time);
		start_time = cur_time;

		while ((int)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) < 1000)
		{
			get_next_frame_and_cur_speed(capture, frame,
				abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
				msec_video_prev_pos, abs_prev_pos);
			QueryPerformanceCounter(&cur_time);
		}

		if (g_stop_run)
		{
			return;
		}

		for (hismith_speed = start_speed; hismith_speed <= end_speed; hismith_speed++)
		{
			bool need_restart;

			do
			{
				show_msg(QString("Starting to get data for speed %1 ...").arg(hismith_speed), 2000);

				set_hismith_speed(0.0);

				QueryPerformanceCounter(&cur_time);
				start_time = cur_time;
				while (((int)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) < 3000) || (cur_speed > 10))
				{
					get_next_frame_and_cur_speed(capture, frame,
						abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
						msec_video_prev_pos, abs_prev_pos);
					QueryPerformanceCounter(&cur_time);
				}

				need_restart = false;

				g_pClient->sendScalar(*g_pMyDevice, (double)hismith_speed / 100.0);

				result_id = 0;
				QueryPerformanceCounter(&cur_time);
				start_time = cur_time;

				while ( ((int)(time_diff_in_milliseconds(cur_time, start_time, Frequency)) < 7000) && !g_stop_run )
				{
					last_abs_prev_pos = abs_cur_pos;
					last_msec_video_prev_pos = msec_video_cur_pos;
					get_next_frame_and_cur_speed(capture, frame,
						abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
						msec_video_prev_pos, abs_prev_pos);
					prev_time = cur_time;
					QueryPerformanceCounter(&cur_time);

					if (result_id < results_size)
					{
						results[result_id].dpos = max(abs_cur_pos - last_abs_prev_pos, 0);
						results[result_id].dt_video = (int)(msec_video_cur_pos - last_msec_video_prev_pos);
						results[result_id].dt_gtc = (int)(time_diff_in_milliseconds(cur_time, prev_time, Frequency));
						result_id++;
					}
					else
					{
						break;
					}
				}

				if (g_stop_run)
				{
					set_hismith_speed(0.0);
					std::this_thread::sleep_for(std::chrono::milliseconds(1000));
					disconnect_from_hismith();
					show_msg("Stoped to get statistics data.", 5000);
					return;
				}

				if (need_restart)
				{
					continue;
				}

				QDomDocument document;
				QDomElement root = document.createElement("speed_statistics_data");
				document.appendChild(root);

				QString result_str;
				for (int i = 0; i < result_id; i++)
				{
					QDomElement sub_data = document.createElement(QString("sub_data_%1").arg(i));
					sub_data.setAttribute("dpos", results[i].dpos);
					sub_data.setAttribute("dt_video", results[i].dt_video);
					sub_data.setAttribute("dt_gtc", results[i].dt_gtc);
					root.appendChild(sub_data);
				}

				QFile file(g_root_dir + QString("\\data\\speed_statistics_data_%1.txt").arg(hismith_speed));
				if (file.open(QFile::WriteOnly | QFile::Text))
				{
					QTextStream ts(&file);
					ts << document.toString();
					file.flush();
					file.close();
				}
			} while (need_restart);
		}
	}

	disconnect_from_hismith();

	QueryPerformanceCounter(&cur_time);
	show_msg(QString("All was done for time: %1 !").arg(VideoTimeToStr(time_diff_in_milliseconds(cur_time, get_statistics_start_time, Frequency)).c_str()), "Get Statistics Data");
}

void test_hismith(int hismith_speed)
{
	int cur_pos, res;
	__int64 msec_video_cur_pos = -1, last_msec_video_prev_pos;
	int abs_cur_pos, abs_prev_pos = 0;
	__int64 msec_video_prev_pos = -1;
	int dpos = 0;
	double cur_speed = -1;

	g_ccxlcx_lh_ratio = -1.0;
	g_max_ccxlcx_lh_ratio_prev_to_cur_dif = -1.0;

	//-----------------------------------------------------
	// Connecting to Hismith
	// NOTE: At first start: intiface central

	show_msg("Connecting to Hismith...", 120000, MessageType::Clean);

	if (!connect_to_hismith())
	{
		show_msg("", 0, MessageType::Clean);
		return;
	}

	//-----------------------------------------------------
	// Connecting to Webcam

	cv::Mat frame, prev_frame, res_frame;
	cv::VideoCapture capture;

	if (init_camera(capture))
	{
		g_threaded_capture.start(&capture);
		get_new_camera_frame(capture, frame, msec_video_cur_pos);
		last_msec_video_prev_pos = msec_video_cur_pos;
		if (!get_hismith_pos_by_image(frame, cur_pos))
		{
			g_threaded_capture.stop();
			g_high_precision_timer_guard.Stop();
			capture.release();
			return;
		}
		abs_cur_pos = get_abs_to_target_pos(cur_pos, 0);
	}

	show_msg("", 0, MessageType::Clean);

	//-----------------------------------------------------
	// Moving Hismith and checking get_hismith_pos_by_image

	if (capture.isOpened())
	{
		hismith_speed = (int)(set_hismith_speed((double)hismith_speed/ 100.0)*100.0);

		std::vector<QPair<__int64, int>> collected_data;
		QString add_data;
		cv::String title("Test Webcam+Hismith");

		cv::namedWindow(title, 1);
		cv::setWindowProperty(title, cv::WND_PROP_TOPMOST, 1);
		int sw = (int)GetSystemMetrics(SM_CXSCREEN);
		int sh = (int)GetSystemMetrics(SM_CYSCREEN);
		cv::moveWindow(title, (sw - g_webcam_frame_width) / 2, (sh - g_webcam_frame_height) / 2);

		while (1)
		{
			last_msec_video_prev_pos = msec_video_cur_pos;
			if (!get_next_frame_and_cur_speed(capture, frame,
				abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
				msec_video_prev_pos, abs_prev_pos, true, &res_frame, title, add_data))
			{
				break;
			}

			if (last_msec_video_prev_pos <= ((msec_video_cur_pos / 1000) * 1000))
			{
				collected_data.push_back(QPair<__int64, int>(msec_video_cur_pos, abs_cur_pos));

				if (collected_data.size() >= 10)
				{
					double avg_speed = (double)((collected_data[collected_data.size()-1].second - collected_data[collected_data.size() - 10].second) * 1000.0) / (double)(collected_data[collected_data.size() - 1].first - collected_data[collected_data.size() - 10].first);

					add_data = QString("rotation_speed_total_average for the last 10 seconds: %1\nset_hismith_speed: %2\n").arg(avg_speed).arg(hismith_speed);
				}
			}

			int key = cv::waitKey(1);

			if ( (key == 27 /* Esc key */) ||
				( (key == -1) && (cv::getWindowProperty(title, cv::WND_PROP_VISIBLE) != 1.0) ) )
			{
				save_BGR_image(frame, g_root_dir + "\\res_data\\orig.bmp");
				save_BGR_image(res_frame, g_root_dir + "\\res_data\\res.bmp");
				break;
			}
		}

		g_threaded_capture.stop();
		g_high_precision_timer_guard.Stop();
		capture.release();
	}

	disconnect_from_hismith();
	cv::destroyAllWindows();
}

void add_xml_element(QDomDocument& doc, QDomElement& root, QString element_name, QString text_value)
{
	QDomElement elem = doc.createElement(element_name);
	QDomText node_txt = doc.createTextNode(text_value);
	elem.appendChild(node_txt);
	root.appendChild(elem);
}

void SaveSettings()
{
	QString fpath = g_root_dir + "\\settings.xml";
	QFile xmlFile(fpath);
	if (!xmlFile.open(QFile::WriteOnly | QFile::Text))
	{
		xmlFile.close();
		error_msg(QString("ERROR: can't open settings file for write: %1").arg(fpath));
		return;
	}

	QTextStream xmlContent(&xmlFile);

	QDomDocument document;
	QDomElement root, elem;

	root = document.createElement("settings");
	document.appendChild(root);

	g_functions_move_in_out_variant = g_pW->ui->functionsMoveInOutVariants->currentIndex() + 1;

	add_xml_element(document, root, "max_allowed_hismith_speed", QString::number(g_max_allowed_hismith_speed));
	add_xml_element(document, root, "hismith_speed_for_set_initial_pos", QString::number(g_hismith_speed_for_set_initial_pos));
	add_xml_element(document, root, "min_funscript_relative_move", QString::number(g_min_funscript_relative_move));
	add_xml_element(document, root, "use_modify_funscript_functions", QString::number(g_modify_funscript ? 1 : 0));
	add_xml_element(document, root, "functions_move_variants", g_modify_funscript_function_move_variants);
	add_xml_element(document, root, "functions_move_in_out_variants", g_modify_funscript_function_move_in_out_variants);
	add_xml_element(document, root, "functions_move_in_out_variant", QString::number(g_functions_move_in_out_variant));
	add_xml_element(document, root, "dt_for_get_cur_speed", QString::number(g_dt_for_get_cur_speed));
	add_xml_element(document, root, "min_dt_between_speed_changes_on_slow_moves", QString::number(g_min_dt_between_speed_changes_on_slow_moves));
	add_xml_element(document, root, "min_dt_between_speed_changes_on_fast_moves", QString::number(g_min_dt_between_speed_changes_on_fast_moves));
	add_xml_element(document, root, "fast_move_min_hismith_speed_for_switch_min_dt_between_speed_changes", QString::number(g_fast_move_min_hismith_speed_for_switch_min_dt_between_speed_changes));
	add_xml_element(document, root, "min_dt_start_for_speed_40", QString::number(g_min_dt_start_for_speed_40));
	add_xml_element(document, root, "speed_change_delay", QString::number(g_speed_change_delay));
	add_xml_element(document, root, "cpu_freezes_timeout", QString::number(g_cpu_freezes_timeout));

	add_xml_element(document, root, "B_range", QString("[%1-%2][%3-%4][%5-%6]")
												.arg(g_B_range[0][0])
												.arg(g_B_range[0][1])
												.arg(g_B_range[1][0])
												.arg(g_B_range[1][1])
												.arg(g_B_range[2][0])
												.arg(g_B_range[2][1]));

	add_xml_element(document, root, "G_range", QString("[%1-%2][%3-%4][%5-%6]")
												.arg(g_G_range[0][0])
												.arg(g_G_range[0][1])
												.arg(g_G_range[1][0])
												.arg(g_G_range[1][1])
												.arg(g_G_range[2][0])
												.arg(g_G_range[2][1]));

	add_xml_element(document, root, "max_telescopic_motor_rocker_arm_proportions", QString::number(g_max_telescopic_motor_rocker_arm_proportions));
	add_xml_element(document, root, "min_telescopic_motor_rocker_arm_center_x_proportions", QString::number(g_min_telescopic_motor_rocker_arm_center_x_proportions));
	add_xml_element(document, root, "max_telescopic_motor_rocker_arm_center_x_proportions", QString::number(g_max_telescopic_motor_rocker_arm_center_x_proportions));

	QString selected_webcam = g_pW->ui->Webcams->itemText(g_pW->ui->Webcams->currentIndex());
	QString selected_device = g_pW->ui->Devices->itemText(g_pW->ui->Devices->currentIndex());

	add_xml_element(document, root, "req_webcam_name", selected_webcam);
	add_xml_element(document, root, "webcam_frame_width", QString::number(g_webcam_frame_width));
	add_xml_element(document, root, "webcam_frame_height", QString::number(g_webcam_frame_height));
	add_xml_element(document, root, "webcam_fps", QString::number(g_webcam_fps));
	add_xml_element(document, root, "webcam_focus", QString::number(g_webcam_focus));
	add_xml_element(document, root, "webcam_end_to_end_latency", QString::number(g_webcam_end_to_end_latency));

	add_xml_element(document, root, "intiface_central_client_url", g_intiface_central_client_url.c_str());
	add_xml_element(document, root, "intiface_central_client_port", QString::number(g_intiface_central_client_port));
	add_xml_element(document, root, "hismith_device_name", selected_device);

	add_xml_element(document, root, "vlc_url", g_vlc_url);
	add_xml_element(document, root, "vlc_port", QString::number(g_vlc_port));
	add_xml_element(document, root, "vlc_password", g_vlc_password);

	add_xml_element(document, root, "hotkey_stop", g_hotkey_stop);
	add_xml_element(document, root, "hotkey_pause", g_hotkey_pause);
	add_xml_element(document, root, "hotkey_resume", g_hotkey_resume);
	add_xml_element(document, root, "hotkey_use_modify_funscript_functions", g_hotkey_use_modify_funscript_functions);

	xmlContent << document.toString();
	xmlFile.flush();
	xmlFile.close();
}

bool LoadSettings()
{
	bool res = false;
	QDomDocument doc("data");
	QString fpath = g_root_dir + "\\settings.xml";
	QFile xmlFile(fpath);
	if (!xmlFile.open(QIODevice::ReadOnly))
	{
		error_msg(QString("ERROR: can't open settings file for read: %1").arg(fpath));
		return res;
	}

	{
		QString err_msg;
		int err_line, err_column;
		if (!doc.setContent(&xmlFile, &err_msg, &err_line, &err_column)) {
			error_msg(QString("ERROR: failed to parse xml file: %1\nline:%2 column:%3\nerror_msg:\n%4").arg(fpath).arg(err_line).arg(err_column).arg(err_msg));
			xmlFile.close();
			return res;
		}
	}
	xmlFile.close();

	std::map<QString, QString> data_map;

	QDomElement docElem = doc.documentElement();

	QDomNode n = docElem.firstChild();
	while (!n.isNull()) {
		QDomElement e = n.toElement(); // try to convert the node to an element.
		if (!e.isNull()) {
			QString tag_name = e.tagName();
			QString tag_data = e.text();
			data_map[tag_name] = tag_data;
		}
		n = n.nextSibling();
	}

	g_max_allowed_hismith_speed = data_map["max_allowed_hismith_speed"].toInt();
	g_hismith_speed_for_set_initial_pos = data_map["hismith_speed_for_set_initial_pos"].toInt();
	g_min_funscript_relative_move = data_map["min_funscript_relative_move"].toInt();
	g_dt_for_get_cur_speed = data_map["dt_for_get_cur_speed"].toInt();
	g_min_dt_between_speed_changes_on_slow_moves = data_map["min_dt_between_speed_changes_on_slow_moves"].toInt();
	g_min_dt_between_speed_changes_on_fast_moves = data_map["min_dt_between_speed_changes_on_fast_moves"].toInt();
	g_fast_move_min_hismith_speed_for_switch_min_dt_between_speed_changes = data_map["fast_move_min_hismith_speed_for_switch_min_dt_between_speed_changes"].toInt();
	g_min_dt_start_for_speed_40 = data_map["min_dt_start_for_speed_40"].toInt();
	g_speed_change_delay = data_map["speed_change_delay"].toInt();
	g_cpu_freezes_timeout = data_map["cpu_freezes_timeout"].toInt();

	g_hotkey_stop = data_map["hotkey_stop"];
	g_hotkey_pause = data_map["hotkey_pause"];
	g_hotkey_resume = data_map["hotkey_resume"];
	g_hotkey_use_modify_funscript_functions = data_map["hotkey_use_modify_funscript_functions"];
	g_pW->RegisterHotKeys();

	{
		QString str = data_map["B_range"];
		QRegularExpression re_range("\\[(\\d+)-(\\d+)\\]\\[(\\d+)-(\\d+)\\]\\[(\\d+)-(\\d+)\\]");
		QRegularExpressionMatch match;
		match = re_range.match(str);
		if (!match.hasMatch())
		{
			show_msg(QString("ERROR: B_range has wrong format in file: %1").arg(fpath));
			return res;
		}
		g_B_range[0][0] = match.captured(1).toInt();
		g_B_range[0][1] = match.captured(2).toInt();
		g_B_range[1][0] = match.captured(3).toInt();
		g_B_range[1][1] = match.captured(4).toInt();
		g_B_range[2][0] = match.captured(5).toInt();
		g_B_range[2][1] = match.captured(6).toInt();
	}

	{
		QString str = data_map["G_range"];
		QRegularExpression re_range("\\[(\\d+)-(\\d+)\\]\\[(\\d+)-(\\d+)\\]\\[(\\d+)-(\\d+)\\]");
		QRegularExpressionMatch match;
		match = re_range.match(str);
		if (!match.hasMatch())
		{
			show_msg(QString("ERROR: G_range has wrong format in file: %1").arg(fpath));
			return res;
		}
		g_G_range[0][0] = match.captured(1).toInt();
		g_G_range[0][1] = match.captured(2).toInt();
		g_G_range[1][0] = match.captured(3).toInt();
		g_G_range[1][1] = match.captured(4).toInt();
		g_G_range[2][0] = match.captured(5).toInt();
		g_G_range[2][1] = match.captured(6).toInt();
	}

	g_max_telescopic_motor_rocker_arm_proportions = data_map["max_telescopic_motor_rocker_arm_proportions"].toDouble();
	g_min_telescopic_motor_rocker_arm_center_x_proportions = data_map["min_telescopic_motor_rocker_arm_center_x_proportions"].toDouble();
	g_max_telescopic_motor_rocker_arm_center_x_proportions = data_map["max_telescopic_motor_rocker_arm_center_x_proportions"].toDouble();

	g_req_webcam_name = data_map["req_webcam_name"];
	g_webcam_frame_width = data_map["webcam_frame_width"].toInt();
	g_webcam_frame_height = data_map["webcam_frame_height"].toInt();
	g_webcam_fps = data_map["webcam_fps"].toDouble();
	g_webcam_focus = data_map["webcam_focus"].toInt();
	g_webcam_end_to_end_latency = data_map["webcam_end_to_end_latency"].toInt();

	g_intiface_central_client_url = data_map["intiface_central_client_url"].toStdString();
	g_intiface_central_client_port = data_map["intiface_central_client_port"].toInt();

	g_hismith_device_name = data_map["hismith_device_name"];

	g_vlc_url = data_map["vlc_url"];
	g_vlc_port = data_map["vlc_port"].toInt();
	g_vlc_password = data_map["vlc_password"];

	g_modify_funscript = (data_map["use_modify_funscript_functions"].toInt() == 0) ? false : true;
	g_modify_funscript_function_move_variants = data_map["functions_move_variants"];
	g_modify_funscript_function_move_in_out_variants = data_map["functions_move_in_out_variants"];
	g_functions_move_in_out_variant = data_map["functions_move_in_out_variant"].toInt();

	//------------------------------------------------------------------------------------------------

	g_pW->ui->speedLimit->setText(QString::number(g_max_allowed_hismith_speed));
	g_pW->ui->minRelativeMove->setText(QString::number(g_min_funscript_relative_move));

	g_pW->ui->modifyFunscript->setChecked(g_modify_funscript);

	QString tmp_modify_funscript_function_move_variants = g_modify_funscript_function_move_variants;
	QStringList modify_funscript_function_move_variants = g_modify_funscript_function_move_variants.mid(1, g_modify_funscript_function_move_variants.size() - 2).split("],[");
	for (QString& modify_funscript_function_move_variant : modify_funscript_function_move_variants)
	{
		g_pW->ui->functionsMoveVariants->addItem("[" + modify_funscript_function_move_variant + "]");
	}
	g_modify_funscript_function_move_variants = tmp_modify_funscript_function_move_variants;


	QString tmp_modify_funscript_function_move_in_out_variants = g_modify_funscript_function_move_in_out_variants;
	QStringList modify_funscript_function_move_in_out_variants = g_modify_funscript_function_move_in_out_variants.split(";");
	for (QString& modify_funscript_function_move_in_out_variant : modify_funscript_function_move_in_out_variants)
	{
		g_pW->ui->functionsMoveInOutVariants->addItem(modify_funscript_function_move_in_out_variant);
	}
	g_modify_funscript_function_move_in_out_variants = tmp_modify_funscript_function_move_in_out_variants;

	g_pW->ui->functionsMoveInOutVariants->setCurrentIndex(g_functions_move_in_out_variant - 1);

	//--------------------

	DeviceEnumerator de;
	std::map<int, InputDevice> devices = de.getVideoDevicesMap();
	for (auto const& device : devices) {
		g_pW->ui->Webcams->addItem(device.second.deviceName.c_str());
		if (QString(device.second.deviceName.c_str()).contains(g_req_webcam_name))
		{
			g_pW->ui->Webcams->setCurrentIndex(g_pW->ui->Webcams->count() - 1);
		}
	}
	//--------------------

	//--------------------
	get_devices_list(false);

	for (DeviceClass& dev : g_myDevices)
	{
		g_pW->ui->Devices->addItem(dev.deviceName.c_str());
		if (QString(dev.deviceName.c_str()).contains(g_hismith_device_name))
		{
			g_pW->ui->Devices->setCurrentIndex(g_pW->ui->Devices->count() - 1);
		}
	}
	//--------------------

	res = true;

	return res;
}

void test_vlc()
{
	//-----------------------------------------------------
	// Connecting to VLC player with already opened video

	g_pNetworkAccessManager = new QNetworkAccessManager();

	QString concatenated = ":" + g_vlc_password; //username:password
	QByteArray data = concatenated.toLocal8Bit().toBase64();
	QString headerData = "Basic " + data;
	g_NetworkRequest.setRawHeader("Authorization", headerData.toLocal8Bit());
	g_NetworkRequest.setTransferTimeout(1000);

	int cur_video_pos = 0, prev_video_pos = 0, video_pos = 0, dt;
	__int64 vlc_sys_time = -1;
	double cur_rate = 1;
	bool is_video_paused, is_vlc_time_in_milliseconds;
	QString cur_video_filename, prev_video_filename;
	LARGE_INTEGER cur_time;

	show_msg(QString("test_vlc started"));

	while (1)
	{
		prev_video_pos = cur_video_pos;
		make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, cur_video_filename, is_vlc_time_in_milliseconds, video_pos, vlc_sys_time, cur_rate);
		get_cur_video_pos(is_video_paused, video_pos, vlc_sys_time, cur_rate, cur_time, cur_video_pos);
		if (cur_video_filename != prev_video_filename)
		{
			prev_video_filename = cur_video_filename;
			prev_video_pos = cur_video_pos;
		}

		if (cur_video_pos < prev_video_pos)
		{
			dt = prev_video_pos - cur_video_pos;
			error_msg(QString("ERROR: cur_video_pos < prev_video_pos, dt: %1").arg(dt));
		}
	}

	show_msg(QString("test_vlc ended"));
}

int main(int argc, char *argv[])
{
	std::srand(std::time(nullptr)); // use current time as seed for random generator

	GdiplusStartupInput gdiplusStartupInput;
	ULONG_PTR gdiplusToken;
	GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    QApplication a(argc, argv);
	g_root_dir = a.applicationDirPath();
	QIcon icon(":/images/icon.ico");
	a.setWindowIcon(icon);

	{
		QFile file(g_root_dir + "\\res_data\\!results_for_get_parsed_funscript_data.txt");
		file.resize(0);
		file.close();
	}

    MainWindow w;
	g_pW = &w;
    w.show();

	if (!LoadSettings())
	{
		return 0;
	}

	// for testing functions:
	{
		//get_statistics_with_hismith();

		//get_performance_with_hismith(5);

		//test_camera();

		//test_err_frame(g_root_dir + "\\error_data\\2025.01.14_22.47.31_frame_orig.bmp");
		//test_err_frame(g_root_dir + "\\error_data\\2025.08.05_18.14.09_frame_orig.bmp");
		//test_err_frame(g_root_dir + "\\error_data\\2026.04.15_19.42.19_frame_orig.bmp");

		//show_msg(QString("test message"), 5000, true, false, 0.1);
		//show_msg(QString("123"));
		//show_msg(QString("567"));
		//show_msg("", 0, MessageType::Clean);

		//test_hismith(5);

		//cv::destroyAllWindows();

		//test_vlc();

		//return 0;
	}

    return a.exec();
}
