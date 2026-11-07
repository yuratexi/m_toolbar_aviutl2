//----------------------------------------------------------------------------------
//	UIプリセット プラグイン for AviUtl ExEdit2
//	プロジェクトのアスペクト比ごとに UI レイアウトを保存・適用する
//----------------------------------------------------------------------------------
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"

#define PLUGIN_NAME       L"UIプリセット"
#define PANEL_WINDOW_NAME L"UIプリセット"
#define IDC_BTN_SAVE      1001
#define IDC_BTN_APPLY     1002
#define IDC_BTN_LIST      1003
#define IDC_BTN_DELETE    1004
#define IDC_BTN_AUTO      1005
#define IDC_LBL_RATIO     1101
#define IDC_LBL_STATUS    1102

static HINSTANCE      g_hInst = nullptr;
static LOG_HANDLE*    logger  = nullptr;
static CONFIG_HANDLE* config  = nullptr;
static EDIT_HANDLE*   edit_handle = nullptr;
static HWND           g_hwnd_panel = nullptr;
static HWND           g_hwnd_ratio = nullptr;
static HWND           g_hwnd_status = nullptr;
static HWND           g_hwnd_auto = nullptr;

static std::wstring g_plugin_dir;
static std::wstring g_presets_path;
static std::wstring g_config_path;

static bool  g_auto_apply = true;
static double g_match_tolerance = 0.02; // |a-b| / max(a,b) の許容
static int   g_last_w = 0;
static int   g_last_h = 0;
static bool  g_applying = false;

//---------------------------------------------------------------------
//	DllMain
//---------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID) {
	if (dwReason == DLL_PROCESS_ATTACH) {
		g_hInst = hInst;
	}
	return TRUE;
}

//---------------------------------------------------------------------
//	ログ
//---------------------------------------------------------------------
static void logf(const wchar_t* fmt, ...) {
	if (!logger) return;
	wchar_t msg[512];
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
	va_end(ap);
	logger->log(logger, msg);
}

static void warnf(const wchar_t* fmt, ...) {
	if (!logger) return;
	wchar_t msg[512];
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
	va_end(ap);
	logger->warn(logger, msg);
}

//---------------------------------------------------------------------
//	パス / ディレクトリ
//---------------------------------------------------------------------
static std::wstring join_path(std::wstring base, const wchar_t* leaf) {
	if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base += L'\\';
	base += leaf;
	return base;
}

static void ensure_plugin_dirs() {
	std::wstring base;
	if (config && config->app_data_path) {
		base = config->app_data_path;
	}
	if (base.empty()) {
		wchar_t mod[MAX_PATH] = {};
		GetModuleFileNameW(g_hInst, mod, MAX_PATH);
		base = mod;
		auto pos = base.rfind(L'\\');
		if (pos != std::wstring::npos) base = base.substr(0, pos);
	}
	CreateDirectoryW(join_path(base, L"Plugin").c_str(), nullptr);
	g_plugin_dir = join_path(base, L"Plugin\\m_ui_preset_aviutl2");
	CreateDirectoryW(g_plugin_dir.c_str(), nullptr);
	g_presets_path = join_path(g_plugin_dir, L"presets.ini");
	g_config_path  = join_path(g_plugin_dir, L"config.ini");
}

//---------------------------------------------------------------------
//	アスペクト比ユーティリティ
//---------------------------------------------------------------------
static int gcd_int(int a, int b) {
	a = std::abs(a); b = std::abs(b);
	while (b) { int t = a % b; a = b; b = t; }
	return a ? a : 1;
}

struct AspectKey {
	int w = 0;
	int h = 0;
	bool is_default = false;

	std::string key() const {
		if (is_default) return "default";
		char buf[64];
		sprintf_s(buf, "%d:%d", w, h);
		return buf;
	}

	std::wstring wkey() const {
		if (is_default) return L"default";
		wchar_t buf[64];
		swprintf_s(buf, L"%d:%d", w, h);
		return buf;
	}

	double ratio() const {
		if (h == 0) return 0.0;
		return (double)w / (double)h;
	}
};

