//----------------------------------------------------------------------------------
//	ツールバープラグイン for AviUtl ExEdit2
//----------------------------------------------------------------------------------
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include <vector>
#include <string>
#include <cstdio>

#include "plugin2.h"
#include "logger2.h" // ログ出力
#include "config2.h" // 設定関連

#define ToolbarWindowName L"ツールバー"

#define IDC_TOOLBAR  2001  // ツールバーコントロールID
#define IDC_BTN_BASE 3000  // ユーザーボタンIDのベース値

EDIT_HANDLE*   edit_handle;
LOG_HANDLE*    logger;
CONFIG_HANDLE* config;
HWND           g_hwnd_toolbar = nullptr;
static HIMAGELIST g_himl       = nullptr;

// オブジェクト作成コールバック用パラメータ
struct CreateObjectParam {
	LPCWSTR effect;
};

// 設定ファイルから読み込んだボタン定義
struct UserButton {
	bool         is_sep;    // true = 区切り線
	std::wstring label;     // ツールバー表示ラベル
	std::wstring type;      // アクション種別 ("object" or "command")
	std::wstring param;     // パラメータ (エフェクト名 or メニューID)
};

static std::vector<UserButton>   g_user_buttons;  // 読み込んだボタン定義
static std::vector<int>          g_button_indices; // ボタンID -> g_user_buttons のインデックス
static int                       g_button_size = 24; // ボタンサイズ (デフォルト: 24x24)
static std::wstring              g_tooltip_text;    // ツールチップ表示用バッファ

// 前方宣言
static int resolve_command_id(const std::wstring& param);

//---------------------------------------------------------------------
//	必要とする本体バージョン番号取得関数
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) DWORD RequiredVersion() {
	return 2003300;
}

//---------------------------------------------------------------------
//	ログ出力機能初期化関数 (未定義なら呼ばれません)
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) {
	logger = handle;
}

//---------------------------------------------------------------------
//	設定関連初期化関数 (未定義なら呼ばれません)
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle) {
	config = handle;
}

//---------------------------------------------------------------------
//	プラグインDLL初期化関数 (未定義なら呼ばれません)
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version) {
	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC  = ICC_BAR_CLASSES;
	InitCommonControlsEx(&icc);
	return true;
}

//---------------------------------------------------------------------
//	プラグインDLL解放関数 (未定義なら呼ばれません)
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void UninitializePlugin() {
	if (g_himl) {
		ImageList_Destroy(g_himl);
		g_himl = nullptr;
	}
}

//---------------------------------------------------------------------
//	ウィンドウプロシージャ
//---------------------------------------------------------------------
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	switch (message) {
	case WM_SIZE:
		// コンテナのサイズ変更に合わせてツールバーを再配置する
		if (g_hwnd_toolbar) {
			SendMessage(g_hwnd_toolbar, TB_AUTOSIZE, 0, 0);
		}
		return 0;

	case WM_NOTIFY: {
		LPNMHDR pnmhdr = (LPNMHDR)lparam;
		if (pnmhdr->code == TTN_GETDISPINFO) {
			LPNMTTDISPINFOW pttdi = (LPNMTTDISPINFOW)lparam;
			int btn_idx = (int)(pttdi->hdr.idFrom) - IDC_BTN_BASE;
			if (btn_idx >= 0 && btn_idx < (int)g_button_indices.size()) {
				int user_btn_idx = g_button_indices[btn_idx];
				const auto& btn = g_user_buttons[user_btn_idx];
					if (btn.type == L"object") {
						g_tooltip_text = btn.param; // エフェクト名を表示
					} else if (btn.type == L"command") {
						g_tooltip_text = btn.label; // ラベル名を表示
					} else {
						g_tooltip_text = btn.label;
					}
				pttdi->lpszText = const_cast<LPWSTR>(g_tooltip_text.c_str());
				pttdi->hinst   = nullptr;
			}
			return 0;
		}
		break;
	}

	case WM_COMMAND: {
		int btn_idx = LOWORD(wparam) - IDC_BTN_BASE;
		if (btn_idx >= 0 && btn_idx < (int)g_button_indices.size()) {
			int user_btn_idx = g_button_indices[btn_idx];
			const auto& btn = g_user_buttons[user_btn_idx];

			if (btn.type == L"object" && edit_handle) {
				// オブジェクト作成
				CreateObjectParam param = { btn.param.c_str() };
				edit_handle->call_edit_section_param(&param, [](void* p, EDIT_SECTION* edit) {
					auto param = static_cast<CreateObjectParam*>(p);
					if (edit->create_object(param->effect, edit->info->layer, edit->info->frame, 0)) {
						logger->log(logger, L"オブジェクトを作成しました");
					}
					else {
						logger->warn(logger, L"オブジェクトの作成に失敗しました");
					}
				});
				SetFocus(nullptr);
			}
			else if (btn.type == L"command" && edit_handle) {
				// メニューコマンド実行（メインウィンドウに WM_COMMAND を送信）
				int cmdId = resolve_command_id(btn.param);
				HWND hwndMain = edit_handle->get_host_app_window();
				if (hwndMain && cmdId > 0) {
					SendMessage(hwndMain, WM_COMMAND, cmdId, 0);
					if (logger) {
						wchar_t msg[128];
						swprintf_s(msg, L"コマンド実行: %s (ID: %d)", btn.label.c_str(), cmdId);
						logger->log(logger, msg);
					}
				}
			}
		}
		return 0;
	}
	}
	return DefWindowProc(hwnd, message, wparam, lparam);
}

