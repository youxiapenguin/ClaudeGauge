/*********************************************************
 * Kimi for Coding 用量桌面工具（独立程序，由 Claude 用量工具改造）
 * ------------------------------------------------------
 * 两种显示形态，右键菜单切换并持久化到 config.ini：
 *   形态1 悬浮窗：分层窗口（UpdateLayeredWindow + GDI+），圆角卡片 + 柔和
 *                 阴影 + 抗锯齿，紧凑；标题粗体 + 三行细体 + 圆角进度条；
 *                 白/黑两套主题；透明度/置顶/锁定位置可调。
 *   形态2 任务栏条：SetParent 嵌进 Shell_TrayWnd，真透明（LWA_COLORKEY），
 *                 两行（套餐名 + 精简用量），悬停弹 tooltip 看重置时间。
 * 数据：后台线程每 REFRESH_MINUTES 用 WinINet 原生 HTTPS 调一次
 *       https://api.kimi.com/coding/v1/usages（Bearer 认证，nlohmann/json 解析）。
 *       返回的数值全是字符串形式的百分比：usage=本周配额、limits[0]=5 小时
 *       滑动窗口、parallel=并发会话、totalQuota=总池。实测纯查询不耗配额。
 * 嵌入任务栏的手法参考开源项目 TrafficMonitor 的同类公开 Win32 思路，
 * 本程序为独立重写，未复制其源码。
 * 编译：mingw-w64（见 build.sh，链接 -lgdiplus -lwininet）
 *********************************************************/
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <wininet.h>
#include <algorithm>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include "json.hpp"

using namespace Gdiplus;

// ====== 配置 ======
static const int REFRESH_MINUTES = 10;
static const int STRIP_WIDTH_96  = 240;    // 任务栏条每账户宽度（96 DPI）
static const int FLOAT_W_96      = 196;    // 悬浮窗宽（含阴影边距）
static const int FLOAT_H_96      = 108;    // 单账户悬浮窗总高
static const int ACCOUNT_H_96    = 92;     // 每多一个账户增加的高度
static const int SHADOW_M_96     = 8;      // 阴影边距

static const COLORREF STRIP_COLORKEY = RGB(255, 0, 255);
static const COLORREF STRIP_TEXT     = RGB(255, 255, 255);
static const COLORREF STRIP_WARN     = RGB(255, 150, 140);

// 主题：单色系，标题=深/醒目，正文=同色系浅一档（拉开可区分）。
// 悬浮窗用 f* 颜色（按底色深浅搭配）；任务栏底是深色，用 tb* 亮色保证清晰。
struct Theme {
    const wchar_t* name;
    Color    fbg, ftitle, fbody, fbarFg, fbarBg;  // 悬浮窗
    COLORREF tbTitle, tbBody;                     // 任务栏（行1=深/亮，行2=浅）
};
static const Theme THEMES[] = {
    { L"红(白底)",   Color(255,0xf6,0xf5,0xf1), Color(255,0xb0,0x2a,0x1e), Color(255,0xc4,0x6a,0x60),
      Color(255,0xb0,0x2a,0x1e), Color(255,0xe2,0xd8,0xd4), RGB(0xff,0x6f,0x61), RGB(0xd8,0x90,0x89) },
    { L"橘黄(黑底)", Color(255,0x1c,0x1c,0x1d), Color(255,0xf5,0xa6,0x23), Color(255,0xbe,0x90,0x4a),
      Color(255,0xf5,0xa6,0x23), Color(255,0x3a,0x30,0x16), RGB(0xf5,0xa6,0x23), RGB(0xbe,0x90,0x4a) },
    { L"蓝(黑底)",   Color(255,0x16,0x18,0x1d), Color(255,0x5a,0xa8,0xff), Color(255,0x86,0x9c,0xc0),
      Color(255,0x5a,0xa8,0xff), Color(255,0x20,0x30,0x40), RGB(0x5a,0xa8,0xff), RGB(0x8f,0x9b,0xbf) },
    { L"绿(黑底)",   Color(255,0x14,0x19,0x1a), Color(255,0x4f,0xd1,0x6a), Color(255,0x86,0xa8,0x8c),
      Color(255,0x4f,0xd1,0x6a), Color(255,0x1d,0x33,0x22), RGB(0x4f,0xd1,0x6a), RGB(0x86,0xb0,0x90) },
    { L"墨绿(浅底)", Color(255,0xee,0xf2,0xec), Color(255,0x14,0x54,0x3a), Color(255,0x55,0x82,0x6a),
      Color(255,0x14,0x54,0x3a), Color(255,0xcd,0xd9,0xcf), RGB(0x3f,0xae,0x7a), RGB(0x88,0xb8,0x9e) },
    { L"紫(黑底)",   Color(255,0x19,0x16,0x1f), Color(255,0xb0,0x7c,0xf0), Color(255,0x90,0x80,0xb0),
      Color(255,0xb0,0x7c,0xf0), Color(255,0x2a,0x23,0x36), RGB(0xb0,0x7c,0xf0), RGB(0x9a,0x8a,0xc0) },
    { L"橙黄(黑底)", Color(255,0x1c,0x1a,0x17), Color(255,0xff,0x8a,0x3d), Color(255,0xcf,0x8a,0x5a),
      Color(255,0xff,0x8a,0x3d), Color(255,0x3a,0x2a,0x1a), RGB(0xff,0x8a,0x3d), RGB(0xcf,0x8a,0x5a) },
};
static const int THEME_COUNT = 7;

static const wchar_t* RUN_KEY  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_NAME = L"ClaudeTaskbar";

enum {
    ID_MODE_FLOAT = 1001, ID_MODE_TASKBAR,
    ID_TOPMOST, ID_LOCKPOS,
    ID_OPA_100, ID_OPA_90, ID_OPA_80, ID_OPA_70, ID_OPA_60,
    ID_AUTOSTART, ID_REFRESH_NOW, ID_ABOUT, ID_EXIT,
    ID_THEME_BASE = 1100,   // 主题 i = ID_THEME_BASE + i
};
#define WM_REFRESH (WM_APP + 1)
#define WM_TRAY    (WM_APP + 2)
// ==================

