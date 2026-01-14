#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <powrprof.h>
#include <vector>
#include <algorithm>
#include <string> // 使用 wstring
#include <shellapi.h>

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Kernel32.lib")
#pragma comment(lib, "Shell32.lib") // 新增 Shell32

// ================= 常量定义 =================
constexpr GUID kGuidSubVideo = { 0x7516b95f,0xf776,0x4464,{0x8c,0x53,0x06,0x16,0x7f,0x40,0xcc,0x99} };
constexpr GUID kGuidVideoBrightness = { 0xaded5e82,0xb909,0x4619,{0x99,0x49,0xf5,0xd7,0x1d,0xac,0x0b,0xcb} };

#define ID_TIMER_DEBOUNCE 1
#define DEBOUNCE_DELAY_MS 400

GUID g_activeScheme = { 0 };

// ================= 辅助函数 =================

bool IsAdministrator() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
}

// 辅助执行命令（静默模式，不闪烁黑框）
void ExecuteSilent(const std::wstring& parameters) {
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = nullptr;
    sei.lpVerb = L"open";
    sei.lpFile = L"schtasks.exe";
    sei.lpParameters = parameters.c_str();
    sei.nShow = SW_HIDE; // 关键：隐藏窗口

    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 5000); // 等待最多5秒
        CloseHandle(sei.hProcess);
    }
}

// ================= 核心同步逻辑 =================
void PerformSync() {
    GUID* pActive = nullptr;
    if (PowerGetActiveScheme(nullptr, &pActive) != ERROR_SUCCESS) return;
    g_activeScheme = *pActive;
    
    // 获取当前亮度
    DWORD ac = 0, dc = 0;
    DWORD resAc = PowerReadACValueIndex(nullptr, &g_activeScheme, &kGuidSubVideo, &kGuidVideoBrightness, &ac);
    DWORD resDc = PowerReadDCValueIndex(nullptr, &g_activeScheme, &kGuidSubVideo, &kGuidVideoBrightness, &dc);

    // 只要有一个读取成功就继续，防止某些极端情况
    if (resAc != ERROR_SUCCESS && resDc != ERROR_SUCCESS) {
        LocalFree(pActive);
        return;
    }
    
    ac = std::clamp<DWORD>(ac, 0, 100);
    dc = std::clamp<DWORD>(dc, 0, 100);

    // 动态枚举方案，确保用户新增/删除方案后也能正常工作
    // 这里的开销极小，且仅在防抖后触发一次，完全可以接受
    DWORD index = 0;
    while (true) {
        GUID scheme;
        DWORD bufSize = sizeof(scheme);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, (UCHAR*)&scheme, &bufSize) != ERROR_SUCCESS) {
            break;
        }
        index++;

        // 跳过当前激活的方案
        if (IsEqualGUID(scheme, g_activeScheme)) continue;

        // Smart Write: 只有值不同时才写入，减少注册表操作
        DWORD currentAC = 0, currentDC = 0;
        
        if (PowerReadACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &currentAC) == ERROR_SUCCESS) {
            if (currentAC != ac) PowerWriteACValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, ac);
        }
        
        if (PowerReadDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, &currentDC) == ERROR_SUCCESS) {
            if (currentDC != dc) PowerWriteDCValueIndex(nullptr, &scheme, &kGuidSubVideo, &kGuidVideoBrightness, dc);
        }
    }
    
    // 激活方案变更（如果修改了非激活方案，有些系统可能不需要这步，但加上保险）
    // 注意：不要调用 PowerSetActiveScheme，因为我们只是改了属性，不是切方案
    
    LocalFree(pActive);
}

// ================= 自启逻辑 =================
void HandleAutoRun(const wchar_t* cmd) {
    if (!cmd || !*cmd) return;

    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return;

    // 使用 wstring 进行安全拼接
    std::wstring pathStr = L"\"" + std::wstring(exePath) + L"\"";

    if (wcsstr(cmd, L"--onar")) {
        // 创建任务
        std::wstring args = L"/Create /F /RL HIGHEST /SC ONLOGON /TN \"PowerBrightnessSync\" /TR " + pathStr;
        ExecuteSilent(args);
    }
    else if (wcsstr(cmd, L"--ofar")) {
        // 删除任务
        ExecuteSilent(L"/Delete /F /TN \"PowerBrightnessSync\"");
    }
}

// ================= 窗口过程 =================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE && lp) {
            auto pbs = (PPOWERBROADCAST_SETTING)lp;
            if (IsEqualGUID(pbs->PowerSetting, kGuidVideoBrightness)) {
                SetTimer(hwnd, ID_TIMER_DEBOUNCE, DEBOUNCE_DELAY_MS, nullptr);
            }
        }
        return TRUE;

    case WM_TIMER:
        if (wp == ID_TIMER_DEBOUNCE) {
            KillTimer(hwnd, ID_TIMER_DEBOUNCE);
            PerformSync();
            // 每次同步完清理内存
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ================= 入口点 =================
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int) {
    // 1. 处理参数 (自启设置)
    if (lpCmdLine && *lpCmdLine) {
        if (wcsstr(lpCmdLine, L"--onar") || wcsstr(lpCmdLine, L"--ofar")) {
            // 需要管理员权限才能操作任务计划程序
            if (!IsAdministrator()) {
                MessageBoxW(nullptr, L"设置自启动需要管理员权限。", L"提示", MB_OK | MB_ICONWARNING);
                return 1;
            }
            HandleAutoRun(lpCmdLine);
            return 0;
        }
    }

    // 2. 单例互斥
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\PowerBrightnessSync_Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // 3. 权限检查 (运行时也需要管理员权限以写入电源设置)
    if (!IsAdministrator()) {
        MessageBoxW(nullptr, L"请以管理员身份运行以启用亮度同步。", L"PowerBrightnessSync", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // 4. 初始化
    PerformSync();

    // 5. 创建 Message-Only Window
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PBS_Lite_v2";
    RegisterClassW(&wc);
    
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // 6. 注册通知
    HPOWERNOTIFY hNotify = RegisterPowerSettingNotification(hwnd, &kGuidVideoBrightness, DEVICE_NOTIFY_WINDOW_HANDLE);

    // 7. 初始内存修剪
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    // 8. 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hNotify) UnregisterPowerSettingNotification(hNotify);
    if (hMutex) CloseHandle(hMutex);
    return (int)msg.wParam;
}