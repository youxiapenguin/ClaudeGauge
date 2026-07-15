/*********************************************************
 * Claude Code 用量桌面工具（独立程序）
 * ------------------------------------------------------
 * 两种显示形态，右键菜单切换并持久化到 config.ini：
 *   形态1 悬浮窗：分层窗口（UpdateLayeredWindow + GDI+），圆角卡片 + 柔和
 *                 阴影 + 抗锯齿，紧凑；标题粗体 + 三行细体 + 圆角进度条；
 *                 白/黑两套主题；透明度/置顶/锁定位置可调。
 *   形态2 任务栏条：SetParent 嵌进 Shell_TrayWnd，真透明（LWA_COLORKEY），
 *                 两行（套餐名 + 精简用量），悬停弹 tooltip 看重置时间。
 * 数据：后台线程每 REFRESH_MINUTES 调一次 WSL 探针，解析 key=value 缓存。
 * 嵌入任务栏的手法参考开源项目 TrafficMonitor 的同类公开 Win32 思路，
 * 本程序为独立重写，未复制其源码。
 * 编译：mingw-w64（见 build.sh，链接 -lgdiplus）
 *********************************************************/
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <algorithm>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

using namespace Gdiplus;

// ====== 配置 ======
static const int REFRESH_MINUTES = 10;
static const int STRIP_WIDTH_96  = 240;    // 任务栏条宽度（96 DPI）
static const int FLOAT_W_96      = 196;    // 悬浮窗宽（含阴影边距）
static const int FLOAT_H_96      = 108;    // 悬浮窗高
static const int SHADOW_M_96     = 8;      // 阴影边距
// 探针在 WSL 里的路径：运行时由 exe 旁边的 cg_probe.py 推导（无写死个人路径，整个文件夹可移植）
static std::wstring g_probeWsl;

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

struct Config {
    bool taskbarMode = false;
    int  themeIdx    = 0;       // 0..THEME_COUNT-1
    bool alwaysOnTop = true;
    bool lockPos     = false;
    int  transparency = 90;     // 百分比
    int  x = 80, y = 80;
    std::wstring distro;        // 指定的 WSL 发行版（空=自动探测/默认）
};
static Config g_cfg;

struct Shared {
    std::wstring sessionPct = L"--", weekallPct = L"--", sonnetPct = L"--";
    std::wstring sessionRst, weekallRst, sonnetRst;
    std::wstring plan = L"Claude Max";
    int sessionRemain = -1, weekallRemain = -1, sonnetRemain = -1;  // 距重置剩余分钟，-1=未知
    bool ok = false;
};
static Shared g_shared;

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
    wchar_t db[128];
    GetPrivateProfileStringW(L"general", L"distro", L"", db, 128, p.c_str());
    g_cfg.distro = db;
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
    WritePrivateProfileStringW(L"general", L"distro", g_cfg.distro.c_str(), p.c_str());
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