//---------------------------------------------------------------------
//	設定ファイル関数群
//---------------------------------------------------------------------

// メニューを再帰的に走査してテキストからIDを検索する
static int find_menu_item_id(HMENU hmenu, const std::wstring& target) {
	if (!hmenu) return -1;
	int count = GetMenuItemCount(hmenu);
	for (int i = 0; i < count; i++) {
		MENUITEMINFOW mii = {};
		mii.cbSize    = sizeof(MENUITEMINFOW);
		mii.fMask     = MIIM_FTYPE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
		wchar_t buf[256] = {};
		mii.dwTypeData = buf;
		mii.cch       = _countof(buf);
		if (!GetMenuItemInfoW(hmenu, i, TRUE, &mii)) continue;
		if (mii.hSubMenu) {
			int id = find_menu_item_id(mii.hSubMenu, target);
			if (id > 0) return id;
		}
		// MFT_STRING は 0 なので & では判定不可。セパレータでなくテキストがあればテキスト項目
		if (!(mii.fType & MFT_SEPARATOR) && buf[0] != L'\0') {
			std::wstring text(buf);
			// タブ以降（ショートカット表示）を除去
			auto tab = text.find(L'\t');
			if (tab != std::wstring::npos) text = text.substr(0, tab);
			// '&'（アクセラレータキー）を除去
			std::wstring clean;
			for (auto c : text) { if (c != L'&') clean += c; }
			if (clean == target) return (int)mii.wID;
		}
	}
	return -1;
}

// コマンドIDを解決する（数値またはメニュー名）
static int resolve_command_id(const std::wstring& param) {
	// 全文字が数字なら数値IDとして使用（後方互換）
	if (!param.empty()) {
		bool is_num = true;
		for (auto c : param) { if (!iswdigit(c)) { is_num = false; break; } }
		if (is_num) return _wtoi(param.c_str());
	}
	// メニュー名で検索
	if (!edit_handle) return -1;
	HWND hwndMain = edit_handle->get_host_app_window();
	if (!hwndMain) return -1;
	HMENU hmenu = GetMenu(hwndMain);
	if (!hmenu) {
		if (logger) logger->warn(logger, L"メインウィンドウにメニューが見つかりません (GetMenu が nullptr)");
		return -1;
	}
	int id = find_menu_item_id(hmenu, param);
	if (id > 0 && logger) {
		wchar_t msg[256];
		swprintf_s(msg, L"メニュー名解決: '%s' -> ID: %d", param.c_str(), id);
		logger->verbose(logger, msg);
	} else if (id < 0 && logger) {
		std::wstring msg = L"メニュー名が見つかりません: '" + param + L"'";
		logger->warn(logger, msg.c_str());
	}
	return id;
}

// プラグインデータディレクトリのパスを取得する
static std::wstring get_plugin_dir() {
	std::wstring base(config->app_data_path);
	if (!base.empty() && base.back() != L'\\') base += L'\\';
	return base + L"Plugin\\m_toolbar_aviutl2";
}

// 設定ファイルのパスを取得する
static std::wstring get_config_path() {
	return get_plugin_dir() + L"\\toolbar_buttons.ini";
}

// アイコン画像のパスを取得する (<plugin_dir>\icon\<label>.bmp)
static std::wstring get_icon_path(const std::wstring& label) {
	return get_plugin_dir() + L"\\icon\\" + label + L".bmp";
}