static std::mutex        g_mtx;
static std::atomic<bool> g_stop{false};
static HANDLE            g_wake = nullptr;   // auto-reset 事件：唤醒 worker（立即刷新/退出）
static HWND              g_hwnd       = nullptr;
static UINT              g_taskbarMsg = 0;
static HINSTANCE         g_hInst      = nullptr;
static HWND              g_tip        = nullptr;
static ULONG_PTR         g_gdip       = 0;
static NOTIFYICONDATAW   g_nid{};
static HICON             g_trayIcon   = nullptr;

struct AccountCfg {
    std::wstring name;   // 显示名（如"大号"），空则显示套餐名
    std::wstring key;    // Kimi for Coding API Key
};

struct Config {
    bool taskbarMode = false;
    int  themeIdx    = 0;       // 0..THEME_COUNT-1
    bool alwaysOnTop = true;
    bool lockPos     = false;
    int  transparency = 90;     // 百分比
    int  x = 80, y = 80;
    std::vector<AccountCfg> accounts;   // 支持多个 key：api_key, api_key_2...
};
static Config g_cfg;

// 单个账户的实时数据
struct AccountData {
    std::wstring name;                                // 账户显示名
    std::wstring fivehPct = L"--", weekPct = L"--", poolPct = L"--";
    std::wstring fivehRst, weekRst;                   // 重置时刻（本地 "07-23 17:18"），总池无
    std::wstring plan = L"Kimi";
    int fivehRemain = -1, weekRemain = -1;            // 距重置剩余分钟，-1=未知/已过期
    int parUsed = 0, parLimit = 0;                    // 并发会话 当前/上限
    bool ok = false;
};
static std::vector<AccountData> g_accounts;
static int g_accountCount = 0;   // 上次成功渲染时的账户数（用于窗口尺寸/位置计算）

// 剩余分钟 -> "剩 2h31m" / "剩 5天3h" / "剩 12m"（-1 返回空）
// compactDays=true 时多天只给"剩 N天"（悬浮窗行宽有限，避免和长标签 WEEKLY·SONNET 挤）
static std::wstring RemainText(int m, bool compactDays = false) {
    if (m < 0) return L"";
    if (m >= 1440) { int d = m / 1440, h = (m % 1440) / 60;
        if (compactDays) return L"剩 " + std::to_wstring(d) + L"天";
        return L"剩 " + std::to_wstring(d) + L"天" + (h ? std::to_wstring(h) + L"h" : L""); }
    if (m >= 60) { int h = m / 60, mm = m % 60; return L"剩 " + std::to_wstring(h) + L"h" + std::to_wstring(mm) + L"m"; }
    return L"剩 " + std::to_wstring(m) + L"m";
}

// ---------- 日志 ----------
static void Log(const std::string& s) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring lp = std::wstring(tmp) + L"ClaudeGauge.log";
    // 超过 256KB 就清空重写，防止常驻几个月日志无限膨胀
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    bool tooBig = GetFileAttributesExW(lp.c_str(), GetFileExInfoStandard, &fa)
                  && fa.nFileSizeLow > 256 * 1024;
    HANDLE h = CreateFileW(lp.c_str(),
        FILE_APPEND_DATA | (tooBig ? GENERIC_WRITE : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        tooBig ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    std::string line = s + "\r\n"; DWORD w = 0;
    WriteFile(h, line.c_str(), (DWORD)line.size(), &w, nullptr);
    CloseHandle(h);
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring ExeDirFile(const wchar_t* name) {
    wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p = p.substr(0, slash + 1);
    return p + name;
}
static std::wstring ExePath() {
    wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf);
}

// ---------- config.ini ----------
static std::wstring CfgPath() { return ExeDirFile(L"config.ini"); }
static void LoadConfig() {
    std::wstring p = CfgPath(); wchar_t buf[64];
    GetPrivateProfileStringW(L"general", L"mode", L"floating", buf, 64, p.c_str());
    g_cfg.taskbarMode = (lstrcmpiW(buf, L"taskbar") == 0);
    g_cfg.themeIdx = (int)GetPrivateProfileIntW(L"general", L"theme", 0, p.c_str());
    if (g_cfg.themeIdx < 0 || g_cfg.themeIdx >= THEME_COUNT) g_cfg.themeIdx = 0;
    g_cfg.alwaysOnTop = GetPrivateProfileIntW(L"general", L"topmost", 1, p.c_str()) != 0;
    g_cfg.lockPos     = GetPrivateProfileIntW(L"general", L"lock", 0, p.c_str()) != 0;
    g_cfg.transparency = (int)GetPrivateProfileIntW(L"general", L"transparency", 90, p.c_str());
    if (g_cfg.transparency < 30) g_cfg.transparency = 30;
    if (g_cfg.transparency > 100) g_cfg.transparency = 100;
    g_cfg.x = (int)GetPrivateProfileIntW(L"general", L"x", 80, p.c_str());
    g_cfg.y = (int)GetPrivateProfileIntW(L"general", L"y", 80, p.c_str());

    // 读取多个 key：api_key / name, api_key_2 / name_2, ... 一直到 api_key_10 / name_10
    g_cfg.accounts.clear();
    wchar_t kb[512], nb[64];
    for (int i = 1; i <= 10; ++i) {
        wchar_t keyName[32], nameName[32];
        if (i == 1) {
            wcscpy(keyName, L"api_key");
            wcscpy(nameName, L"name");
        } else {
            wsprintfW(keyName, L"api_key_%d", i);
            wsprintfW(nameName, L"name_%d", i);
        }
        GetPrivateProfileStringW(L"kimi", keyName, L"", kb, 512, p.c_str());
        if (wcslen(kb) == 0) continue;   // 跳过空位，允许中间有空行
        AccountCfg ac;
        ac.key = kb;
        GetPrivateProfileStringW(L"kimi", nameName, L"", nb, 64, p.c_str());
        ac.name = nb;
        g_cfg.accounts.push_back(ac);
    }
}
static void SaveConfig() {
    std::wstring p = CfgPath(); wchar_t n[16];
    WritePrivateProfileStringW(L"general", L"mode", g_cfg.taskbarMode ? L"taskbar" : L"floating", p.c_str());
    wsprintfW(n, L"%d", g_cfg.themeIdx); WritePrivateProfileStringW(L"general", L"theme", n, p.c_str());
    WritePrivateProfileStringW(L"general", L"topmost", g_cfg.alwaysOnTop ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"general", L"lock", g_cfg.lockPos ? L"1" : L"0", p.c_str());
    wsprintfW(n, L"%d", g_cfg.transparency); WritePrivateProfileStringW(L"general", L"transparency", n, p.c_str());
    wsprintfW(n, L"%d", g_cfg.x); WritePrivateProfileStringW(L"general", L"x", n, p.c_str());
    wsprintfW(n, L"%d", g_cfg.y); WritePrivateProfileStringW(L"general", L"y", n, p.c_str());
    // [kimi] api_key / name 只在 LoadConfig 读，这里绝不回写，避免把 key 截断/覆盖
}

// ---------- 自启动 ----------
static bool IsAutostart() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    LONG r = RegQueryValueExW(k, RUN_NAME, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
static void SetAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, RUN_NAME, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, RUN_NAME);
    RegCloseKey(k);
}

