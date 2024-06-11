
#include "mainwindow.h"
#include "./ui_mainwindow.h"


//---------------------------------------------------------------

int g_max_allowed_hismith_speed;
int g_min_funscript_relative_move;

int g_dt_for_get_cur_speed;

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

//---------------------------------------------------------------

void save_BGR_image(cv::Mat &frame, QString fpath)
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

void get_new_camera_frame(cv::VideoCapture &capture, cv::Mat& frame)
{
	if (!capture.read(frame))
	{
		error_msg(QString("ERROR: capture.read(frame) failed"));
	}
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
		
		show_frame_in_cv_window("w", p_draw_frame);
		cv::waitKey(0);
	}
	else
	{
		MessageBoxPos(NULL, msg.toStdWString().c_str(), L"Error", MB_OK | MB_SETFOREGROUND | MB_SYSTEMMODAL | MB_ICONERROR);
	}
	cv::destroyAllWindows();	
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


void callbackFunction(const mhl::Messages msg) {
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

bool get_hismith_pos_by_image(cv::Mat& frame, int& pos, bool show_results = false, cv::Mat *p_res_frame = NULL, double *p_cur_speed = NULL, int* p_dt_get_speed = NULL, QString add_data = QString())
{
	bool res = false;
	cv::Mat img, img_b, img_g, img_right;
	custom_buffer<CMyClosedFigure> figures_b;
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
		[&img, &img_b, &figures_b, width, height] {
			get_binary_image(img, g_B_range, img_b, 3);
			simple_buffer<u8> Im(width * height);
			GreyscaleMatToImage(img_b, width, height, Im);
			SearchClosedFigures(Im, width, height, (u8)255, figures_b);
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

	double max_rad_prop = 8.0 / 23;

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

	for (int id = 0; id < figures_b.m_size; id++)
	{
		CMyClosedFigure* pFigure = &(figures_b[id]);
		int x = pFigure->m_minX, y = pFigure->m_minY, w = pFigure->width(), h = pFigure->height();
		int size = pFigure->m_PointsArray.m_size;

		if (((x + w) < (2 * width) / 3) && (h > 2 * w) && (h > (2 * height) / 15) && ((y + h / 2) < (3 * height) / 4) && (y + ((2 * h) / 3) >= height / 4))
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
		error_msg("ERROR: Failed to find big left vertical border blue color figure", &frame, &frame_upd, NULL, 0, height / 4, (2 * width) / 3, (3 * height) / 4);
		return res;
	}

	for (int id = 0; id < figures_b.m_size; id++)
	{
		CMyClosedFigure* pFigure = &(figures_b[id]);
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
		error_msg("ERROR: Failed to find big right blue color figure", &frame, &frame_upd, NULL, l_x + ((3 * l_h) / 2), l_y - (l_h / 2), width, l_y + ((4 * l_h) / 2));
		return res;
	}

	int min_cw = 5;
	int min_sy = min(l_y + (l_h / 4), r_y);
	int max_sy = max(l_y + ((3 * l_h) / 4), r_y + r_h);
	std::vector<CMyClosedFigure*> c_figures;

	for (int id = 0; id < figures_b.m_size; id++)
	{
		CMyClosedFigure* pFigure = &(figures_b[id]);
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
	double exp_c_cx_ratio = (double)225.0 / 830.0;
	int exp_c_cx = l_cx + (int)((double)(r_cx - l_cx) * exp_c_cx_ratio);
	int c_cx;
	int c_cy;

	if (c_x == -1)
	{
		c_cx = exp_c_cx;
		c_w = max(r_w, r_h);
		c_h = c_w;
		c_x = c_cx - (c_w / 2);
		c_cy = l_cy + (((r_cy - l_cy) * (c_cx - l_cx)) / (r_cx - l_cx));
		c_y = c_cy - (c_h / 2);
	}
	else
	{
		c_cx = c_x + (c_w / 2);
		c_cy = l_cy + (((r_cy - l_cy) * (c_cx - l_cx)) / (r_cx - l_cx));
		c_h = max((c_cy - c_y) * 2, (c_y + c_h - 1 - c_cy) * 2);
		c_y = c_cy - (c_h / 2);

		double c_cx_ratio = (double)(c_cx - l_cx) / (double)(r_cx - l_cx);
		double diff = (abs(c_cx_ratio - exp_c_cx_ratio) * 100.0) / exp_c_cx_ratio;

		if (diff > 30)
		{
			cv::Mat frame_upd;
			frame.copyTo(frame_upd);
			frame_upd.setTo(cv::Scalar(255, 0, 0), img_b);
			error_msg("ERROR: got strange center position", &frame, &frame_upd, NULL, c_x, c_y, c_x + c_w - 1, c_y + c_h - 1);
		}
	}

	for (int id = 0; id < figures_g.m_size; id++)
	{
		CMyClosedFigure* pFigure = &(figures_g[id]);
		int x = pFigure->m_minX, y = pFigure->m_minY, w = pFigure->width(), h = pFigure->height();
		int size = pFigure->m_PointsArray.m_size;

		int g_to_c_distance_pow2 = pow2((x + (w / 2)) - c_cx) + pow2((y + (h / 2)) - c_cy);
		int g_to_r_distance_pow2 = pow2((x + (w / 2)) - r_cx) + pow2((y + (h / 2)) - r_cy);
		if ((double)g_to_c_distance_pow2 / g_to_r_distance_pow2 <= max_rad_prop * max_rad_prop)
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

	if (!p_best_match_g_figure)
	{
		cv::Mat frame_upd;
		frame.copyTo(frame_upd);
		frame_upd.setTo(cv::Scalar(255, 0, 0), img_g);
		error_msg("ERROR: Failed to find green color figure", &frame, &frame_upd, NULL, l_x, min(l_y, r_y), r_x, min(l_y + l_h, r_y + r_h));
		return res;
	}

	int g_cx = g_x + (g_w / 2), g_cy = g_y + (g_h / 2);

	double g_to_c_distance = sqrt((double)(pow2(g_cx - c_cx) + pow2(g_cy - c_cy)));
	double r_to_c_distance = sqrt((double)(pow2(r_cx - c_cx) + pow2(r_cy - c_cy)));

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

		int cur_speed = -1;
		int dt_get_speed = -1;
		if (p_cur_speed)
		{
			cur_speed = *p_cur_speed;
		}
		if (p_dt_get_speed)
		{
			dt_get_speed = *p_dt_get_speed;
		}

		double g_to_r_distance = (int)sqrt((double)(pow2(g_cx - r_cx) + pow2(g_cy - r_cy)));
		cv::String text = cv::format("Press 'Esc' for stop to show\n%s" "pos: %d, g_to_c_distance: %f\ng_to_r_distance: %f, r_to_c_distance: %f dt: %d dt_c: %d cur_speed: %d dt_get_speed: %d", add_data.toStdString().c_str(), pos, g_to_c_distance, g_to_r_distance, r_to_c_distance, dt, dt1, cur_speed, dt_get_speed);

		draw_text(text.c_str(), img_res);

		if (p_res_frame)
		{
			img_res.copyTo(*p_res_frame);
		}

		show_frame_in_cv_window("w", &img_res);
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

void get_performance()
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

		auto t = std::time(nullptr);
		auto tm = *std::localtime(&t);
		std::ostringstream oss;
		oss << std::put_time(&tm, "%d_%m_%Y_%H_%M_%S");
		QString time_str = oss.str().c_str();
		save_BGR_image(bad_frame, g_root_dir + "\\error_data\\" + time_str + QString("_slow_frame_orig_dt_%1.bmp").arg(max_dt));

		show_msg(QString("%1_%2").arg((double)1000.0 / ((double)(GetTickCount64() - start_time) / cnt)).arg(max_dt), 10000);
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

void get_initial_data()
{
	cv::Mat frame;
	
	std::vector<int> speeds{5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};

	QFile xmlFile(g_root_dir + "\\data\\data.xml");
	if (!xmlFile.open(QFile::WriteOnly | QFile::Text))
	{
		xmlFile.close();
		error_msg("ERROR: data.xml file already opened or there is another issue");
	}
	QTextStream xmlContent(&xmlFile);

	QDomDocument document;

	QDomElement root = document.createElement("speeds_data_list");
	document.appendChild(root);

	const int rel_loc_start[8] = {-45, 45, 90 + 45, 180 + 45, 270 + 45, 360 + 45, 360 + 90 + 45, 360 + 180 + 45 };

	for (int speed : speeds)
	{
		cv::String video_file_name = cv::format((g_root_dir.toStdString() + "\\data\\speed_%d.mp4").c_str(), speed);
		cv::VideoCapture capture(video_file_name);

		std::list<QPair<double, double>> speed_data[4];
		int speed_data_average[4];
		int speed_data_total_average = 0;

		if (capture.isOpened())
		{
			if (!capture.read(frame))
			{
				error_msg(cv::format("ERROR: Failed to read first frame from video: %s", video_file_name.c_str()).c_str());
			}

			int res, cur_pos, cur_rel_pos, prev_pos, start_pos;
			int num_rotations, prev_loc, cur_loc, cur_rel_loc, prev_rel_loc;
			double cur_speed, dpos, dt;
			__int64 msec_video_cur_pos, msec_video_prev_pos, msec_video_start_pos;
			
			msec_video_start_pos = capture.get(cv::CAP_PROP_POS_MSEC);
			if (!get_hismith_pos_by_image(frame, start_pos))
			{
				capture.release();
				return;
			}

			msec_video_prev_pos = msec_video_cur_pos = msec_video_start_pos;
			prev_pos = cur_pos = start_pos;

			prev_loc = 0;
			if ((prev_pos >= 45) && (prev_pos < (90 + 45)))
			{
				prev_loc = 1;
			}
			else if ((prev_pos >= (90 + 45)) && (prev_pos < (180 + 45)))
			{
				prev_loc = 2;
			}
			else if ((prev_pos >= (180 + 45)) && (prev_pos < (270 + 45)))
			{
				prev_loc = 3;
			}
			num_rotations = 0;

			while (capture.isOpened() && capture.read(frame))
			{
				msec_video_cur_pos = capture.get(cv::CAP_PROP_POS_MSEC);
				if (!get_hismith_pos_by_image(frame, cur_pos))
				{
					capture.release();
					return;
				}

				cur_loc = 0;
				if ((cur_pos >= 45) && (cur_pos < (90 + 45)))
				{
					cur_loc = 1;
				}
				else if ((cur_pos >= (90 + 45)) && (cur_pos < (180 + 45)))
				{
					cur_loc = 2;
				}
				else if ((cur_pos >= (180 + 45)) && (cur_pos < (270 + 45)))
				{
					cur_loc = 3;
				}
				
				cur_rel_pos = cur_pos;
				cur_rel_loc = cur_loc;
				prev_rel_loc = prev_loc;
				if (cur_pos < prev_pos)
				{
					num_rotations++;
				}
				if ( (cur_pos < prev_pos) || (cur_loc < prev_loc) )
				{
					if (cur_loc == 0)
					{
						if (cur_pos < rel_loc_start[4])
						{
							cur_rel_pos = cur_pos + 360;
						}						
						else
						{
							cur_rel_pos = cur_rel_pos;
						}
					}
					else
					{
						cur_rel_pos = cur_pos + 360;
					}
					cur_rel_loc = 4 + cur_loc;

					if (prev_loc == 0)
					{
						prev_rel_loc = 4;
					}
				}

				dt = (double)(msec_video_cur_pos - msec_video_prev_pos);
				cur_speed = (double)((cur_rel_pos - prev_pos) * 1000) / dt;

				if (cur_loc != prev_loc)
				{	
					dpos = cur_rel_pos - rel_loc_start[cur_rel_loc];
					dt = (double)(dpos * 1000) / cur_speed;
					speed_data[cur_loc].push_back(QPair<double, double>(dpos, dt));

					if (prev_rel_loc == 4)
					{
						prev_rel_loc = prev_rel_loc;
					}

					if ( (dpos < 0) || (dpos > 90) )
					{
						error_msg(cv::format("ERROR: Got wrond dpos: %f for cur_loc for frame: %s from video: %s", dpos, VideoTimeToStr(msec_video_cur_pos).c_str(), video_file_name.c_str()).c_str());
					}

					dpos = rel_loc_start[prev_rel_loc + 1] - prev_pos;
					dt = (double)(dpos * 1000) / cur_speed;
					speed_data[prev_loc].push_back(QPair<double, double>(dpos, dt));

					if ((dpos < 0) || (dpos > 90))
					{
						error_msg(cv::format("ERROR: Got wrond dpos: %f for prev_loc for frame: %s from video: %s", dpos, VideoTimeToStr(msec_video_cur_pos).c_str(), video_file_name.c_str()).c_str());
					}

					if (cur_rel_loc < prev_rel_loc)
					{
						error_msg(cv::format("ERROR: Got cur_rel_loc < prev_rel_loc for frame: %s from video: %s", VideoTimeToStr(msec_video_cur_pos).c_str(), video_file_name.c_str()).c_str());
					}

					for (int rel_loc = prev_rel_loc + 1; rel_loc < cur_rel_loc; rel_loc++)
					{
						int loc = (rel_loc < 4) ? rel_loc : rel_loc - 4;
						dpos = 90;
						dt = (double)(dpos * 1000) / cur_speed;
						speed_data[loc].push_back(QPair<double, double>(dpos, dt));
					}					
				}
				else
				{
					dpos = cur_rel_pos - prev_pos;
					dt = (double)(dpos * 1000) / cur_speed;
					speed_data[cur_loc].push_back(QPair<double, double>(dpos, dt));

					if ((dpos < 0) || (dpos > 90))
					{
						error_msg(cv::format("ERROR: Got wrond dpos: %f for prev_loc==cur_loc for frame: %s from video: %s", dpos, VideoTimeToStr(msec_video_cur_pos).c_str(), video_file_name.c_str()).c_str());
					}
				}

				prev_loc = cur_loc;
				prev_pos = cur_pos;
				msec_video_prev_pos = msec_video_cur_pos;
			}

			speed_data_total_average = ((cur_pos - start_pos + (num_rotations * 360)) * 1000) / (msec_video_cur_pos - msec_video_start_pos);
		}
		else
		{
			error_msg(cv::format("ERROR: Failed to open video: %s", video_file_name.c_str()).c_str());
		}

		if (speed_data_total_average == 0)
		{
			error_msg(cv::format("ERROR: Got 0 speed_data_total_average for video: %s", video_file_name.c_str()).c_str());
		}

		for (int loc = 0; loc < 4; loc++)
		{
			double total_dpos = 0;
			double total_dt = 0;
			for (QPair<double, double> &data : speed_data[loc])
			{
				total_dpos += data.first;
				total_dt += data.second;
			}

			speed_data_average[loc] = (total_dpos * 1000.0) / total_dt;
		}

		QDomElement speed_data_average_data = document.createElement("speed_data_average");
		speed_data_average_data.setAttribute("hismith_speed", speed);
		speed_data_average_data.setAttribute(QString("rotation_speed_total_average"), speed_data_total_average);
		for (int loc = 0; loc < 4; loc++)
		{
			speed_data_average_data.setAttribute(QString("rotation_speed_average_%1").arg(loc), speed_data_average[loc]);
		}
		root.appendChild(speed_data_average_data);

		capture.release();
	}

	xmlContent << document.toString();
	xmlFile.flush();
	xmlFile.close();
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
	
	while (1)
	{
		int width = frame.cols;
		int height = frame.rows;

		cv::Mat img, img_b, img_g, img_intersection;
				
		cv::cvtColor(frame, img, cv::COLOR_BGR2YUV);

		concurrency::parallel_invoke(
			[&img, &img_b, width, height] {
				get_binary_image(img, g_B_range, img_b, 3);
			},
			[&img, &img_g, width, height] {
				get_binary_image(img, g_G_range, img_g, 3);
			}
			);

		frame.copyTo(img);

		img.setTo(cv::Scalar(255, 0, 0), img_b);
		img.setTo(cv::Scalar(0, 255, 0), img_g);

		cv::bitwise_and(img_b, img_g, img_intersection);
		img.setTo(cv::Scalar(0, 0, 255), img_intersection);

		draw_text(QString("b[%1-%2][%3-%4][%5-%6] g[%7-%8][%9-%10][%11-%12]")
			.arg(g_B_range[0][0])
			.arg(g_B_range[0][1])
			.arg(g_B_range[1][0])
			.arg(g_B_range[1][1])
			.arg(g_B_range[2][0])
			.arg(g_B_range[2][1])
			.arg(g_G_range[0][0])
			.arg(g_G_range[0][1])
			.arg(g_G_range[1][0])
			.arg(g_G_range[1][1])
			.arg(g_G_range[2][0])
			.arg(g_G_range[2][1])
			, img);
		show_frame_in_cv_window("w", &img);

		int key = cv::waitKey(0);

		if (key == Qt::Key_Escape)
			break;

		else if (key == 'w')
		{
			g_B_range[0][0] = min(g_B_range[0][0] + 1, 255);
		}
		else if (key == 'q')
		{
			g_B_range[0][0] = max(g_B_range[0][0] - 1, 0);
		}
		else if (key == 'r')
		{
			g_B_range[0][1] = min(g_B_range[0][1] + 1, 255);
		}
		else if (key == 'e')
		{
			g_B_range[0][1] = max(g_B_range[0][1] - 1, 0);
		}

		else if (key == 's')
		{
			g_B_range[1][0] = min(g_B_range[1][0] + 1, 255);
		}
		else if (key == 'a')
		{
			g_B_range[1][0] = max(g_B_range[1][0] - 1, 0);
		}
		else if (key == 'f')
		{
			g_B_range[1][1] = min(g_B_range[1][1] + 1, 255);
		}
		else if (key == 'd')
		{
			g_B_range[1][1] = max(g_B_range[1][1] - 1, 0);
		}

		else if (key == 'x')
		{
			g_B_range[2][0] = min(g_B_range[2][0] + 1, 255);
		}
		else if (key == 'z')
		{
			g_B_range[2][0] = max(g_B_range[2][0] - 1, 0);
		}
		else if (key == 'v')
		{
			g_B_range[2][1] = min(g_B_range[2][1] + 1, 255);
		}
		else if (key == 'c')
		{
			g_B_range[2][1] = max(g_B_range[2][1] - 1, 0);
		}

		//-----------------------

		else if (key == 'y')
		{
			g_G_range[0][0] = min(g_G_range[0][0] + 1, 255);
		}
		else if (key == 't')
		{
			g_G_range[0][0] = max(g_G_range[0][0] - 1, 0);
		}
		else if (key == 'i')
		{
			g_G_range[0][1] = min(g_G_range[0][1] + 1, 255);
		}
		else if (key == 'u')
		{
			g_G_range[0][1] = max(g_G_range[0][1] - 1, 0);
		}

		else if (key == 'h')
		{
			g_G_range[1][0] = min(g_G_range[1][0] + 1, 255);
		}
		else if (key == 'g')
		{
			g_G_range[1][0] = max(g_G_range[1][0] - 1, 0);
		}
		else if (key == 'k')
		{
			g_G_range[1][1] = min(g_G_range[1][1] + 1, 255);
		}
		else if (key == 'j')
		{
			g_G_range[1][1] = max(g_G_range[1][1] - 1, 0);
		}

		else if (key == 'n')
		{
			g_G_range[2][0] = min(g_G_range[2][0] + 1, 255);
		}
		else if (key == 'b')
		{
			g_G_range[2][0] = max(g_G_range[2][0] - 1, 0);
		}
		else if (key == ',')
		{
			g_G_range[2][1] = min(g_G_range[2][1] + 1, 255);
		}
		else if (key == 'm')
		{
			g_G_range[2][1] = max(g_G_range[2][1] - 1, 0);
		}
	}
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

		capture.set(cv::CAP_PROP_FRAME_WIDTH, g_webcam_frame_width);
		capture.set(cv::CAP_PROP_FRAME_HEIGHT, g_webcam_frame_height);

		cv::namedWindow("w", 1);

		while (capture.read(frame))
		{
			int width = frame.cols;
			int height = frame.rows;

			cv::Mat img, img_b, img_g, img_intersection;
			cv::cvtColor(frame, img, cv::COLOR_BGR2YUV);

			concurrency::parallel_invoke(
				[&img, &img_b, width, height] {
					get_binary_image(img, g_B_range, img_b, 3);
				},
				[&img, &img_g, width, height] {
					get_binary_image(img, g_G_range, img_g, 3);
				}
			);

			frame.copyTo(img);

			img.setTo(cv::Scalar(255, 0, 0), img_b);
			img.setTo(cv::Scalar(0, 255, 0), img_g);

			cv::bitwise_and(img_b, img_g, img_intersection);
			img.setTo(cv::Scalar(0, 0, 255), img_intersection);

			draw_text(QString("Press 'Esc' for stop to show\nCurent colors: b[%1-%2][%3-%4][%5-%6] | g[%7-%8][%9-%10][%11-%12]\nPress b[q/w/e/r][a/s/d/f][z/x/c/v] | g[t/y/u/i][g/h/j/k][b/n/m/,] for change colors")
				.arg(g_B_range[0][0])
				.arg(g_B_range[0][1])
				.arg(g_B_range[1][0])
				.arg(g_B_range[1][1])
				.arg(g_B_range[2][0])
				.arg(g_B_range[2][1])
				.arg(g_G_range[0][0])
				.arg(g_G_range[0][1])
				.arg(g_G_range[1][0])
				.arg(g_G_range[1][1])
				.arg(g_G_range[2][0])
				.arg(g_G_range[2][1])
				, img);
			show_frame_in_cv_window("w", &img);

			int key = cv::waitKey(1);

			if (key == 27) // Esc key to stop
				break;

			else if (key == 'w')
			{
				g_B_range[0][0] = min(g_B_range[0][0] + 1, 255);
			}
			else if (key == 'q')
			{
				g_B_range[0][0] = max(g_B_range[0][0] - 1, 0);
			}
			else if (key == 'r')
			{
				g_B_range[0][1] = min(g_B_range[0][1] + 1, 255);
			}
			else if (key == 'e')
			{
				g_B_range[0][1] = max(g_B_range[0][1] - 1, 0);
			}

			else if (key == 's')
			{
				g_B_range[1][0] = min(g_B_range[1][0] + 1, 255);
			}
			else if (key == 'a')
			{
				g_B_range[1][0] = max(g_B_range[1][0] - 1, 0);
			}
			else if (key == 'f')
			{
				g_B_range[1][1] = min(g_B_range[1][1] + 1, 255);
			}
			else if (key == 'd')
			{
				g_B_range[1][1] = max(g_B_range[1][1] - 1, 0);
			}

			else if (key == 'x')
			{
				g_B_range[2][0] = min(g_B_range[2][0] + 1, 255);
			}
			else if (key == 'z')
			{
				g_B_range[2][0] = max(g_B_range[2][0] - 1, 0);
			}
			else if (key == 'v')
			{
				g_B_range[2][1] = min(g_B_range[2][1] + 1, 255);
			}
			else if (key == 'c')
			{
				g_B_range[2][1] = max(g_B_range[2][1] - 1, 0);
			}
			
			//-----------------------

			else if (key == 'y')
			{
				g_G_range[0][0] = min(g_G_range[0][0] + 1, 255);
			}
			else if (key == 't')
			{
				g_G_range[0][0] = max(g_G_range[0][0] - 1, 0);
			}
			else if (key == 'i')
			{
				g_G_range[0][1] = min(g_G_range[0][1] + 1, 255);
			}
			else if (key == 'u')
			{
				g_G_range[0][1] = max(g_G_range[0][1] - 1, 0);
			}

			else if (key == 'h')
			{
				g_G_range[1][0] = min(g_G_range[1][0] + 1, 255);
			}
			else if (key == 'g')
			{
				g_G_range[1][0] = max(g_G_range[1][0] - 1, 0);
			}
			else if (key == 'k')
			{
				g_G_range[1][1] = min(g_G_range[1][1] + 1, 255);
			}
			else if (key == 'j')
			{
				g_G_range[1][1] = max(g_G_range[1][1] - 1, 0);
			}

			else if (key == 'n')
			{
				g_G_range[2][0] = min(g_G_range[2][0] + 1, 255);
			}
			else if (key == 'b')
			{
				g_G_range[2][0] = max(g_G_range[2][0] - 1, 0);
			}
			else if (key == ',')
			{
				g_G_range[2][1] = min(g_G_range[2][1] + 1, 255);
			}
			else if (key == 'm')
			{
				g_G_range[2][1] = max(g_G_range[2][1] - 1, 0);
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

bool get_parsed_funscript_data(QString funscript_fname, std::vector<QPair<int, int>>& funscript_data_maped)
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
	const int size = actions.size();
	std::vector<QPair<int, int>> funscript_data(size);
	funscript_data_maped.resize(size);

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

		funscript_data[id].first = at;
		funscript_data[id].second = 100 - pos;
		if (!((pos >= 0) && (pos <= 100)))
		{
			show_msg(QString("ERROR: Strange pose:%1 in funscript file: %2 it should be in range 0-100").arg(pos).arg(funscript_fname));
			return res;
		}
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

							for (int i = id1; i <= id2; i++)
							{
								result_details += QString("%1").arg(funscript_data[i].second);
								if (i < id2)
								{
									result_details += " | ";
								}
							}

							result_details += QString("\n%1 | ").arg(funscript_data[id1].second);
							for (int i = id1 + 1; i <= id2; i++)
							{
								int res_inv_pos = funscript_data[id1].second + ((i_move[i - (id1 + 1)] * rel_move) / total_move);
								result_details += QString("%1").arg(res_inv_pos);
								if (i < id2)
								{
									result_details += " | ";
								}
							}

							result_details += QString("\n[at:%1][inv_pos:%2] | ").arg(funscript_data[id1].first).arg(funscript_data[id1].second);
							for (int i = id1 + 1; i <= id2; i++)
							{
								int res_inv_pos = funscript_data[id1].second + ((i_move[i - (id1 + 1)] * rel_move) / total_move);

								if ((res_inv_pos > 100) || (res_inv_pos < 0))
								{
									show_msg("ERROR: unexpected case (res_inv_pos > 100) || (res_inv_pos < 0)");
								}

								result_details += QString("[at:%1][inv_pos:%2][res_inv_pos:%3]").arg(funscript_data[i].first).arg(funscript_data[i].second).arg(res_inv_pos);
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

			for (int i = prev_top_end_id + 1; i <= id; i++)
			{
				double rel_move = ((double)(funscript_data[i].second - min_pos) * 100.0) / (double)(max_pos - min_pos);
				int dpos = rel_move_to_dpos(rel_move);
				funscript_data_maped[i].first = funscript_data[i].first;
				funscript_data_maped[i].second = funscript_data_maped[prev_top_end_id].second + dpos;
			}
		}
	}

	if (result_details.size() > 0)
	{
		save_text_to_file(g_root_dir + "\\res_data\\!results_for_get_parsed_funscript_data.txt",
			"File path: " + funscript_fname + "\n----------\n" + result_details + "\n----------\n\n",
			QFile::WriteOnly | QFile::Text);

		QString result_funscript = "{\"actions\":[";
		for (id = 0; id < size; id++)
		{
			result_funscript += QString("{\"at\":%1,\"pos\":%1}").arg(funscript_data[id].first).arg(funscript_data[id].second);
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

struct SpeedData
{
	int speed_data_average[4];
	int speed_data_total_average;
};

void get_average_speed_data(std::map<int, SpeedData> &speed_data_map)
{
	QDomDocument doc("data");
	QFile xmlFile(g_root_dir + "\\data\\data.xml");
	if (!xmlFile.open(QIODevice::ReadOnly))
		return;
	if (!doc.setContent(&xmlFile)) {
		xmlFile.close();
		return;
	}
	xmlFile.close();

	QDomElement docElem = doc.documentElement();	

	QDomNode n = docElem.firstChild();
	while (!n.isNull()) {
		QDomElement e = n.toElement(); // try to convert the node to an element.
		if (!e.isNull()) {
			QString tag_name = e.tagName();

			if (tag_name == "speed_data_average")
			{
				int speed = e.attribute("hismith_speed").toInt();
				SpeedData speed_data;
				speed_data.speed_data_total_average = e.attribute("rotation_speed_total_average").toInt();
				speed_data.speed_data_average[0] = e.attribute("rotation_speed_average_0").toInt();
				speed_data.speed_data_average[1] = e.attribute("rotation_speed_average_1").toInt();
				speed_data.speed_data_average[2] = e.attribute("rotation_speed_average_2").toInt();
				speed_data.speed_data_average[3] = e.attribute("rotation_speed_average_3").toInt();
				speed_data_map[speed] = speed_data;
			}
		}
		n = n.nextSibling();
	}
}

int get_optimal_hismith_speed(std::map<int, SpeedData> &speed_data_map, int req_speed, int end_pos, int cur_pos)
{
	const int N = 11;
	int speed_keys[N] = {5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
	int res = 100, prev_speed_data_total_average, prev_speed, cur_speed, cur_speed_data_total_average;

	if (req_speed <= 0)
		return 0;
	
	//if (end_pos - cur_pos > 360 - 90)
	{
		prev_speed_data_total_average = 0;
		prev_speed = 0;
		for (int i = 0; i < N; i++)
		{
			cur_speed = speed_keys[i];
			cur_speed_data_total_average = speed_data_map[cur_speed].speed_data_total_average;
			if (req_speed <= cur_speed_data_total_average)
			{
				if (i > 0)
				{
					prev_speed = speed_keys[i - 1];
					prev_speed_data_total_average = speed_data_map[prev_speed].speed_data_total_average;

				}

				res = prev_speed + (((req_speed - prev_speed_data_total_average) * (cur_speed - prev_speed)) / (cur_speed_data_total_average - prev_speed_data_total_average));
				break;
			}
		}
	}
	//else
	//{
	//	int cur_loc = get_loc(cur_pos);
	//	int end_loc = get_loc(end_pos);
	//	int loc, locs_n, locs[4], sub_res[4];

	//	locs_n = 1;
	//	loc = cur_loc;
	//	locs[0] = cur_loc;

	//	while (loc != end_loc)
	//	{
	//		loc = (loc + 1) % 4;
	//		locs[locs_n] = loc;			
	//		locs_n++;
	//	}

	//	res = 0;
	//	for (int loc_i = 0; loc_i < locs_n; loc_i++)
	//	{
	//		loc = locs[loc_i];
	//		prev_speed_data_total_average = 0;
	//		prev_speed = 0;

	//		for (int i = 0; i < N; i++)
	//		{
	//			cur_speed = speed_keys[i];
	//			cur_speed_data_total_average = speed_data_map[cur_speed].speed_data_average[loc];
	//			if (req_speed <= cur_speed_data_total_average)
	//			{
	//				if (i > 0)
	//				{
	//					prev_speed = speed_keys[i - 1];
	//					prev_speed_data_total_average = speed_data_map[prev_speed].speed_data_average[loc];
	//				}

	//				sub_res[loc_i] = prev_speed + (((req_speed - prev_speed_data_total_average) * (cur_speed - prev_speed)) / (cur_speed_data_total_average - prev_speed_data_total_average));
	//				res += sub_res[loc_i];
	//				break;
	//			}
	//		}
	//	}

	//	res = res / locs_n;
	//}

	if (res > g_max_allowed_hismith_speed)
	{
		res = g_max_allowed_hismith_speed;
	}

	return res;
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
	__int64& msec_video_prev_pos, int& abs_prev_pos, bool show_results = false, cv::Mat* p_res_frame = NULL, QString add_data = QString())
{
	int prev_pos, res, dpos;
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
	if (!get_hismith_pos_by_image(frame, cur_pos, show_results, p_res_frame, &cur_speed, &dt, add_data))
	{
		return false;
	}
	dpos = update_abs_pos(cur_pos, prev_pos, abs_cur_pos, frame/*, prev_frame*/, cur_speed);

	if (msec_video_prev_pos != -1)
	{
		_tmp_msec_video_prev_poss[_num_prev_poss] = msec_video_cur_pos;
		_tmp_abs_prev_poss[_num_prev_poss] = abs_cur_pos;
		_num_prev_poss++;

		cur_speed = (double)((abs_cur_pos - abs_prev_pos) * 1000.0) / (double)dt;

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

		HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
		FillRect(hdc, &rc, brush);

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
	int abs_cur_pos = 0, abs_prev_pos = 0;
	int dpos;
	double cur_speed, prev_cur_speed = 0;
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

		get_new_camera_frame(capture, frame/*, prev_frame*/);
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

	{
		QFile file(g_root_dir + "\\res_data\\!results.txt");
		file.resize(0);
		file.close();
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
						funscript_fname = info.path() + "/" + info.completeBaseName() + ".funscript";
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
			} while (last_play_video_filename == video_filename);
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
			if (!get_parsed_funscript_data(funscript_fname, funscript_data_maped_full))
			{
				do
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds);
				} while (last_play_video_filename == video_filename);
				last_play_video_filename = video_filename;
				continue;
			}			
		}

		std::map<int, SpeedData> speed_data_map;
		get_average_speed_data(speed_data_map);

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

		if (funscript_data_maped.size() == 0)
		{
			show_msg(QString("There is not funscript data at this video pos in forward dirrection\nThe first action is at: %1").arg(VideoTimeToStr(funscript_data_maped_full[0].first).c_str()), 5000);
			do
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, is_video_paused, video_filename, is_vlc_time_in_milliseconds);
			} while ((last_play_video_filename == video_filename) && (cur_video_pos >= search_video_pos));
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
				int cur_speed;
				int set_speed;
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

			
			start_time = GetTickCount64();
			start_video_pos = cur_video_pos;
			QString start_video_name = video_filename;
			//waiting for video unpaused
			while (is_video_paused || (cur_video_pos < (funscript_data_maped[0].first - 1000)))
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

			__int64 action_start_time, action_start_speed, avg_speed_start_time, speed_change_time, prev_get_speed_time = start_time;
			int start_abs_pos, avg_speed_start_abs_cur_pos, exp_abs_cur_pos_to_req_time, tmp_val, dif_cur_vs_req_action_start_time, last_set_action_id_for_dif_cur_vs_req_exp_pos;

			int exp_abs_pos_before_speed_change, action_start_abs_pos;
			int req_speed, req_cur_speed, got_avg_speed, req_new_speed;
			double optimal_hismith_speed = 0, optimal_hismith_speed_prev = 0, optimal_hismith_start_speed = 0;
			QString actions_end_with = "success";
			int prev_cur_video_pos = cur_video_pos;
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
				} while (prev_video_pos / 1000 == cur_video_pos / 1000);

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
				if (dt < 0)
				{
					action_id++;
					continue;
				}

				std::thread *p_get_vlc_status = new std::thread( [&_tmp_cur_video_pos, &_tmp_is_paused, &_tmp_video_filename, &_tmp_is_vlc_time_in_milliseconds] {
					_tmp_cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, _tmp_is_paused, _tmp_video_filename, _tmp_is_vlc_time_in_milliseconds);
					}
				);

				req_speed = ((funscript_data_maped[action_id].second - abs_cur_pos) * 1000.0) / dt;
				optimal_hismith_speed_prev = optimal_hismith_speed;
				exp_abs_pos_before_speed_change = abs_cur_pos + ((cur_speed * (double)g_speed_change_delay) / 1000.0);
				optimal_hismith_speed = (double)get_optimal_hismith_speed(speed_data_map, req_speed, funscript_data_maped[action_id].second, exp_abs_pos_before_speed_change) / 100.0;
				
				int optimal_hismith_start_speed_int;
				
				if (optimal_hismith_speed >= optimal_hismith_speed_prev)
				{
					optimal_hismith_start_speed_int = (int)((min(max(optimal_hismith_speed_prev +
						((optimal_hismith_speed - optimal_hismith_speed_prev) * g_increase_hismith_speed_start_multiplier), 0.0),
						(double)g_max_allowed_hismith_speed / 100.0)) * 100.0);
				}
				else
				{
					optimal_hismith_start_speed_int = (int)((min(max(optimal_hismith_speed_prev +
						((optimal_hismith_speed - optimal_hismith_speed_prev) * g_slowdown_hismith_speed_start_multiplier), 0.0),
						(double)g_max_allowed_hismith_speed / 100.0)) * 100.0);
				}

				optimal_hismith_start_speed = (double)(optimal_hismith_start_speed_int)/100.0;

				optimal_hismith_start_speed = set_hithmith_speed(optimal_hismith_start_speed);
				speed_change_was_obtained = false;

				dtime = (action_id < actions_size - 1) ? g_speed_change_delay : 0;
				
				double cur_set_hismith_speed = optimal_hismith_start_speed;
				do
				{

					if (g_stop_run || g_pause || is_video_paused || (cur_video_pos > funscript_data_maped[min(action_id + 1, actions_size - 1)].first) ||
						(cur_video_pos < prev_cur_video_pos) || (last_play_video_filename != video_filename))
					{
						break;
					}
					
					get_next_frame_and_cur_speed_res = get_next_frame_and_cur_speed(capture, frame,
						abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
						msec_video_prev_pos, abs_prev_pos);
					
					if (!get_next_frame_and_cur_speed_res)
					{
						break;
					}

					cur_time = GetTickCount64();

					if ((int)(cur_time - prev_get_speed_time) > g_cpu_freezes_timeout)
					{
						break;
					}
					
					if (funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time)) > 1000)
					{
						int dt_prev_from_action_start = ((start_video_pos + (int)(prev_get_speed_time - start_time)) - funscript_data_maped[action_id - 1].first);
						int dt_cur_from_action_start = ((start_video_pos + (int)(cur_time - start_time)) - funscript_data_maped[action_id - 1].first);						

						if ( (dt_cur_from_action_start >= 1000) &&
							 (dt_prev_from_action_start <= ((dt_cur_from_action_start / 1000) * 1000)) )
						{
							p_get_vlc_status->join();
							delete p_get_vlc_status;
							cur_video_pos = _tmp_cur_video_pos;
							is_video_paused = _tmp_is_paused;
							video_filename = _tmp_video_filename;
							is_vlc_time_in_milliseconds = _tmp_is_vlc_time_in_milliseconds;

							p_get_vlc_status = new std::thread([&_tmp_cur_video_pos, &_tmp_is_paused, &_tmp_video_filename, &_tmp_is_vlc_time_in_milliseconds] {
								_tmp_cur_video_pos = make_vlc_status_request(g_pNetworkAccessManager, g_NetworkRequest, _tmp_is_paused, _tmp_video_filename, _tmp_is_vlc_time_in_milliseconds);
								}
							);
						}
					}

					{
						int _id = last_set_action_id_for_dif_cur_vs_req_exp_pos + 1;
						while ( _id <= action_id)
						{
							if (((start_video_pos + (int)(prev_get_speed_time - start_time)) - funscript_data_maped[_id].first <= 0) &&
								((start_video_pos + (int)(cur_time - start_time)) - funscript_data_maped[_id].first >= 0))
							{
								exp_abs_cur_pos_to_req_time = abs_cur_pos - ((((cur_speed + prev_cur_speed) / 2) * (double)((start_video_pos + (int)(cur_time - start_time) - funscript_data_maped[_id].first))) / 1000.0);
								results[_id - 1].dif_cur_vs_req_exp_pos = exp_abs_cur_pos_to_req_time - funscript_data_maped[_id].second;
								last_set_action_id_for_dif_cur_vs_req_exp_pos = _id;
							}

							_id++;
						}

						prev_get_speed_time = cur_time;
						prev_cur_speed = cur_speed;
					}

					exp_abs_pos_before_speed_change = abs_cur_pos + ((cur_speed * (double)g_speed_change_delay) / 1000.0);

					if (abs_cur_pos >= funscript_data_maped[action_id].second)
					{
						if (cur_set_hismith_speed != 0)
						{
							req_cur_speed = 0;
							cur_set_hismith_speed = set_hithmith_speed(0);
							hismith_speed_changed += QString("[set_spd:%1 tm_ofs:%2 cur_spd: %3 req_cur_spd: %4 req_pos_vs_cur: %5]").arg(cur_set_hismith_speed).arg((start_video_pos + (int)(cur_time - start_time)) - funscript_data_maped[action_id - 1].first).arg(cur_speed).arg(req_cur_speed).arg(funscript_data_maped[action_id].second - abs_cur_pos);
						}
					}
					else if ((!speed_change_was_obtained) &&
						(((action_start_speed > req_speed) && (cur_speed <= req_speed + ((action_start_speed - req_speed) / 4))) ||
							((action_start_speed < req_speed) && (cur_speed >= req_speed - ((req_speed - action_start_speed) / 4)))))
					{

						//dt = funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time));
						//req_speed = max(((funscript_data_maped[action_id].second - abs_cur_pos) * 1000.0) / dt, 0);
						//optimal_hismith_speed = (double)get_optimal_hismith_speed(speed_data_map, req_speed, funscript_data_maped[action_id].second, exp_abs_pos_before_speed_change) / 100.0;
						//set_hithmith_speed(optimal_hismith_speed);
						//cur_set_hismith_speed = optimal_hismith_speed;
						speed_change_time = cur_time;
						speed_change_was_obtained = true;
					}
					else if (speed_change_was_obtained)
					{
						dt = funscript_data_maped[action_id].first - (start_video_pos + (int)(cur_time - start_time));
						req_cur_speed = max(((funscript_data_maped[action_id].second - abs_cur_pos) * 1000.0) / dt, 0);

						if (cur_speed > req_cur_speed)
						{
							//double _optimal_hismith_speed_cur = (double)get_optimal_hismith_speed(speed_data_map, cur_speed, funscript_data_maped[action_id].second, abs_cur_pos) / 100.0;
							//double _optimal_hismith_speed_req = (double)get_optimal_hismith_speed(speed_data_map, req_cur_speed, funscript_data_maped[action_id].second, abs_cur_pos) / 100.0;
							//cur_set_hismith_speed = cur_set_hismith_speed * (_optimal_hismith_speed_req / _optimal_hismith_speed_cur);

							tmp_val = (int)(cur_set_hismith_speed * 100.0);
							tmp_val = max(tmp_val - max((int)((double)tmp_val * g_slowdown_hismith_speed_change_multiplier), 1), 0); //-%5
							cur_set_hismith_speed = (double)(tmp_val) / 100.0;

							cur_set_hismith_speed = set_hithmith_speed(cur_set_hismith_speed);

							hismith_speed_changed += QString("[set_spd:%1 tm_ofs:%2 cur_spd: %3 req_cur_spd: %4 req_pos_vs_cur: %5]").arg(cur_set_hismith_speed).arg((start_video_pos + (int)(cur_time - start_time)) - funscript_data_maped[action_id - 1].first).arg(cur_speed).arg(req_cur_speed).arg(funscript_data_maped[action_id].second - abs_cur_pos);
						}
						else if (cur_speed < req_cur_speed)
						{
							//double _optimal_hismith_speed_cur = (double)get_optimal_hismith_speed(speed_data_map, cur_speed, funscript_data_maped[action_id].second, abs_cur_pos) / 100.0;
							//double _optimal_hismith_speed_req = (double)get_optimal_hismith_speed(speed_data_map, req_cur_speed, funscript_data_maped[action_id].second, abs_cur_pos) / 100.0;
							//cur_set_hismith_speed = cur_set_hismith_speed * (_optimal_hismith_speed_req / _optimal_hismith_speed_cur);

							tmp_val = (int)(cur_set_hismith_speed * 100.0);
							tmp_val = min(tmp_val + max((int)((double)tmp_val * g_increase_hismith_speed_change_multiplier), 1), g_max_allowed_hismith_speed); //+%5
							cur_set_hismith_speed = (double)(tmp_val) / 100.0;

							cur_set_hismith_speed = set_hithmith_speed(cur_set_hismith_speed);

							hismith_speed_changed += QString("[set_spd:%1 tm_ofs:%2 cur_spd: %3 req_cur_spd: %4 req_pos_vs_cur: %5]").arg(cur_set_hismith_speed).arg((start_video_pos + (int)(cur_time - start_time)) - funscript_data_maped[action_id - 1].first).arg(cur_speed).arg(req_cur_speed).arg(funscript_data_maped[action_id].second - abs_cur_pos);
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
				
				p_get_vlc_status->join();
				delete p_get_vlc_status;
				cur_video_pos = _tmp_cur_video_pos;
				is_video_paused = _tmp_is_paused;
				video_filename = _tmp_video_filename;
				is_vlc_time_in_milliseconds = _tmp_is_vlc_time_in_milliseconds;
				
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
				results[action_id - 1].cur_speed = (int)cur_speed;
				results[action_id - 1].set_speed = (speed_change_time == -1) ?  -1: (int)(optimal_hismith_speed * 100.0);
				results[action_id - 1].optimal_hismith_start_speed = (int)(optimal_hismith_start_speed * 100.0);
				results[action_id - 1].hismith_speed_changed = hismith_speed_changed;

				if ((int)(cur_time - prev_get_speed_time) > g_cpu_freezes_timeout)
				{
					show_msg("It looks you get CPU feezes now\nrestarting all actions.");
					actions_size = action_id;
					break;
				}

				if (!get_next_frame_and_cur_speed_res)
				{
					actions_size = action_id;
					break;
				}
				
				if (g_stop_run || g_pause || is_video_paused || 
					(cur_video_pos > funscript_data_maped[min(action_id + 1, actions_size - 1)].first + (is_vlc_time_in_milliseconds ? 0 : 1000)) ||
					(cur_video_pos < prev_cur_video_pos) || (last_play_video_filename != video_filename))
				{
					if (cur_video_pos > funscript_data_maped[min(action_id + 1, actions_size - 1)].first + (is_vlc_time_in_milliseconds ? 0 : 1000))
					{
						actions_end_with = QString("cur_video_pos (%1) > funscript_data_maped[min(action_id + 1, actions_size - 1)].first (%2)").arg(cur_video_pos).arg(funscript_data_maped[min(action_id + 1, actions_size - 1)].first);
					}
					else if ((action_id > 1) && (cur_video_pos < prev_cur_video_pos))
					{
						actions_end_with = QString("cur_video_pos (%1) < prev_cur_video_pos (%2)").arg(cur_video_pos).arg(prev_cur_video_pos);
					}
					else
					{
						actions_end_with = QString("g_stop_run || g_pause || is_video_paused || (last_play_video_filename != video_filename)");
					}

					last_play_video_filename = video_filename;
					actions_size = action_id;
					break;
				}

				prev_cur_video_pos = cur_video_pos;
				action_id++;
			}			

			QString result_str;
			for (int i = 0; i < actions_size; i++)
			{
				result_str += QString("dif_end_pos:%1 req_dpos:%2+(%3) len:%4 dif_start_t:%5 dif_end_t:%6 start_t:%7 dif_spd_ch:%8 req_pos:%9 last_cur_pos:%10 "
									"req_spd:%11 got_avg_spd:%12 cur_spd:%13 set_h_spd:%14 opt_start_spd:%15 spd_changed:%16\n")
					.arg(results[i].dif_cur_vs_req_exp_pos)
					.arg(results[i].req_dpos)
					.arg(results[i].req_dpos_add)
					.arg(results[i].action_length_time)
					.arg(results[i].dif_cur_vs_req_action_start_time)
					.arg(results[i].dif_cur_vs_req_action_end_time)					
					.arg(results[i].action_start_video_time)
					.arg(results[i].dif_speed_changed_vs_req_action_start_time)					
					.arg(results[i].req_abs_cur_pos)
					.arg(results[i].abs_cur_pos)
					.arg(results[i].req_speed)
					.arg(results[i].got_avg_speed)
					.arg(results[i].cur_speed)
					.arg(results[i].set_speed)
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
		error_msg("ERROR: Looks \"Intiface Central\" not started or no any device connected to it");
		return res;
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
		error_msg("ERROR: Looks \"Intiface Central\" not started or no any device connected to it");
		return res;
	}

	if (!g_pMyDevice)
	{
		error_msg(QString("ERROR: Selected Hismith device is not currently present"));
		return res;
	}

	return res;
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

		get_new_camera_frame(capture, frame/*, prev_frame*/);
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

		while (1)
		{
			last_msec_video_prev_pos = msec_video_cur_pos;
			if (!get_next_frame_and_cur_speed(capture, frame,
				abs_cur_pos, cur_pos, msec_video_cur_pos, cur_speed,
				msec_video_prev_pos, abs_prev_pos, true, &res_frame, add_data))
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

			if (key == 27)  // Esc key to stop
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

	g_req_webcam_name = data_map["req_webcam_name"];
	g_webcam_frame_width = data_map["webcam_frame_width"].toInt();
	g_webcam_frame_height = data_map["webcam_frame_height"].toInt();

	g_intiface_central_client_url = data_map["intiface_central_client_url"].toStdString();
	g_intiface_central_client_port = data_map["intiface_central_client_port"].toInt();

	g_hismith_device_name = data_map["hismith_device_name"];

	g_vlc_url = data_map["vlc_url"];
	g_vlc_port = data_map["vlc_port"].toInt();
	g_vlc_password = data_map["vlc_password"];


	pW->ui->speedLimit->setText(QString::number(g_max_allowed_hismith_speed));
	pW->ui->minRelativeMove->setText(QString::number(g_min_funscript_relative_move));

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
	
    MainWindow w;
	pW = &w;
    w.show();

	if (!LoadSettings())
	{
		return 0;
	}


	//SaveSettings();

	//get_initial_data();

	//get_performance();

	//test_camera();

	//test_err_frame(g_root_dir + "\\error_data\\05_06_2024_19_30_22_frame_orig.bmp");

	//test_hismith(5);

	//get_delay_data();	

	//cv::destroyAllWindows();

	//return 0;

    return a.exec();
}
