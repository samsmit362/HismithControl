
#include "mainwindow.h"
#include "./ui_mainwindow.h"


//---------------------------------------------------------------

int g_max_allowed_hismith_speed;
int g_min_funscript_relative_move;

int g_dt_for_get_cur_speed;
const int g_min_dt_for_set_hismith_speed = 20;

double g_increase_hismith_speed_start_multiplier;
double g_slowdown_hismith_speed_start_multiplier;
double g_increase_hismith_speed_change_multiplier;
double g_slowdown_hismith_speed_change_multiplier;
int g_speed_change_delay;
int g_cpu_freezes_timeout;

//YUV:
int g_B_range[3][2];

//YUV:
int g_G_range[3][2];

double g_max_telescopic_motor_rocker_arm_proportions;
double g_min_telescopic_motor_rocker_arm_center_x_proportions;
double g_max_telescopic_motor_rocker_arm_center_x_proportions;

QString g_req_webcam_name;
int g_webcam_frame_width;
int g_webcam_frame_height;

std::string g_intiface_central_client_url;
int g_intiface_central_client_port;
QString g_hismith_device_name;

QString g_vlc_url;
int g_vlc_port;
QString g_vlc_password;

//---------------------------------------------------------------

QString g_root_dir;

Client* g_pClient = NULL;
std::vector<DeviceClass> g_myDevices;
DeviceClass* g_pMyDevice = NULL;

QNetworkAccessManager* g_pNetworkAccessManager = NULL;
QNetworkRequest g_NetworkRequest;

bool g_stop_run = false;
bool g_pause = false;

MainWindow* pW = NULL;

std::mutex g_stop_mutex;
std::condition_variable g_stop_cvar;

bool g_work_in_progress = false;

int g_avg_time_delay = 0;

int g_save_images = true;

bool g_modify_funscript = false;

// "[0.25:0.38|0.75:0.62],[0.25:0.12|0.75:0.87]"; // [fast:slow:fast],[slow:fast:slow]
QString g_modify_funscript_function_move_variants;

//"[0-200:0],[200-maximum:1]";
//"[0-200:0|1],[200-maximum:1]";
//"[0-200:random],[200-maximum:1]";
//"[0-maximum:random]";
QString g_modify_funscript_function_move_in_variants;
QString g_modify_funscript_function_move_out_variants;

//---------------------------------------------------------------

void save_BGR_image(cv::Mat &frame, QString fpath)
{
	if (g_save_images)
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
		file.close();
		error_msg(QString("ERROR: file [%1] already opened for write or there is another issue").arg(fpath));
	}
	QTextStream ts(&file);

	ts << text;
	file.flush();
	file.close();
}

QString get_cur_time_str()
{
	auto t = std::time(nullptr);
	auto tm = *std::localtime(&t);
	std::ostringstream oss;
	oss << std::put_time(&tm, "%d_%m_%Y_%H_%M_%S");
	QString time_str = oss.str().c_str();
	return time_str;
}

void get_new_camera_frame(cv::VideoCapture &capture, cv::Mat& frame)
{
	cv::Mat prev_frame;
	frame.copyTo(prev_frame);

	do
	{
		if (!capture.read(frame))
		{
			error_msg(QString("ERROR: capture.read(frame) failed"));
		}
		
	} 
	while (	(prev_frame.size == frame.size) && 
			(prev_frame.channels() == frame.channels()) && 
			(memcmp(prev_frame.data, frame.data, frame.channels() * frame.size[0] * frame.size[1]) == 0) &&
			!g_stop_run && 
			!g_pause );
}

double set_hithmith_speed(double speed)
{
	double res_speed = 0;
	if (g_pClient && g_pMyDevice)
	{
		int speed_int = speed > 0 ? speed * 100.0 : 0;		
		if (speed_int > g_max_allowed_hismith_speed)
		{
			speed_int = g_max_allowed_hismith_speed;
		}
		res_speed = (double)speed_int / 100.0;
		g_pClient->sendScalar(*g_pMyDevice, res_speed);
	}

	return res_speed;
}

void draw_text(QString text, cv::Mat &frame, int x1 = -1, int y1 = -1, int x2 = -1, int y2 = -1)
{
	int fontFace = cv::FONT_HERSHEY_SIMPLEX;
	double fontScale = 1;
	int thickness = 3;
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
			cv::Scalar(0, 111, 221), thickness, 8);

		y_offset += textSize.height + (2*thickness) + 5;
	}

	if (x1 != -1)
	{
		cv::rectangle(frame, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(0, 0, 255));
	}
}

//---------------------------------------------------------------
// NOTE: QT Doesn't allow to create GUI in a non-main GUI thread
// like QMessageBox for example, so using Win API
//---------------------------------------------------------------

int _tmp_MsgBox_X;
int _tmp_MsgBox_Y;
static void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
	if ((GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CHILD) == 0)
		SetWindowPos(hwnd, NULL, _tmp_MsgBox_X, _tmp_MsgBox_Y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

int MessageBoxPos(HWND hWnd, const WCHAR* sText, const WCHAR* sCaption, UINT uType, DWORD dwMilliseconds = -1, int X = -1, int Y = -1)
{
	int iResult;
	HWINEVENTHOOK hHook = 0;
	if (X >= 0)
	{
		hHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, &WinEventProc, GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
		_tmp_MsgBox_X = X;
		_tmp_MsgBox_Y = Y;
	}

	if (dwMilliseconds > 0)
	{
		// Displays a message box, and dismisses it after the specified timeout.
		typedef int(__stdcall* MSGBOXWAPI)(IN HWND hWnd, IN LPCWSTR lpText, IN LPCWSTR lpCaption, IN UINT uType, IN WORD wLanguageId, IN DWORD dwMilliseconds);

		HMODULE hUser32 = LoadLibraryA("user32.dll");
		if (hUser32)
		{
			auto MessageBoxTimeoutW = (MSGBOXWAPI)GetProcAddress(hUser32, "MessageBoxTimeoutW");

			iResult = MessageBoxTimeoutW(hWnd, sText, sCaption, uType, 0, dwMilliseconds);

			FreeLibrary(hUser32);
		}
		else
			iResult = MessageBox(hWnd, sText, sCaption, uType);
	}
	else
	{
		iResult = MessageBox(hWnd, sText, sCaption, uType);
	}

	if (hHook)  UnhookWinEvent(hHook);

	return iResult;
}

void show_frame_in_cv_window(cv::String wname, cv::Mat* p_frame)
{
	cv::imshow(wname, *p_frame);
	cv::setWindowProperty(wname, cv::WND_PROP_TOPMOST, 1);
	cv::Rect rc = cv::getWindowImageRect(wname);
	int sw = (int)GetSystemMetrics(SM_CXSCREEN);
	int sh = (int)GetSystemMetrics(SM_CYSCREEN);
	cv::moveWindow(wname, (sw - rc.width) / 2, (sh - rc.height) / 2);
}

void error_msg(QString msg, cv::Mat* p_frame, cv::Mat* p_frame_upd, cv::Mat *p_prev_frame, int x1, int y1, int x2, int y2)
{
	disconnect_from_hismith();

	auto t = std::time(nullptr);
	auto tm = *std::localtime(&t);
	std::ostringstream oss;
	oss << std::put_time(&tm, "%d_%m_%Y_%H_%M_%S");
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
		
		show_frame_in_cv_window("Error", p_draw_frame);
		cv::waitKey(0);
	}
	else
	{
		MessageBoxPos(NULL, msg.toStdWString().c_str(), L"Error", MB_OK | MB_SETFOREGROUND | MB_SYSTEMMODAL | MB_ICONERROR);
	}
	cv::destroyAllWindows();	
}

void warning_msg(QString msg, QString title = "")
{
	MessageBoxPos(NULL, msg.toStdWString().c_str(), title.toStdWString().c_str(), MB_OK | MB_SETFOREGROUND | MB_SYSTEMMODAL | MB_ICONWARNING);
}

void show_msg(QString msg, QString title)
{
	MessageBoxPos(NULL, msg.toStdWString().c_str(), title.toStdWString().c_str(), MB_OK | MB_SETFOREGROUND | MB_SYSTEMMODAL | MB_ICONINFORMATION);
}

//---------------------------------------------------------------