// ---------- Kimi for Coding 用量接口（WinINet 原生 HTTPS）----------
struct KimiUsage {
    int fivehUsed = -1, weekUsed = -1, poolUsed = -1;   // 已用百分比，-1=无数据
    int fivehRemain = -1, weekRemain = -1;              // 距重置分钟，-1=无/已过期
    std::wstring fivehRst, weekRst;                     // 重置时刻（本地短格式）
    std::wstring level;                                 // 会员等级原始值 LEVEL_*
    int parUsed = 0, parLimit = 0;
    std::string err;                                    // 空 = 成功
};

// "2026-07-23T17:18:57.116725Z"（UTC）-> SYSTEMTIME；失败返回 false
static bool IsoToFileTime(const std::string& iso, FILETIME& ft) {
    int Y, M, D, h, m, s;
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) < 6) return false;
    SYSTEMTIME st{}; st.wYear = (WORD)Y; st.wMonth = (WORD)M; st.wDay = (WORD)D;
    st.wHour = (WORD)h; st.wMinute = (WORD)m; st.wSecond = (WORD)s;
    return SystemTimeToFileTime(&st, &ft) != 0;
}
// 距现在分钟数；已过去/解析失败返回 -1（5h 窗口闲置时 resetTime 会在过去）
static int IsoToRemainMinutes(const std::string& iso) {
    FILETIME ft, now;
    if (!IsoToFileTime(iso, ft)) return -1;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = ft.dwLowDateTime;  a.HighPart = ft.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    if (a.QuadPart <= b.QuadPart) return -1;
    return (int)((a.QuadPart - b.QuadPart) / 10000000ULL / 60ULL);
}
// UTC ISO -> 本地短格式 "07-23 17:18"（tooltip 用）；失败返回空
static std::wstring IsoToLocalShort(const std::string& iso) {
    FILETIME ft, lft; SYSTEMTIME st;
    if (!IsoToFileTime(iso, ft)) return L"";
    if (!FileTimeToLocalFileTime(&ft, &lft)) return L"";
    if (!FileTimeToSystemTime(&lft, &st)) return L"";
    wchar_t buf[32];
    wsprintfW(buf, L"%02d-%02d %02d:%02d", st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

static bool HttpGet(const wchar_t* url, const std::wstring& headers, std::string& out, std::string& err) {
    HINTERNET hNet = InternetOpenW(L"ClaudeGauge/2.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hNet) { err = "InternetOpen err=" + std::to_string(GetLastError()); return false; }
    DWORD timeout = 15000;   // 连接/收发各 15s 兜底，worker 线程卡住也不怕
    InternetSetOptionW(hNet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(hNet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(hNet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    HINTERNET hUrl = InternetOpenUrlW(hNet, url, headers.c_str(), (DWORD)headers.size(),
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) { err = "OpenUrl err=" + std::to_string(GetLastError()); InternetCloseHandle(hNet); return false; }
    DWORD status = 0, sz = sizeof(status);
    HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sz, nullptr);
    if (status != 200) {
        err = "HTTP " + std::to_string(status);
        InternetCloseHandle(hUrl); InternetCloseHandle(hNet); return false;
    }
    char buf[16384]; DWORD n = 0;
    while (InternetReadFile(hUrl, buf, sizeof(buf), &n) && n > 0) out.append(buf, n);
    InternetCloseHandle(hUrl); InternetCloseHandle(hNet);
    if (out.empty()) err = "empty body";
    return !out.empty();
}

static KimiUsage FetchKimiUsage(const std::wstring& key) {
    KimiUsage r;
    if (key.empty()) { r.err = "config.ini 缺 [kimi] api_key"; return r; }
    std::string body;
    std::wstring auth = L"Authorization: Bearer " + key + L"\r\n";
    if (!HttpGet(L"https://api.kimi.com/coding/v1/usages", auth, body, r.err)) return r;
    try {
        auto j = nlohmann::json::parse(body);
        // 数值字段全是字符串形式的百分比（"17"），兼容数字写法
        auto pct = [](const nlohmann::json& o, const char* k) -> int {
            if (!o.is_object() || !o.contains(k)) return -1;
            const auto& v = o[k];
            if (v.is_string()) return atoi(v.get<std::string>().c_str());
            if (v.is_number()) return v.get<int>();
            return -1;
        };
        // 本周配额（resetTime ≈ 6 天后）
        const auto& u = j["usage"];
        r.weekUsed = pct(u, "used");
        std::string urt = u.value("resetTime", "");
        r.weekRemain = IsoToRemainMinutes(urt);
        r.weekRst = IsoToLocalShort(urt);
        // 5 小时滑动窗口（300 分钟；闲置时 resetTime 可能在过去 → Remain=-1 不显示倒数）
        if (j.contains("limits") && j["limits"].is_array() && !j["limits"].empty()) {
            const auto& d = j["limits"][0]["detail"];
            r.fivehUsed = pct(d, "used");
            std::string frt = d.value("resetTime", "");
            r.fivehRemain = IsoToRemainMinutes(frt);
            r.fivehRst = IsoToLocalShort(frt);
        }
        // 总池：只有 remaining，used% = 100 - remaining；无重置时间
        int poolRem = pct(j["totalQuota"], "remaining");
        if (poolRem >= 0) r.poolUsed = 100 - poolRem;
        // 并发会话
        const auto& pa = j["parallel"];
        r.parLimit = pct(pa, "limit");
        if (pa.contains("details") && pa["details"].is_array()) r.parUsed = (int)pa["details"].size();
        // 会员等级
        if (j.contains("user") && j["user"].contains("membership"))
            r.level = Utf8ToWide(j["user"]["membership"].value("level", ""));
    } catch (const std::exception& e) {
        r.err = std::string("json: ") + e.what();
        r.fivehUsed = r.weekUsed = r.poolUsed = -1;   // 解析失败不当成功数据
    }
    return r;
}

static std::wstring LevelText(const std::wstring& lv) {
    if (lv == L"LEVEL_ADVANCED") return L"高级版";
    if (lv == L"LEVEL_PROFESSIONAL") return L"专业版";
    return lv;   // 未知等级原样显示
}

static void WorkerLoop() {
    for (int i = 0; i < 15 && !g_stop; ++i) Sleep(100);
    while (!g_stop) {
        LoadConfig();   // 允许热更新：用户改 config.ini 加 key 无需重启
        int nAcc = (int)g_cfg.accounts.size();
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_accounts.resize(nAcc);
            for (int i = 0; i < nAcc; ++i) g_accounts[i].name = g_cfg.accounts[i].name;
        }
        if (nAcc == 0) {
            Log("no api keys configured");
            WaitForSingleObject(g_wake, 5000);
            continue;
        }
        bool anyOk = false, anyFail = false;
        for (int i = 0; i < nAcc; ++i) {
            KimiUsage u = FetchKimiUsage(g_cfg.accounts[i].key);
            bool ok = u.err.empty();
            if (ok) anyOk = true; else anyFail = true;
            Log("kimi account=" + std::to_string(i) +
                " ok=" + std::to_string(ok) +
                " 5h=" + std::to_string(u.fivehUsed) +
                " week=" + std::to_string(u.weekUsed) +
                " pool=" + std::to_string(u.poolUsed) +
                " par=" + std::to_string(u.parUsed) + "/" + std::to_string(u.parLimit) +
                " err=" + u.err);
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                auto& a = g_accounts[i];
                a.name = g_cfg.accounts[i].name;
                if (ok) {   // 只在成功时更新该账户数据；失败保留该账户上次好数据
                    auto p = [](int v) { return v < 0 ? std::wstring(L"--") : std::to_wstring(v) + L"%"; };
                    a.fivehPct = p(u.fivehUsed); a.weekPct = p(u.weekUsed); a.poolPct = p(u.poolUsed);
                    a.fivehRst = u.fivehRst; a.weekRst = u.weekRst;
                    a.fivehRemain = u.fivehRemain; a.weekRemain = u.weekRemain;
                    a.parUsed = u.parUsed; a.parLimit = u.parLimit;
                    std::wstring pl = L"Kimi " + LevelText(u.level);
                    if (u.parLimit > 0) pl += L" · 并发 " + std::to_wstring(u.parUsed) + L"/" + std::to_wstring(u.parLimit);
                    a.plan = pl;
                }
                a.ok = ok;
            }
        }
        if (g_hwnd) PostMessageW(g_hwnd, WM_REFRESH, 0, 0);
        // 全部成功 10 分钟；任一失败 60 秒后重试；可被 g_wake 唤醒
        DWORD waitMs = anyFail ? 60 * 1000 : (REFRESH_MINUTES * 60 * 1000);
        WaitForSingleObject(g_wake, waitMs);
    }
}

// ---------- DPI ----------
// 已 SetProcessDPIAware，进程生命周期内 DPI 不变：首次取一次缓存，免得每次重绘反复 GetDC
static int g_dpi = 0;
static int DpiScale(int v) {
    if (!g_dpi) {
        HDC dc = GetDC(nullptr); g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
    }
    return MulDiv(v, g_dpi, 96);
}
static int PctToInt(const std::wstring& p) {
    int v = 0; bool any = false;
    for (wchar_t c : p) { if (c >= L'0' && c <= L'9') { v = v * 10 + (c - L'0'); any = true; } else if (any) break; }
    return any ? v : 0;
}

// ============================================================
//                 任务栏嵌入 + 定位 + 条绘制
// ============================================================
static void EmbedIntoTaskbar(HWND hwnd) {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return;
    SetParent(hwnd, tray);
    LONG_PTR st = GetWindowLongPtrW(hwnd, GWL_STYLE);
    st = (st & ~WS_POPUP) | WS_CHILD;
    SetWindowLongPtrW(hwnd, GWL_STYLE, st | WS_VISIBLE);
}
static void DetachFromTaskbar(HWND hwnd) {
    SetParent(hwnd, nullptr);
    LONG_PTR st = GetWindowLongPtrW(hwnd, GWL_STYLE);
    st = (st & ~WS_CHILD) | WS_POPUP;
    SetWindowLongPtrW(hwnd, GWL_STYLE, st);
}
struct ScanCtx { DWORD explorerPid, ourPid; int center, rightLimit, obstacleLeft; };
static BOOL CALLBACK EnumChildProc(HWND h, LPARAM lp) {
    ScanCtx* c = (ScanCtx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == c->explorerPid || pid == c->ourPid) return TRUE;
    RECT r; GetWindowRect(h, &r);
    if (r.right - r.left <= 0) return TRUE;
    if (r.left >= c->center && r.left < c->rightLimit && r.left < c->obstacleLeft) c->obstacleLeft = r.left;
    return TRUE;
}
static void Reposition(HWND hwnd) {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return;
    RECT tr; GetWindowRect(tray, &tr);
    int trayH = tr.bottom - tr.top, trayW = tr.right - tr.left;
    int rightLimitScreen = tr.right;
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    if (notify) { RECT nr; GetWindowRect(notify, &nr); rightLimitScreen = nr.left; }
    ScanCtx ctx; ctx.explorerPid = 0; GetWindowThreadProcessId(tray, &ctx.explorerPid);
    ctx.ourPid = GetCurrentProcessId(); ctx.center = tr.left + trayW / 2;
    ctx.rightLimit = rightLimitScreen; ctx.obstacleLeft = 0x7fffffff;
    EnumChildWindows(tray, EnumChildProc, (LPARAM)&ctx);

    int N = 1;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        N = std::max(1, (int)g_accounts.size());
    }
    int colW = DpiScale(STRIP_WIDTH_96);
    int w = colW * N;
    int gap = DpiScale(6);
    int anchorRight = (ctx.obstacleLeft != 0x7fffffff) ? ctx.obstacleLeft - gap : rightLimitScreen - gap;
    POINT p{ anchorRight - w, tr.top }; ScreenToClient(tray, &p);
    int x = p.x; if (x < 0) x = 0;
    MoveWindow(hwnd, x, 0, w, trayH, TRUE);
}
// 任务栏条：两行（套餐名 + 用量），colorkey 真透明
// 把颜色往白色方向调淡 t（0~1）—— 用于任务栏下行"同色系浅一档"
static COLORREF Lighten(COLORREF c, double t) {
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    r += (int)((255 - r) * t); g += (int)((255 - g) * t); b += (int)((255 - b) * t);
    return RGB(r, g, b);
}
static void PaintStrip(HWND hwnd, HDC hdc, RECT& rc) {
    HBRUSH bg = CreateSolidBrush(STRIP_COLORKEY);
    FillRect(hdc, &rc, bg); DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);

    int N = 1;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        N = std::max(1, (int)g_accounts.size());
    }
    int colW = ((rc.right - rc.left) + N / 2) / N;   // 四舍五入分栏

    // 关键：NONANTIALIASED_QUALITY 关掉抗锯齿，避免文字边缘混入 colorkey 产生粉/紫毛边
    HFONT f1 = CreateFontW(-DpiScale(12), 0,0,0, FW_BOLD, 0,0,0, GB2312_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, 0, L"Microsoft YaHei UI");
    HFONT f2 = CreateFontW(-DpiScale(12), 0,0,0, FW_NORMAL, 0,0,0, GB2312_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, 0, L"Microsoft YaHei UI");
    const Theme& th = THEMES[g_cfg.themeIdx];
    int half = (rc.bottom - rc.top) / 2;
    HFONT old = (HFONT)SelectObject(hdc, f1);

    for (int ai = 0; ai < N; ++ai) {
        AccountData a;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (ai < (int)g_accounts.size()) a = g_accounts[ai];
        }
        int left = rc.left + ai * colW + DpiScale(4);
        int right = rc.left + (ai + 1) * colW - DpiScale(2);
        if (right <= left) continue;
        RECT r1{ left, rc.top, right, rc.top + half + DpiScale(1) };
        RECT r2{ left, rc.top + half - DpiScale(1), right, rc.bottom };
        std::wstring title = a.name.empty() ? a.plan : a.name;
        if (ai == 0 && title.find(L"(主)") == std::wstring::npos) title += L"(主)";
        std::wstring usage = L"5H " + a.fivehPct + L" 周 " + a.weekPct + L" 池 " + a.poolPct;
        if (a.parLimit > 0) usage += L" · 并发 " + std::to_wstring(a.parUsed) + L"/" + std::to_wstring(a.parLimit);
        SetTextColor(hdc, a.ok ? th.tbTitle : STRIP_WARN);
        DrawTextW(hdc, title.c_str(), -1, &r1, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, f2);
        SetTextColor(hdc, a.ok ? Lighten(th.tbTitle, 0.50) : STRIP_WARN);
        DrawTextW(hdc, usage.c_str(), -1, &r2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, f1);
    }
    SelectObject(hdc, old); DeleteObject(f1); DeleteObject(f2);
}