// 必要なディレクトリを作成する (既存の場合は無視)
static void ensure_plugin_dirs() {
	std::wstring base(config->app_data_path);
	if (!base.empty() && base.back() != L'\\') base += L'\\';
	CreateDirectoryW((base + L"Plugin").c_str(), nullptr);
	CreateDirectoryW((base + L"Plugin\\m_toolbar_aviutl2").c_str(), nullptr);
	CreateDirectoryW((base + L"Plugin\\m_toolbar_aviutl2\\icon").c_str(), nullptr);
}

// アイコンフォルダにヘルプファイルを生成する
static void write_help_file(const std::wstring& icon_dir) {
	std::wstring help_path = icon_dir + L"\\うまく表示されない場合.txt";
	// 既に存在する場合は上書きしない
	if (GetFileAttributesW(help_path.c_str()) != INVALID_FILE_ATTRIBUTES) return;

	FILE* fp = nullptr;
	if (_wfopen_s(&fp, help_path.c_str(), L"w, ccs=UTF-8") != 0 || !fp) return;
	fputws(
		L"■ ツールバーに画像が表示されない場合の対処法\n"
		L"\n"
		L"【推奨される画像形式】\n"
		L"・24ビット ビットマップ (.bmp)\n"
		L"・推奨サイズ: 16×16 ピクセル (他のサイズでも自動リサイズされます)\n"
		L"・toolbar_buttons.ini の button_size= でボタンサイズを変更できます (8～64)\n"
		L"\n"
		L"【手順1】Windows ペイントで画像を開く\n"
		L"1. 画像ファイルを右クリック → [編集] (ペイントで開く)\n"
		L"2. [ファイル] → [名前を付けて保存] → [BMP 画像]\n"
		L"3. ファイル名の下にある [ファイルの種類] で「24ビット ビットマップ」を選択\n"
		L"4. このフォルダ (icon) に保存\n"
		L"\n"
		L"【手順2】ファイル名を確認\n"
		L"・ファイル名は toolbar_buttons.ini の「ラベル」部分と完全一致する必要があります\n"
		L"  例: toolbar_buttons.ini に「画像,画像ファイル」と書かれている場合\n"
		L"      → ファイル名は「画像.bmp」にする\n"
		L"\n"
		L"・ファイル名に使えない文字 (\\, /, :, *, ?, \", <, >, |) が含まれていないか確認\n"
		L"\n"
		L"【手順3】AviUtl2 を再起動\n"
		L"・画像を追加・変更した後は AviUtl2 を再起動してください\n"
		L"\n"
		L"【トラブルシューティング】\n"
		L"・ログウィンドウ (表示 → ログ) で \"BMP検出\" や \"画像を読み込みました\" が\n"
		L"  表示されているか確認してください\n"
		L"・\"LoadImageW失敗\" が表示される場合は、ペイントで保存し直してください\n"
		L"\n"
		L"【配置場所】\n"
		L"・このフォルダ (Plugin\\m_toolbar_aviutl2\\icon) に .bmp ファイルを配置\n"
		L"・設定ファイル: Plugin\\m_toolbar_aviutl2\\toolbar_buttons.ini\n",
		fp);
	fclose(fp);
}