int get_video_dev_id()
{
	DeviceEnumerator de;

	// Video Devices
	std::map<int, InputDevice> devices = de.getVideoDevicesMap();
	int video_dev_id = -1;

	QString selected_webcam = pW->ui->Webcams->itemText(pW->ui->Webcams->currentIndex());

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

bool get_hismith_pos_by_image(cv::Mat& frame, int& pos, bool show_results = false, cv::Mat *p_res_frame = NULL, double *p_cur_speed = NULL, cv::String title = "", QString add_data = QString())
{
	static double ccxlcx_lh_ratio = -1.0;

	bool res = false;
	cv::Mat img, img_b, img_g, img_right;
	custom_buffer<CMyClosedFigure> figures_b;
	simple_buffer<CMyClosedFigure*> p_figures_b;
	int figures_b_N;
	custom_buffer<CMyClosedFigure> figures_g;
	__int64 start_time, t1;

	if (show_results)
	{
		start_time = GetTickCount64();
	}

	cv::cvtColor(frame, img, cv::COLOR_BGR2YUV);
	int width = img.cols;
	int height = img.rows;

	concurrency::parallel_invoke(
		[&img, &img_b, &figures_b, &p_figures_b, &figures_b_N, width, height] {
			get_binary_image(img, g_B_range, img_b, 3);
			simple_buffer<u8> Im(width * height);
			GreyscaleMatToImage(img_b, width, height, Im);
			SearchClosedFigures(Im, width, height, (u8)255, figures_b);

			figures_b_N = figures_b.size();

			if (figures_b_N > 0)
			{
				p_figures_b.set_size(figures_b_N);
				for (int i = 0; i < figures_b_N; i++)
				{
					p_figures_b[i] = &(figures_b[i]);
				}

				int id1 = 0;
				while (id1 < figures_b_N - 1)
				{
					CMyClosedFigure* pFigure1 = p_figures_b[id1];

					int id2 = id1 + 1;

					while (id2 < figures_b_N)
					{
						CMyClosedFigure* pFigure2 = p_figures_b[id2];

						int cx1 = (pFigure1->m_minX + pFigure1->m_maxX) / 2;
						int cy1 = (pFigure1->m_minY + pFigure1->m_maxY) / 2;

						int cx2 = (pFigure2->m_minX + pFigure2->m_maxX) / 2;
						int cy2 = (pFigure2->m_minY + pFigure2->m_maxY) / 2;

						int min_x = min(pFigure1->m_minX, pFigure2->m_minX);
						int max_x = max(pFigure1->m_maxX, pFigure2->m_maxX);
						int min_y = min(pFigure1->m_minY, pFigure2->m_minY);
						int max_y = max(pFigure1->m_maxY, pFigure2->m_maxY);

						// intersect by x
						if (max_x - min_x + 1 < pFigure1->width() + pFigure2->width())
						{
							int dist_by_y = (max_y - min_y + 1) - pFigure1->height() - pFigure2->height();
							int max_h = max(pFigure1->height(), pFigure2->height());

							if (dist_by_y < max_h)
							{
								// join two figures
								*pFigure1 += *pFigure2;

								for (int id3 = id2; id3 < figures_b_N - 1; id3++)
								{
									p_figures_b[id3] = p_figures_b[id3 + 1];
								}

								figures_b_N--;
								continue;
							}
						}

						id2++;
					}

					id1++;
				}
			}
		},
		[&img, &img_g, &figures_g, width, height] {
			get_binary_image(img, g_G_range, img_g, 3);
			simple_buffer<u8> Im(width * height);
			GreyscaleMatToImage(img_g, width, height, Im);
			SearchClosedFigures(Im, width, height, (u8)255, figures_g);
		}
	);

	if (show_results)
	{
		t1 = GetTickCount64();
	}

	int c_x = -1, c_y = -1, c_w = -1, c_h = -1;
	CMyClosedFigure* p_best_match_l_figure = NULL;
	int l_x = -1, l_y = -1, l_w = -1, l_h = -1;
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

		if (((x + w) < (2 * width) / 3) && (h > 2 * w) && (h > (2 * height) / 15) && ((y + h / 2) < (3 * height) / 4))
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

	if (!p_best_match_l_figure)
	{
		cv::Mat frame_upd;
		frame.copyTo(frame_upd);
		frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
		frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
		cv::rectangle(frame_upd, cv::Point(0, 0), cv::Point(max(5, ((2 * height) / 15)/10), (2 * height) / 15), cv::Scalar(0, 255, 0));
		error_msg("ERROR: Failed to find big left vertical border blue color figure\n(min search size in top left green rectangle)", &frame, &frame_upd, NULL, 0, 0, (2 * width) / 3, (3 * height) / 4);
		return res;
	}

	for (int id = 0; id < figures_b_N; id++)
	{
		CMyClosedFigure* pFigure = p_figures_b[id];
		int x = pFigure->m_minX, y = pFigure->m_minY, w = pFigure->width(), h = pFigure->height();
		int size = pFigure->m_PointsArray.m_size;

		if (pFigure == p_best_match_l_figure)
		{
			continue;
		}

		if ((x > l_x + ((3 * l_h) / 2)) && (y > l_y - (l_h / 2)) && (y + h < l_y + ((4 * l_h) / 2)))
		{
			if (size > max_size_r)
			{
				max_size_r = size;
				p_best_match_r_figure = pFigure;
				r_x = x;
				r_y = y;
				r_w = w;
				r_h = h;
			}
		}
	}

	if (!p_best_match_r_figure)
	{
		cv::Mat frame_upd;
		frame.copyTo(frame_upd);
		frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
		frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
		error_msg("ERROR: Failed to find big right blue color figure", &frame, &frame_upd, NULL, l_x + ((3 * l_h) / 2), l_y - (l_h / 2), width, l_y + ((4 * l_h) / 2));
		return res;
	}

	int min_cw = 5;
	int min_sy = min(l_y + (l_h / 4), r_y);
	int max_sy = max(l_y + ((3 * l_h) / 4), r_y + r_h);
	std::vector<CMyClosedFigure*> c_figures;

	for (int id = 0; id < figures_b_N; id++)
	{
		CMyClosedFigure* pFigure = p_figures_b[id];
		int x = pFigure->m_minX, y = pFigure->m_minY, w = pFigure->width(), h = pFigure->height();
		int size = pFigure->m_PointsArray.m_size;

		if ((pFigure == p_best_match_l_figure) || (pFigure == p_best_match_r_figure))
		{
			continue;
		}

		if ((x > l_x + l_w) && (x + w < ((l_x + l_w + r_x) / 2)) && (y + h >= min_sy) && (y <= max_sy) && (w >= min_cw) /*&& (h >= min_ch)*/)
		{
			if (show_results)
			{
				c_figures.push_back(pFigure);
			}

			if (c_x == -1)
			{
				c_x = x;
				c_y = y;
				c_w = w;
				c_h = h;
			}
			else
			{
				int min_x = min(c_x, x);
				int max_x = max(c_x + c_w - 1, x + w - 1);
				int min_y = min(c_y, y);
				int max_y = max(c_y + c_h - 1, y + h - 1);
				c_x = min_x;
				c_w = max_x - min_x + 1;
				c_y = min_y;
				c_h = max_y - min_y + 1;
			}
		}
	}

	int l_cx = l_x + (l_w / 2);
	int l_cy = l_y + (l_h / 2);
	int r_cx = r_x + (r_w / 2);
	int r_cy = r_y + (r_h / 2);
	int c_cx;
	int c_cy;

	double ccxlcx_lh_ratio_prev = ccxlcx_lh_ratio;

	if (r_cx == l_cx)
	{
		cv::Mat frame_upd;
		frame.copyTo(frame_upd);
		frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
		frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
		cv::rectangle(frame_upd, cv::Rect(l_x, l_y, l_w, l_h), cv::Scalar(0, 0, 255), 3);
		cv::circle(frame_upd, cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), int(max(r_w / 2, r_h / 2)), cv::Scalar(0, 0, 255), 3);
		error_msg(QString("ERROR: unexpected issue, centers of left and right blue figures are same\n"), &frame, &frame_upd);
	}

	if (c_x == -1)
	{
		if (ccxlcx_lh_ratio > 0)
		{
			c_cx = l_cx + (int)(ccxlcx_lh_ratio * (double)l_h);
			c_w = max(r_w, r_h);
			c_h = c_w;
			c_x = c_cx - (c_w / 2);
			c_cy = l_cy + (((r_cy - l_cy) * (c_cx - l_cx)) / (r_cx - l_cx));
			c_y = c_cy - (c_h / 2);
		}
		else
		{
			cv::Mat frame_upd;
			frame.copyTo(frame_upd);
			frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
			frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
			cv::rectangle(frame_upd, cv::Rect(l_x, l_y, l_w, l_h), cv::Scalar(0, 0, 255), 3);
			cv::circle(frame_upd, cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), int(max(r_w / 2, r_h / 2)), cv::Scalar(0, 0, 255), 3);
			error_msg(QString("ERROR: failed to get telescopic motor rocker arm center x position\n"), &frame, &frame_upd);
		}
	}
	else
	{
		c_cx = c_x + (c_w / 2);
		c_cy = l_cy + (((r_cy - l_cy) * (c_cx - l_cx)) / (r_cx - l_cx));
		c_h = max((c_cy - c_y) * 2, (c_y + c_h - 1 - c_cy) * 2);
		c_y = c_cy - (c_h / 2);

		ccxlcx_lh_ratio = (double)(c_cx - l_cx) / (double)l_h;

		if ( (ccxlcx_lh_ratio < g_min_telescopic_motor_rocker_arm_center_x_proportions) ||
			(ccxlcx_lh_ratio > g_max_telescopic_motor_rocker_arm_center_x_proportions) )
		{
			cv::Mat img_res;
			frame.copyTo(img_res);

			img_res.setTo(cv::Scalar(255, 0, 0), GetFigureMask(p_best_match_l_figure, width, height));
			img_res.setTo(cv::Scalar(255, 0, 0), GetFigureMask(p_best_match_r_figure, width, height));
			for (CMyClosedFigure* c_figure : c_figures)
			{
				img_res.setTo(cv::Scalar(255, 0, 0), GetFigureMask(c_figure, width, height));
			}

			cv::rectangle(img_res, cv::Rect(l_x, l_y, l_w, l_h), cv::Scalar(0, 0, 255), 3);
			cv::circle(img_res, cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), int(max(r_w / 2, r_h / 2)), cv::Scalar(0, 0, 255), 3);
			cv::circle(img_res, cv::Point(c_x + int(c_w / 2), c_y + int(c_h / 2)), int(max(c_w / 2, c_h / 2)), cv::Scalar(0, 0, 255), 3);

			cv::line(img_res, cv::Point(c_cx, c_cy - int(c_h / 4)), cv::Point(c_cx, c_cy + int(c_h / 4)), cv::Scalar(0, 170, 0), 5);
			cv::line(img_res, cv::Point(l_x + int(l_w / 2), l_y + int(l_h / 2)), cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), cv::Scalar(0, 170, 0), 5);

			error_msg(QString(	"ERROR: got strange center position:\n"
								"telescopic_motor_rocker_arm_center_x_proportions:%1\n"
								"min_telescopic_motor_rocker_arm_center_x_proportions:%2\n"
								"max_telescopic_motor_rocker_arm_center_x_proportions:%3\n")
								.arg(ccxlcx_lh_ratio)
								.arg(g_min_telescopic_motor_rocker_arm_center_x_proportions)
								.arg(g_max_telescopic_motor_rocker_arm_center_x_proportions),
								&frame, &img_res);
		}
	}

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
		cv::Mat frame_upd;
		frame.copyTo(frame_upd);
		frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
		frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
		error_msg("ERROR: Failed to find green color figure", &frame, &frame_upd, NULL, l_x, min(l_y, r_y), r_x, min(l_y + l_h, r_y + r_h));
		return res;
	}

	int g_cx = g_x + (g_w / 2), g_cy = g_y + (g_h / 2);

	double g_to_c_distance = sqrt((double)(pow2(g_cx - c_cx) + pow2(g_cy - c_cy)));
	double r_to_c_distance = sqrt((double)(pow2(r_cx - c_cx) + pow2(r_cy - c_cy)));

	if (g_to_c_distance * r_to_c_distance == 0)
	{
		cv::Mat frame_upd;
		frame.copyTo(frame_upd);
		frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
		frame_upd.setTo(cv::Scalar(0, 255, 0), img_g);
		error_msg("ERROR: unexpected issue: g_to_c_distance * r_to_c_distance == 0", &frame, &frame_upd);
		return res;
	}

	int g_to_c_cx = g_cx - c_cx;
	int g_to_c_cy_inv = -(g_cy - c_cy);

	int r_to_c_cx = r_cx - c_cx;
	int r_to_c_cy_inv = -(r_cy - c_cy);

	// From scalar vector multiplication a_vec * b_vac
	// cos(alpha) = (a_x*b_x + a_y*b_y)/|a|*|b|
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
		int dt = (int)(GetTickCount64() - start_time);
		int dt1 = (int)(t1 - start_time);

		cv::Mat img_res = frame.clone();

		img_res.setTo(cv::Scalar(255, 0, 0), GetFigureMask(p_best_match_l_figure, width, height));
		img_res.setTo(cv::Scalar(255, 0, 0), GetFigureMask(p_best_match_r_figure, width, height));
		for (CMyClosedFigure* c_figure : c_figures)
		{
			img_res.setTo(cv::Scalar(255, 0, 0), GetFigureMask(c_figure, width, height));
		}
		img_res.setTo(cv::Scalar(0, 255, 0), GetFigureMask(p_best_match_g_figure, width, height));

		cv::rectangle(img_res, cv::Rect(l_x, l_y, l_w, l_h), cv::Scalar(0, 0, 255), 3);
		cv::circle(img_res, cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), int(max(r_w / 2, r_h / 2)), cv::Scalar(0, 0, 255), 3);
		cv::circle(img_res, cv::Point(c_x + int(c_w / 2), c_y + int(c_h / 2)), int(max(c_w / 2, c_h / 2)), cv::Scalar(0, 0, 255), 3);
		cv::circle(img_res, cv::Point(g_x + int(g_w / 2), g_y + int(g_h / 2)), int(max(g_w / 2, g_h / 2)), cv::Scalar(0, 0, 255), 3);

		cv::line(img_res, cv::Point(c_x + int(c_w / 2), c_y + int(c_h / 2)), cv::Point(g_x + int(g_w / 2), g_y + int(g_h / 2)), cv::Scalar(0, 170, 0), 5);
		cv::line(img_res, cv::Point(l_x + int(l_w / 2), l_y + int(l_h / 2)), cv::Point(r_x + int(r_w / 2), r_y + int(r_h / 2)), cv::Scalar(0, 170, 0), 5);

		if (ccxlcx_lh_ratio_prev > 0)
		{
			int c_cx_exp = l_cx + (int)(ccxlcx_lh_ratio_prev * (double)l_h);
			int c_w_exp = max(r_w, r_h);
			int c_h_exp = c_w;
			int c_x_exp = c_cx_exp - (c_w_exp / 2);
			int c_cy_exp = l_cy + (((r_cy - l_cy) * (c_cx_exp - l_cx)) / (r_cx - l_cx));
			int c_y_exp = c_cy_exp - (c_h_exp / 2);
			cv::rectangle(img_res, cv::Rect(c_x_exp, c_y_exp, c_w_exp, c_h_exp), cv::Scalar(0, 0, 255), 1);
			cv::line(img_res, cv::Point(c_cx_exp, c_cy_exp - 4), cv::Point(c_cx_exp, c_cy_exp + 4), cv::Scalar(0, 0, 255), 1);
		}

		int cur_speed = -1;
		int dt_get_speed = -1;
		if (p_cur_speed)
		{
			cur_speed = *p_cur_speed;
		}

		double g_to_c_distance = (int)sqrt((double)(pow2(g_cx - c_cx) + pow2(g_cy - c_cy)));
		double g_to_r_distance = (int)sqrt((double)(pow2(g_cx - r_cx) + pow2(g_cy - r_cy)));
		double telescopic_motor_rocker_arm_proportions = g_to_r_distance > 0 ? g_to_c_distance / g_to_r_distance : 0;

		cv::String text = cv::format("%s" "pos: %d cur_speed: %d\ntelescopic_motor_rocker_arm_proportions: %f\ntelescopic_motor_rocker_arm_center_x_proportions: %f\nperformance data: dt_get_pos_total: %d dt_get_pos_conversion: %d", add_data.toStdString().c_str(), pos, cur_speed, telescopic_motor_rocker_arm_proportions, ccxlcx_lh_ratio, dt, dt1);

		draw_text(text.c_str(), img_res);

		if (p_res_frame)
		{
			img_res.copyTo(*p_res_frame);
		}

		cv::imshow(title, img_res);
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