// ============================================================
//              悬浮窗：GDI+ 分层窗口绘制（圆角+阴影）
// ============================================================
static void AddRoundRect(GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

static void RenderFloating() {
    if (!g_hwnd) return;
    int W = DpiScale(FLOAT_W_96);
    int accountH = DpiScale(ACCOUNT_H_96);
    int SM = DpiScale(SHADOW_M_96);
    int N = 1;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        N = std::max(1, (int)g_accounts.size());
    }
    g_accountCount = N;
    int H = SM * 2 + accountH * N;
    const Theme& th = THEMES[g_cfg.themeIdx];

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldb = SelectObject(mem, dib);

    std::vector<AccountData> accs(N);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (int i = 0; i < N; ++i) {
            if (i < (int)g_accounts.size()) accs[i] = g_accounts[i];
        }
    }

    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        float cx = (float)SM, cyBase = (float)SM - DpiScale(1);
        float cw = (float)(W - 2 * SM);
        float rad = (float)DpiScale(10);

        // 柔和阴影（多层低 alpha 扩散）
        for (int i = DpiScale(6); i >= 1; --i) {
            GraphicsPath sp2;
            AddRoundRect(sp2, cx - i, cyBase + DpiScale(2), cw + 2 * i, accountH * N + 2 * i, rad + i);
            SolidBrush sb(Color(10, 0, 0, 0));
            g.FillPath(&sb, &sp2);
        }
        // 卡片（统一一个圆角大卡片，多账户时纵向拉长）
        GraphicsPath card; AddRoundRect(card, cx, cyBase, cw, (float)(accountH * N), rad);
        SolidBrush cb(th.fbg); g.FillPath(&cb, &card);

        float pad = (float)DpiScale(11);
        float left = cx + pad, innerR = cx + cw - pad, innerW = innerR - left;

        FontFamily fam(L"Microsoft YaHei UI");
        Font fTitle(&fam, (REAL)DpiScale(13), FontStyleBold, UnitPixel);
        Font fLabel(&fam, (REAL)DpiScale(10), FontStyleRegular, UnitPixel);
        SolidBrush brTitle(th.ftitle), brText(th.fbody);

        // 三行
        struct Row { const wchar_t* label; int remIdx; };
        Row rows[3] = { { L"5小时窗口", 0 }, { L"本周配额", 1 }, { L"总池", -1 } };
        float rowH = (REAL)DpiScale(22);
        float lineH = (REAL)DpiScale(14);
        float barH = (REAL)DpiScale(4);
        float pctW = (REAL)DpiScale(34);
        Font fSmall(&fam, (REAL)DpiScale(9), FontStyleRegular, UnitPixel);
        StringFormat sfFar; sfFar.SetAlignment(StringAlignmentFar);
        StringFormat sfNear; sfNear.SetAlignment(StringAlignmentNear);

        for (int ai = 0; ai < N; ++ai) {
            float cy = cyBase + ai * accountH;
            const AccountData& a = accs[ai];
            // 标题：左侧名称/套餐，右侧并发小字；主号(ai==0)自动加"(主)"
            std::wstring title = a.name.empty() ? a.plan : a.name;
            if (ai == 0 && title.find(L"(主)") == std::wstring::npos) title += L"(主)";
            g.DrawString(title.c_str(), -1, &fTitle, PointF(left, cy + (REAL)DpiScale(6)), &brTitle);
            if (a.parLimit > 0) {
                std::wstring par = L"并发 " + std::to_wstring(a.parUsed) + L"/" + std::to_wstring(a.parLimit);
                RectF pr(left, cy + (REAL)DpiScale(7), innerW, lineH);
                g.DrawString(par.c_str(), -1, &fSmall, pr, &sfFar, &brText);
            }

            float rowsTop = cy + (REAL)DpiScale(25);
            std::wstring pcts[3] = { a.fivehPct, a.weekPct, a.poolPct };
            int rems[3] = { a.fivehRemain, a.weekRemain, -1 };
            for (int i = 0; i < 3; ++i) {
                float ry = rowsTop + i * rowH;
                RectF rowRect(left, ry, innerW, lineH);
                g.DrawString(rows[i].label, -1, &fLabel, rowRect, &sfNear, &brText);   // 标签（左）
                g.DrawString(pcts[i].c_str(), -1, &fLabel, rowRect, &sfFar, &brText);  // 百分比（最右）
                // 剩余时间：会话"剩 2h31m"，周"剩 N天"（紧凑）
                std::wstring rt = RemainText(rems[i], true);
                if (!rt.empty()) {
                    RectF rr(left, ry + (REAL)DpiScale(1), innerW - pctW - (REAL)DpiScale(4), lineH);
                    g.DrawString(rt.c_str(), -1, &fSmall, rr, &sfFar, &brText);
                }
                // 整行宽进度条
                float by = ry + lineH + (REAL)DpiScale(2);
                float br = barH / 2.0f;
                GraphicsPath track; AddRoundRect(track, left, by, innerW, barH, br);
                SolidBrush tb(th.fbarBg); g.FillPath(&tb, &track);
                int pv = PctToInt(pcts[i]); if (pv < 0) pv = 0; if (pv > 100) pv = 100;
                float fw = innerW * pv / 100.0f;
                if (fw > barH) {
                    GraphicsPath fill; AddRoundRect(fill, left, by, fw, barH, br);
                    SolidBrush fb(th.fbarFg); g.FillPath(&fb, &fill);
                }
            }
        }
    }

    POINT src{ 0, 0 }; SIZE sz{ W, H };
    BYTE alpha = (BYTE)(g_cfg.transparency * 255 / 100);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, oldb); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, screen);
}
// 任务栏条绘制走 WM_PAINT
static void OnPaintStrip(HWND hwnd) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    PaintStrip(hwnd, mem, rc);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ============================================================
//                   系统托盘图标（稳定的菜单入口）
// ============================================================
static HICON MakeTrayIcon() {
    int s = 32;
    Bitmap bmp(s, s, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0, 0, 0, 0));
    GraphicsPath p; AddRoundRect(p, 1, 1, (float)(s - 2), (float)(s - 2), 7);
    SolidBrush b(Color(255, 0xf5, 0xa6, 0x23));   // 橙黄底
    g.FillPath(&b, &p);
    FontFamily fam(L"Microsoft YaHei UI");
    Font f(&fam, 21.0f, FontStyleBold, UnitPixel);
    SolidBrush wb(Color(255, 0x1c, 0x1c, 0x1c));
    StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(L"K", -1, &f, RectF(0, 1, (REAL)s, (REAL)s), &sf, &wb);
    HICON ic = nullptr; bmp.GetHICON(&ic);
    return ic;
}
static void AddTrayIcon(HWND hwnd) {
    if (!g_trayIcon) g_trayIcon = MakeTrayIcon();
    g_nid = NOTIFYICONDATAW{};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_trayIcon;
    lstrcpynW(g_nid.szTip, L"Kimi 用量 — 右键设置", 64);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = nullptr; }
}
// 把用量+重置时间写进托盘图标的悬停提示（悬停那个"K"图标即可看到，稳定可靠）
static void UpdateTrayTip() {
    if (g_nid.cbSize == 0) return;
    std::wstring t;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto line = [](const std::wstring& pct, int remain, const std::wstring& rst) {
            std::wstring s = pct;
            std::wstring rt = RemainText(remain);
            if (!rt.empty()) s += L" · " + rt;
            else if (!rst.empty()) s += L" · 重置 " + rst;
            return s;
        };
        for (size_t i = 0; i < g_accounts.size(); ++i) {
            const AccountData& a = g_accounts[i];
            if (i > 0) t += L"\r\n";
            t += (a.name.empty() ? a.plan : a.name);
            t += L"\r\n5H " + line(a.fivehPct, a.fivehRemain, a.fivehRst);
            t += L"\r\n周 " + line(a.weekPct, a.weekRemain, a.weekRst);
            t += L"\r\n池 " + a.poolPct;
        }
    }
    if (t.empty()) t = L"Kimi 用量 — 右键设置";
    lstrcpynW(g_nid.szTip, t.c_str(), 128);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ============================================================