static AspectKey make_aspect(int width, int height) {
	AspectKey k;
	if (width <= 0 || height <= 0) {
		k.is_default = true;
		return k;
	}
	int g = gcd_int(width, height);
	k.w = width / g;
	k.h = height / g;
	return k;
}

static AspectKey parse_aspect_key(const std::string& s) {
	AspectKey k;
	if (s == "default" || s.empty()) {
		k.is_default = true;
		return k;
	}
	int a = 0, b = 0;
	if (sscanf_s(s.c_str(), "%d:%d", &a, &b) == 2 && a > 0 && b > 0) {
		int g = gcd_int(a, b);
		k.w = a / g;
		k.h = b / g;
		return k;
	}
	k.is_default = true;
	return k;
}

static double relative_diff(double a, double b) {
	const double m = (std::max)(std::abs(a), std::abs(b));
	if (m < 1e-9) return 0.0;
	return std::abs(a - b) / m;
}

//---------------------------------------------------------------------
//	レイアウトスナップショット
//
//	保存できるもの:
//	  - ホストメインウィンドウの WINDOWPLACEMENT
//	  - 同一プロセスのトップレベルウィンドウ (分離パネル等) の配置・表示状態
//	  - ホスト直下のタイトル付き子ウィンドウ (パネル候補) の相対矩形・表示状態
//	  - シーン解像度 (参考情報)
//	保存できないもの (SDK/公開APIが無い):
//	  - ドッキング分割ツリー / タブグループ構成そのもの
//	  - AviUtl2 本体の「レイアウト設定」バイナリ形式
//---------------------------------------------------------------------
struct WindowEntry {
	std::wstring title;
	bool top_level = false;
	bool visible = true;
	int left = 0, top = 0, right = 0, bottom = 0;
	UINT show_cmd = SW_SHOWNORMAL;
};

struct LayoutSnapshot {
	int scene_w = 0;
	int scene_h = 0;
	WINDOWPLACEMENT host = {};
	bool has_host = false;
	std::vector<WindowEntry> windows;
};

struct Preset {
	AspectKey key;
	int scene_w = 0;
	int scene_h = 0;
	LayoutSnapshot layout;
	std::wstring note;
};

static std::map<std::string, Preset> g_presets;

struct EnumCtx {
	DWORD pid = 0;
	HWND host = nullptr;
	LayoutSnapshot* snap = nullptr;
};

static bool get_window_title(HWND hwnd, std::wstring& out) {
	wchar_t buf[256] = {};
	int n = GetWindowTextW(hwnd, buf, _countof(buf));
	if (n <= 0) return false;
	out.assign(buf, n);
	return true;
}

static BOOL CALLBACK enum_top_level_proc(HWND hwnd, LPARAM lp) {
	auto* ctx = reinterpret_cast<EnumCtx*>(lp);
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid != ctx->pid) return TRUE;
	if (hwnd == ctx->host) return TRUE;
	if (!IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
		// 非表示のトップレベルもタイトルがあれば記録
	}
	LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	if (!(style & WS_CAPTION) && !(style & WS_POPUP)) return TRUE;

	std::wstring title;
	if (!get_window_title(hwnd, title)) return TRUE;
	if (title == PANEL_WINDOW_NAME) return TRUE; // 自パネルは除外

	WINDOWPLACEMENT wp = {};
	wp.length = sizeof(wp);
	if (!GetWindowPlacement(hwnd, &wp)) return TRUE;

	WindowEntry e;
	e.title = title;
	e.top_level = true;
	e.visible = IsWindowVisible(hwnd) != FALSE;
	e.left = wp.rcNormalPosition.left;
	e.top = wp.rcNormalPosition.top;
	e.right = wp.rcNormalPosition.right;
	e.bottom = wp.rcNormalPosition.bottom;
	e.show_cmd = wp.showCmd;
	ctx->snap->windows.push_back(e);
	return TRUE;
}