// ---------- 跑一条命令抓 stdout ----------
// 关键：wsl.exe 在"无控制台(CREATE_NO_WINDOW) + stdout 是管道"时输出转发会失效、吐空。
// 因此这里把子进程 stdout/stderr 重定向到一个真实临时文件句柄（文件句柄不走那套转发），
// 进程结束后再把文件读回来。这样后台静默运行也能稳定拿到 wsl.exe 的输出。
static std::string RunCmdCapture(const std::wstring& cmdline, DWORD timeoutMs) {
    wchar_t tmpDir[MAX_PATH]; if (!GetTempPathW(MAX_PATH, tmpDir)) lstrcpyW(tmpDir, L"C:\\");
    wchar_t tmpFile[MAX_PATH]; if (!GetTempFileNameW(tmpDir, L"cg", 0, tmpFile)) return "";

    // 经 cmd.exe 重定向到该文件：cmd.exe /c <命令> > "tmpFile" 2>&1
    // 必须走 cmd.exe —— 它会给 wsl.exe 分配隐藏控制台，wsl.exe 才肯在后台吐输出；
    // 直接 CreateProcess(wsl.exe) + CREATE_NO_WINDOW 会拿到空输出。
    std::wstring cmd = L"cmd.exe /c " + cmdline + L" > \"" + tmpFile + L"\" 2>&1";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // 给子进程一个永远可被 wsl.exe 翻译的工作目录（C:\Windows）。否则若本程序的
    // 工作目录是 UNC/\\wsl.localhost\... 之类，wsl.exe 会"Failed to translate"并输出空。
    wchar_t winDir[MAX_PATH]; if (!GetWindowsDirectoryW(winDir, MAX_PATH)) lstrcpyW(winDir, L"C:\\");

    // Job Object 兜底：cmd.exe 会再 fork 出 wsl.exe，若 wsl.exe 本身卡死不返回，
    // 之前超时只 TerminateProcess 了 cmd.exe，孙进程 wsl.exe 就成孤儿永久挂着——
    // 攒得多了会堵死 WSL 的调度队列，导致新请求也跟着卡死（2026-07 实测攒了几百个
    // 孤儿 wsl.exe 把 WSL 拖垮，新起的 wsl -d Ubuntu 直接超时无响应）。
    // CREATE_SUSPENDED 先挂起进程、分配进 Job 后再 Resume，保证 cmd.exe 之后 fork
    // 出的 wsl.exe 也落在同一个 Job 里；超时时 TerminateJobObject 把整棵进程树一起清掉。
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, winDir, &si, &pi);
    if (!ok) { if (hJob) CloseHandle(hJob); DeleteFileW(tmpFile); return ""; }
    if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
    ResumeThread(pi.hThread);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) {
        if (hJob) TerminateJobObject(hJob, 1); else TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (hJob) CloseHandle(hJob);   // KILL_ON_JOB_CLOSE：若还有存活的子孙进程，关句柄时一并清掉

    std::string out;
    HANDLE rf = CreateFileW(tmpFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (rf != INVALID_HANDLE_VALUE) {
        char buf[4096]; DWORD n = 0;
        while (ReadFile(rf, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
        CloseHandle(rf);
    }
    DeleteFileW(tmpFile);
    return out;
}

// Windows 路径 -> WSL 路径：直接字符串转换（D:\a\b -> /mnt/d/a/b），
// 不调 wslpath，避免中文路径经 wsl.exe 参数桥时的编码问题。
static std::wstring WinToWslPath(const std::wstring& winPath) {
    if (winPath.size() >= 2 && winPath[1] == L':') {
        std::wstring r = L"/mnt/";
        r += (wchar_t)towlower(winPath[0]);
        r += winPath.substr(2);
        for (auto& c : r) if (c == L'\\') c = L'/';
        return r;
    }
    return winPath;
}

// 定位 exe 旁边的 cg_probe.py，换算成 WSL 路径
static void ResolveProbe() {
    g_probeWsl = WinToWslPath(ExeDirFile(L"cg_probe.py"));
}

// ---------- WSL 发行版自动探测 ----------
// 背景：用户机器上常有多个发行版（如默认是空壳的 docker-desktop），裸 wsl.exe
// 会打到错的那个，找不到 python3/claude。这里自动挑出"装了 python3 且 claude 已登录"的发行版。
static std::vector<std::wstring> ListDistros() {
    std::string raw = RunCmdCapture(L"wsl.exe -l -q", 8000);   // 输出为 UTF-16LE
    std::vector<std::wstring> out; std::wstring cur;
    for (size_t i = 0; i + 1 < raw.size(); i += 2) {
        wchar_t ch = (wchar_t)((unsigned char)raw[i] | ((unsigned char)raw[i + 1] << 8));
        if (ch == L'\r') continue;
        if (ch == L'\n' || ch == 0) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
// 该发行版里有 python3 且 ~/.claude/.credentials.json 存在（即已登录）→ 就是它
static bool DistroUsable(const std::wstring& name) {
    // 注意：wsl.exe 的 -d 不能给带引号的发行版名（它自解析命令行，会把引号算进名字 → NOT_FOUND）
    std::wstring cmd = L"wsl.exe -d " + name +
        L" python3 -c \"import os;print(os.path.exists(os.path.expanduser('~/.claude/.credentials.json')))\"";
    return RunCmdCapture(cmd, 15000).find("True") != std::string::npos;
}
static void DetectDistro() {
    if (!g_cfg.distro.empty() && DistroUsable(g_cfg.distro)) return;   // 已记住且仍可用
    for (auto& d : ListDistros()) {
        if (DistroUsable(d)) { g_cfg.distro = d; SaveConfig(); return; }
    }
    g_cfg.distro.clear();   // 没找到就退回裸 wsl.exe（至少不崩）
}

// ---------- 探针 ----------
static std::string RunProbe() {
    if (g_probeWsl.empty()) return "";
    std::wstring cmd = L"wsl.exe ";
    if (!g_cfg.distro.empty()) cmd += L"-d " + g_cfg.distro + L" ";   // 发行版名不能加引号
    cmd += L"python3 \"" + g_probeWsl + L"\"";
    return RunCmdCapture(cmd, 120000);
}
static std::map<std::string, std::string> ParseKV(const std::string& s) {
    std::map<std::string, std::string> m; size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? s.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos) m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}
static void WorkerLoop() {
    for (int i = 0; i < 15 && !g_stop; ++i) Sleep(100);
    DetectDistro();   // 先挑出正确的 WSL 发行版（避免打到 docker-desktop 等空壳）
    { std::string d; for (wchar_t c : g_cfg.distro) d += (char)c; Log("DetectDistro -> '" + d + "'"); }
    int fails = 0;   // 连续失败计数：满 3 次重探发行版（治启动时 WSL 没起/发行版失效）
    while (!g_stop) {
        auto kv = ParseKV(RunProbe());
        Log("probe OK=" + kv["OK"] + " sess=" + kv["SESSION_PCT"] + " err=" + kv["ERR"]);
        std::wstring sp = L"--", wp = L"--", np = L"--", sr, wr, nr;
        std::wstring pl = kv["PLAN"].empty() ? L"" : Utf8ToWide(kv["PLAN"]);
        int sRem = -1, wRem = -1, nRem = -1;
        bool ok = false;
        if (kv["OK"] == "1") {
            ok = true;
            auto pct = [](const std::string& p) { return p.empty() ? std::wstring(L"--") : Utf8ToWide(p) + L"%"; };
            auto toi = [](const std::string& v) { return v.empty() ? -1 : atoi(v.c_str()); };
            sp = pct(kv["SESSION_PCT"]); wp = pct(kv["WEEKALL_PCT"]); np = pct(kv["WEEKSONNET_PCT"]);
            sr = Utf8ToWide(kv["SESSION_RESET"]); wr = Utf8ToWide(kv["WEEKALL_RESET"]); nr = Utf8ToWide(kv["WEEKSONNET_RESET"]);
            sRem = toi(kv["SESSION_REMAIN"]); wRem = toi(kv["WEEKALL_REMAIN"]); nRem = toi(kv["WEEKSONNET_REMAIN"]);
        }
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (ok) {   // 只在成功时更新数据；失败保留上次好数据，不清成 "--"
                g_shared.sessionPct = sp; g_shared.weekallPct = wp; g_shared.sonnetPct = np;
                g_shared.sessionRst = sr; g_shared.weekallRst = wr; g_shared.sonnetRst = nr;
                g_shared.sessionRemain = sRem; g_shared.weekallRemain = wRem; g_shared.sonnetRemain = nRem;
                if (!pl.empty()) g_shared.plan = pl;
            }
            g_shared.ok = ok;
        }
        if (g_hwnd) PostMessageW(g_hwnd, WM_REFRESH, 0, 0);
        if (ok) fails = 0;
        else if (++fails >= 3) { DetectDistro(); fails = 0;   // 连败 3 次：重探发行版后清零，避免每轮都重探
            std::string d; for (wchar_t c : g_cfg.distro) d += (char)c; Log("3 fails -> re-DetectDistro '" + d + "'"); }
        // 自动重连：成功 10 分钟刷一次；失败 60 秒后重试；等待可被 g_wake 唤醒（立即刷新/退出）
        DWORD waitMs = ok ? (REFRESH_MINUTES * 60 * 1000) : 60 * 1000;
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
    int w = DpiScale(STRIP_WIDTH_96), gap = DpiScale(6);
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

    std::wstring plan, usage; bool ok;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ok = g_shared.ok; plan = g_shared.plan;
        usage = L"5H " + g_shared.sessionPct + L"  WEEK " + g_shared.weekallPct + L"  SNT " + g_shared.sonnetPct;
    }
    // 关键：NONANTIALIASED_QUALITY 关掉抗锯齿，避免文字边缘混入 colorkey 产生粉/紫毛边
    HFONT f1 = CreateFontW(-DpiScale(13), 0,0,0, FW_BOLD, 0,0,0, GB2312_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, 0, L"Microsoft YaHei UI");
    HFONT f2 = CreateFontW(-DpiScale(13), 0,0,0, FW_NORMAL, 0,0,0, GB2312_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, 0, L"Microsoft YaHei UI");
    const Theme& th = THEMES[g_cfg.themeIdx];
    int half = (rc.bottom - rc.top) / 2;
    RECT r1{ rc.left + DpiScale(4), rc.top, rc.right, rc.top + half + DpiScale(1) };
    RECT r2{ rc.left + DpiScale(4), rc.top + half - DpiScale(1), rc.right, rc.bottom };
    HFONT old = (HFONT)SelectObject(hdc, f1);
    SetTextColor(hdc, ok ? th.tbTitle : STRIP_WARN);   // 行1 套餐
    DrawTextW(hdc, plan.c_str(), -1, &r1, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    SelectObject(hdc, f2);
    SetTextColor(hdc, ok ? Lighten(th.tbTitle, 0.50) : STRIP_WARN);   // 行2 用量：同色系调淡(浅)+非粗体
    DrawTextW(hdc, usage.c_str(), -1, &r2, DT_LEFT | DT_TOP | DT_SINGLELINE);
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
    int W = DpiScale(FLOAT_W_96), H = DpiScale(FLOAT_H_96), SM = DpiScale(SHADOW_M_96);
    const Theme& th = THEMES[g_cfg.themeIdx];

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldb = SelectObject(mem, dib);

    // 取数据
    std::wstring sp, wp, np, pl; int rem[3];
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        sp = g_shared.sessionPct; wp = g_shared.weekallPct; np = g_shared.sonnetPct; pl = g_shared.plan;
        rem[0] = g_shared.sessionRemain; rem[1] = g_shared.weekallRemain; rem[2] = g_shared.sonnetRemain;
    }

    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        float cx = (float)SM, cy = (float)SM - DpiScale(1);
        float cw = (float)(W - 2 * SM), ch = (float)(H - 2 * SM);
        float rad = (float)DpiScale(10);

        // 柔和阴影（多层低 alpha 扩散）
        for (int i = DpiScale(6); i >= 1; --i) {
            GraphicsPath sp2;
            AddRoundRect(sp2, cx - i, cy + DpiScale(2), cw + 2 * i, ch + 2 * i, rad + i);
            SolidBrush sb(Color(10, 0, 0, 0));
            g.FillPath(&sb, &sp2);
        }
        // 卡片
        GraphicsPath card; AddRoundRect(card, cx, cy, cw, ch, rad);
        SolidBrush cb(th.fbg); g.FillPath(&cb, &card);

        float pad = (float)DpiScale(11);
        float left = cx + pad, innerR = cx + cw - pad, innerW = innerR - left;

        FontFamily fam(L"Microsoft YaHei UI");
        Font fTitle(&fam, (REAL)DpiScale(14), FontStyleBold, UnitPixel);
        Font fLabel(&fam, (REAL)DpiScale(10), FontStyleRegular, UnitPixel);
        SolidBrush brTitle(th.ftitle), brText(th.fbody), brSub(th.fbody);

        // 标题
        g.DrawString(pl.c_str(), -1, &fTitle, PointF(left, cy + (REAL)DpiScale(7)), &brTitle);

        // 三行
        struct Row { const wchar_t* label; std::wstring pct; };
        Row rows[3] = { { L"SESSION · 5H", sp }, { L"WEEKLY · ALL", wp }, { L"WEEKLY · SONNET", np } };
        float rowsTop = cy + (REAL)DpiScale(28);
        float rowH = (REAL)DpiScale(22);
        float lineH = (REAL)DpiScale(14);
        float barH = (REAL)DpiScale(4);
        float pctW = (REAL)DpiScale(34);
        Font fSmall(&fam, (REAL)DpiScale(9), FontStyleRegular, UnitPixel);
        StringFormat sfFar; sfFar.SetAlignment(StringAlignmentFar);
        StringFormat sfNear; sfNear.SetAlignment(StringAlignmentNear);

        for (int i = 0; i < 3; ++i) {
            float ry = rowsTop + i * rowH;
            RectF rowRect(left, ry, innerW, lineH);
            g.DrawString(rows[i].label, -1, &fLabel, rowRect, &sfNear, &brText);   // 标签（左）
            g.DrawString(rows[i].pct.c_str(), -1, &fLabel, rowRect, &sfFar, &brSub); // 百分比（最右）
            // 剩余时间：会话"剩 2h31m"，周"剩 N天"（紧凑），三行都显示
            std::wstring rt = RemainText(rem[i], true);
            if (!rt.empty()) {
                RectF rr(left, ry + (REAL)DpiScale(1), innerW - pctW - (REAL)DpiScale(4), lineH);
                g.DrawString(rt.c_str(), -1, &fSmall, rr, &sfFar, &brSub);
            }
            // 整行宽进度条
            float by = ry + lineH + (REAL)DpiScale(2);
            float br = barH / 2.0f;
            GraphicsPath track; AddRoundRect(track, left, by, innerW, barH, br);
            SolidBrush tb(th.fbarBg); g.FillPath(&tb, &track);
            int pv = PctToInt(rows[i].pct); if (pv < 0) pv = 0; if (pv > 100) pv = 100;
            float fw = innerW * pv / 100.0f;
            if (fw > barH) {
                GraphicsPath fill; AddRoundRect(fill, left, by, fw, barH, br);
                SolidBrush fb(th.fbarFg); g.FillPath(&fb, &fill);
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
    g.DrawString(L"C", -1, &f, RectF(0, 1, (REAL)s, (REAL)s), &sf, &wb);
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
    lstrcpynW(g_nid.szTip, L"Claude 用量 — 右键设置", 64);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = nullptr; }
}
// 把用量+重置时间写进托盘图标的悬停提示（悬停那个"C"图标即可看到，稳定可靠）
static void UpdateTrayTip() {
    if (g_nid.cbSize == 0) return;
    std::wstring t;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        // 优先显示"剩多久"（szTip 上限 128 字符，比重置时刻更省也更直观）；没有才退回重置时刻
        auto line = [](const std::wstring& pct, int remain, const std::wstring& rst) {
            std::wstring s = pct;
            std::wstring rt = RemainText(remain);
            if (!rt.empty()) s += L" · " + rt;
            else if (!rst.empty()) s += L" · 重置 " + rst;
            return s;
        };
        t  = g_shared.plan;
        t += L"\r\n会话 "   + line(g_shared.sessionPct, g_shared.sessionRemain, g_shared.sessionRst);
        t += L"\r\n本周 "   + line(g_shared.weekallPct, g_shared.weekallRemain, g_shared.weekallRst);
        t += L"\r\nSonnet " + line(g_shared.sonnetPct,  g_shared.sonnetRemain,  g_shared.sonnetRst);
    }
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
        tip  = L"SESSION 5H: " + g_shared.sessionPct;
        if (!g_shared.sessionRst.empty()) tip += L" (resets " + g_shared.sessionRst + L")";
        tip += L"\r\nWEEKLY ALL: " + g_shared.weekallPct;
        if (!g_shared.weekallRst.empty()) tip += L" (resets " + g_shared.weekallRst + L")";
        tip += L"\r\nWEEKLY SONNET: " + g_shared.sonnetPct;
        if (!g_shared.sonnetRst.empty()) tip += L" (resets " + g_shared.sonnetRst + L")";
    }
    TOOLINFOW ti{}; ti.cbSize = sizeof(ti); ti.hwnd = g_hwnd; ti.uId = (UINT_PTR)g_hwnd;
    ti.lpszText = &tip[0];
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
        int w = DpiScale(FLOAT_W_96), h = DpiScale(FLOAT_H_96);
        HWND z = g_cfg.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
        SetWindowPos(hwnd, z, g_cfg.x, g_cfg.y, w, h, SWP_SHOWWINDOW);
        CreateTooltip(hwnd);   // 悬浮窗也支持悬停看重置时间
        RenderFloating();
        SetTimer(hwnd, 1, 60000, nullptr);   // 悬浮窗：每分钟倒数一次
    }
}

static void Refresh(HWND hwnd) {
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
                L"ClaudeGauge   v1.0\n"
                L"作者：初见\n\n"
                L"在桌面/任务栏显示 Claude Code 的用量。\n"
                L"第三方小工具，非 Anthropic 官方产品。\n\n"
                L"鸣谢：pyte、mingw-w64、GDI+",
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
            else {   // 悬浮窗：剩余分钟各减 1，重绘倒数
                { std::lock_guard<std::mutex> lk(g_mtx);
                  if (g_shared.sessionRemain > 0) g_shared.sessionRemain--;
                  if (g_shared.weekallRemain > 0) g_shared.weekallRemain--;
                  if (g_shared.sonnetRemain  > 0) g_shared.sonnetRemain--; }
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

    ResolveProbe();   // 定位 exe 旁的 cg_probe.py（无写死路径）
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