//                   tooltip（两种形态都用）
// ============================================================
static void UpdateTooltipText() {
    if (!g_tip) return;
    std::wstring tip;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (size_t i = 0; i < g_accounts.size(); ++i) {
            const AccountData& a = g_accounts[i];
            if (i > 0) tip += L"\r\n";
            tip += (a.name.empty() ? a.plan : a.name) + L"\r\n";
            tip += L"5小时窗口: " + a.fivehPct;
            if (!a.fivehRst.empty()) tip += L" (重置 " + a.fivehRst + L")";
            tip += L"\r\n本周配额: " + a.weekPct;
            if (!a.weekRst.empty()) tip += L" (重置 " + a.weekRst + L")";
            tip += L"\r\n总池: " + a.poolPct;
        }
    }
    if (tip.empty()) tip = L"Kimi 用量";
    static wchar_t buf[512];
    lstrcpynW(buf, tip.c_str(), 512);
    TOOLINFOW ti{}; ti.cbSize = sizeof(ti); ti.hwnd = g_hwnd; ti.uId = (UINT_PTR)g_hwnd;
    ti.lpszText = buf;
    SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}
static void CreateTooltip(HWND hwnd) {
    if (g_tip) return;
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd, nullptr, g_hInst, nullptr);
    if (!g_tip) return;
    RECT rc; GetClientRect(hwnd, &rc);
    TOOLINFOW ti{}; ti.cbSize = sizeof(ti); ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = hwnd; ti.uId = (UINT_PTR)hwnd; ti.rect = rc;
    static wchar_t buf[8] = L"..."; ti.lpszText = buf;
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 400);
    UpdateTooltipText();
}
static void DestroyTooltip() { if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; } }