static BOOL CALLBACK enum_child_proc(HWND hwnd, LPARAM lp) {
	auto* ctx = reinterpret_cast<EnumCtx*>(lp);
	// 直下の子のみ (孫はドッキング内部構造なのでスキップ)
	if (GetParent(hwnd) != ctx->host) return TRUE;

	std::wstring title;
	if (!get_window_title(hwnd, title)) return TRUE;
	if (title == PANEL_WINDOW_NAME) return TRUE;

	RECT rc = {};
	if (!GetWindowRect(hwnd, &rc)) return TRUE;
	POINT tl{ rc.left, rc.top };
	POINT br{ rc.right, rc.bottom };
	ScreenToClient(ctx->host, &tl);
	ScreenToClient(ctx->host, &br);

	WindowEntry e;
	e.title = title;
	e.top_level = false;
	e.visible = IsWindowVisible(hwnd) != FALSE;
	e.left = tl.x;
	e.top = tl.y;
	e.right = br.x;
	e.bottom = br.y;
	e.show_cmd = e.visible ? SW_SHOW : SW_HIDE;
	ctx->snap->windows.push_back(e);
	return TRUE;
}

static bool capture_layout(LayoutSnapshot& snap, int scene_w, int scene_h) {
	snap = {};
	snap.scene_w = scene_w;
	snap.scene_h = scene_h;
	if (!edit_handle) return false;
	HWND host = edit_handle->get_host_app_window();
	if (!host) return false;

	snap.host.length = sizeof(snap.host);
	if (GetWindowPlacement(host, &snap.host)) {
		snap.has_host = true;
	}

	EnumCtx ctx;
	ctx.pid = GetCurrentProcessId();
	ctx.host = host;
	ctx.snap = &snap;
	EnumWindows(enum_top_level_proc, reinterpret_cast<LPARAM>(&ctx));
	EnumChildWindows(host, enum_child_proc, reinterpret_cast<LPARAM>(&ctx));
	return true;
}

struct FindByTitle {
	const std::wstring* title = nullptr;
	HWND found = nullptr;
	HWND host = nullptr;
	bool top_level = false;
	DWORD pid = 0;
};

static BOOL CALLBACK find_top_proc(HWND hwnd, LPARAM lp) {
	auto* ctx = reinterpret_cast<FindByTitle*>(lp);
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid != ctx->pid) return TRUE;
	if (hwnd == ctx->host) return TRUE;
	std::wstring title;
	if (!get_window_title(hwnd, title)) return TRUE;
	if (title == *ctx->title) {
		ctx->found = hwnd;
		return FALSE;
	}
	return TRUE;
}

static BOOL CALLBACK find_child_proc(HWND hwnd, LPARAM lp) {
	auto* ctx = reinterpret_cast<FindByTitle*>(lp);
	if (GetParent(hwnd) != ctx->host) return TRUE;
	std::wstring title;
	if (!get_window_title(hwnd, title)) return TRUE;
	if (title == *ctx->title) {
		ctx->found = hwnd;
		return FALSE;
	}
	return TRUE;
}

static HWND find_window_by_title(HWND host, const WindowEntry& e) {
	FindByTitle ctx;
	ctx.title = &e.title;
	ctx.host = host;
	ctx.top_level = e.top_level;
	ctx.pid = GetCurrentProcessId();
	if (e.top_level) {
		EnumWindows(find_top_proc, reinterpret_cast<LPARAM>(&ctx));
	} else {
		EnumChildWindows(host, find_child_proc, reinterpret_cast<LPARAM>(&ctx));
	}
	return ctx.found;
}

