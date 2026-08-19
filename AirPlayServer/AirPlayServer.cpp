// AirPlayServer.cpp : AirPlay Receiver - Main Program
// Starts automatically and waits for AirPlay connections.
// Window shows home screen with ImGui UI.
//
#include <windows.h>
#include <signal.h>
#include <string.h>
#include "Airplay2Head.h"
#include "CAirServerCallback.h"
#include "SDL.h"
#include "CSDLPlayer.h"
#include "DebugLogger.h"

// Global player pointer for cleanup handlers
static CSDLPlayer* g_pPlayer = NULL;
static volatile bool g_bShuttingDown = false;

// Cleanup function to stop server gracefully
void CleanupAndShutdown()
{
    if (g_bShuttingDown) {
        return;  // Prevent re-entrancy
    }
    g_bShuttingDown = true;

    if (g_pPlayer != NULL) {
        g_pPlayer->m_server.stop();
        Sleep(150);
    }
    DebugLogger::Write("shutdown", "cleanup requested");
    DebugLogger::Stop();
}

// atexit handler as a fallback
void AtExitHandler()
{
    CleanupAndShutdown();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    const bool debugRequested = lpCmdLine &&
        (strstr(lpCmdLine, "--debug") != NULL || strstr(lpCmdLine, "/debug") != NULL);
    if (DebugLogger::Start(lpCmdLine)) {
        std::string logPath = DebugLogger::Path();
        std::wstring wPath;
        int pathWLen = MultiByteToWideChar(CP_ACP, 0, logPath.c_str(), (int)logPath.length(), NULL, 0);
        if (pathWLen > 0) {
            wPath.resize(pathWLen);
            MultiByteToWideChar(CP_ACP, 0, logPath.c_str(), (int)logPath.length(), &wPath[0], pathWLen);
        }
        std::wstring wmsg = L"调试日志已启用。\n\n日志文件：\n" + wPath;
        MessageBoxW(NULL, wmsg.c_str(), L"AirPlayServer - 调试日志",
            MB_OK | MB_ICONINFORMATION);
    }
    else if (debugRequested) {
        MessageBoxW(NULL,
            L"已请求调试日志，但无法创建日志文件。\n\n"
            L"请检查 %LOCALAPPDATA% 是否可写后重试。",
            L"AirPlayServer - 调试日志错误", MB_OK | MB_ICONERROR);
    }
    DebugLogger::Write("startup", "AirPlayServer starting; pid=%lu", GetCurrentProcessId());

    // Enable per-monitor DPI awareness for sharp rendering on high-DPI displays
    // Use runtime loading since these APIs require Windows 10 1703+
    {
        HMODULE hUser32 = GetModuleHandle(TEXT("user32.dll"));
        if (hUser32) {
            // Try Per-Monitor V2 first (best quality, Windows 10 1703+)
            typedef BOOL(WINAPI* SetDpiAwarenessContextFn)(HANDLE);
            SetDpiAwarenessContextFn fnCtx = (SetDpiAwarenessContextFn)
                GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
            if (fnCtx) {
                fnCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            } else {
                // Fallback: basic DPI awareness (Vista+)
                typedef BOOL(WINAPI* SetProcessDPIAwareFn)();
                SetProcessDPIAwareFn fnDpi = (SetProcessDPIAwareFn)
                    GetProcAddress(hUser32, "SetProcessDPIAware");
                if (fnDpi) fnDpi();
            }
        }
    }

    // Register atexit handler as fallback
    atexit(AtExitHandler);

    // Get default device name (PC name)
    char hostName[512] = { 0 };
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    gethostname(hostName, sizeof(hostName) - 1);
    if (strlen(hostName) == 0) {
        DWORD n = sizeof(hostName) - 1;
        if (::GetComputerNameA(hostName, &n)) {
            if (n > 0 && n < sizeof(hostName)) {
                hostName[n] = '\0';
            }
        }
    }
    if (strlen(hostName) == 0) {
        strcpy_s(hostName, sizeof(hostName), "AirPlay Server");
    }

    // Check Bonjour Service (required for mDNS device discovery)
    {
        SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
        if (hSCM)
        {
            // First try with start permission, fall back to query-only
            // (SERVICE_START requires admin — don't let that look like "not installed")
            bool bCanStart = true;
            SC_HANDLE hSvc = OpenServiceA(hSCM, "Bonjour Service",
                SERVICE_QUERY_STATUS | SERVICE_START);
            if (!hSvc)
            {
                bCanStart = false;
                hSvc = OpenServiceA(hSCM, "Bonjour Service", SERVICE_QUERY_STATUS);
            }

            if (!hSvc)
            {
                // Truly not installed
                CloseServiceHandle(hSCM);
                int choice = MessageBoxW(NULL,
                    L"未安装 Apple Bonjour。\n\n"
                    L"AirPlay 设备发现需要 Bonjour。\n\n"
                    L"点击“确定”打开 Bonjour 下载页，或点击“取消”退出。",
                    L"AirPlay Server - 未找到 Bonjour",
                    MB_OKCANCEL | MB_ICONWARNING);
                if (choice == IDOK)
                    ShellExecuteA(NULL, "open", "https://support.apple.com/kb/DL999",
                        NULL, NULL, SW_SHOWNORMAL);
                WSACleanup();
                return 1;
            }

            // Service exists — check its state
            SERVICE_STATUS ss = {};
            if (QueryServiceStatus(hSvc, &ss))
            {
                if (ss.dwCurrentState == SERVICE_STOPPED ||
                    ss.dwCurrentState == SERVICE_PAUSED)
                {
                    if (bCanStart)
                    {
                        // Try to start it
                        if (!StartServiceA(hSvc, 0, NULL))
                        {
                            DWORD err = GetLastError();
                            if (err != ERROR_SERVICE_ALREADY_RUNNING)
                            {
                                wchar_t msg[256];
                                _snwprintf_s(msg, sizeof(msg)/sizeof(msg[0]), _TRUNCATE,
                                    L"Bonjour 服务无法启动（错误 %lu）。\n\n"
                                    L"请尝试以管理员身份运行，或通过 services.msc 手动启动该服务。",
                                    err);
                                MessageBoxW(NULL, msg, L"AirPlay Server - Bonjour 错误",
                                    MB_OK | MB_ICONWARNING);
                                CloseServiceHandle(hSvc);
                                CloseServiceHandle(hSCM);
                                WSACleanup();
                                return 1;
                            }
                        }
                        else
                        {
                            // Wait up to 5 seconds for it to reach SERVICE_RUNNING
                            for (int i = 0; i < 50; i++)
                            {
                                Sleep(100);
                                if (QueryServiceStatus(hSvc, &ss) &&
                                    ss.dwCurrentState == SERVICE_RUNNING)
                                    break;
                            }
                        }
                    }
                    else
                    {
                        // No permission to start — ask user to do it manually
                        MessageBoxW(NULL,
                            L"Bonjour 服务已安装但未运行。\n\n"
                            L"请尝试以管理员身份运行，或通过 services.msc 手动启动该服务。",
                            L"AirPlay Server - Bonjour 已停止",
                            MB_OK | MB_ICONWARNING);
                        CloseServiceHandle(hSvc);
                        CloseServiceHandle(hSCM);
                        WSACleanup();
                        return 1;
                    }
                }
            }

            CloseServiceHandle(hSvc);
            CloseServiceHandle(hSCM);
        }
    }

    CSDLPlayer player;
    g_pPlayer = &player;  // Set global pointer for cleanup handlers
    player.setServerName(hostName);

    if (!player.init()) {
        DebugLogger::Write("startup", "player initialization failed");
        g_pPlayer = NULL;
        return 1;
    }

    player.loopEvents();
    DebugLogger::Write("shutdown", "event loop exited");
    DebugLogger::Stop();

    // Clear global pointer before player is destroyed
    g_pPlayer = NULL;

    return 0;
}
