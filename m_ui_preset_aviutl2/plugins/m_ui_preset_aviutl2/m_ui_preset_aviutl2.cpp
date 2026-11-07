//----------------------------------------------------------------------------------
//	UIプリセット プラグイン for AviUtl ExEdit2
//	名前付き UI レイアウトを用意し、ユーザーが選んでプロジェクトに紐づける
//----------------------------------------------------------------------------------
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

#include <algorithm>
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

// プロジェクトファイルへ保存するキー (プラグイン固有領域 / UTF-8)
static const char* k_project_preset_key = "ui_preset";

#define IDC_LIST          1001
#define IDC_BTN_APPLY     1002
#define IDC_BTN_SAVE      1003
#define IDC_BTN_DELETE    1004
#define IDC_BTN_DEFAULT   1005
#define IDC_BTN_BIND      1006
#define IDC_BTN_FOLDER    1007
#define IDC_LBL_INFO      1101
#define IDC_LBL_PROJECT   1102
#define IDC_LBL_DEFAULT   1103
#define IDC_EDIT_NAME     1201
#define IDC_EDIT_RATIO    1202
#define IDOK_SAVE_DLG     1
#define IDCANCEL_SAVE_DLG 2

static HINSTANCE      g_hInst = nullptr;
static LOG_HANDLE*    logger  = nullptr;
static CONFIG_HANDLE* config  = nullptr;
static EDIT_HANDLE*   edit_handle = nullptr;

static HWND g_hwnd_panel   = nullptr;
static HWND g_hwnd_list    = nullptr;
static HWND g_hwnd_info    = nullptr;
static HWND g_hwnd_project = nullptr;
static HWND g_hwnd_default = nullptr;

static std::wstring g_plugin_dir;
static std::wstring g_presets_path;
static std::wstring g_config_path;

static std::wstring g_default_preset;   // グローバル既定プリセット名
static std::wstring g_project_preset;   // 現プロジェクトに紐づく名前 (未設定は空)
static std::wstring g_selected_preset;  // パネルで選択中
static bool         g_applying = false;

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
//	文字列 / パス
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

static std::wstring trim_ws(std::wstring s) {
	while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
	while (!s.empty() && iswspace(s.back())) s.pop_back();
	return s;
}

static bool is_safe_preset_name(const std::wstring& name) {
	if (name.empty() || name.size() > 64) return false;
	if (name == L"settings" || name == L"default") return false;
	for (wchar_t c : name) {
		if (c == L'[' || c == L']' || c == L'=' || c == L'\n' || c == L'\r' || c == L'\\') return false;
	}
	return true;
}

static std::wstring join_path(std::wstring base, const wchar_t* leaf) {
	if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base += L'\\';
	base += leaf;
	return base;
}

static void ensure_plugin_dirs() {
	std::wstring base;
	if (config && config->app_data_path) base = config->app_data_path;
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
//	アスペクト比 (ラベル用。自動適用には使わない)
//---------------------------------------------------------------------
static int gcd_int(int a, int b) {
	a = std::abs(a); b = std::abs(b);
	while (b) { int t = a % b; a = b; b = t; }
	return a ? a : 1;
}

static std::wstring ratio_label_from_size(int w, int h) {
	if (w <= 0 || h <= 0) return L"";
	int g = gcd_int(w, h);
	wchar_t buf[32];
	swprintf_s(buf, L"%d:%d", w / g, h / g);
	return buf;
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
//	レイアウトスナップショット (Win32 で取れる範囲)
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
	std::wstring name;
	std::wstring ratio; // 組織用ラベル (例: 16:9)。空でも可
	LayoutSnapshot layout;
};

static std::map<std::wstring, Preset> g_presets; // name -> preset

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
	if (pid != ctx->pid || hwnd == ctx->host) return TRUE;

	LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	if (!(style & WS_CAPTION) && !(style & WS_POPUP)) return TRUE;

	std::wstring title;
	if (!get_window_title(hwnd, title)) return TRUE;
	if (title == PANEL_WINDOW_NAME) return TRUE;

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
	if (GetWindowPlacement(host, &snap.host)) snap.has_host = true;

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
	DWORD pid = 0;
};