void get_performance_with_video()
{
	cv::String video_file_name = g_root_dir.toStdString() + "\\data\\speed_100.mp4";
	cv::VideoCapture capture(video_file_name);
	cv::Mat frame, bad_frame;

	if (capture.isOpened())
	{
		capture.read(frame);

		int res, pos, cnt = 0;
		int max_dt = 0, dt;
		__int64 start_time = GetTickCount64();
		__int64 call_start_time;
		while (capture.read(frame))
		{
			call_start_time = GetTickCount64();
			if (!get_hismith_pos_by_image(frame, pos))
			{
				capture.release();
				return;
			}
			dt = (int)(GetTickCount64() - call_start_time);
			if (dt > max_dt)
			{
				max_dt = dt;
				frame.copyTo(bad_frame);
			}
			cnt++;
		}

		if (cnt > 0)
		{
			auto t = std::time(nullptr);
			auto tm = *std::localtime(&t);
			std::ostringstream oss;
			oss << std::put_time(&tm, "%d_%m_%Y_%H_%M_%S");
			QString time_str = oss.str().c_str();
			save_BGR_image(bad_frame, g_root_dir + "\\error_data\\" + time_str + QString("_slow_frame_orig_dt_%1.bmp").arg(max_dt));

			show_msg(QString("%1_%2").arg((double)1000.0 / ((double)(GetTickCount64() - start_time) / cnt)).arg(max_dt), 10000);
		}
	}

	capture.release();
}