// ============================================================
//                    形态切换
// ============================================================
static void ApplyMode(HWND hwnd) {
    KillTimer(hwnd, 1);
    // 先清掉 layered 位再按形态重设（colorkey 与 UpdateLayeredWindow 不能混）
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);

    int N = 1;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        N = std::max(1, (int)g_accounts.size());
    }
    g_accountCount = N;

    if (g_cfg.taskbarMode) {
        DestroyTooltip();
        ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        ex |= WS_EX_LAYERED; ex &= ~WS_EX_TOPMOST;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
        EmbedIntoTaskbar(hwnd);
        SetLayeredWindowAttributes(hwnd, STRIP_COLORKEY, 0, LWA_COLORKEY);
        CreateTooltip(hwnd);
        Reposition(hwnd);
        SetTimer(hwnd, 1, 1000, nullptr);
        ShowWindow(hwnd, SW_SHOW);
        InvalidateRect(hwnd, nullptr, TRUE);
    } else {
        DetachFromTaskbar(hwnd);
        ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        ex |= WS_EX_LAYERED | WS_EX_TOOLWINDOW;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
        int w = DpiScale(FLOAT_W_96);
        int h = DpiScale(FLOAT_H_96 + (N - 1) * ACCOUNT_H_96);
        HWND z = g_cfg.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
        SetWindowPos(hwnd, z, g_cfg.x, g_cfg.y, w, h, SWP_SHOWWINDOW);
        CreateTooltip(hwnd);   // 悬浮窗也支持悬停看重置时间
        RenderFloating();
        SetTimer(hwnd, 1, 60000, nullptr);   // 悬浮窗：每分钟倒数一次
    }
}