static bool apply_layout(const LayoutSnapshot& snap) {
	if (!edit_handle) return false;
	HWND host = edit_handle->get_host_app_window();
	if (!host) return false;

	g_applying = true;

	if (snap.has_host) {
		WINDOWPLACEMENT wp = snap.host;
		wp.length = sizeof(wp);
		SetWindowPlacement(host, &wp);
	}

	int applied = 0;
	int missing = 0;
	for (const auto& e : snap.windows) {
		HWND hwnd = find_window_by_title(host, e);
		if (!hwnd) {
			++missing;
			continue;
		}
		if (e.top_level) {
			WINDOWPLACEMENT wp = {};
			wp.length = sizeof(wp);
			GetWindowPlacement(hwnd, &wp);
			wp.rcNormalPosition.left = e.left;
			wp.rcNormalPosition.top = e.top;
			wp.rcNormalPosition.right = e.right;
			wp.rcNormalPosition.bottom = e.bottom;
			wp.showCmd = e.visible ? SW_SHOWNORMAL : SW_HIDE;
			SetWindowPlacement(hwnd, &wp);
		} else {
			const int w = e.right - e.left;
			const int h = e.bottom - e.top;
			if (w > 0 && h > 0) {
				SetWindowPos(hwnd, nullptr, e.left, e.top, w, h,
					SWP_NOZORDER | SWP_NOACTIVATE);
			}
			ShowWindow(hwnd, e.visible ? SW_SHOW : SW_HIDE);
		}
		++applied;
	}

	g_applying = false;
	logf(L"[UIプリセット] 適用: %d ウィンドウ (未検出 %d)", applied, missing);
	return applied > 0 || snap.has_host;
}

//---------------------------------------------------------------------
//	INI シリアライズ
//---------------------------------------------------------------------
static std::string narrow_utf8(const std::wstring& ws) {
	if (ws.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], n, nullptr, nullptr);
	return s;
}

static std::wstring widen_utf8(const std::string& s) {
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring ws(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], n);
	return ws;
}

static std::string escape_title(const std::wstring& title) {
	std::string s = narrow_utf8(title);
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		if (c == '|' || c == ';' || c == '\\') {
			out.push_back('\\');
			out.push_back(c);
		} else if (c == '\n' || c == '\r') {
			continue;
		} else {
			out.push_back(c);
		}
	}
	return out;
}

static std::wstring unescape_title(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '\\' && i + 1 < s.size()) {
			out.push_back(s[++i]);
		} else {
			out.push_back(s[i]);
		}
	}
	return widen_utf8(out);
}

static std::string serialize_windows(const std::vector<WindowEntry>& windows) {
	std::ostringstream oss;
	for (size_t i = 0; i < windows.size(); ++i) {
		const auto& e = windows[i];
		if (i) oss << ';';
		oss << escape_title(e.title) << '|'
			<< (e.top_level ? 1 : 0) << '|'
			<< (e.visible ? 1 : 0) << '|'
			<< e.left << ',' << e.top << ',' << e.right << ',' << e.bottom << '|'
			<< e.show_cmd;
	}
	return oss.str();
}

static std::vector<WindowEntry> deserialize_windows(const std::string& s) {
	std::vector<WindowEntry> out;
	size_t i = 0;
	while (i < s.size()) {
		std::string piece;
		while (i < s.size()) {
			if (s[i] == '\\' && i + 1 < s.size()) {
				piece.push_back(s[i]);
				piece.push_back(s[i + 1]);
				i += 2;
				continue;
			}
			if (s[i] == ';') { ++i; break; }
			piece.push_back(s[i++]);
		}
		if (piece.empty()) continue;

		std::vector<std::string> parts;
		std::string cur;
		for (size_t j = 0; j < piece.size(); ++j) {
			if (piece[j] == '\\' && j + 1 < piece.size()) {
				cur.push_back(piece[j]);
				cur.push_back(piece[j + 1]);
				++j;
				continue;
			}
			if (piece[j] == '|') {
				parts.push_back(cur);
				cur.clear();
			} else {
				cur.push_back(piece[j]);
			}
		}
		parts.push_back(cur);
		if (parts.size() < 4) continue;

		WindowEntry e;
		e.title = unescape_title(parts[0]);
		e.top_level = (parts[1] == "1");
		e.visible = (parts[2] == "1");
		sscanf_s(parts[3].c_str(), "%d,%d,%d,%d", &e.left, &e.top, &e.right, &e.bottom);
		if (parts.size() >= 5) {
			try { e.show_cmd = (UINT)std::stoul(parts[4]); } catch (...) {}
		}
		out.push_back(e);
	}
	return out;
}