// デフォルト設定ファイルを生成する
static void write_default_config(const std::wstring& path) {
	FILE* fp = nullptr;
	if (_wfopen_s(&fp, path.c_str(), L"w, ccs=UTF-8") != 0 || !fp) return;
	fputws(
		L"# ツールバーボタン設定\n"
		L"# 形式: ラベル,タイプ,パラメータ\n"
		L"#   タイプ: object (オブジェクト作成) / command (メニューコマンド実行)\n"
		L"#   パラメータ: エフェクト名 または メニュー名 (例: 元に戻す) または メニューID (10進数)\n"
		L"# 区切り線: ---\n"
		L"# 空行・# で始まる行は無視されます\n"
		L"\n"
		L"# ボタンサイズ (デフォルト: 24、範囲: 8～64)\n"
		L"button_size=24\n"
		L"\n"
		L"# メディアオブジェクト\n"
		L"動画,object,動画ファイル\n"
		L"画像,object,画像ファイル\n"
		L"音声,object,音声ファイル\n"
		L"モデル,object,モデルファイル\n"
		L"---\n"
		L"テキスト,object,テキスト\n"
		L"図形,object,図形\n"
		L"シーン,object,シーン\n"
		L"---\n"
		L"フレーム,object,フレームバッファ\n"
		L"オーディオ,object,オーディオバッファ\n"
		L"直前,object,直前オブジェクト\n"
		L"パーティクル,object,パーティクル出力\n"
		L"\n"
		L"# フィルタ効果（基本）\n"
		L"部分,object,部分フィルタ\n"
		L"走査線,object,走査線\n"
		L"周辺ボケ,object,周辺ボケ光量\n"
		L"\n"
		L"# 制御オブジェクト\n"
		L"グループ,object,グループ制御\n"
		L"グループ(音声),object,グループ制御(音声)\n"
		L"カメラ,object,カメラ制御\n"
		L"時間制御,object,時間制御(オブジェクト)\n"
		L"画像合成,object,画像合成(オブジェクト)\n"
		L"\n"
		L"# エフェクト（描画）\n"
		L"音声波形表示,object,音声波形表示\n"
		L"集中線,object,集中線\n"
		L"カウンター,object,カウンター\n"
		L"レンズフレア,object,レンズフレア\n"
		L"雲,object,雲\n"
		L"星,object,星\n"
		L"雪,object,雪\n"
		L"雨,object,雨\n"
		L"ランダム小物,object,ランダム小物配置(カメラ制御)\n"
		L"ライン,object,ライン(移動軌跡)\n"
		L"扇型,object,扇型\n"
		L"多角形,object,多角形\n"
		L"フレア,object,フレア\n"
		L"水面,object,水面\n"
		L"\n"
		L"# 色調整・クリッピング\n"
		L"色調補正,object,色調補正\n"
		L"グラデーション,object,グラデーション\n"
		L"単色化,object,単色化\n"
		L"特定色,object,特定色域変換\n"
		L"拡張色,object,拡張色設定\n"
		L"クリッピング,object,クリッピング\n"
		L"斜めクリッピング,object,斜めクリッピング\n"
		L"扇クリッピング,object,扇クリッピング\n"
		L"マスク,object,マスク\n"
		L"\n"
		L"# ぼかし・ブラー\n"
		L"ぼかし,object,ぼかし\n"
		L"レンズ,object,レンズブラー\n"
		L"放射,object,放射ブラー\n"
		L"方向,object,方向ブラー\n"
		L"モーション,object,モーションブラー\n"
		L"\n"
		L"# 発光・色ずれ\n"
		L"グロー,object,グロー\n"
		L"発光,object,発光\n"
		L"閃光,object,閃光\n"
		L"拡散光,object,拡散光\n"
		L"色ずれ,object,色ずれ\n"
		L"\n"
		L"# 画像処理\n"
		L"エッジ,object,エッジ抽出\n"
		L"波紋,object,波紋\n"
		L"ラスター,object,ラスター\n"
		L"ディスプレイ,object,ディスプレイスメントマップ\n"
		L"画像合成,object,画像合成\n"
		L"シャープ,object,シャープ\n"
		L"ノイズ除去,object,ノイズ除去\n"
		L"インター,object,インターレース解除\n"
		L"ノイズ,object,ノイズ\n"
		L"モザイク,object,モザイク\n"
		L"\n"
		L"# シーンチェンジ\n"
		L"シーンチェンジ,object,シーンチェンジ\n"
		L"発光(シーン),object,発光(シーンチェンジ)\n"
		L"レンズ(シーン),object,レンズブラー(シーンチェンジ)\n"
		L"ドア(シーン),object,ドア(シーンチェンジ)\n"
		L"起き上がる(シーン),object,起き上がる(シーンチェンジ)\n"
		L"リール(シーン),object,リール回転(シーンチェンジ)\n"
		L"図形ワイプ(シーン),object,図形ワイプ(シーンチェンジ)\n"
		L"図形で隠す(シーン),object,図形で隠す(シーンチェンジ)\n"
		L"図形で隠す(放射)(シーン),object,図形で隠す(放射)(シーンチェンジ)\n"
		L"砕け散る(シーン),object,砕け散る(シーンチェンジ)\n"
		L"ページ(シーン),object,ページめくり(シーンチェンジ)\n"
		L"\n"
		L"# 音声・その他\n"
		L"ローテーション,object,ローテーション\n"
		L"反転,object,反転\n"
		L"音量フェード,object,音量フェード\n"
		L"モノラル化,object,モノラル化\n"
		L"音量調整,object,音量調整\n"
		L"リール回転,object,リール回転\n"
		L"粒子化,object,粒子化\n"
		L"\n"
		L"# === メニューコマンド ===\n"
		L"\n"
		L"# 表示切り替え\n"
		L"BPM表示,command,グリッド(BPM)の表示\n"
		L"XY軸表示,command,グリッド(XY軸)の表示\n"
		L"カメラ軸表示,command,グリッド(カメラ制御)の表示\n"
		L"波形表示,command,音声波形の表示\n"
		L"カメラ視点,command,編集視点(カメラ制御)表示\n"
		L"クリッピング視覚化,command,クリッピングオブジェクトの視覚化\n"
		L"音量メーター,command,音量レベルメーターの表示\n"
		L"\n"
		L"# 編集操作（基本）\n"
		L"元に戻す,command,元に戻す\n"
		L"やり直し,command,やり直し\n"
		L"キャッシュを破棄,command,キャッシュを破棄\n"
		L"切り取り,command,オブジェクトを切り取り\n"
		L"コピー,command,オブジェクトをコピー\n"
		L"貼り付け,command,オブジェクトを貼り付け\n"
		L"削除,command,オブジェクトを削除\n"
		L"複製,command,オブジェクトを複製\n"
		L"分割,command,オブジェクトを分割\n"
		L"\n"
		L"# オブジェクト操作（配置）\n"
		L"左詰め,command,オブジェクトを左側に詰める\n"
		L"切り取り詰め,command,オブジェクトを切り取りして詰める\n"
		L"切り出し,command,オブジェクトを切り出し\n"
		L"長さ変更,command,オブジェクトの長さを変更\n"
		L"名前変更,command,オブジェクト名を変更\n"
		L"表示切替,command,オブジェクトの表示・非表示\n"
		L"\n"
		L"# オブジェクト操作（その他）\n"
		L"選択範囲を切り取り,command,選択範囲を切り取り\n"
		L"選択範囲を切り取り詰め,command,選択範囲を切り取りして詰める\n"
		L"画像コピー,command,クリップボードにイメージをコピー\n"
		L"画像コピー透過,command,クリップボードにイメージをコピー(背景透過)\n"
		L"中間点を追加,command,オブジェクトの中間点を追加\n"
		L"中間点を削除,command,オブジェクトの中間点を削除\n"
		L"グループ化,command,オブジェクトのグループ化\n"
		L"グループ解除,command,オブジェクトのグループ解除\n"
		L"\n"
		L"# 選択操作\n"
		L"選択開始フレーム設定,command,現在のフレームを選択開始フレームにする\n"
		L"選択終了フレーム設定,command,現在のフレームを選択終了フレームにする\n"
		L"フレーム選択解除,command,フレーム範囲選択を解除\n"
		L"全選択,command,全てのオブジェクトを選択\n"
		L"以降のオブジェクト選択,command,現在のフレーム以降に開始するオブジェクトを選択\n"
		L"以前のオブジェクト選択,command,現在のフレーム以前に終了するオブジェクトを選択\n"
		L"フレーム上のオブジェクト選択,command,現在のフレーム上にあるオブジェクトを選択\n"
		L"\n"
		L"# オブジェクト整列\n"
		L"開始点を揃える,command,オブジェクトの開始点を揃える\n"
		L"終了点を揃える,command,オブジェクトの終了点を揃える\n"
		L"開始終了点を揃える,command,オブジェクトの開始終了点を揃える\n"
		L"詰めて開始位置揃え,command,オブジェクトを詰めて開始位置を揃える\n"
		L"詰めて終了位置揃え,command,オブジェクトを詰めて終了位置を揃える\n"
		L"均等配置,command,オブジェクトを均等配置\n"
		L"\n"
		L"# オブジェクト選択・移動\n"
		L"現在フレームのオブジェクト選択,command,現在のフレームのオブジェクトを選択\n"
		L"次のオブジェクト選択,command,次のオブジェクトを選択\n"
		L"前のオブジェクト選択,command,前のオブジェクトを選択\n"
		L"\n"
		L"# メディア操作\n"
		L"上に移動,command,メディアを上に移動\n"
		L"下に移動,command,メディアを下に移動\n"
		L"左に移動,command,メディアを左に移動\n"
		L"右に移動,command,メディアを右に移動\n"
		L"拡大,command,メディアを拡大\n"
		L"縮小,command,メディアを縮小\n"
		L"左回転,command,メディアを左回転\n"
		L"右回転,command,メディアを右回転\n"
		L"\n"
		L"# テキスト・レイヤー操作\n"
		L"テキスト編集,command,テキストを編集\n"
		L"レイヤーロック,command,現在のレイヤーをロック\n"
		L"レイヤー表示切替,command,現在のレイヤーを表示/非表示\n"
		L"レイヤー名変更,command,現在のレイヤー名を変更\n"
		L"レイヤー挿入,command,現在のレイヤーの上に挿入\n"
		L"レイヤー削除,command,現在のレイヤーを削除\n"
		L"\n"
		L"# シーン操作\n"
		L"シーン新規作成,command,シーンを新規作成\n"
		L"シーン複製,command,現在のシーンを複製\n"
		L"シーン削除,command,現在のシーンを削除\n"
		L"シーン変更,command,現在のシーンを変更\n"
		L"シーン設定,command,現在のシーンを設定\n"
		L"\n"
		L"# プロジェクト操作\n"
		L"新規,command,プロジェクトを新規作成\n"
		L"開く,command,プロジェクトを開く\n"
		L"閉じる,command,プロジェクトを閉じる\n"
		L"保存,command,プロジェクトを保存\n"
		L"別名保存,command,プロジェクトを別名で保存\n"
		L"\n"
		L"# ファイル出力\n"
		L"AVI出力,command,AVIファイル出力\n"
		L"PNG出力,command,PNGファイル出力\n"
		L"JPEG出力,command,JPEGファイル出力\n"
		L"連番出力,command,連番ファイル出力\n"
		L"WAV出力32bit,command,WAVファイル出力 (32bit float)\n"
		L"WAV出力16bit,command,WAVファイル出力 (16bit short)\n"
		L"\n"
		L"# 設定メニュー\n"
		L"ショートカット設定,command,ショートカットキーの設定\n"
		L"オブジェクト追加メニュー設定,command,オブジェクト追加メニューの設定\n"
		L"トラックバー移動メニュー設定,command,トラックバー移動メニューの設定\n"
		L"時間制御メニュー設定,command,時間制御メニューの設定\n"
		L"プリセット色メニュー設定,command,プリセット色メニューの設定\n"
		L"フォントメニュー設定,command,フォントメニューの設定\n"
		L"入力プラグイン設定,command,入力プラグインの設定\n"
		L"バックアップ履歴設定,command,バックアップ履歴の設定\n"
		L"キャッシュサイズ設定,command,キャッシュサイズの設定\n"
		L"ファイル出力設定,command,ファイル出力の設定\n"
		L"共通操作設定,command,共通操作の設定\n"
		L"画面設定,command,画面の設定\n"
		L"ログ設定,command,ログの設定\n"
		L"言語設定,command,言語の設定\n"
		L"\n"
		L"# 情報・フォルダ\n"
		L"プラグイン情報,command,プラグイン情報\n"
		L"スクリプト情報,command,スクリプト情報\n"
		L"パッケージ情報,command,パッケージ情報\n"
		L"アプリデータフォルダ,command,アプリケーションデータフォルダ\n"
		L"スクリプトフォルダ,command,スクリプトフォルダ\n"
		L"プラグインフォルダ,command,プラグインフォルダ\n"
		L"図形フォルダ,command,図形ファイルフォルダ\n"
		L"遷移画像フォルダ,command,遷移画像ファイルフォルダ\n"
		L"エイリアスフォルダ,command,エイリアスファイルフォルダ\n"
		L"オブジェクトプリセットフォルダ,command,オブジェクトプリセットフォルダ\n"
		L"初期値プリセットフォルダ,command,初期値・プリセットフォルダ\n"
		L"バックアップフォルダ,command,バックアップファイルフォルダ\n"
		L"ログフォルダ,command,ログファイルフォルダ\n"
		L"言語フォルダ,command,言語ファイルフォルダ\n"
		L"AviUtlのお部屋,command,AviUtlのお部屋\n"
		L"バージョン情報,command,バージョン情報\n",
		fp);
	fclose(fp);
}