static void Refresh(HWND hwnd) {
    int N = 1;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        N = std::max(1, (int)g_accounts.size());
    }
    // 账户数变化时（用户加/删 key）需要重设窗口尺寸
    if (N != g_accountCount) { ApplyMode(hwnd); return; }
    UpdateTrayTip();
    if (g_cfg.taskbarMode) { InvalidateRect(hwnd, nullptr, FALSE); UpdateTooltipText(); }
    else { RenderFloating(); UpdateTooltipText(); }
}

// ============================================================
//                    右键菜单
// ============================================================
static void ShowContextMenu(HWND hwnd, int x, int y) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_cfg.taskbarMode ? 0 : MF_CHECKED), ID_MODE_FLOAT,   L"悬浮窗模式");
    AppendMenuW(m, MF_STRING | (g_cfg.taskbarMode ? MF_CHECKED : 0), ID_MODE_TASKBAR, L"任务栏模式");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU themeSub = CreatePopupMenu();
    for (int i = 0; i < THEME_COUNT; ++i)
        AppendMenuW(themeSub, MF_STRING | (g_cfg.themeIdx == i ? MF_CHECKED : 0), ID_THEME_BASE + i, THEMES[i].name);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)themeSub, L"主题配色");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_cfg.alwaysOnTop ? MF_CHECKED : 0), ID_TOPMOST, L"总是置顶");
    AppendMenuW(m, MF_STRING | (g_cfg.lockPos ? MF_CHECKED : 0), ID_LOCKPOS, L"锁定位置");
    HMENU sub = CreatePopupMenu();
    int t = g_cfg.transparency;
    AppendMenuW(sub, MF_STRING | (t == 100 ? MF_CHECKED : 0), ID_OPA_100, L"100%");
    AppendMenuW(sub, MF_STRING | (t == 90 ? MF_CHECKED : 0), ID_OPA_90, L"90%");
    AppendMenuW(sub, MF_STRING | (t == 80 ? MF_CHECKED : 0), ID_OPA_80, L"80%");
    AppendMenuW(sub, MF_STRING | (t == 70 ? MF_CHECKED : 0), ID_OPA_70, L"70%");
    AppendMenuW(sub, MF_STRING | (t == 60 ? MF_CHECKED : 0), ID_OPA_60, L"60%");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"透明度");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), ID_AUTOSTART, L"开机自启动");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_REFRESH_NOW, L"立即刷新");
    AppendMenuW(m, MF_STRING, ID_ABOUT, L"关于");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"退出");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
    DestroyMenu(m);
}
static void OnCommand(HWND hwnd, int id) {
    if (id >= ID_THEME_BASE && id < ID_THEME_BASE + THEME_COUNT) {
        g_cfg.themeIdx = id - ID_THEME_BASE; SaveConfig(); Refresh(hwnd); return;
    }
    switch (id) {
        case ID_MODE_FLOAT:   if (g_cfg.taskbarMode) { g_cfg.taskbarMode = false; SaveConfig(); ApplyMode(hwnd); } break;
        case ID_MODE_TASKBAR: if (!g_cfg.taskbarMode) { g_cfg.taskbarMode = true; SaveConfig(); ApplyMode(hwnd); } break;
        case ID_TOPMOST:
            g_cfg.alwaysOnTop = !g_cfg.alwaysOnTop; SaveConfig();
            if (!g_cfg.taskbarMode)
                SetWindowPos(hwnd, g_cfg.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE);
            break;
        case ID_LOCKPOS:  g_cfg.lockPos = !g_cfg.lockPos; SaveConfig(); break;
        case ID_OPA_100: g_cfg.transparency = 100; SaveConfig(); Refresh(hwnd); break;
        case ID_OPA_90:  g_cfg.transparency = 90;  SaveConfig(); Refresh(hwnd); break;
        case ID_OPA_80:  g_cfg.transparency = 80;  SaveConfig(); Refresh(hwnd); break;
        case ID_OPA_70:  g_cfg.transparency = 70;  SaveConfig(); Refresh(hwnd); break;
        case ID_OPA_60:  g_cfg.transparency = 60;  SaveConfig(); Refresh(hwnd); break;
        case ID_AUTOSTART: SetAutostart(!IsAutostart()); break;
        case ID_REFRESH_NOW: if (g_wake) SetEvent(g_wake); break;   // 唤醒 worker 立刻抓一次
        case ID_ABOUT:
            MessageBoxW(hwnd,
                L"ClaudeGauge   v2.0（Kimi 版）\n"
                L"作者：初见\n\n"
                L"在桌面/任务栏显示 Kimi for Coding 的用量。\n"
                L"第三方小工具，非月之暗面官方产品。\n\n"
                L"鸣谢：WinINet、nlohmann/json、mingw-w64、GDI+",
                L"关于 ClaudeGauge", MB_OK | MB_ICONINFORMATION);
            break;
        case ID_EXIT: g_stop = true; if (g_wake) SetEvent(g_wake); DestroyWindow(hwnd); break;
    }
}