static void save_presets_file() {
	ensure_plugin_dirs();
	std::ofstream f(g_presets_path.c_str(), std::ios::binary);
	if (!f.is_open()) {
		warnf(L"presets.ini を書けません");
		return;
	}
	f.write("\xEF\xBB\xBF", 3);
	f << "# m_ui_preset_aviutl2 presets\n";
	f << "# key = reduced aspect (e.g. 16:9) or default\n\n";
	f << "[settings]\n";
	f << "auto_apply=" << (g_auto_apply ? 1 : 0) << "\n";
	f << "match_tolerance=" << g_match_tolerance << "\n\n";

	for (const auto& kv : g_presets) {
		const Preset& p = kv.second;
		f << "[" << kv.first << "]\n";
		f << "scene_w=" << p.scene_w << "\n";
		f << "scene_h=" << p.scene_h << "\n";
		if (p.layout.has_host) {
			const auto& r = p.layout.host.rcNormalPosition;
			f << "host=" << r.left << "," << r.top << "," << r.right << "," << r.bottom
			  << "," << p.layout.host.showCmd << "\n";
		}
		f << "windows=" << serialize_windows(p.layout.windows) << "\n";
		if (!p.note.empty()) {
			f << "note=" << narrow_utf8(p.note) << "\n";
		}
		f << "\n";
	}
}

static void load_presets_file() {
	g_presets.clear();
	ensure_plugin_dirs();
	std::ifstream f(g_presets_path.c_str(), std::ios::binary);
	if (!f.is_open()) return;

	std::string line;
	std::string section;
	Preset cur;
	bool in_settings = false;

	auto flush = [&]() {
		if (!section.empty() && section != "settings") {
			cur.key = parse_aspect_key(section);
			g_presets[section] = cur;
		}
		cur = {};
	};

	while (std::getline(f, line)) {
		if (!line.empty() && (unsigned char)line[0] == 0xEF) {
			// UTF-8 BOM
			if (line.size() >= 3) line = line.substr(3);
		}
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;

		if (line.front() == '[' && line.back() == ']') {
			flush();
			section = line.substr(1, line.size() - 2);
			in_settings = (section == "settings");
			continue;
		}

		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string key = line.substr(0, eq);
		std::string val = line.substr(eq + 1);

		if (in_settings) {
			if (key == "auto_apply") {
				try { g_auto_apply = (std::stoi(val) != 0); } catch (...) {}
			} else if (key == "match_tolerance") {
				try {
					g_match_tolerance = std::stod(val);
					if (g_match_tolerance < 0.0) g_match_tolerance = 0.0;
					if (g_match_tolerance > 0.5) g_match_tolerance = 0.5;
				} catch (...) {}
			}
			continue;
		}

		if (key == "scene_w") {
			try { cur.scene_w = std::stoi(val); } catch (...) {}
		} else if (key == "scene_h") {
			try { cur.scene_h = std::stoi(val); } catch (...) {}
		} else if (key == "host") {
			int l=0,t=0,r=0,b=0; UINT sc = SW_SHOWNORMAL;
			if (sscanf_s(val.c_str(), "%d,%d,%d,%d,%u", &l, &t, &r, &b, &sc) >= 4) {
				cur.layout.has_host = true;
				cur.layout.host.length = sizeof(WINDOWPLACEMENT);
				cur.layout.host.flags = 0;
				cur.layout.host.showCmd = sc;
				cur.layout.host.rcNormalPosition = { l, t, r, b };
			}
		} else if (key == "windows") {
			cur.layout.windows = deserialize_windows(val);
		} else if (key == "note") {
			cur.note = widen_utf8(val);
		}
	}
	flush();
	logf(L"[UIプリセット] プリセット %zu 件を読み込みました", g_presets.size());
}