static BOOL CALLBACK find_top_proc(HWND hwnd, LPARAM lp) {
	auto* ctx = reinterpret_cast<FindByTitle*>(lp);
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid != ctx->pid || hwnd == ctx->host) return TRUE;
	std::wstring title;
	if (!get_window_title(hwnd, title)) return TRUE;
	if (title == *ctx->title) { ctx->found = hwnd; return FALSE; }
	return TRUE;
}

static BOOL CALLBACK find_child_proc(HWND hwnd, LPARAM lp) {
	auto* ctx = reinterpret_cast<FindByTitle*>(lp);
	if (GetParent(hwnd) != ctx->host) return TRUE;
	std::wstring title;
	if (!get_window_title(hwnd, title)) return TRUE;
	if (title == *ctx->title) { ctx->found = hwnd; return FALSE; }
	return TRUE;
}

static HWND find_window_by_title(HWND host, const WindowEntry& e) {
	FindByTitle ctx;
	ctx.title = &e.title;
	ctx.host = host;
	ctx.pid = GetCurrentProcessId();
	if (e.top_level) EnumWindows(find_top_proc, reinterpret_cast<LPARAM>(&ctx));
	else EnumChildWindows(host, find_child_proc, reinterpret_cast<LPARAM>(&ctx));
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

	int applied = 0, missing = 0;
	for (const auto& e : snap.windows) {
		HWND hwnd = find_window_by_title(host, e);
		if (!hwnd) { ++missing; continue; }
		if (e.top_level) {
			WINDOWPLACEMENT wp = {};
			wp.length = sizeof(wp);
			GetWindowPlacement(hwnd, &wp);
			wp.rcNormalPosition = { e.left, e.top, e.right, e.bottom };
			wp.showCmd = e.visible ? SW_SHOWNORMAL : SW_HIDE;
			SetWindowPlacement(hwnd, &wp);
		} else {
			const int w = e.right - e.left;
			const int h = e.bottom - e.top;
			if (w > 0 && h > 0) {
				SetWindowPos(hwnd, nullptr, e.left, e.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
			}
			ShowWindow(hwnd, e.visible ? SW_SHOW : SW_HIDE);
		}
		++applied;
	}
	g_applying = false;
	logf(L"[UIプリセット] レイアウト適用: %d 窓 (未検出 %d)", applied, missing);
	return applied > 0 || snap.has_host;
}

//---------------------------------------------------------------------
//	シリアライズ
//---------------------------------------------------------------------
static std::string escape_title(const std::wstring& title) {
	std::string s = narrow_utf8(title);
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		if (c == '|' || c == ';' || c == '\\') { out.push_back('\\'); out.push_back(c); }
		else if (c != '\n' && c != '\r') out.push_back(c);
	}
	return out;
}