void test_by_video()
{
	cv::String video_file_name = g_root_dir.toStdString() + "\\data\\speed_100.mp4";
	cv::VideoCapture capture(video_file_name);
	cv::Mat frame, bad_frame;

	if (capture.isOpened())
	{
		capture.read(frame);

		cv::String title("Test By Video");

		cv::namedWindow(title, 1);
		cv::setWindowProperty(title, cv::WND_PROP_TOPMOST, 1);
		int sw = (int)GetSystemMetrics(SM_CXSCREEN);
		int sh = (int)GetSystemMetrics(SM_CYSCREEN);
		cv::moveWindow(title, (sw - g_webcam_frame_width) / 2, (sh - g_webcam_frame_height) / 2);

		int pos;
		while (capture.read(frame))
		{
			if (!get_hismith_pos_by_image(frame, pos, true, NULL, NULL, title))
			{
				capture.release();
				return;
			}

			int key = cv::waitKey(0);
		}		
	}

	capture.release();
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

	int pos, dt;
	__int64 start_time = GetTickCount64();
	get_hismith_pos_by_image(frame, pos, true);
	dt = (int)(GetTickCount64() - start_time);

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

		draw_text(QString("Press 'Enter' for use current colors as original colors. Current vs original colors:\nb[%1(%2)-%3(%4)][%5(%6)-%7(%8)][%9(%10)-%11(%12)]\ng[%13(%14)-%15(%16)][%17(%18)-%19(%20)][%21(%22)-%23(%24)]\nPress b[q/w/e/r][a/s/d/f][z/x/c/v] | g[t/y/u/i][g/h/j/k][b/n/m/,] for change colors")
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
		cv::imshow(title, img);

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

void test_camera()
{
	int video_dev_id = get_video_dev_id();
	if (video_dev_id == -1)
		return;
	cv::VideoCapture capture(video_dev_id);

	if (capture.isOpened())
	{
		cv::Mat frame;
		int B_range[3][2];
		int G_range[3][2];
		cv::String title("Test Webcam");

		std::copy(&g_B_range[0][0], &g_B_range[0][0] + 3 * 2, &B_range[0][0]);
		std::copy(&g_G_range[0][0], &g_G_range[0][0] + 3 * 2, &G_range[0][0]);

		capture.set(cv::CAP_PROP_FRAME_WIDTH, g_webcam_frame_width);
		capture.set(cv::CAP_PROP_FRAME_HEIGHT, g_webcam_frame_height);
		capture.set(cv::CAP_PROP_BUFFERSIZE, 1);

		cv::namedWindow(title, 1);
		cv::setWindowProperty(title, cv::WND_PROP_TOPMOST, 1);
		int sw = (int)GetSystemMetrics(SM_CXSCREEN);
		int sh = (int)GetSystemMetrics(SM_CYSCREEN);
		cv::moveWindow(title, (sw - g_webcam_frame_width) / 2, (sh - g_webcam_frame_height) / 2);

		while (capture.read(frame))
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

			draw_text(QString("Press 'Enter' for use current colors as original colors. Current vs original colors:\nb[%1(%2)-%3(%4)][%5(%6)-%7(%8)][%9(%10)-%11(%12)]\ng[%13(%14)-%15(%16)][%17(%18)-%19(%20)][%21(%22)-%23(%24)]\nPress b[q/w/e/r][a/s/d/f][z/x/c/v] | g[t/y/u/i][g/h/j/k][b/n/m/,] for change colors")
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
			cv::imshow(title, img);

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
		}

		capture.release();
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
		std::vector<QPair<QPair<int, int>, std::vector<int>>> modify_funscript_move_in_functions;
		std::vector<QPair<QPair<int, int>, std::vector<int>>> modify_funscript_move_out_functions;

		// "[0.25:0.38|0.75:0.62],[0.25:0.12|0.75:0.87]"; // [fast:slow:fast],[slow:fast:slow]
		QStringList modify_funscript_function_move_variants = g_modify_funscript_function_move_variants.mid(1, g_modify_funscript_function_move_variants.size() - 2).split("],[");
		for (QString& modify_funscript_function_move_variant : modify_funscript_function_move_variants)
		{
			std::vector<QPair<double, double>> modify_funscript_move_function;

			QStringList modify_funscript_function_move_variant_actions = modify_funscript_function_move_variant.split("|");
			for (QString& modify_funscript_function_move_variant_action : modify_funscript_function_move_variant_actions)
			{
				QRegularExpression re_modify_funscript_function_move_variant_action("^([\\d\\.]+):([\\d\\.]+)$");
				QRegularExpressionMatch match;

				match = re_modify_funscript_function_move_variant_action.match(modify_funscript_function_move_variant_action);
				if (!match.hasMatch())
				{
					error_msg(QString("ERROR: incorrect format of modify_funscript_function_move_variant_action: %1, it should be ddt(0.0-1.0):ddpos[0.0-1.0]").arg(modify_funscript_function_move_variant_action));
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

		auto get_move_functions = [&modify_funscript_move_functions](QString modify_funscript_function_move_variants, QString modify_funscript_function_move_variants_type, std::vector<QPair<QPair<int, int>, std::vector<int>>>& _modify_funscript_move_functions) {
			bool res = false;
			//"[0-200:0],[200-maximum:1]";
			//"[0-200:0|1],[200-maximum:1]";
			//"[0-200:random],[200-maximum:1]";
			//"[0-maximum:random]";
			QStringList _modify_funscript_function_move_variants = modify_funscript_function_move_variants.mid(1, modify_funscript_function_move_variants.size() - 2).split("],[");
			for (QString& sub_modify_funscript_function_move_variant : _modify_funscript_function_move_variants)
			{
				QRegularExpression re_modify_funscript_move_in_function("^(\\d+)\\-(\\d+|maximum):([\\d\\|]+|random)$");
				QRegularExpressionMatch match;

				match = re_modify_funscript_move_in_function.match(sub_modify_funscript_function_move_variant);
				if (!match.hasMatch())
				{
					error_msg(QString("ERROR: incorrect format of g_modify_funscript_function_move_%1_variants: %2,\nit should be :\n"\
						"[min_speed_in_rpm_1-max_speed_in_rpm_1:variant_1_1|(or)variant_1_2|...],[min_speed_in_rpm_2-max_speed_in_rpm_2:variant_2_1|(or)variant_2_2|...],...\n"\
						"Regexp: [(\\d+)\\-(\\d+|maximum):([\\d\\|]+|random)],[(\\d+)\\-(\\d+|maximum):([\\d\\|]+|random)],...\nFor example:\n" \
						"[0-200:0],[200-maximum:1]\n"\
						"[0-200:0|1],[200-maximum:1]\n"\
						"[0-200:random],[200-maximum:1]\n"\
						"[0-maximum:random]").arg(modify_funscript_function_move_variants_type).arg(modify_funscript_function_move_variants));
					return res;
				}
				else
				{
					QString val;
					int min_speed = match.captured(1).toInt();

					val = match.captured(2);
					int max_speed = (val == "maximum") ? -1 : val.toInt();

					if ((max_speed != -1) && (max_speed <= min_speed))
					{
						error_msg(QString("ERROR: incorrect format of max_speed: %1 in sub_modify_funscript_function_move_%2_variant: %3, max_speed should be > min_speed").arg(max_speed).arg(modify_funscript_function_move_variants_type).arg(sub_modify_funscript_function_move_variant));
						return res;
					}

					val = match.captured(3);
					std::vector<int> variants;
					if (val == "random")
					{
						for (int variant = 0; variant < modify_funscript_move_functions.size(); variant++)
						{
							variants.push_back(variant);
						}
					}
					else
					{
						QStringList _variants = val.split("|");
						for (QString& _variant : _variants)
						{
							int variant = _variant.toInt();

							if (!((variant >= 0) && (variant < modify_funscript_move_functions.size())))
							{
								error_msg(QString("ERROR: incorrect format of variant: %1 in sub_modify_funscript_function_move_%2_variant: %3, it should be >= 0, < modify_funscript_move_functions.size() (%4)").arg(variant).arg(modify_funscript_function_move_variants_type).arg(sub_modify_funscript_function_move_variant).arg(modify_funscript_move_functions.size()));
								return res;
							}

							variants.push_back(variant);
						}
					}

					_modify_funscript_move_functions.push_back(QPair<QPair<int, int>, std::vector<int>>(QPair<int, int>(min_speed, max_speed), variants));
				}
			}

			res = true;
			return res;
		};

		res = get_move_functions(g_modify_funscript_function_move_in_variants, "in", modify_funscript_move_in_functions);
		if (!res)
		{
			return res;
		}

		res = get_move_functions(g_modify_funscript_function_move_out_variants, "out", modify_funscript_move_out_functions);
		if (!res)
		{
			return res;
		}

		std::list<QPair<int, int>> funscript_data_list;

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

							std::vector<QPair<QPair<int, int>, std::vector<int>>>& modify_funscript_move_type_functions = (prev_move_dirrection == 1) ? modify_funscript_move_in_functions : modify_funscript_move_out_functions;
							for (int var = 0; var < modify_funscript_move_type_functions.size(); var++)
							{
								if ((avg_cur_speed_in_rpm >= modify_funscript_move_type_functions[var].first.first) &&
									((modify_funscript_move_type_functions[var].first.second == -1) ? true : (avg_cur_speed_in_rpm <= modify_funscript_move_type_functions[var].first.second)))
								{
									int modify_funscript_function_move_variant;
									int vars_size = modify_funscript_move_type_functions[var].second.size();
									if (vars_size == 1)
									{
										modify_funscript_function_move_variant = modify_funscript_move_type_functions[var].second[0];
									}
									else
									{
										modify_funscript_function_move_variant = modify_funscript_move_type_functions[var].second[std::rand() % vars_size];
									}

									int total_move = funscript_data[id - 1].second - funscript_data[prev_top_end_id].second;
									std::vector<QPair<double, double>>& modify_funscript_move_function = modify_funscript_move_functions[modify_funscript_function_move_variant];

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
							}

							funscript_data_list.push_back(funscript_data[id - 1]);							
						}
						else
						{
							funscript_data_list.push_back(funscript_data[id - 1]);
						}

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

		int dpos = 0;
		int dt_video = 0;
		for (int i = speed_data.speed_statistics_data.size() - 1; i > 30; i--)
		{
			dt_video += speed_data.speed_statistics_data[i].dt_video;
			dpos += speed_data.speed_statistics_data[i].dpos;
			if (dt_video > 5000)
			{
				break;
			}
		}

		if (dt_video == 0)
		{
			error_msg(QString("ERROR: got dt_video == 0 (1) in file: %1").arg(fpath));
			return res;
		}

		speed_data.total_average_speed = (dpos * 1000) / dt_video;

		dt_video = 0;
		for (int i = 0; i < speed_data.speed_statistics_data.size()-1; i++)
		{
			if ( (speed_data.speed_statistics_data[i].dpos == 0) || (speed_data.speed_statistics_data[i + 1].dpos == 0) )
			{
				dt_video += speed_data.speed_statistics_data[i].dt_video;
			}
			else
			{
				if (speed_data.speed_statistics_data[i].dpos < speed_data.speed_statistics_data[i + 1].dpos)
				{
					int dt = speed_data.speed_statistics_data[i].dt_video - ((speed_data.speed_statistics_data[i + 1].dt_video * speed_data.speed_statistics_data[i].dpos) / speed_data.speed_statistics_data[i + 1].dpos);
					if (dt > 0)
					{
						dt_video += dt;
					}
				}
				
				break;
			}
		}

		speed_data.time_delay = dt_video;
		g_avg_time_delay += speed_data.time_delay;

		for (int i = 0; i < speed_data.speed_statistics_data.size(); i++)
		{
			dpos = 0;
			dt_video = 0;
			for (int j = i; j >= 0; j--)
			{
				dt_video += speed_data.speed_statistics_data[j].dt_video;
				dpos += speed_data.speed_statistics_data[j].dpos;
				if (dt_video >= g_dt_for_get_cur_speed)
				{
					break;
				}
			}

			if (dt_video < g_dt_for_get_cur_speed)
			{
				dt_video = g_dt_for_get_cur_speed;
			}

			if (dt_video == 0)
			{
				error_msg(QString("ERROR: got dt_video == 0 (2) in file: %1").arg(fpath));
				return res;
			}

			speed_data.speed_statistics_data[i].avg_cur_speed = (dpos * 1000) / dt_video;
		}

		{
			int n = 0;
			int avg_speed = 0;
			int s = 0;

			int dt_video = 0;
			for (int i = speed_data.speed_statistics_data.size() - 1; i > 30; i--)
			{
				avg_speed += speed_data.speed_statistics_data[i].avg_cur_speed;
				dt_video += speed_data.speed_statistics_data[i].dt_video;
				n++;
				if (dt_video > 5000)
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

			dt_video = 0;
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

				int dt = ((speed_data.speed_statistics_data[i + 1].dt_video * speed_data.speed_statistics_data[i].dpos) / speed_data.speed_statistics_data[i + 1].dpos);
				if (dt < speed_data.speed_statistics_data[i].dt_video)
				{
					dt_video += dt;
				}
				else
				{
					dt_video += speed_data.speed_statistics_data[i].dt_video;
				}
			}
			else
			{
				dt_video += speed_data.speed_statistics_data[i].dt_video;
			}
			i++;

			while (i < speed_data.speed_statistics_data.size())
			{
				dt_video += speed_data.speed_statistics_data[i].dt_video;

				if (speed_data.speed_statistics_data[i].avg_cur_speed >= min_avg_speed)
				{
					break;
				}
				i++;
			}

			if (dt_video == 0)
			{
				error_msg(QString("ERROR: got dt_video == 0 (3) in file: %1").arg(fpath));
				return res;
			}

			speed_data.average_rate_of_change_of_speed = (double)(speed_data.speed_statistics_data[i].avg_cur_speed) / (double)dt_video;
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

int get_d_form_poses(const int &abs_pos1, const int &pos2)
{
	int abs_dif1 = abs((abs_pos1 % 360) - pos2);
	int abs_dif2 = abs((abs_pos1 % 360) + 360 - pos2);
	int abs_dif3 = abs((abs_pos1 % 360) - pos2 - 360);
	int first_min = min(abs_dif1, abs_dif2);
	return min(first_min, abs_dif3);
}

int get_abs_pos(const int &cur_pos)
{
	return (cur_pos > 270) ? (cur_pos - 360) : cur_pos;
}

int update_abs_pos(const int &cur_pos, const int &prev_pos, int &abs_cur_pos, cv::Mat& frame/*, cv::Mat& prev_frame*/, double prev_speed)
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
int _num_prev_poss;

void shift_get_next_frame_and_cur_speed_data(int dpos)
{
	for (int i = 0; i < _num_prev_poss; i++)
	{
		_tmp_abs_prev_poss[i] += dpos;
	}
}

bool get_next_frame_and_cur_speed(cv::VideoCapture& capture, cv::Mat& frame/*, cv::Mat& prev_frame*/,
	int& abs_cur_pos, int& cur_pos, __int64& msec_video_cur_pos, double& cur_speed,
	__int64& msec_video_prev_pos, int& abs_prev_pos, bool show_results = false, cv::Mat* p_res_frame = NULL, cv::String title = "", QString add_data = QString())
{
	int prev_pos, dpos;
	int dt;

	prev_pos = cur_pos;

	get_new_camera_frame(capture, frame/*, prev_frame*/);

	msec_video_cur_pos = capture.get(cv::CAP_PROP_POS_MSEC);

	if (msec_video_prev_pos != -1)
	{
		if (_num_prev_poss == _tmp_prev_poss_max_N - 1)
		{
			for (int i = 0; i < _num_prev_poss; i++)
			{
				_tmp_msec_video_prev_poss[i] = _tmp_msec_video_prev_poss[i + 1];
				_tmp_abs_prev_poss[i] = _tmp_abs_prev_poss[i + 1];
			}
			_num_prev_poss--;			
		}

		msec_video_prev_pos = _tmp_msec_video_prev_poss[0];
		abs_prev_pos = _tmp_abs_prev_poss[0];
	}

	dt = (int)(msec_video_cur_pos - msec_video_prev_pos);

	if (!get_hismith_pos_by_image(frame, cur_pos, show_results, p_res_frame, &cur_speed, title, add_data))
	{
		return false;
	}
	dpos = update_abs_pos(cur_pos, prev_pos, abs_cur_pos, frame/*, prev_frame*/, cur_speed);

	if (msec_video_prev_pos != -1)
	{
		_tmp_msec_video_prev_poss[_num_prev_poss] = msec_video_cur_pos;
		_tmp_abs_prev_poss[_num_prev_poss] = abs_cur_pos;
		_num_prev_poss++;

		if (dt > 0)
		{
			cur_speed = (double)((abs_cur_pos - abs_prev_pos) * 1000.0) / (double)dt;
		}

		if (dt >= g_dt_for_get_cur_speed)
		{
			for (int i = 0; i < _num_prev_poss; i++)
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
		
		_tmp_msec_video_prev_poss[_num_prev_poss] = msec_video_cur_pos;
		_tmp_abs_prev_poss[_num_prev_poss] = abs_cur_pos;
		_num_prev_poss++;
	}

	return true;
}
//------------------------------------------

void get_delay_data()
{
	int cur_pos = 0, prev_pos = 0, dpos = 0, prev_dpos = 0, abs_prev_pos;
	__int64 msec_video_cur_pos = -1, msec_video_prev_pos = -1, msec_video_prev2_pos = -1;
	double cur_speed = -1, prev_speed = -1, dt = 0, ddt = 0, dmove = 0;
	int abs_cur_pos = 0;
	int delay_for_start, delay_for_switch_20_to_40;
	__int64 start_time = 0, cur_time = 0, delay_between_changes = 1000;
	int res;
	//~200 to start +100 for get speed 0.2 or 0.4
	//~50-150 for get speed 0.4 from 0.2 (delay_between_changes 1s or 200ms)
	//~140-190 for get speed 0.8 from 0.4 (delay_between_changes 1s or 200ms)
	//~47-110 for get speed 0.4 from 0.8 (delay_between_changes 1s or 200ms)

	//-----------------------------------------------------
	// Connecting to Hismith
	// NOTE: At first start: intiface central

	if (!connect_to_hismith())
		return;

	//-----------------------------------------------------

	cv::Mat frame, prev_frame;
	int video_dev_id = get_video_dev_id();
	if (video_dev_id == -1)
		return;
	cv::VideoCapture capture(video_dev_id);

	if (capture.isOpened())
	{
		capture.set(cv::CAP_PROP_FRAME_WIDTH, g_webcam_frame_width);
		capture.set(cv::CAP_PROP_FRAME_HEIGHT, g_webcam_frame_height);

		get_new_camera_frame(capture, frame/*, prev_frame*/);
		msec_video_cur_pos = capture.get(cv::CAP_PROP_POS_MSEC);
		if (!get_hismith_pos_by_image(frame, cur_pos))
		{
			capture.release();
			return;
		}
		abs_cur_pos = get_abs_pos(cur_pos);

		start_time = GetTickCount64();
		set_hithmith_speed(0.8);

		msec_video_prev_pos = -1;
		abs_prev_pos = 0;
		cur_speed = -1;
		while (cur_speed < 1300)
		{
			cur_time = GetTickCount64();
			prev_speed = cur_speed;
			if (!get_next_frame_and_cur_speed(capture, frame,
				abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
				msec_video_prev_pos, abs_prev_pos))
			{
				capture.release();
				return;
			}
		}

		start_time = GetTickCount64();
		while ((int)(GetTickCount64() - start_time) < delay_between_changes)
		{
			prev_speed = cur_speed;
			if(!get_next_frame_and_cur_speed(capture, frame,
				abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
				msec_video_prev_pos, abs_prev_pos))
			{
				capture.release();
				return;
			}
		}
		
		start_time = GetTickCount64();
		set_hithmith_speed(0.4);

		while (cur_speed > 900)
		{
			cur_time = GetTickCount64();
			prev_speed = cur_speed;
			if (!get_next_frame_and_cur_speed(capture, frame,
				abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
				msec_video_prev_pos, abs_prev_pos))
			{
				capture.release();
				return;
			}
		}
	}

	disconnect_from_hismith();

	show_msg(QString("delay: %1, cur_speed: %2, prev_speed: %3").arg((int)(cur_time - start_time)).arg(cur_speed).arg(prev_speed));	
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
					set_hithmith_speed(0);
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

int make_vlc_status_request(QNetworkAccessManager *manager, QNetworkRequest &req, bool &is_paused, QString &video_filename, bool &is_vlc_time_in_milliseconds)
{
	bool res = false;
	int cur_video_pos = -1;
	int length;
	double position;
	bool show_warning = true;
	QString ReqUrl(g_vlc_url + ":" + QString::number(g_vlc_port) + "/requests/status.xml");

	while (!res && !g_stop_run)
	{
		req.setUrl(QUrl(ReqUrl));
		QNetworkReply* rep = manager->get(req);
		QObject::connect(manager, &QNetworkAccessManager::finished, rep, &QNetworkReply::deleteLater);
		QByteArray reply_res;

		video_filename.clear();

		QEventLoop loop;
		QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		int loop_res = loop.exec();

		if (rep->isFinished() == true)
		{
			if (rep->error())
			{
				if (show_warning)
				{
					show_msg("Waiting for VLC is starded");
					set_hithmith_speed(0);
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

		if (res)
		{
			QDomDocument doc("data");
			doc.setContent(reply_res);

			QDomElement docElem = doc.documentElement();

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
						cur_video_pos = e.text().toInt();
					}
					else if (tag_name == "length")
					{
						length = e.text().toInt();
					}
					else if (tag_name == "position")
					{
						position = e.text().toDouble();
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
				res = false;
			}
			else
			{				
				if (cur_video_pos < (int)((double)(length * 100) * position))
				{
					is_vlc_time_in_milliseconds = false;
					double cur_video_pos_alt = length * position;

					if (cur_video_pos_alt < cur_video_pos + 1)
					{
						cur_video_pos = max(cur_video_pos * 1000, (int)(cur_video_pos_alt * 1000.0));
					}
					else
					{
						cur_video_pos *= 1000;
					}
				}
				else
				{
					is_vlc_time_in_milliseconds = true;
				}
			}
		}

		if (!res && !g_stop_run)
		{
			if (g_pMyDevice)
				set_hithmith_speed(0);
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
	}

	return cur_video_pos;
}

//---------------------------------------------------------------
// NOTE: QT Doesn't allow to create GUI in a non-main GUI thread
// like QMessageBox for example, so using Win API
//---------------------------------------------------------------
QString  _tmp_msg;
const wchar_t _tmp_CLASS_NAME[] = L"MSG Window Class";
std::mutex _tmp_msg_mutex;
HWND _tmp_hwnd = NULL;
std::thread* _tmp_p_msg_thr = NULL;
bool _tmp_stop_msg = false;

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
		/* */
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
			GetTextExtentPoint32(hdc, line.toStdWString().c_str(), wcslen(_tmp_msg.toStdWString().c_str()), &text_size);

			total_text_size.cx = max(text_size.cx, total_text_size.cx);
			total_text_size.cy += text_size.cy + 10;
		}
		
		int screen_w = GetSystemMetrics(SM_CXSCREEN);
		int screen_h = GetSystemMetrics(SM_CYSCREEN);

		SetWindowPos(hwnd, NULL, (screen_w - (total_text_size.cx + 20))/2, (screen_h - (total_text_size.cy + 10))/2, total_text_size.cx + 20, total_text_size.cy + 10, SWP_NOREDRAW);
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
			GetTextExtentPoint32(hdc, line.toStdWString().c_str(), wcslen(_tmp_msg.toStdWString().c_str()), &text_size);
			rt.top += 10;
			rt.bottom = rt.top + text_size.cy;
			DrawText(hdc, line.toStdWString().c_str(), -1, &rt, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
			rt.top = rt.bottom;
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

void show_msg(QString msg, int timeout)
{	

	if (_tmp_p_msg_thr)
	{		
		_tmp_stop_msg = true;
		if (_tmp_hwnd)
		{
			SendMessage(_tmp_hwnd, WM_CLOSE, 0, 0);
		}
		_tmp_p_msg_thr->join();
		delete _tmp_p_msg_thr;
		_tmp_stop_msg = false;
	}

	_tmp_p_msg_thr = new std::thread([msg, timeout] {
		_tmp_msg_mutex.lock();
		_tmp_msg = msg;
		HINSTANCE hInstance = (HINSTANCE)::GetModuleHandle(NULL);
		WNDCLASSEX wx = {};
		wx.cbSize = sizeof(WNDCLASSEX);
		wx.lpfnWndProc = WndProc;
		wx.hInstance = hInstance;
		wx.lpszClassName = _tmp_CLASS_NAME;
		if (RegisterClassEx(&wx))
		{
			_tmp_hwnd = CreateWindowEx(WS_EX_TOPMOST,
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
				ShowWindow(_tmp_hwnd, SW_SHOWNOACTIVATE);

				LONG lStyle = GetWindowLong(_tmp_hwnd, GWL_STYLE);
				lStyle &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
				SetWindowLong(_tmp_hwnd, GWL_STYLE, lStyle);

				__int64 start_time = GetTickCount64();
				int dt = timeout - (int)(GetTickCount64() - start_time);

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
					dt = timeout - (int)(GetTickCount64() - start_time);
				}

				DestroyWindow(_tmp_hwnd);
				_tmp_hwnd = NULL;
			}

			UnregisterClass(_tmp_CLASS_NAME, hInstance);			
		}
		_tmp_msg_mutex.unlock();
	});
}

//---------------------------------------------------------------

void run_funscript()
{
	const QString video_fname;
	QString funscript_fname, last_load_funscript_fname;
	int d_from_search_start_pos, cur_pos, search_start_pos;
	__int64 msec_video_cur_pos, msec_video_prev_pos;
	double dt = 0, dmove = 0;
	int abs_cur_pos = 0, abs_prev_pos = 0, last_abs_prev_pos = 0;
	int dpos;
	double cur_speed, prev_cur_speed = 0, action_start_speed;
	__int64 cur_time, start_time;
	int cur_video_pos, start_video_pos;
	bool is_video_paused = false;
	QString video_filename, last_play_video_filename;
	std::vector<QPair<int, int>> funscript_data_maped_full;
	bool get_next_frame_and_cur_speed_res = true;
	bool is_vlc_time_in_milliseconds = true;
	int res;

	//-----------------------------------------------------
	// Connecting to Hismith
	// NOTE: At first start: intiface central

	if (!connect_to_hismith())
		return;

	//-----------------------------------------------------
	// Connecting to Web Camera

	cv::Mat frame, prev_frame;
	int video_dev_id = get_video_dev_id();
	if (video_dev_id == -1)
		return;
	cv::VideoCapture capture(video_dev_id);

	if (capture.isOpened())
	{
		capture.set(cv::CAP_PROP_FRAME_WIDTH, g_webcam_frame_width);
		capture.set(cv::CAP_PROP_FRAME_HEIGHT, g_webcam_frame_height);
		capture.set(cv::CAP_PROP_BUFFERSIZE, 1);

		// geting first 30 frames for get better camera focus
		for (int i = 0; i < 30; i++)
		{
			get_new_camera_frame(capture, frame/*, prev_frame*/);
		}

		msec_video_cur_pos = capture.get(cv::CAP_PROP_POS_MSEC);
		if (!get_hismith_pos_by_image(frame, cur_pos))
		{
			capture.release();
			return;
		}
		abs_cur_pos = get_abs_pos(cur_pos);
	}
	else
	{
		error_msg("ERROR: Failed to connect to Web Camera");
	}

	//-----------------------------------------------------
	// Connecting to VLC player with already opened video

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

	while (!g_stop_run && get_next_frame_and_cur_speed_res)
	{
		set_hithmith_speed(0);

		if (g_pause)
		{
			std::unique_lock lk(g_stop_mutex);
			g_stop_cvar.wait(lk, [] { return !g_pause || g_stop_run; });
			if (g_stop_run)
				break;
		}

		cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds);
		last_play_video_filename = video_filename;

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
				cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds);
			} while (last_play_video_filename == video_filename && !g_stop_run);
			last_play_video_filename = video_filename;
			continue;
		}

		//-----------------------------------------------------
		// Load Funscript and Hismith statistical data		
		std::vector<QPair<int, int>> funscript_data_maped;

		if (last_load_funscript_fname != funscript_fname)
		{
			last_load_funscript_fname = funscript_fname;
			funscript_data_maped_full.clear();			
			if (!get_parsed_funscript_data(funscript_fname, funscript_data_maped_full, all_speeds_data))
			{
				do
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds);
				} while (last_play_video_filename == video_filename && !g_stop_run);
				last_play_video_filename = video_filename;
				continue;
			}			
		}

		bool found_start = false;
		int start_id;
		int pos_offset;
		int search_video_pos;
		
		cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds);
		search_video_pos = cur_video_pos;

		for (int i = 0; i < funscript_data_maped_full.size(); i++)
		{
			if (!found_start)
			{
				if (funscript_data_maped_full[i].first > search_video_pos + 500)
				{					
					found_start = true;
					search_start_pos = funscript_data_maped_full[i].second % 360;
					start_id = i;
					pos_offset = funscript_data_maped_full[start_id].second - search_start_pos;
					funscript_data_maped.push_back(QPair<int, int>(funscript_data_maped_full[start_id].first, search_start_pos));
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
				cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds);
			} while ((last_play_video_filename == video_filename) && (cur_video_pos >= search_video_pos) && !g_stop_run);
			last_play_video_filename = video_filename;
			continue;
		}
		else
		{
			if (funscript_data_maped[0].first - search_video_pos > 5000)
			{
				show_msg(QString("The first funscript video action will be afte %1 seconds at pos: %2").arg((funscript_data_maped[0].first - search_video_pos)/1000).arg(VideoTimeToStr(funscript_data_maped[0].first).c_str()), 5000);
			}
		}

		if (capture.isOpened())
		{
			int actions_size = funscript_data_maped.size();

			struct results_data
			{
				int dif_cur_vs_req_action_end_time;
				int dif_cur_vs_req_action_start_time;
				int dif_speed_changed_vs_req_action_start_time;
				int dif_cur_vs_req_exp_pos;				
				int action_length_time;				
				int req_abs_cur_pos;
				int req_dpos;
				int req_dpos_add;
				int abs_cur_pos;				
				int req_speed;				
				int got_avg_speed;
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

			int dtime = 0;			

			if (is_video_paused)
			{
				show_msg("Ready to go", 1000);
			}
			else
			{
				show_msg("Runing!", 1000);
			}

			start_video_pos = cur_video_pos;
			QString start_video_name = video_filename;
			//waiting for video unpaused
			while (is_video_paused || (cur_video_pos < (funscript_data_maped[0].first - min(1000, 2*(funscript_data_maped[1].first - funscript_data_maped[0].first)))))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds);
				start_time = GetTickCount64();
				start_video_pos = cur_video_pos;
				
				if (g_stop_run || g_pause || (last_play_video_filename != video_filename) || (cur_video_pos > funscript_data_maped[1].first) || (cur_video_pos < search_video_pos))
				{
					last_play_video_filename = video_filename;
					actions_size = 1;
					break;
				}
			}

			if (g_stop_run || g_pause || (cur_video_pos > funscript_data_maped[1].first) || (cur_video_pos < search_video_pos))
			{
				continue;
			}

			get_new_camera_frame(capture, frame/*, prev_frame*/);
			msec_video_cur_pos = capture.get(cv::CAP_PROP_POS_MSEC);
			get_next_frame_and_cur_speed_res = get_hismith_pos_by_image(frame, cur_pos);
			if (!get_next_frame_and_cur_speed_res)
			{
				break;
			}
			abs_cur_pos = get_abs_pos(cur_pos);

			int action_id;

			__int64 action_start_time, avg_speed_start_time, speed_change_time, prev_get_speed_time = start_time;
			int start_abs_pos, avg_speed_start_abs_cur_pos, exp_abs_cur_pos_to_req_time, tmp_val, dif_cur_vs_req_action_start_time, last_set_action_id_for_dif_cur_vs_req_exp_pos;

			int exp_abs_pos_before_speed_change, action_start_abs_pos;
			int req_speed, req_cur_speed, got_avg_speed, req_new_speed;
			double optimal_hismith_speed = 0, hismith_speed_prev = 0, optimal_hismith_start_speed = 0, avg_hismith_speed_prev = 0;
			QString actions_end_with = "success";
			bool speed_change_was_obtained = false;
			QString hismith_speed_changed;

			msec_video_prev_pos = -1;
			abs_prev_pos = 0;
			dpos = 0;
			cur_speed = 0;

			__int64 t1, t2, t3, dt_total;

			action_id = 1;

			if (is_vlc_time_in_milliseconds)
			{
				cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds);
				start_time = GetTickCount64();
				start_video_pos = cur_video_pos;
			}
			else
			{
				// for minimize time difference sync waiting for the nearest second change				
				int prev_video_pos;
				do
				{
					prev_video_pos = cur_video_pos;
					cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds);
					start_time = GetTickCount64();
					start_video_pos = cur_video_pos;
				} while ((prev_video_pos / 1000 == cur_video_pos / 1000) && !g_stop_run);

				if (g_stop_run)
				{
					break;
				}

				while (((funscript_data_maped[action_id - 1].first < cur_video_pos) || (funscript_data_maped[action_id - 1].second % 360 != 0)) && (action_id < actions_size))
				{
					action_id++;
				}

				if (action_id < actions_size)
				{
					abs_cur_pos += (funscript_data_maped[action_id - 1].second / 360) * 360;
				}
			}
			
			start_abs_pos = abs_cur_pos;
			last_set_action_id_for_dif_cur_vs_req_exp_pos = action_id - 1;

			int _tmp_cur_video_pos = -1;
			bool _tmp_is_paused = false;
			QString _tmp_video_filename;
			bool _tmp_is_vlc_time_in_milliseconds;
			double cur_set_hismith_speed = 0;
			int prev_cur_video_pos = cur_video_pos;
			std::thread* p_get_vlc_status = NULL;

			// required for check computer freezes
			prev_get_speed_time = GetTickCount64();
			while (action_id < actions_size)
			{				
				action_start_time = cur_time = GetTickCount64();

				if ((funscript_data_maped[action_id].second - abs_cur_pos >= 360 * 2) && (g_max_allowed_hismith_speed < 100))
				{
					abs_cur_pos += 360;
					shift_get_next_frame_and_cur_speed_data(360);
				}

				action_start_abs_pos = abs_cur_pos;
				avg_speed_start_time = -1;
				got_avg_speed = -1;
				action_start_speed = cur_speed;
				speed_change_time = -1;
				hismith_speed_changed.clear();
				dif_cur_vs_req_action_start_time = (start_video_pos + (int)(action_start_time - start_time)) - funscript_data_maped[action_id - 1].first;

				dt = funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time));
				if (dt < g_min_dt_for_set_hismith_speed)
				{
					action_id++;
					continue;
				}

				if (start_video_pos + (int)(cur_time - start_time) - cur_video_pos >= 250)
				{
					p_get_vlc_status = new std::thread([&_tmp_cur_video_pos, &_tmp_is_paused, &_tmp_video_filename, &_tmp_is_vlc_time_in_milliseconds] {
						_tmp_cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, _tmp_is_paused, _tmp_video_filename, _tmp_is_vlc_time_in_milliseconds);
						}
					);
				}

				dpos = funscript_data_maped[action_id].second - abs_cur_pos;
				req_speed = ((funscript_data_maped[action_id].second - abs_cur_pos) * 1000.0) / dt;
				hismith_speed_prev = cur_set_hismith_speed;
				//exp_abs_pos_before_speed_change = abs_cur_pos + ((cur_speed * (double)g_speed_change_delay) / 1000.0);
				optimal_hismith_speed = (double)get_optimal_hismith_speed(all_speeds_data, (int)(hismith_speed_prev*100.0), cur_speed, dpos, dt) / 100.0;				
				avg_hismith_speed_prev = (double)get_avg_hismith_speed(all_speeds_data, action_start_speed)/100.0;
				int optimal_hismith_start_speed_int;
				
				if (action_start_speed <= req_speed)
				{
					double val = min(avg_hismith_speed_prev, hismith_speed_prev);
					optimal_hismith_start_speed_int = (int)((min(max(val +
						((optimal_hismith_speed - val) * g_increase_hismith_speed_start_multiplier), 0.0),
						(double)g_max_allowed_hismith_speed / 100.0)) * 100.0);
				}
				else
				{
					double val = max(avg_hismith_speed_prev, hismith_speed_prev);
					optimal_hismith_start_speed_int = (int)((min(max(val +
						((optimal_hismith_speed - val) * g_slowdown_hismith_speed_start_multiplier), 0.0),
						(double)g_max_allowed_hismith_speed / 100.0)) * 100.0);
				}

				optimal_hismith_start_speed = (double)(optimal_hismith_start_speed_int)/100.0;

				optimal_hismith_start_speed = set_hithmith_speed(optimal_hismith_start_speed);
				speed_change_was_obtained = false;

				dtime = (action_id < actions_size - 1) ? g_speed_change_delay : 0;
				
				cur_set_hismith_speed = optimal_hismith_start_speed;
				do
				{
					if (g_stop_run || g_pause || is_video_paused || (cur_video_pos > (start_video_pos + (int)(cur_time - start_time)) + (is_vlc_time_in_milliseconds ? 300 : 1000)) ||
						(cur_video_pos < prev_cur_video_pos - 300) || (last_play_video_filename != video_filename))
					{
						break;
					}
					
					last_abs_prev_pos = abs_cur_pos;
					get_next_frame_and_cur_speed_res = get_next_frame_and_cur_speed(capture, frame,
						abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
						msec_video_prev_pos, abs_prev_pos);
					
					if (!get_next_frame_and_cur_speed_res)
					{
						break;
					}

					cur_time = GetTickCount64();

					{
						dt = funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time));
						if (dt > 0)
						{
							req_cur_speed = max(((funscript_data_maped[action_id].second - abs_cur_pos) * 1000.0) / dt, 0);
							hismith_speed_changed += QString("[set_h_spd:%1 tm_ofs_to_end:%2 cur_spd: %3 req_cur_spd: %4 act_start_spd: %5 req_pos_vs_cur: %6]").arg(cur_set_hismith_speed).arg(funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time))).arg(cur_speed).arg(req_cur_speed).arg(action_start_speed).arg(funscript_data_maped[action_id].second - abs_cur_pos);
						}
					}

					if ((int)(cur_time - prev_get_speed_time) > g_cpu_freezes_timeout)
					{
						break;
					}
					
					if (funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time)) > 1000)
					{
						if (start_video_pos + (int)(cur_time - start_time) - cur_video_pos >= 1000)
						{
							if (p_get_vlc_status)
							{
								p_get_vlc_status->join();
								delete p_get_vlc_status;
								p_get_vlc_status = NULL;
								prev_cur_video_pos = cur_video_pos;
								cur_video_pos = _tmp_cur_video_pos;
								is_video_paused = _tmp_is_paused;
								video_filename = _tmp_video_filename;
								is_vlc_time_in_milliseconds = _tmp_is_vlc_time_in_milliseconds;
							}

							if (start_video_pos + (int)(cur_time - start_time) - cur_video_pos >= 1000)
							{
								p_get_vlc_status = new std::thread([&_tmp_cur_video_pos, &_tmp_is_paused, &_tmp_video_filename, &_tmp_is_vlc_time_in_milliseconds] {
									_tmp_cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, _tmp_is_paused, _tmp_video_filename, _tmp_is_vlc_time_in_milliseconds);
									}
								);
							}
						}
					}

					{
						int _id = last_set_action_id_for_dif_cur_vs_req_exp_pos + 1;
						while ( _id <= action_id)
						{
							if (((start_video_pos + (int)(prev_get_speed_time - start_time)) - funscript_data_maped[_id].first <= 0) &&
								((start_video_pos + (int)(cur_time - start_time)) - funscript_data_maped[_id].first >= 0))
							{
								if (cur_time != prev_get_speed_time)
								{
									dpos = abs_cur_pos - last_abs_prev_pos;
									exp_abs_cur_pos_to_req_time = abs_cur_pos - ((dpos * (start_video_pos + (int)(cur_time - start_time) - funscript_data_maped[_id].first)) / (cur_time - prev_get_speed_time));
									results[_id - 1].dif_cur_vs_req_exp_pos = exp_abs_cur_pos_to_req_time - funscript_data_maped[_id].second;
									last_set_action_id_for_dif_cur_vs_req_exp_pos = _id;
								}
								else
								{
									results[_id - 1].dif_cur_vs_req_exp_pos = abs_cur_pos - funscript_data_maped[_id].second;
									last_set_action_id_for_dif_cur_vs_req_exp_pos = _id;
								}
							}

							_id++;
						}

						prev_get_speed_time = cur_time;
						prev_cur_speed = cur_speed;
					}

					exp_abs_pos_before_speed_change = abs_cur_pos + ((cur_speed * (double)g_speed_change_delay) / 1000.0);

					if (exp_abs_pos_before_speed_change >= funscript_data_maped[action_id].second)
					{
						if (cur_set_hismith_speed != 0)
						{
							req_cur_speed = 0;
							cur_set_hismith_speed = set_hithmith_speed(0);
							hismith_speed_changed += QString("[spd_change: set_h_spd:%1 tm_ofs_to_end:%2 cur_spd: %3 req_cur_spd: %4 req_pos_vs_cur: %5]").arg(cur_set_hismith_speed).arg(funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time))).arg(cur_speed).arg(req_cur_speed).arg(funscript_data_maped[action_id].second - abs_cur_pos);
						}
					}
					else
					{
						int exp_avg_speed = cur_set_hismith_speed > 0 ? all_speeds_data.speed_data_vector[cur_set_hismith_speed-1].total_average_speed : 0;
						if ( (!speed_change_was_obtained) &&
							( ((action_start_speed > req_speed) && (cur_speed <= req_speed + abs(action_start_speed - req_speed) / 4)) ||
							  ((action_start_speed < req_speed) && (cur_speed >= req_speed - abs(req_speed - action_start_speed) / 2)) ) )
						{

							//dt = funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time));
							//req_speed = max(((funscript_data_maped[action_id].second - abs_cur_pos) * 1000.0) / dt, 0);
							//optimal_hismith_speed = (double)get_optimal_hismith_speed(speed_data_vector, req_speed, funscript_data_maped[action_id].second, exp_abs_pos_before_speed_change) / 100.0;
							//set_hithmith_speed(optimal_hismith_speed);
							//cur_set_hismith_speed = optimal_hismith_speed;
							speed_change_time = cur_time;
							speed_change_was_obtained = true;
						}
						
						if (speed_change_was_obtained)
						{
							dt = funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time));
							dpos = funscript_data_maped[action_id].second - abs_cur_pos;

							if (dt >= g_min_dt_for_set_hismith_speed)
							{
								req_cur_speed = max((dpos * 1000) / dt, 0);

								//if (cur_speed > req_cur_speed)
								//{
								//	//double _optimal_hismith_speed_cur = (double)get_optimal_hismith_speed(speed_data_vector, cur_speed, funscript_data_maped[action_id].second, abs_cur_pos) / 100.0;
								//	//double _optimal_hismith_speed_req = (double)get_optimal_hismith_speed(speed_data_vector, req_cur_speed, funscript_data_maped[action_id].second, abs_cur_pos) / 100.0;
								//	//cur_set_hismith_speed = cur_set_hismith_speed * (_optimal_hismith_speed_req / _optimal_hismith_speed_cur);

								//	tmp_val = (int)(cur_set_hismith_speed * 100.0);
								//	tmp_val = max(tmp_val - max((int)((double)tmp_val * g_slowdown_hismith_speed_change_multiplier), 1), 0); //-%5
								//	cur_set_hismith_speed = (double)(tmp_val) / 100.0;

								//	cur_set_hismith_speed = set_hithmith_speed(cur_set_hismith_speed);

								//	hismith_speed_changed += QString("[spd_change: set_spd:%1 tm_ofs_to_end:%2 cur_spd: %3 req_cur_spd: %4 req_pos_vs_cur: %5]").arg(cur_set_hismith_speed).arg(funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time))).arg(cur_speed).arg(req_cur_speed).arg(funscript_data_maped[action_id].second - abs_cur_pos);
								//}
								//else
								{
									cur_set_hismith_speed = (double)get_optimal_hismith_speed(all_speeds_data, (int)(cur_set_hismith_speed * 100.0), cur_speed, dpos, dt) / 100.0;

									//double _optimal_hismith_speed_cur = (double)get_optimal_hismith_speed(all_speeds_data, cur_speed, funscript_data_maped[action_id].second, abs_cur_pos) / 100.0;
									//double _optimal_hismith_speed_req = (double)get_optimal_hismith_speed(speed_data_vector, req_cur_speed, funscript_data_maped[action_id].second, abs_cur_pos) / 100.0;
									//cur_set_hismith_speed = cur_set_hismith_speed * (_optimal_hismith_speed_req / _optimal_hismith_speed_cur);

									//tmp_val = (int)(cur_set_hismith_speed * 100.0);
									//tmp_val = min(tmp_val + max((int)((double)tmp_val * g_increase_hismith_speed_change_multiplier), 1), g_max_allowed_hismith_speed); //+%5
									//cur_set_hismith_speed = (double)(tmp_val) / 100.0;

									cur_set_hismith_speed = set_hithmith_speed(cur_set_hismith_speed);

									hismith_speed_changed += QString("[spd_change: set_spd:%1 tm_ofs_to_end:%2 cur_spd: %3 req_cur_spd: %4 req_pos_vs_cur: %5]").arg(cur_set_hismith_speed).arg(funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time))).arg(cur_speed).arg(req_cur_speed).arg(funscript_data_maped[action_id].second - abs_cur_pos);
								}
							}
						}
					}
					if (avg_speed_start_time == -1)
					{
						avg_speed_start_time = cur_time;
						avg_speed_start_abs_cur_pos = abs_cur_pos;
					}

				} while ((start_video_pos + (int)(cur_time - start_time)) + dtime < funscript_data_maped[action_id].first);				

				if ((avg_speed_start_time != -1) && (cur_time - avg_speed_start_time >= g_dt_for_get_cur_speed))
				{
					got_avg_speed = ((abs_cur_pos - avg_speed_start_abs_cur_pos) * 1000) / (int)(cur_time - avg_speed_start_time);					
				}
				
				if (p_get_vlc_status)
				{
					p_get_vlc_status->join();
					delete p_get_vlc_status;
					p_get_vlc_status = NULL;
					prev_cur_video_pos = cur_video_pos;
					cur_video_pos = _tmp_cur_video_pos;
					is_video_paused = _tmp_is_paused;
					video_filename = _tmp_video_filename;
					is_vlc_time_in_milliseconds = _tmp_is_vlc_time_in_milliseconds;
				}
				
				results[action_id - 1].action_start_video_time = VideoTimeToStr(funscript_data_maped[action_id - 1].first).c_str();
				results[action_id - 1].dif_cur_vs_req_action_end_time = (start_video_pos + (int)(cur_time - start_time)) - funscript_data_maped[action_id].first;
				results[action_id - 1].dif_cur_vs_req_action_start_time = dif_cur_vs_req_action_start_time;
				results[action_id - 1].dif_speed_changed_vs_req_action_start_time = (speed_change_time == -1) ? -1 : (start_video_pos + (int)(speed_change_time - start_time)) - funscript_data_maped[action_id - 1].first;
				results[action_id - 1].action_length_time = funscript_data_maped[action_id].first - funscript_data_maped[action_id - 1].first;
				results[action_id - 1].req_abs_cur_pos = funscript_data_maped[action_id].second;
				results[action_id - 1].req_dpos = funscript_data_maped[action_id].second - funscript_data_maped[action_id - 1].second;
				results[action_id - 1].req_dpos_add = funscript_data_maped[action_id - 1].second - action_start_abs_pos;
				results[action_id - 1].abs_cur_pos = abs_cur_pos;
				results[action_id - 1].req_speed = req_speed;
				results[action_id - 1].got_avg_speed = got_avg_speed;
				results[action_id - 1].start_speed = (int)action_start_speed;
				results[action_id - 1].end_speed = (int)cur_speed;
				results[action_id - 1].hismith_speed_prev = (int)(hismith_speed_prev * 100.0);
				results[action_id - 1].avg_hismith_speed_prev = (int)(avg_hismith_speed_prev * 100.0);
				results[action_id - 1].optimal_hismith_speed = (int)(optimal_hismith_speed * 100.0);
				results[action_id - 1].optimal_hismith_start_speed = (int)(optimal_hismith_start_speed * 100.0);
				results[action_id - 1].hismith_speed_changed = hismith_speed_changed;

				if ((int)(cur_time - prev_get_speed_time) > g_cpu_freezes_timeout)
				{
					actions_end_with = QString("It looks you get CPU feezes now, restarting all actions.");
					show_msg(actions_end_with);
					actions_size = action_id;
					break;
				}

				if (!get_next_frame_and_cur_speed_res)
				{
					actions_end_with = QString("Failed to get next webcam frame and current rotation speed, restarting all actions.");
					show_msg(actions_end_with);
					actions_size = action_id;
					break;
				}
				
				if (g_stop_run || g_pause || is_video_paused || 
					(cur_video_pos > (start_video_pos + (int)(cur_time - start_time)) + (is_vlc_time_in_milliseconds ? 300 : 1000)) ||
					(cur_video_pos < prev_cur_video_pos - 300) || (last_play_video_filename != video_filename))
				{
					if (last_play_video_filename != video_filename)
					{
						show_msg("Played video was changed");
						actions_end_with = QString("last_play_video_filename (%1) != video_filename (%1)").arg(last_play_video_filename).arg(video_filename);
					}
					else if (cur_video_pos > (start_video_pos + (int)(cur_time - start_time)) + (is_vlc_time_in_milliseconds ? 300 : 1000))
					{
						show_msg("Video time was jumped forward");
						actions_end_with = QString("cur_video_pos (%1) > (start_video_pos + (int)(cur_time - start_time))(%2) + %3").arg(cur_video_pos).arg(start_video_pos + (int)(cur_time - start_time)).arg(is_vlc_time_in_milliseconds ? 300 : 1000);
					}
					else if ((action_id > 1) && (cur_video_pos < prev_cur_video_pos - 300))
					{
						show_msg("Video time was jumped backward");
						actions_end_with = QString("cur_video_pos (%1) < prev_cur_video_pos (%2) - 300").arg(cur_video_pos).arg(prev_cur_video_pos);
					}
					else
					{
						actions_end_with = QString("g_stop_run || g_pause || is_video_paused");
					}

					//show_msg(actions_end_with);

					last_play_video_filename = video_filename;
					actions_size = action_id;
					break;
				}
				
				action_id++;
			}			

			QString result_str;
			for (int i = 0; i < actions_size; i++)
			{
				result_str += QString("dif_end_pos:%1 start_t:%2 len:%3 req_dpos:%4+(%5) dif_start_t:%6 dif_end_t:%7 "
									"start_spd:%8 end_spd:%9 req_avg_speed:%10 prev_h_spd:%11 prev_avg_h_spd:%12 opt_h_spd:%13 opt_start_spd:%14 add_info:%15\n")
					.arg(results[i].dif_cur_vs_req_exp_pos)
					.arg(results[i].action_start_video_time)
					.arg(results[i].action_length_time)
					.arg(results[i].req_dpos)
					.arg(results[i].req_dpos_add)					
					.arg(results[i].dif_cur_vs_req_action_start_time)
					.arg(results[i].dif_cur_vs_req_action_end_time)
					.arg(results[i].start_speed)
					.arg(results[i].end_speed)
					.arg(results[i].req_speed)
					.arg(results[i].hismith_speed_prev)
					.arg(results[i].avg_hismith_speed_prev)
					.arg(results[i].optimal_hismith_speed)
					.arg(results[i].optimal_hismith_start_speed)
					.arg(results[i].hismith_speed_changed);
			}
			result_str += QString("actions_end_with: %1\n\n").arg(actions_end_with);

			QFile file(g_root_dir + "\\res_data\\!results.txt");
			if (file.open(QFile::WriteOnly | QFile::Append | QFile::Text))
			{
				QTextStream ts(&file);
				ts << QString("video_name:%1 start_t:%2[%3 msec] start_pos:%4 req_pos:%5\n%6")
							.arg(start_video_name)
							.arg(VideoTimeToStr(start_video_pos).c_str())
							.arg(start_video_pos)
							.arg(start_abs_pos)
							.arg(funscript_data_maped[0].second)
							.arg(result_str);
				file.flush();
				file.close();
			}

			set_hithmith_speed(0);
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		}
	}
	
	disconnect_from_hismith();

	g_pNetworkAccessManager->deleteLater();

	capture.release();
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