//---------------------------------------------------------------------
//	マッチング
//---------------------------------------------------------------------
static const Preset* find_matching_preset(const AspectKey& current) {
	if (current.is_default) {
		auto it = g_presets.find("default");
		return (it == g_presets.end()) ? nullptr : &it->second;
	}

	// 1) 約分キー完全一致
	auto it = g_presets.find(current.key());
	if (it != g_presets.end()) return &it->second;

	// 2) 相対差が許容内の最近傍
	const double cur = current.ratio();
	const Preset* best = nullptr;
	double best_diff = 1e9;
	for (const auto& kv : g_presets) {
		if (kv.second.key.is_default) continue;
		double d = relative_diff(cur, kv.second.key.ratio());
		if (d < best_diff) {
			best_diff = d;
			best = &kv.second;
		}
	}
	if (best && best_diff <= g_match_tolerance) return best;

	// 3) default フォールバック
	auto def = g_presets.find("default");
	if (def != g_presets.end()) return &def->second;
	return nullptr;
}

static bool get_current_scene_size(int* w, int* h) {
	if (!edit_handle || !w || !h) return false;
	EDIT_INFO info = {};
	edit_handle->get_edit_info(&info, sizeof(info));
	*w = info.width;
	*h = info.height;
	return (*w > 0 && *h > 0);
}

//---------------------------------------------------------------------
//	パネル UI 更新
//---------------------------------------------------------------------
static void update_panel_ui() {
	if (!g_hwnd_ratio) return;
	int w = 0, h = 0;
	wchar_t buf[128];
	if (get_current_scene_size(&w, &h)) {
		AspectKey k = make_aspect(w, h);
		swprintf_s(buf, L"現在: %dx%d (%s)", w, h, k.wkey().c_str());
	} else {
		swprintf_s(buf, L"現在: (シーン未取得)");
	}
	SetWindowTextW(g_hwnd_ratio, buf);

	if (g_hwnd_status) {
		AspectKey k = make_aspect(w, h);
		const Preset* p = find_matching_preset(k);
		if (p) {
			swprintf_s(buf, L"マッチ: %s (%zu win)", p->key.wkey().c_str(), p->layout.windows.size());
		} else {
			swprintf_s(buf, L"マッチ: なし");
		}
		SetWindowTextW(g_hwnd_status, buf);
	}
	if (g_hwnd_auto) {
		SetWindowTextW(g_hwnd_auto, g_auto_apply ? L"自動適用: ON" : L"自動適用: OFF");
	}
}

//---------------------------------------------------------------------
//	操作本体
//---------------------------------------------------------------------
static void do_save_current() {
	int w = 0, h = 0;
	if (!get_current_scene_size(&w, &h)) {
		warnf(L"シーン解像度を取得できません");
		return;
	}
	AspectKey key = make_aspect(w, h);
	LayoutSnapshot snap;
	if (!capture_layout(snap, w, h)) {
		warnf(L"レイアウトの取得に失敗しました");
		return;
	}

	Preset p;
	p.key = key;
	p.scene_w = w;
	p.scene_h = h;
	p.layout = snap;
	g_presets[key.key()] = p;
	save_presets_file();
	logf(L"[UIプリセット] 保存: %s (%dx%d) windows=%zu",
		key.wkey().c_str(), w, h, snap.windows.size());
	update_panel_ui();
}

static void do_save_default() {
	int w = 0, h = 0;
	get_current_scene_size(&w, &h);
	LayoutSnapshot snap;
	if (!capture_layout(snap, w, h)) {
		warnf(L"レイアウトの取得に失敗しました");
		return;
	}
	Preset p;
	p.key.is_default = true;
	p.scene_w = w;
	p.scene_h = h;
	p.layout = snap;
	g_presets["default"] = p;
	save_presets_file();
	logf(L"[UIプリセット] default を保存 (windows=%zu)", snap.windows.size());
	update_panel_ui();
}

static bool do_apply_matching(bool quiet_if_missing) {
	int w = 0, h = 0;
	if (!get_current_scene_size(&w, &h)) {
		if (!quiet_if_missing) warnf(L"シーン解像度を取得できません");
		return false;
	}
	AspectKey key = make_aspect(w, h);
	const Preset* p = find_matching_preset(key);
	if (!p) {
		if (!quiet_if_missing) {
			logf(L"[UIプリセット] %s に対応するプリセットがありません", key.wkey().c_str());
		}
		return false;
	}
	logf(L"[UIプリセット] %s → プリセット %s を適用",
		key.wkey().c_str(), p->key.wkey().c_str());
	bool ok = apply_layout(p->layout);
	update_panel_ui();
	return ok;
}