// 設定ファイルを読み込んで g_user_buttons に格納する
static void load_config(const std::wstring& path) {
	g_user_buttons.clear();
	g_button_size = 16; // デフォルト値にリセット
	FILE* fp = nullptr;
	if (_wfopen_s(&fp, path.c_str(), L"r, ccs=UTF-8") != 0 || !fp) return;
	wchar_t line[512];
	while (fgetws(line, _countof(line), fp)) {
		// 末尾の改行を除去
		int len = (int)wcslen(line);
		while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r'))
			line[--len] = L'\0';
		// 空行・コメント行をスキップ
		if (len == 0 || line[0] == L'#') continue;
		// button_size= 行を解析
		if (wcsncmp(line, L"button_size=", 12) == 0) {
			int size = _wtoi(line + 12);
			if (size >= 8 && size <= 64) { // 8～64の範囲で制限
				g_button_size = size;
			}
			continue;
		}
		// 区切り線
		if (wcscmp(line, L"---") == 0) {
			g_user_buttons.push_back({ true, L"", L"", L"" });
			continue;
		}
		// カンマで分割（ラベル,type,param）
		wchar_t* comma1 = wcschr(line, L',');
		if (!comma1 || comma1 == line) continue;
		wchar_t* comma2 = wcschr(comma1 + 1, L',');
		if (!comma2) continue;

		std::wstring label(line, comma1);
		std::wstring type(comma1 + 1, comma2);
		std::wstring param(comma2 + 1);
		if (label.empty() || type.empty() || param.empty()) continue;
		g_user_buttons.push_back({ false, label, type, param });
	}
	fclose(fp);
}