bool get_devices_list()
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

	if (g_myDevices.size() == 0)
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
	QString selected_device = pW->ui->Devices->itemText(pW->ui->Devices->currentIndex());
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
	__int64 msec_video_prev_pos = -1, start_time, cur_time, prev_time, msec_video_start_pos;
	int dpos = 0;
	double cur_speed = -1;
	int max_dt_according_webcam_to_get_new_frame_and_speed = 0;
	int max_dt_according_GetTickCount = 0;

	show_msg("It can takes about 10 seconds, please wait...", 2000);

	//-----------------------------------------------------
	// Connecting to Hismith
	// NOTE: At first start: intiface central

	if (!connect_to_hismith())
		return;

	//-----------------------------------------------------
	// Connecting to Web Camera	

	cv::Mat frame, bad_frame, prev_frame, res_frame;
	int video_dev_id = get_video_dev_id();
	if (video_dev_id == -1)
		return;
	cv::VideoCapture capture(video_dev_id);

	if (capture.isOpened())
	{
		capture.set(cv::CAP_PROP_FRAME_WIDTH, g_webcam_frame_width);
		capture.set(cv::CAP_PROP_FRAME_HEIGHT, g_webcam_frame_height);

		// geting first 30 frames for get better camera focus
		for (int i = 0; i < 30; i++)
		{
			get_new_camera_frame(capture, frame/*, prev_frame*/);
		}

		last_msec_video_prev_pos = msec_video_cur_pos = capture.get(cv::CAP_PROP_POS_MSEC);
		if (!get_hismith_pos_by_image(frame, cur_pos))
		{
			capture.release();
			return;
		}
		abs_cur_pos = get_abs_pos(cur_pos);

		hismith_speed = (int)(set_hithmith_speed((double)hismith_speed / 100.0) * 100.0);		

		get_next_frame_and_cur_speed(capture, frame,
			abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
			msec_video_prev_pos, abs_prev_pos);

		start_time = cur_time = GetTickCount64();
		msec_video_start_pos = msec_video_cur_pos;

		int num_frames = 0, num_frame_video, num_frame_gtc;
		bool last_get_frame_status = false;

		while (1)
		{			
			last_msec_video_prev_pos = msec_video_cur_pos;
			last_get_frame_status = get_next_frame_and_cur_speed(capture, frame,
				abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
				msec_video_prev_pos, abs_prev_pos);
			if (!last_get_frame_status)
			{
				capture.release();
				break;
			}
			prev_time = cur_time;
			cur_time = GetTickCount64();
			num_frames++;			

			if (msec_video_cur_pos - last_msec_video_prev_pos > max_dt_according_webcam_to_get_new_frame_and_speed)
			{
				max_dt_according_webcam_to_get_new_frame_and_speed = msec_video_cur_pos - last_msec_video_prev_pos;
				num_frame_video = num_frames;
			}

			if ((int)(cur_time - prev_time) > max_dt_according_GetTickCount)
			{
				max_dt_according_GetTickCount = (int)(cur_time - prev_time);
				num_frame_gtc = num_frames;
				//frame.copyTo(bad_frame);
			}

			if (cur_time - start_time > 5000)
			{
				break;
			}
		}		

		set_hithmith_speed(0);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));

		if (num_frames > 0)
		{
			//auto t = std::time(nullptr);
			//auto tm = *std::localtime(&t);
			//std::ostringstream oss;
			//oss << std::put_time(&tm, "%d_%m_%Y_%H_%M_%S");
			//QString time_str = oss.str().c_str();
			//save_BGR_image(bad_frame, g_root_dir + "\\error_data\\" + time_str + QString("_slow_frame_gtc_dt_%1.bmp").arg(max_dt_according_GetTickCount));

			show_msg(QString("last_get_frame_status: %1\n"
				"max_dt_according_webcam_to_get_new_frame_and_speed:%2 frame_number:%3\n"
				"max_dt_according_GetTickCount:%4 frame_number:%5\n"
				"avg_dt_according_webcam_to_get_new_frame_and_speed:%6\n"
				"avg_dt_according_GetTickCount:%7")
				.arg(last_get_frame_status)
				.arg(max_dt_according_webcam_to_get_new_frame_and_speed)
				.arg(num_frame_video)
				.arg(max_dt_according_GetTickCount)
				.arg(num_frame_gtc)
				.arg((int)(msec_video_cur_pos - msec_video_start_pos) / num_frames)
				.arg((int)(cur_time - start_time) / num_frames)
				,
				"Performance Results");
		}
	}

	disconnect_from_hismith();
}