static void do_list_presets() {
	if (g_presets.empty()) {
		logf(L"[UIプリセット] 登録なし");
		return;
	}
	logf(L"[UIプリセット] ---- 一覧 (auto=%d tol=%.3f) ----",
		g_auto_apply ? 1 : 0, g_match_tolerance);
	for (const auto& kv : g_presets) {
		const Preset& p = kv.second;
		logf(L"  %s  scene=%dx%d  windows=%zu  host=%d",
			p.key.wkey().c_str(), p.scene_w, p.scene_h,
			p.layout.windows.size(), p.layout.has_host ? 1 : 0);
	}
}

static void do_delete_current() {
	int w = 0, h = 0;
	if (!get_current_scene_size(&w, &h)) {
		warnf(L"シーン解像度を取得できません");
		return;
	}
	AspectKey key = make_aspect(w, h);
	auto it = g_presets.find(key.key());
	if (it == g_presets.end()) {
		logf(L"[UIプリセット] %s のプリセットは存在しません", key.wkey().c_str());
		return;
	}
	g_presets.erase(it);
	save_presets_file();
	logf(L"[UIプリセット] 削除: %s", key.wkey().c_str());
	update_panel_ui();
}

static void do_toggle_auto() {
	g_auto_apply = !g_auto_apply;
	save_presets_file();
	logf(L"[UIプリセット] 自動適用: %s", g_auto_apply ? L"ON" : L"OFF");
	update_panel_ui();
}