// ============================================================
//                    窗口过程
// ============================================================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarMsg && g_taskbarMsg != 0) {
        AddTrayIcon(hwnd);   // explorer 重启后托盘图标会丢，重建
        if (g_cfg.taskbarMode) {
            EmbedIntoTaskbar(hwnd);
            SetLayeredWindowAttributes(hwnd, STRIP_COLORKEY, 0, LWA_COLORKEY);
            Reposition(hwnd);
        }
        return 0;
    }
    switch (msg) {
        case WM_REFRESH: Refresh(hwnd); return 0;
        case WM_TRAY:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                POINT pt; GetCursorPos(&pt); ShowContextMenu(hwnd, pt.x, pt.y);
            }
            return 0;
        case WM_TIMER:
            if (g_cfg.taskbarMode) { Reposition(hwnd); InvalidateRect(hwnd, nullptr, FALSE); UpdateTooltipText(); }
            else {   // 悬浮窗：每个账户剩余分钟各减 1，重绘倒数
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    for (auto& a : g_accounts) {
                        if (a.fivehRemain > 0) a.fivehRemain--;
                        if (a.weekRemain > 0) a.weekRemain--;
                    }
                }
                RenderFloating();
            }
            return 0;
        case WM_PAINT:
            if (g_cfg.taskbarMode) OnPaintStrip(hwnd);
            else { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); RenderFloating(); }
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONDOWN:
            if (!g_cfg.taskbarMode && !g_cfg.lockPos) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            return 0;
        case WM_EXITSIZEMOVE:
            if (!g_cfg.taskbarMode) {
                RECT r; GetWindowRect(hwnd, &r);
                g_cfg.x = r.left; g_cfg.y = r.top; SaveConfig();
            }
            return 0;
        case WM_CONTEXTMENU: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (x == -1 && y == -1) { RECT r; GetWindowRect(hwnd, &r); x = r.left; y = r.top; }
            ShowContextMenu(hwnd, x, y); return 0;
        }
        case WM_RBUTTONUP: { POINT pt; GetCursorPos(&pt); ShowContextMenu(hwnd, pt.x, pt.y); return 0; }
        case WM_COMMAND: OnCommand(hwnd, LOWORD(wp)); return 0;
        case WM_DESTROY: g_stop = true; if (g_wake) SetEvent(g_wake); KillTimer(hwnd, 1); DestroyTooltip(); RemoveTrayIcon(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();   // 声明 DPI 感知：避免高缩放下窗口被系统拉伸变糊
    HANDLE mtx = CreateMutexW(nullptr, FALSE, L"ClaudeTaskbarWidget_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    (void)mtx;

    g_hInst = hInst;
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdip, &gsi, nullptr);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    LoadConfig();
    g_taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ClaudeTaskbarWidgetClass";
    ATOM atom = RegisterClassW(&wc);
    Log("=== start === atom=" + std::to_string(atom) + " err=" + std::to_string(GetLastError()));

    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, wc.lpszClassName, L"ClaudeUsage",
        WS_POPUP, g_cfg.x, g_cfg.y, DpiScale(FLOAT_W_96), DpiScale(FLOAT_H_96),
        nullptr, nullptr, hInst, nullptr);
    Log("CreateWindow hwnd=" + std::to_string((size_t)g_hwnd) + " err=" + std::to_string(GetLastError()));
    if (!g_hwnd) return 1;

    ApplyMode(g_hwnd);
    AddTrayIcon(g_hwnd);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset，先建再起线程
    std::thread(WorkerLoop).detach();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g_stop = true;
    if (g_gdip) GdiplusShutdown(g_gdip);
    return 0;
}