void get_statistics_with_hismith()
{
	int cur_pos, res;
	__int64 msec_video_cur_pos = -1, msec_video_prev_pos = -1, last_msec_video_prev_pos;
	int abs_cur_pos, abs_prev_pos = 0, last_abs_prev_pos;
	__int64 get_statistics_start_time, start_time, cur_time, prev_time, msec_video_start_pos;
	int dpos = 0;
	double cur_speed = -1;
	int max_dt_according_webcam_to_get_new_frame_and_speed = 0;
	int max_dt_according_GetTickCount = 0;
	int hismith_speed;

	g_work_in_progress = true;

	get_statistics_start_time = GetTickCount64();

	show_msg("It can takes ~20 minutes, please wait...\nIt will get data for speed from 1-100", 5000);	

	//-----------------------------------------------------
	// Connecting to Hismith
	// NOTE: At first start: intiface central

	if (!connect_to_hismith())
	{
		g_work_in_progress = false;
		g_stop_run = false;
		return;
	}

	//-----------------------------------------------------
	// Connecting to Web Camera	

	cv::Mat frame, bad_frame, prev_frame, res_frame;
	int video_dev_id = get_video_dev_id();
	if (video_dev_id == -1)
	{
		g_work_in_progress = false;
		g_stop_run = false;
		return;
	}
	cv::VideoCapture capture(video_dev_id);

	if (capture.isOpened())
	{
		capture.set(cv::CAP_PROP_FRAME_WIDTH, g_webcam_frame_width);
		capture.set(cv::CAP_PROP_FRAME_HEIGHT, g_webcam_frame_height);

		// geting first 30 frames for get better camera focus
		for (int i = 0; i < 30; i++)
		{
			get_new_camera_frame(capture, frame/*, prev_frame*/);
		}

		if (!get_hismith_pos_by_image(frame, cur_pos))
		{
			capture.release();
			g_work_in_progress = false;
			g_stop_run = false;
			return;
		}
		abs_cur_pos = get_abs_pos(cur_pos);

		int results_size = 30 * 100, result_id;
		std::vector<statistics_data> results(results_size);

		start_time = cur_time = GetTickCount64();
		while ((int)(GetTickCount64() - start_time) < 1000)
		{
			get_next_frame_and_cur_speed(capture, frame,
				abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
				msec_video_prev_pos, abs_prev_pos);
		}		

		for (hismith_speed = 1; hismith_speed <= g_max_allowed_hismith_speed; hismith_speed++)
		{
			bool need_restart;

			do
			{
				show_msg(QString("Starting to get data for speed %1 ...").arg(hismith_speed), 2000);

				set_hithmith_speed(0);

				start_time = cur_time = GetTickCount64();
				while (((int)(GetTickCount64() - start_time) < 3000) || (cur_speed > 10))
				{
					get_next_frame_and_cur_speed(capture, frame,
						abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
						msec_video_prev_pos, abs_prev_pos);
				}

				need_restart = false;
				start_time = cur_time = GetTickCount64();

				g_pClient->sendScalar(*g_pMyDevice, (double)hismith_speed / 100.0);

				result_id = 0;
				start_time = cur_time = GetTickCount64();

				while ( ((int)(cur_time - start_time) < 7000) && !g_stop_run )
				{
					last_abs_prev_pos = abs_cur_pos;
					last_msec_video_prev_pos = msec_video_cur_pos;
					get_next_frame_and_cur_speed(capture, frame,
						abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
						msec_video_prev_pos, abs_prev_pos);
					prev_time = cur_time;
					cur_time = GetTickCount64();

					if (abs_cur_pos - last_abs_prev_pos < 0)
					{
						need_restart = true;
						break;
					}

					if (result_id < results_size)
					{
						results[result_id].dpos = (abs_cur_pos - last_abs_prev_pos);
						results[result_id].dt_video = (int)(msec_video_cur_pos - last_msec_video_prev_pos);
						results[result_id].dt_gtc = (int)(cur_time - prev_time);
						result_id++;
					}
					else
					{
						break;
					}
				}				

				if (g_stop_run)
				{
					set_hithmith_speed(0);
					std::this_thread::sleep_for(std::chrono::milliseconds(1000));
					disconnect_from_hismith();
					g_work_in_progress = false;
					g_stop_run = false;
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

	show_msg(QString("All was done for time: %1 !").arg(VideoTimeToStr(GetTickCount64() - get_statistics_start_time).c_str()), "Get Statistics Data");
}

void test_hismith(int hismith_speed)
{
	int cur_pos, res;
	__int64 msec_video_cur_pos = -1, last_msec_video_prev_pos;
	int abs_cur_pos, abs_prev_pos = 0;
	__int64 msec_video_prev_pos = -1;
	int dpos = 0;
	double cur_speed = -1;

	//-----------------------------------------------------
	// Connecting to Hismith
	// NOTE: At first start: intiface central

	if (!connect_to_hismith())
		return;

	//-----------------------------------------------------
	// Connecting to Web Camera	

	cv::Mat frame, prev_frame, res_frame;
	int video_dev_id = get_video_dev_id();
	if (video_dev_id == -1)
		return;
	cv::VideoCapture capture(video_dev_id);

	if (capture.isOpened())
	{
		capture.set(cv::CAP_PROP_FRAME_WIDTH, g_webcam_frame_width);
		capture.set(cv::CAP_PROP_FRAME_HEIGHT, g_webcam_frame_height);
		capture.set(cv::CAP_PROP_BUFFERSIZE, 1);

		// geting first 30 frames for get better camera focus
		for (int i = 0; i < 30; i++)
		{
			get_new_camera_frame(capture, frame/*, prev_frame*/);
		}
		last_msec_video_prev_pos = msec_video_cur_pos = capture.get(cv::CAP_PROP_POS_MSEC);
		if (!get_hismith_pos_by_image(frame, cur_pos))
		{
			capture.release();
			return;
		}
		abs_cur_pos = get_abs_pos(cur_pos);
	}

	//-----------------------------------------------------
	// Moving Hismith and checking get_hismith_pos_by_image

	if (capture.isOpened())
	{
		hismith_speed = (int)(set_hithmith_speed((double)hismith_speed/ 100.0)*100.0);

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
				capture.release();
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

	add_xml_element(document, root, "max_allowed_hismith_speed", QString::number(g_max_allowed_hismith_speed));
	add_xml_element(document, root, "min_funscript_relative_move", QString::number(g_min_funscript_relative_move));
	add_xml_element(document, root, "use_modify_funscript_functions", QString::number(g_modify_funscript ? 1 : 0));
	add_xml_element(document, root, "functions_move_variants", g_modify_funscript_function_move_variants);
	add_xml_element(document, root, "functions_move_in", g_modify_funscript_function_move_in_variants);
	add_xml_element(document, root, "functions_move_out", g_modify_funscript_function_move_out_variants);
	add_xml_element(document, root, "dt_for_get_cur_speed", QString::number(g_dt_for_get_cur_speed));
	add_xml_element(document, root, "increase_hismith_speed_start_multiplier", QString::number(g_increase_hismith_speed_start_multiplier));
	add_xml_element(document, root, "slowdown_hismith_speed_start_multiplier", QString::number(g_slowdown_hismith_speed_start_multiplier));
	add_xml_element(document, root, "increase_hismith_speed_change_multiplier", QString::number(g_increase_hismith_speed_change_multiplier));
	add_xml_element(document, root, "slowdown_hismith_speed_change_multiplier", QString::number(g_slowdown_hismith_speed_change_multiplier));
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

	QString selected_webcam = pW->ui->Webcams->itemText(pW->ui->Webcams->currentIndex());
	QString selected_device = pW->ui->Devices->itemText(pW->ui->Devices->currentIndex());

	add_xml_element(document, root, "req_webcam_name", selected_webcam);
	add_xml_element(document, root, "webcam_frame_width", QString::number(g_webcam_frame_width));
	add_xml_element(document, root, "webcam_frame_height", QString::number(g_webcam_frame_height));

	add_xml_element(document, root, "intiface_central_client_url", g_intiface_central_client_url.c_str());
	add_xml_element(document, root, "intiface_central_client_port", QString::number(g_intiface_central_client_port));
	add_xml_element(document, root, "hismith_device_name", selected_device);

	add_xml_element(document, root, "vlc_url", g_vlc_url);
	add_xml_element(document, root, "vlc_port", QString::number(g_vlc_port));
	add_xml_element(document, root, "vlc_password", g_vlc_password);

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
	if (!doc.setContent(&xmlFile)) {
		xmlFile.close();
		return res;
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
	g_min_funscript_relative_move = data_map["min_funscript_relative_move"].toInt();
	g_dt_for_get_cur_speed = data_map["dt_for_get_cur_speed"].toInt();
	g_increase_hismith_speed_start_multiplier = data_map["increase_hismith_speed_start_multiplier"].toDouble();
	g_slowdown_hismith_speed_start_multiplier = data_map["slowdown_hismith_speed_start_multiplier"].toDouble();
	g_increase_hismith_speed_change_multiplier = data_map["increase_hismith_speed_change_multiplier"].toDouble();
	g_slowdown_hismith_speed_change_multiplier = data_map["slowdown_hismith_speed_change_multiplier"].toDouble();
	g_speed_change_delay = data_map["speed_change_delay"].toInt();
	g_cpu_freezes_timeout = data_map["cpu_freezes_timeout"].toInt();

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

	g_intiface_central_client_url = data_map["intiface_central_client_url"].toStdString();
	g_intiface_central_client_port = data_map["intiface_central_client_port"].toInt();

	g_hismith_device_name = data_map["hismith_device_name"];

	g_vlc_url = data_map["vlc_url"];
	g_vlc_port = data_map["vlc_port"].toInt();
	g_vlc_password = data_map["vlc_password"];

	g_modify_funscript = (data_map["use_modify_funscript_functions"].toInt() == 0) ? false : true;
	g_modify_funscript_function_move_variants = data_map["functions_move_variants"];
	g_modify_funscript_function_move_in_variants = data_map["functions_move_in"];
	g_modify_funscript_function_move_out_variants = data_map["functions_move_out"];

	//------------------------------------------------------------------------------------------------

	pW->ui->speedLimit->setText(QString::number(g_max_allowed_hismith_speed));
	pW->ui->minRelativeMove->setText(QString::number(g_min_funscript_relative_move));

	pW->ui->modifyFunscript->setChecked(g_modify_funscript);
	pW->ui->functionsMoveVariants->setText(g_modify_funscript_function_move_variants);
	pW->ui->functionsMoveIn->setText(g_modify_funscript_function_move_in_variants);
	pW->ui->functionsMoveOut->setText(g_modify_funscript_function_move_out_variants);

	//--------------------

	DeviceEnumerator de;
	std::map<int, InputDevice> devices = de.getVideoDevicesMap();
	for (auto const& device : devices) {
		pW->ui->Webcams->addItem(device.second.deviceName.c_str());
		if (QString(device.second.deviceName.c_str()).contains(g_req_webcam_name))
		{
			pW->ui->Webcams->setCurrentIndex(pW->ui->Webcams->count() - 1);
		}
	}
	//--------------------

	//--------------------
	get_devices_list();

	for (DeviceClass& dev : g_myDevices)
	{
		pW->ui->Devices->addItem(dev.deviceName.c_str());
		if (QString(dev.deviceName.c_str()).contains(g_hismith_device_name))
		{
			pW->ui->Devices->setCurrentIndex(pW->ui->Devices->count() - 1);
		}
	}
	//--------------------

	res = true;

	return res;
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
	g_root_dir = a.applicationDirPath();
	
	{
		QFile file(g_root_dir + "\\res_data\\!results.txt");
		file.resize(0);
		file.close();
	}

	{
		QFile file(g_root_dir + "\\res_data\\!results_for_get_parsed_funscript_data.txt");
		file.resize(0);
		file.close();
	}	

    MainWindow w;
	pW = &w;
    w.show();

	if (!LoadSettings())
	{
		return 0;
	}

	//get_statistics_with_hismith();

	//get_performance_with_hismith(5);

	//test_camera();

	//test_by_video();
	//test_err_frame(g_root_dir + "\\error_data\\01_08_2024_23_03_20_frame_orig.bmp");

	//test_hismith(5);

	//get_delay_data();	

	//cv::destroyAllWindows();

	//return 0;

    return a.exec();
}