static void do_open_folder() {
	ensure_plugin_dirs();
	ShellExecuteW(nullptr, L"open", g_plugin_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void maybe_auto_apply_from_scene(EDIT_SECTION* edit) {
	if (!g_auto_apply || g_applying || !edit || !edit->info) return;
	const int w = edit->info->width;
	const int h = edit->info->height;
	if (w <= 0 || h <= 0) return;
	if (w == g_last_w && h == g_last_h) return;
	g_last_w = w;
	g_last_h = h;
	update_panel_ui();
	do_apply_matching(true);
}

//---------------------------------------------------------------------
//	メニュー / シーン変更コールバック
//---------------------------------------------------------------------
static void cb_save(void*) { do_save_current(); }
static void cb_save_default(void*) { do_save_default(); }
static void cb_apply(void*) { do_apply_matching(false); }
static void cb_list(void*) { do_list_presets(); }
static void cb_delete(void*) { do_delete_current(); }
static void cb_toggle_auto(void*) { do_toggle_auto(); }
static void cb_open_folder(void*) { do_open_folder(); }

static void on_change_scene(EDIT_SECTION* edit) {
	maybe_auto_apply_from_scene(edit);
}

static void on_project_load(PROJECT_FILE*) {
	// ロード直後はシーン情報が揃う前のことがあるので、少し遅延せず
	// change_scene 側に任せる。ここでも現在値で試行する。
	int w = 0, h = 0;
	if (get_current_scene_size(&w, &h)) {
		g_last_w = 0;
		g_last_h = 0;
		if (g_auto_apply) do_apply_matching(true);
		update_panel_ui();
	}
}

//---------------------------------------------------------------------
//	パネルウィンドウ
//---------------------------------------------------------------------
static int layout_row_h() {
	if (config) {
		int h = config->get_layout_size(config, "SettingItemHeight");
		if (h > 0) return h;
	}
	return 28;
}

static HWND make_button(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
	return CreateWindowExW(
		0, WC_BUTTONW, text,
		WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
		x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

static HWND make_label(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
	return CreateWindowExW(
		0, WC_STATICW, text,
		WS_VISIBLE | WS_CHILD | SS_LEFT,
		x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDC_BTN_SAVE:   do_save_current(); break;
		case IDC_BTN_APPLY:  do_apply_matching(false); break;
		case IDC_BTN_LIST:   do_list_presets(); break;
		case IDC_BTN_DELETE: do_delete_current(); break;
		case IDC_BTN_AUTO:   do_toggle_auto(); break;
		}
		SetFocus(nullptr);
		return 0;
	case WM_SIZE:
		update_panel_ui();
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void create_panel(HOST_APP_TABLE* host) {
	WNDCLASSEXW wcex = {};
	wcex.cbSize = sizeof(wcex);
	wcex.lpszClassName = PANEL_WINDOW_NAME;
	wcex.lpfnWndProc = panel_proc;
	wcex.hInstance = g_hInst;
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassExW(&wcex);

	g_hwnd_panel = CreateWindowExW(
		0, PANEL_WINDOW_NAME, PANEL_WINDOW_NAME,
		WS_POPUP,
		CW_USEDEFAULT, CW_USEDEFAULT, 280, 220,
		nullptr, nullptr, g_hInst, nullptr);
	if (!g_hwnd_panel) return;

	const int row = layout_row_h();
	const int pad = 8;
	const int bw = 250;
	int y = pad;

	g_hwnd_ratio = make_label(g_hwnd_panel, IDC_LBL_RATIO, L"現在: -", pad, y, bw, row);
	y += row + 2;
	g_hwnd_status = make_label(g_hwnd_panel, IDC_LBL_STATUS, L"マッチ: -", pad, y, bw, row);
	y += row + 6;

	make_button(g_hwnd_panel, IDC_BTN_SAVE, L"現在の比率で保存", pad, y, bw, row);
	y += row + 4;
	make_button(g_hwnd_panel, IDC_BTN_APPLY, L"マッチするプリセットを適用", pad, y, bw, row);
	y += row + 4;
	make_button(g_hwnd_panel, IDC_BTN_LIST, L"プリセット一覧 (ログ)", pad, y, bw, row);
	y += row + 4;
	make_button(g_hwnd_panel, IDC_BTN_DELETE, L"現在の比率のプリセットを削除", pad, y, bw, row);
	y += row + 4;
	g_hwnd_auto = make_button(g_hwnd_panel, IDC_BTN_AUTO, L"自動適用: ON", pad, y, bw, row);

	host->register_window_client(PANEL_WINDOW_NAME, g_hwnd_panel);
	update_panel_ui();
}

//---------------------------------------------------------------------
//	プラグイン入口
//---------------------------------------------------------------------
COMMON_PLUGIN_TABLE common_plugin_table = {
	PLUGIN_NAME,
	L"UIプリセット (アスペクト比) version 1.0",
};

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() {
	return 2003300;
}

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) {
	logger = handle;
}

EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle) {
	config = handle;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD /*version*/) {
	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&icc);
	return true;
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
	g_hwnd_panel = nullptr;
	g_hwnd_ratio = nullptr;
	g_hwnd_status = nullptr;
	g_hwnd_auto = nullptr;
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
	return &common_plugin_table;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
	ensure_plugin_dirs();
	load_presets_file();

	edit_handle = host->create_edit_handle();

	host->register_edit_menu_param(L"UIプリセット\\現在の比率で保存", nullptr, cb_save);
	host->register_edit_menu_param(L"UIプリセット\\defaultとして保存", nullptr, cb_save_default);
	host->register_edit_menu_param(L"UIプリセット\\マッチするプリセットを適用", nullptr, cb_apply);
	host->register_edit_menu_param(L"UIプリセット\\プリセット一覧をログに表示", nullptr, cb_list);
	host->register_edit_menu_param(L"UIプリセット\\現在の比率のプリセットを削除", nullptr, cb_delete);
	host->register_edit_menu_param(L"UIプリセット\\自動適用の切替", nullptr, cb_toggle_auto);
	host->register_edit_menu_param(L"UIプリセット\\設定フォルダを開く", nullptr, cb_open_folder);

	host->register_change_scene_handler(on_change_scene);
	host->register_project_load_handler(on_project_load);

	create_panel(host);

	logf(L"[UIプリセット] 初期化完了 (auto=%d)", g_auto_apply ? 1 : 0);
}
