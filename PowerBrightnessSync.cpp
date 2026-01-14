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
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // ---------------------------------------------------------
    // 第一步：优先处理命令行参数 (完全绕过互斥体)
    // ---------------------------------------------------------
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool isSetupMode = false; // 标记是否为设置模式

    if (argv && argc > 1) {
        // 遍历所有参数查找命令
        for (int i = 1; i < argc; ++i) {
            if (lstrcmpiW(argv[i], L"--onar") == 0) {
                isSetupMode = true;
                if (!IsAdministrator()) {
                    MessageBoxW(nullptr, L"设置自启动失败：\n需要管理员权限。", L"权限不足", MB_OK | MB_ICONERROR);
                } else {
                    int ret = ManageAutoRun(true); // 你的自启函数
                    if (ret == 1) MessageBoxW(nullptr, L"自启动设置成功！", L"提示", MB_OK | MB_ICONINFORMATION);
                    else MessageBoxW(nullptr, L"自启动设置失败！\n请检查任务计划服务是否开启。", L"错误", MB_OK | MB_ICONERROR);
                }
                break; // 找到命令就跳出循环
            }
            else if (lstrcmpiW(argv[i], L"--ofar") == 0) {
                isSetupMode = true;
                if (!IsAdministrator()) {
                    MessageBoxW(nullptr, L"取消自启动失败：\n需要管理员权限。", L"权限不足", MB_OK | MB_ICONERROR);
                } else {
                    ManageAutoRun(false);
                    MessageBoxW(nullptr, L"已取消开机自启动。", L"提示", MB_OK | MB_ICONINFORMATION);
                }
                break;
            }
        }
    }
    if (argv) LocalFree(argv);

    // 【核心关键点】：如果是设置模式，执行完立刻退出程序！
    // 绝对不要让代码运行到下面的 CreateMutex，否则会和正在运行的实例冲突。
    if (isSetupMode) {
        return 0; 
    }

    // ---------------------------------------------------------
    // 第二步：由于没有参数，说明是正常启动，现在开始检查互斥体
    // ---------------------------------------------------------
    
    // 检查是否已经有实例在运行
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\PowerBrightnessSync_Instance_v2");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 如果已经有一个在运行，直接退出，保持单例
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // ---------------------------------------------------------
    // 第三步：权限检查与初始化 (只有获胜的单例才会执行到这里)
    // ---------------------------------------------------------
    
    // 运行时必须是管理员，否则无法写入电源设置
    if (!IsAdministrator()) {
        // 可以选择静默退出，或者弹窗提示（建议开发阶段弹窗，稳定后静默）
        // MessageBoxW(nullptr, L"请以管理员身份运行以启用同步。", L"权限提示", MB_OK);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // 初始化同步一次
    PerformSync();

    // ---------------------------------------------------------
    // 第四步：创建消息窗口并进入循环
    // ---------------------------------------------------------
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PBS_Lite_Host";
    
    if (!RegisterClassW(&wc)) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // 创建隐藏窗口用于接收电源通知
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    HPOWERNOTIFY hNotify = RegisterPowerSettingNotification(hwnd, &kGuidVideoBrightness, DEVICE_NOTIFY_WINDOW_HANDLE);
    
    // 启动完成，修剪内存
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---------------------------------------------------------
    // 第五步：清理资源
    // ---------------------------------------------------------
    if (hNotify) UnregisterPowerSettingNotification(hNotify);
    if (hMutex) CloseHandle(hMutex);

    return (int)msg.wParam;
}