//---------------------------------------------------------------------
//	プラグイン登録関数
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
	// プラグインの情報を設定
	host->set_plugin_information(L"Aviutl2にツールバーを追加するプラグイン(byゆらてぃー) version 1.00");

	// コンテナウィンドウクラスの登録
	WNDCLASSEXW wcex = {};
	wcex.cbSize        = sizeof(WNDCLASSEX);
	wcex.lpszClassName = ToolbarWindowName;
	wcex.lpfnWndProc   = wnd_proc;
	wcex.hInstance     = GetModuleHandle(0);
	wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	if (!RegisterClassEx(&wcex)) {
		return;
	}

	// コンテナウィンドウの作成
	// 親ウィンドウの指定無しでWS_CHILDが作れないので一旦WS_POPUPで作成しています
	auto hwnd = CreateWindowEx(
		0,
		ToolbarWindowName,
		ToolbarWindowName,
		WS_POPUP,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		nullptr, nullptr,
		GetModuleHandle(0),
		nullptr);
	if (!hwnd) {
		return;
	}

	// ツールバーコントロールの作成
	g_hwnd_toolbar = CreateWindowEx(
		0,
		TOOLBARCLASSNAME,
		nullptr,
		WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_WRAPABLE | TBSTYLE_TOOLTIPS | CCS_NODIVIDER,
		0, 0, 0, 0,
		hwnd,
		(HMENU)IDC_TOOLBAR,
		GetModuleHandle(0),
		nullptr);
	if (!g_hwnd_toolbar) {
		return;
	}

	// ツールバーの初期化
	SendMessage(g_hwnd_toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

	// 設定ファイルを読み込む
	if (config) {
		ensure_plugin_dirs();
		// ヘルプファイルを生成 (初回のみ)
		write_help_file(get_plugin_dir() + L"\\icon");
		auto cfg_path = get_config_path();
		if (GetFileAttributesW(cfg_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
			write_default_config(cfg_path);
			if (logger) logger->log(logger, L"toolbar_buttons.ini を生成しました");
		}
		load_config(cfg_path);
	}

	// イメージリストのサイズを設定から取得したサイズで作成
	if (g_himl) { ImageList_Destroy(g_himl); }
	g_himl = ImageList_Create(g_button_size, g_button_size, ILC_COLOR32, 0, 16);
	SendMessage(g_hwnd_toolbar, TB_SETIMAGELIST, 0, (LPARAM)g_himl);
	SendMessage(g_hwnd_toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);

	// 読み込んだボタン定義を元にツールバーを動的に構築する
	g_button_indices.clear();
	std::vector<TBBUTTON> tb_buttons;
	int btn_id = IDC_BTN_BASE;
	for (size_t i = 0; i < g_user_buttons.size(); i++) {
		const auto& btn = g_user_buttons[i];
		if (btn.is_sep) {
			TBBUTTON sep = { 0, 0, TBSTATE_ENABLED, BTNS_SEP, {0,0}, 0, 0 };
			tb_buttons.push_back(sep);
		} else {
			g_button_indices.push_back((int)i); // ボタンID -> g_user_buttons インデックス

			// ボタン表示用のラベル文字列 (ツールチップはWM_NOTIFYで処理)
			std::wstring label_z = btn.label + L'\0';
			int label_idx = (int)SendMessage(g_hwnd_toolbar, TB_ADDSTRING, 0, (LPARAM)label_z.c_str());

			// ラベル名に対応するBMP画像を読み込む
			int img_idx = I_IMAGENONE;
			if (config) {
				std::wstring icon_path = get_icon_path(btn.label);
				// ファイル存在確認
				if (GetFileAttributesW(icon_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
					// ファイルを開いてBMPヘッダーを検証
					HANDLE hFile = CreateFileW(icon_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
					if (hFile != INVALID_HANDLE_VALUE) {
						BITMAPFILEHEADER bmfh;
						DWORD dwRead;
						if (ReadFile(hFile, &bmfh, sizeof(BITMAPFILEHEADER), &dwRead, nullptr) && dwRead == sizeof(BITMAPFILEHEADER)) {
							if (bmfh.bfType == 0x4D42) { // 'BM'
								// BMPヘッダー読み込み
								BITMAPINFOHEADER bmih;
								if (ReadFile(hFile, &bmih, sizeof(BITMAPINFOHEADER), &dwRead, nullptr) && dwRead == sizeof(BITMAPINFOHEADER)) {
									CloseHandle(hFile);

									if (logger) {
										wchar_t msg[256];
										swprintf_s(msg, L"BMP検出: %dx%d, %dbit", bmih.biWidth, bmih.biHeight, bmih.biBitCount);
										logger->verbose(logger, msg);
									}

									// LoadImageWで読み込み（複数の方法を試行）
									HBITMAP hBmp = nullptr;

									// 方法1: 標準的な読み込み
									hBmp = (HBITMAP)LoadImageW(nullptr, icon_path.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);

									// 方法2: CREATEDIBSECTION
									if (!hBmp) {
										hBmp = (HBITMAP)LoadImageW(nullptr, icon_path.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
									}

									if (hBmp) {
										// 設定されたサイズにリサイズ
										HBITMAP hBmpResized = (HBITMAP)CopyImage(hBmp, IMAGE_BITMAP, g_button_size, g_button_size, LR_COPYDELETEORG);
										if (hBmpResized) {
											int added = ImageList_Add(g_himl, hBmpResized, nullptr);
											if (added >= 0) {
												img_idx = added;
												if (logger) {
													std::wstring msg = L"画像を読み込みました: " + btn.label;
													logger->log(logger, msg.c_str());
												}
											} else if (logger) {
												logger->warn(logger, L"ImageList_Add失敗");
											}
											DeleteObject(hBmpResized);
										} else {
											DeleteObject(hBmp);
										}
									} else if (logger) {
										logger->warn(logger, L"LoadImageW失敗: すべての方法が失敗しました");
									}
								} else {
									CloseHandle(hFile);
									if (logger) logger->warn(logger, L"BMPヘッダー読み込み失敗");
								}
							} else {
								CloseHandle(hFile);
								if (logger) logger->warn(logger, L"BMP形式ではありません");
							}
						} else {
							CloseHandle(hFile);
							if (logger) logger->warn(logger, L"ファイル読み込み失敗");
						}
					} else if (logger) {
						logger->warn(logger, L"ファイルオープン失敗");
					}
				} else if (logger) {
					std::wstring msg = L"画像ファイルが見つかりません: " + icon_path;
					logger->verbose(logger, msg.c_str());
				}
			}
			// 画像なし → テキスト表示 / 画像あり → 画像のみ表示 (ツールチップはWM_NOTIFYで対応)
			BYTE style = BTNS_BUTTON | BTNS_AUTOSIZE;
			if (img_idx == I_IMAGENONE) style |= BTNS_SHOWTEXT;
			TBBUTTON tb = { img_idx, btn_id, TBSTATE_ENABLED, style, {0,0}, 0, (INT_PTR)label_idx };
			tb_buttons.push_back(tb);
			btn_id++; // 次のボタンID
		}
	}
	if (!tb_buttons.empty()) {
		SendMessage(g_hwnd_toolbar, TB_ADDBUTTONS, (WPARAM)tb_buttons.size(), (LPARAM)tb_buttons.data());
	}
	SendMessage(g_hwnd_toolbar, TB_AUTOSIZE, 0, 0);

	// ウィンドウクライアントとして登録
	host->register_window_client(ToolbarWindowName, hwnd);

	// 編集ハンドルを作成
	edit_handle = host->create_edit_handle();
}