static std::wstring unescape_title(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '\\' && i + 1 < s.size()) out.push_back(s[++i]);
		else out.push_back(s[i]);
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
			if (s[i] == '\\' && i + 1 < s.size()) { piece.push_back(s[i]); piece.push_back(s[i + 1]); i += 2; continue; }
			if (s[i] == ';') { ++i; break; }
			piece.push_back(s[i++]);
		}
		if (piece.empty()) continue;

		std::vector<std::string> parts;
		std::string cur;
		for (size_t j = 0; j < piece.size(); ++j) {
			if (piece[j] == '\\' && j + 1 < piece.size()) { cur.push_back(piece[j]); cur.push_back(piece[++j]); continue; }
			if (piece[j] == '|') { parts.push_back(cur); cur.clear(); }
			else cur.push_back(piece[j]);
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

static void save_config_file() {
	ensure_plugin_dirs();
	std::ofstream f(g_config_path.c_str(), std::ios::binary);
	if (!f.is_open()) return;
	f.write("\xEF\xBB\xBF", 3);
	f << "default_preset=" << narrow_utf8(g_default_preset) << "\n";
}

static void load_config_file() {
	g_default_preset.clear();
	ensure_plugin_dirs();
	std::ifstream f(g_config_path.c_str(), std::ios::binary);
	if (!f.is_open()) return;
	std::string line;
	while (std::getline(f, line)) {
		if (!line.empty() && (unsigned char)line[0] == 0xEF && line.size() >= 3) line = line.substr(3);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		if (line.substr(0, eq) == "default_preset") {
			g_default_preset = trim_ws(widen_utf8(line.substr(eq + 1)));
		}
	}
}

static void save_presets_file() {
	ensure_plugin_dirs();
	std::ofstream f(g_presets_path.c_str(), std::ios::binary);
	if (!f.is_open()) {
		warnf(L"presets.ini を書けません");
		return;
	}
	f.write("\xEF\xBB\xBF", 3);
	f << "# m_ui_preset_aviutl2 named presets\n";
	f << "# section name = preset name\n";
	f << "# ratio = organizational label (e.g. 16:9), not used for auto-apply\n\n";

	for (const auto& kv : g_presets) {
		const Preset& p = kv.second;
		f << "[" << narrow_utf8(p.name) << "]\n";
		if (!p.ratio.empty()) f << "ratio=" << narrow_utf8(p.ratio) << "\n";
		f << "scene_w=" << p.layout.scene_w << "\n";
		f << "scene_h=" << p.layout.scene_h << "\n";
		if (p.layout.has_host) {
			const auto& r = p.layout.host.rcNormalPosition;
			f << "host=" << r.left << "," << r.top << "," << r.right << "," << r.bottom
			  << "," << p.layout.host.showCmd << "\n";
		}
		f << "windows=" << serialize_windows(p.layout.windows) << "\n\n";
	}
}

static void load_presets_file() {
	g_presets.clear();
	ensure_plugin_dirs();
	std::ifstream f(g_presets_path.c_str(), std::ios::binary);
	if (!f.is_open()) return;

	std::string line, section;
	Preset cur;
	auto flush = [&]() {
		if (section.empty() || section == "settings") return;
		cur.name = widen_utf8(section);
		if (!is_safe_preset_name(cur.name) && cur.name != L"default") {
			// 旧形式の "16:9" キーも名前として許容
		}
		if (!cur.name.empty()) g_presets[cur.name] = cur;
		cur = {};
	};

	while (std::getline(f, line)) {
		if (!line.empty() && (unsigned char)line[0] == 0xEF && line.size() >= 3) line = line.substr(3);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		if (line.front() == '[' && line.back() == ']') {
			flush();
			section = line.substr(1, line.size() - 2);
			continue;
		}
		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string key = line.substr(0, eq);
		std::string val = line.substr(eq + 1);

		// 旧 settings セクションは無視 (auto_apply 等は廃止)
		if (section == "settings") continue;

		if (key == "ratio") cur.ratio = widen_utf8(val);
		else if (key == "scene_w") { try { cur.layout.scene_w = std::stoi(val); } catch (...) {} }
		else if (key == "scene_h") { try { cur.layout.scene_h = std::stoi(val); } catch (...) {} }
		else if (key == "host") {
			int l=0,t=0,r=0,b=0; UINT sc = SW_SHOWNORMAL;
			if (sscanf_s(val.c_str(), "%d,%d,%d,%d,%u", &l, &t, &r, &b, &sc) >= 4) {
				cur.layout.has_host = true;
				cur.layout.host.length = sizeof(WINDOWPLACEMENT);
				cur.layout.host.showCmd = sc;
				cur.layout.host.rcNormalPosition = { l, t, r, b };
			}
		} else if (key == "windows") {
			cur.layout.windows = deserialize_windows(val);
		}
	}
	flush();
	logf(L"[UIプリセット] プリセット %zu 件を読み込み", g_presets.size());
}

//---------------------------------------------------------------------
//	プロジェクト紐づけ (PROJECT_FILE プラグイン領域)
//---------------------------------------------------------------------
struct ProjectBindParam {
	std::wstring name;
	bool clear = false;
	bool ok = false;
};

static void write_project_preset_cb(void* p, EDIT_SECTION* edit) {
	auto* param = static_cast<ProjectBindParam*>(p);
	if (!edit || !edit_handle) return;
	PROJECT_FILE* proj = edit->get_project_file(edit_handle);
	if (!proj) {
		warnf(L"PROJECT_FILE を取得できません");
		return;
	}
	if (param->clear || param->name.empty()) {
		proj->set_param_string(k_project_preset_key, "");
	} else {
		proj->set_param_string(k_project_preset_key, narrow_utf8(param->name).c_str());
	}
	param->ok = true;
}

static bool bind_preset_to_project(const std::wstring& name) {
	if (!edit_handle) return false;
	ProjectBindParam param;
	param.name = name;
	param.clear = name.empty();
	if (!edit_handle->call_edit_section_param(&param, write_project_preset_cb)) {
		warnf(L"プロジェクトへの書き込みに失敗 (編集ロック不可?)");
		return false;
	}
	if (!param.ok) return false;
	g_project_preset = name;
	logf(L"[UIプリセット] プロジェクト紐づけ: %s",
		name.empty() ? L"(クリア)" : name.c_str());
	return true;
}

static void on_project_save(PROJECT_FILE* project) {
	if (!project) return;
	// メモリ上の紐づけを確実に永続化
	if (g_project_preset.empty()) {
		project->set_param_string(k_project_preset_key, "");
	} else {
		project->set_param_string(k_project_preset_key, narrow_utf8(g_project_preset).c_str());
	}
}

static bool apply_named_preset(const std::wstring& name, bool bind_project);

static void on_project_load(PROJECT_FILE* project) {
	g_project_preset.clear();
	if (project) {
		LPCSTR v = project->get_param_string(k_project_preset_key);
		if (v && v[0]) g_project_preset = widen_utf8(v);
	}

	std::wstring to_apply;
	if (!g_project_preset.empty() && g_presets.count(g_project_preset)) {
		to_apply = g_project_preset;
		logf(L"[UIプリセット] プロジェクト紐づけ '%s' を適用", to_apply.c_str());
	} else if (!g_project_preset.empty()) {
		warnf(L"プロジェクト指定のプリセット '%s' が見つかりません → 既定を試行",
			g_project_preset.c_str());
	}

	if (to_apply.empty() && !g_default_preset.empty() && g_presets.count(g_default_preset)) {
		to_apply = g_default_preset;
		logf(L"[UIプリセット] 既定プリセット '%s' を適用", to_apply.c_str());
	}

	if (!to_apply.empty()) {
		// ロード直後はウィンドウ再配置中のことがあるので適用のみ (再バインドしない)
		apply_named_preset(to_apply, false);
	}
}

//---------------------------------------------------------------------
//	パネル UI
//---------------------------------------------------------------------
static void refresh_list() {
	if (!g_hwnd_list) return;
	SendMessageW(g_hwnd_list, LB_RESETCONTENT, 0, 0);
	int select = -1;
	int i = 0;
	for (const auto& kv : g_presets) {
		std::wstring label = kv.second.name;
		if (!kv.second.ratio.empty()) label += L"  [" + kv.second.ratio + L"]";
		SendMessageW(g_hwnd_list, LB_ADDSTRING, 0, (LPARAM)label.c_str());
		if (!g_selected_preset.empty() && kv.first == g_selected_preset) select = i;
		else if (g_selected_preset.empty() && !g_project_preset.empty() && kv.first == g_project_preset) select = i;
		else if (g_selected_preset.empty() && g_project_preset.empty() && kv.first == g_default_preset) select = i;
		++i;
	}
	if (select >= 0) SendMessageW(g_hwnd_list, LB_SETCURSEL, select, 0);
}

static std::wstring list_selected_name() {
	if (!g_hwnd_list) return g_selected_preset;
	int idx = (int)SendMessageW(g_hwnd_list, LB_GETCURSEL, 0, 0);
	if (idx < 0) return {};
	wchar_t buf[256] = {};
	SendMessageW(g_hwnd_list, LB_GETTEXT, idx, (LPARAM)buf);
	std::wstring label(buf);
	auto br = label.find(L"  [");
	if (br != std::wstring::npos) label = label.substr(0, br);
	return trim_ws(label);
}

static void update_panel_labels() {
	wchar_t buf[256];
	int w = 0, h = 0;
	if (g_hwnd_info) {
		if (get_current_scene_size(&w, &h)) {
			swprintf_s(buf, L"シーン: %dx%d (%s) ※ラベル参考", w, h, ratio_label_from_size(w, h).c_str());
		} else {
			swprintf_s(buf, L"シーン: (未取得)");
		}
		SetWindowTextW(g_hwnd_info, buf);
	}
	if (g_hwnd_project) {
		if (g_project_preset.empty()) swprintf_s(buf, L"プロジェクト紐づけ: (なし)");
		else swprintf_s(buf, L"プロジェクト紐づけ: %s", g_project_preset.c_str());
		SetWindowTextW(g_hwnd_project, buf);
	}
	if (g_hwnd_default) {
		if (g_default_preset.empty()) swprintf_s(buf, L"既定プリセット: (なし)");
		else swprintf_s(buf, L"既定プリセット: %s", g_default_preset.c_str());
		SetWindowTextW(g_hwnd_default, buf);
	}
}

static void update_panel_ui() {
	refresh_list();
	update_panel_labels();
}

static bool apply_named_preset(const std::wstring& name, bool bind_project) {
	auto it = g_presets.find(name);
	if (it == g_presets.end()) {
		warnf(L"プリセット '%s' がありません", name.c_str());
		return false;
	}
	g_selected_preset = name;
	bool ok = apply_layout(it->second.layout);
	if (ok && bind_project) {
		bind_preset_to_project(name);
	}
	update_panel_ui();
	return ok;
}

//---------------------------------------------------------------------
//	名前入力ダイアログ
//---------------------------------------------------------------------
struct SaveDlgData {
	std::wstring name;
	std::wstring ratio;
	bool ok = false;
};

static LRESULT CALLBACK save_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	SaveDlgData* data = reinterpret_cast<SaveDlgData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	switch (msg) {
	case WM_CREATE: {
		auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
		data = reinterpret_cast<SaveDlgData*>(cs->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
		CreateWindowExW(0, WC_STATICW, L"プリセット名:", WS_CHILD | WS_VISIBLE,
			12, 12, 260, 20, hwnd, nullptr, g_hInst, nullptr);
		HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, data->name.c_str(),
			WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
			12, 34, 300, 24, hwnd, (HMENU)IDC_EDIT_NAME, g_hInst, nullptr);
		CreateWindowExW(0, WC_STATICW, L"比率ラベル (例 16:9 / 任意):", WS_CHILD | WS_VISIBLE,
			12, 68, 260, 20, hwnd, nullptr, g_hInst, nullptr);
		CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, data->ratio.c_str(),
			WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
			12, 90, 300, 24, hwnd, (HMENU)IDC_EDIT_RATIO, g_hInst, nullptr);
		CreateWindowExW(0, WC_BUTTONW, L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
			140, 130, 80, 28, hwnd, (HMENU)IDOK_SAVE_DLG, g_hInst, nullptr);
		CreateWindowExW(0, WC_BUTTONW, L"キャンセル", WS_CHILD | WS_VISIBLE,
			232, 130, 80, 28, hwnd, (HMENU)IDCANCEL_SAVE_DLG, g_hInst, nullptr);
		SetFocus(edit);
		return 0;
	}
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK_SAVE_DLG && data) {
			wchar_t name[128] = {}, ratio[64] = {};
			GetDlgItemTextW(hwnd, IDC_EDIT_NAME, name, _countof(name));
			GetDlgItemTextW(hwnd, IDC_EDIT_RATIO, ratio, _countof(ratio));
			data->name = trim_ws(name);
			data->ratio = trim_ws(ratio);
			if (!is_safe_preset_name(data->name)) {
				MessageBoxW(hwnd, L"プリセット名が不正です。\n空・settings・[]=\\ 改行は使えません。",
					PLUGIN_NAME, MB_OK | MB_ICONWARNING);
				return 0;
			}
			data->ok = true;
			DestroyWindow(hwnd);
			return 0;
		}
		if (LOWORD(wp) == IDCANCEL_SAVE_DLG) {
			DestroyWindow(hwnd);
			return 0;
		}
		break;
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool prompt_save_name(HWND owner, std::wstring& name, std::wstring& ratio) {
	static bool registered = false;
	if (!registered) {
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpszClassName = L"m_ui_preset_save_dlg";
		wc.lpfnWndProc = save_dlg_proc;
		wc.hInstance = g_hInst;
		wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		RegisterClassExW(&wc);
		registered = true;
	}

	SaveDlgData data;
	data.name = name;
	data.ratio = ratio;

	HWND dlg = CreateWindowExW(
		WS_EX_DLGMODALFRAME, L"m_ui_preset_save_dlg", L"UIプリセットを保存",
		WS_CAPTION | WS_POPUP | WS_SYSMENU,
		CW_USEDEFAULT, CW_USEDEFAULT, 340, 200,
		owner, nullptr, g_hInst, &data);
	if (!dlg) return false;

	if (owner) EnableWindow(owner, FALSE);
	ShowWindow(dlg, SW_SHOW);
	MSG msg;
	while (IsWindow(dlg)) {
		const BOOL gm = GetMessageW(&msg, nullptr, 0, 0);
		if (gm == 0) {
			// ホストの WM_QUIT を奪わないよう付け直す
			PostQuitMessage((int)msg.wParam);
			break;
		}
		if (gm < 0) break;
		if (!IsDialogMessageW(dlg, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	if (owner) {
		EnableWindow(owner, TRUE);
		SetForegroundWindow(owner);
	}

	if (!data.ok) return false;
	name = data.name;
	ratio = data.ratio;
	return true;
}

//---------------------------------------------------------------------
//	操作
//---------------------------------------------------------------------
static void do_save_as() {
	int w = 0, h = 0;
	get_current_scene_size(&w, &h);

	std::wstring name = g_selected_preset;
	std::wstring ratio = ratio_label_from_size(w, h);
	if (name.empty() && !ratio.empty()) name = ratio; // 初期案として比率文字列

	HWND owner = g_hwnd_panel;
	if (!owner && edit_handle) owner = edit_handle->get_host_app_window();
	if (!prompt_save_name(owner, name, ratio)) return;

	LayoutSnapshot snap;
	if (!capture_layout(snap, w, h)) {
		warnf(L"レイアウト取得に失敗");
		return;
	}

	Preset p;
	p.name = name;
	p.ratio = ratio;
	p.layout = snap;
	g_presets[name] = p;
	g_selected_preset = name;
	save_presets_file();
	logf(L"[UIプリセット] 保存: '%s' ratio=%s windows=%zu",
		name.c_str(), ratio.c_str(), snap.windows.size());
	update_panel_ui();
}

static void do_apply_selected(bool bind) {
	std::wstring name = list_selected_name();
	if (name.empty()) name = g_selected_preset;
	if (name.empty()) {
		warnf(L"プリセットが選択されていません");
		return;
	}
	apply_named_preset(name, bind);
}

static void do_delete_selected() {
	std::wstring name = list_selected_name();
	if (name.empty()) {
		warnf(L"削除するプリセットが選択されていません");
		return;
	}
	g_presets.erase(name);
	if (g_selected_preset == name) g_selected_preset.clear();
	if (g_default_preset == name) {
		g_default_preset.clear();
		save_config_file();
	}
	if (g_project_preset == name) {
		bind_preset_to_project(L"");
	}
	save_presets_file();
	logf(L"[UIプリセット] 削除: '%s'", name.c_str());
	update_panel_ui();
}

static void do_set_default() {
	std::wstring name = list_selected_name();
	if (name.empty() || !g_presets.count(name)) {
		warnf(L"既定にするプリセットを選択してください");
		return;
	}
	g_default_preset = name;
	save_config_file();
	logf(L"[UIプリセット] 既定プリセット: '%s'", name.c_str());
	update_panel_ui();
}

static void do_bind_selected() {
	std::wstring name = list_selected_name();
	if (name.empty() || !g_presets.count(name)) {
		warnf(L"プロジェクトに紐づけるプリセットを選択してください");
		return;
	}
	if (bind_preset_to_project(name)) update_panel_ui();
}

static void do_list_log() {
	logf(L"[UIプリセット] ---- 一覧 (default=%s / project=%s) ----",
		g_default_preset.empty() ? L"(なし)" : g_default_preset.c_str(),
		g_project_preset.empty() ? L"(なし)" : g_project_preset.c_str());
	for (const auto& kv : g_presets) {
		logf(L"  '%s'  ratio=%s  windows=%zu",
			kv.second.name.c_str(),
			kv.second.ratio.empty() ? L"-" : kv.second.ratio.c_str(),
			kv.second.layout.windows.size());
	}
}

static void do_open_folder() {
	ensure_plugin_dirs();
	ShellExecuteW(nullptr, L"open", g_plugin_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

//---------------------------------------------------------------------
//	メニューコールバック
//---------------------------------------------------------------------
static void cb_save(void*) { do_save_as(); }
static void cb_apply(void*) { do_apply_selected(true); }
static void cb_bind(void*) { do_bind_selected(); }
static void cb_default(void*) { do_set_default(); }
static void cb_delete(void*) { do_delete_selected(); }
static void cb_list(void*) { do_list_log(); }
static void cb_folder(void*) { do_open_folder(); }

static void on_change_scene(EDIT_SECTION* /*edit*/) {
	// 比率スニッフによる自動適用はしない。ラベル表示だけ更新。
	if (!g_applying) update_panel_labels();
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

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_COMMAND: {
		const int id = LOWORD(wp);
		const int code = HIWORD(wp);
		if (id == IDC_LIST && code == LBN_SELCHANGE) {
			g_selected_preset = list_selected_name();
			return 0;
		}
		if (id == IDC_LIST && code == LBN_DBLCLK) {
			do_apply_selected(true);
			return 0;
		}
		switch (id) {
		case IDC_BTN_APPLY:   do_apply_selected(true); break;
		case IDC_BTN_SAVE:    do_save_as(); break;
		case IDC_BTN_DELETE:  do_delete_selected(); break;
		case IDC_BTN_DEFAULT: do_set_default(); break;
		case IDC_BTN_BIND:    do_bind_selected(); break;
		case IDC_BTN_FOLDER:  do_open_folder(); break;
		}
		SetFocus(nullptr);
		return 0;
	}
	case WM_SIZE: {
		RECT rc = {};
		GetClientRect(hwnd, &rc);
		const int pad = 8;
		const int row = layout_row_h();
		int y = pad;
		const int cw = (std::max)(40, (int)rc.right - pad * 2);
		if (g_hwnd_info) { SetWindowPos(g_hwnd_info, nullptr, pad, y, cw, row, SWP_NOZORDER); y += row + 2; }
		if (g_hwnd_project) { SetWindowPos(g_hwnd_project, nullptr, pad, y, cw, row, SWP_NOZORDER); y += row + 2; }
		if (g_hwnd_default) { SetWindowPos(g_hwnd_default, nullptr, pad, y, cw, row, SWP_NOZORDER); y += row + 4; }
		const int list_h = (std::max)(60, (int)rc.bottom - y - (row + 4) * 3 - pad);
		if (g_hwnd_list) { SetWindowPos(g_hwnd_list, nullptr, pad, y, cw, list_h, SWP_NOZORDER); y += list_h + 4; }
		const int bw = (cw - 4) / 2;
		auto place2 = [&](int id1, int id2) {
			HWND a = GetDlgItem(hwnd, id1);
			HWND b = GetDlgItem(hwnd, id2);
			if (a) SetWindowPos(a, nullptr, pad, y, bw, row, SWP_NOZORDER);
			if (b) SetWindowPos(b, nullptr, pad + bw + 4, y, bw, row, SWP_NOZORDER);
			y += row + 4;
		};
		place2(IDC_BTN_APPLY, IDC_BTN_SAVE);
		place2(IDC_BTN_BIND, IDC_BTN_DEFAULT);
		place2(IDC_BTN_DELETE, IDC_BTN_FOLDER);
		return 0;
	}
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND make_btn(HWND parent, int id, const wchar_t* text) {
	return CreateWindowExW(0, WC_BUTTONW, text, WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
		0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
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
		0, PANEL_WINDOW_NAME, PANEL_WINDOW_NAME, WS_POPUP,
		CW_USEDEFAULT, CW_USEDEFAULT, 320, 360,
		nullptr, nullptr, g_hInst, nullptr);
	if (!g_hwnd_panel) return;

	g_hwnd_info = CreateWindowExW(0, WC_STATICW, L"", WS_VISIBLE | WS_CHILD | SS_LEFT,
		0, 0, 10, 10, g_hwnd_panel, (HMENU)IDC_LBL_INFO, g_hInst, nullptr);
	g_hwnd_project = CreateWindowExW(0, WC_STATICW, L"", WS_VISIBLE | WS_CHILD | SS_LEFT,
		0, 0, 10, 10, g_hwnd_panel, (HMENU)IDC_LBL_PROJECT, g_hInst, nullptr);
	g_hwnd_default = CreateWindowExW(0, WC_STATICW, L"", WS_VISIBLE | WS_CHILD | SS_LEFT,
		0, 0, 10, 10, g_hwnd_panel, (HMENU)IDC_LBL_DEFAULT, g_hInst, nullptr);
	g_hwnd_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"",
		WS_VISIBLE | WS_CHILD | WS_VSCROLL | LBS_NOTIFY,
		0, 0, 10, 10, g_hwnd_panel, (HMENU)IDC_LIST, g_hInst, nullptr);

	make_btn(g_hwnd_panel, IDC_BTN_APPLY, L"適用して紐づけ");
	make_btn(g_hwnd_panel, IDC_BTN_SAVE, L"名前を付けて保存");
	make_btn(g_hwnd_panel, IDC_BTN_BIND, L"プロジェクトに紐づけ");
	make_btn(g_hwnd_panel, IDC_BTN_DEFAULT, L"既定に設定");
	make_btn(g_hwnd_panel, IDC_BTN_DELETE, L"削除");
	make_btn(g_hwnd_panel, IDC_BTN_FOLDER, L"フォルダ");

	host->register_window_client(PANEL_WINDOW_NAME, g_hwnd_panel);
	SendMessageW(g_hwnd_panel, WM_SIZE, 0, 0);
	update_panel_ui();
}

//---------------------------------------------------------------------
//	入口
//---------------------------------------------------------------------
COMMON_PLUGIN_TABLE common_plugin_table = {
	PLUGIN_NAME,
	L"UIプリセット (名前付き / プロジェクト紐づけ) version 1.1",
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
	g_hwnd_panel = g_hwnd_list = g_hwnd_info = g_hwnd_project = g_hwnd_default = nullptr;
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
	return &common_plugin_table;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
	ensure_plugin_dirs();
	load_config_file();
	load_presets_file();

	edit_handle = host->create_edit_handle();

	host->register_edit_menu_param(L"UIプリセット\\名前を付けて保存", nullptr, cb_save);
	host->register_edit_menu_param(L"UIプリセット\\選択を適用して紐づけ", nullptr, cb_apply);
	host->register_edit_menu_param(L"UIプリセット\\プロジェクトに紐づけ", nullptr, cb_bind);
	host->register_edit_menu_param(L"UIプリセット\\選択を既定に設定", nullptr, cb_default);
	host->register_edit_menu_param(L"UIプリセット\\選択を削除", nullptr, cb_delete);
	host->register_edit_menu_param(L"UIプリセット\\一覧をログに表示", nullptr, cb_list);
	host->register_edit_menu_param(L"UIプリセット\\設定フォルダを開く", nullptr, cb_folder);

	host->register_project_load_handler(on_project_load);
	host->register_project_save_handler(on_project_save);
	host->register_change_scene_handler(on_change_scene);

	create_panel(host);
	logf(L"[UIプリセット] 初期化完了 (presets=%zu default=%s)",
		g_presets.size(),
		g_default_preset.empty() ? L"(なし)" : g_default_preset.c_str());
}
