// InputBridge 原生转交的窗口级集成测试。
// 用隐藏的宿主窗口与 Chrome_WidgetWin_1 子窗口模拟 WebView2 的客户区，验证
// WM_ACTIVATE、客户区坐标转换、滚轮屏幕坐标转换、键盘/字符转交，以及子窗口重建后的重新定位。
// 测试只投递窗口消息，不移动系统光标，也不改变前台窗口。

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>

#include <windows.h>
#include <windowsx.h>

#include "InputBridge.h"

namespace
{

constexpr wchar_t kHostClass[] = L"MaaEnd.CloudShellTestHost";
constexpr wchar_t kWidgetClass[] = L"Chrome_WidgetWin_1";
constexpr wchar_t kDecoyClass[] = L"MaaEnd.CloudShellTestDecoy";

struct Recorded
{
    UINT message = 0;
    WPARAM wparam = 0;
    LPARAM lparam = 0;
};

struct WidgetLog
{
    std::vector<Recorded> messages;
};

WidgetLog* g_widget_log = nullptr;
int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

LRESULT CALLBACK widgetProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (g_widget_log) {
        g_widget_log->messages.push_back(Recorded { .message = message, .wparam = wparam, .lparam = lparam });
    }
    // Chromium 的输入窗口自己消费激活消息，不把宿主带到前台；DefWindowProc 会在测试进程里激活窗口。
    if (message == WM_ACTIVATE) {
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool registerWindowClass(const wchar_t* name, WNDPROC proc)
{
    WNDCLASSEXW wc { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = name;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void pumpMessages()
{
    MSG msg {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void clearLog()
{
    g_widget_log->messages.clear();
}

const Recorded* findLast(UINT message)
{
    for (auto it = g_widget_log->messages.rbegin(); it != g_widget_log->messages.rend(); ++it) {
        if (it->message == message) {
            return &*it;
        }
    }
    return nullptr;
}

int recordCount(UINT message)
{
    int count = 0;
    for (const auto& record : g_widget_log->messages) {
        if (record.message == message) {
            ++count;
        }
    }
    return count;
}

HWND createHidden(HWND parent, const wchar_t* cls, int x, int y, int width, int height)
{
    return CreateWindowExW(
        0,
        cls,
        L"",
        WS_CHILD,
        x,
        y,
        width,
        height,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
}

bool envFlag(const wchar_t* name)
{
    wchar_t value[16] {};
    const DWORD length = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
    return length > 0 && static_cast<size_t>(length) < std::size(value);
}

void printWindow(const char* label, HWND hwnd)
{
    wchar_t cls[128] {};
    wchar_t title[128] {};
    DWORD pid = 0;
    if (hwnd) {
        GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
        GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        GetWindowThreadProcessId(hwnd, &pid);
    }
    std::printf("%s hwnd=%p pid=%lu class=%ls title=%ls\n", label, static_cast<void*>(hwnd), pid, cls, title);
}

void checkMousePoint(const Recorded* record, int expected_x, int expected_y, const char* what)
{
    if (!record) {
        check(false, what);
        return;
    }
    check(GET_X_LPARAM(record->lparam) == expected_x && GET_Y_LPARAM(record->lparam) == expected_y, what);
}

} // namespace

int main()
{
    if (!registerWindowClass(kHostClass, DefWindowProcW) || !registerWindowClass(kWidgetClass, widgetProc)
        || !registerWindowClass(kDecoyClass, DefWindowProcW)) {
        std::printf("FAIL: cannot register test window classes (error %lu)\n", GetLastError());
        return 1;
    }

    POINT cursor_before {};
    GetCursorPos(&cursor_before);
    const HWND foreground_before = GetForegroundWindow();

    const HWND host = CreateWindowExW(
        0,
        kHostClass,
        L"input bridge test host",
        WS_OVERLAPPED,
        100,
        100,
        400,
        300,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (!host) {
        std::printf("FAIL: cannot create host window (error %lu)\n", GetLastError());
        return 1;
    }

    WidgetLog log;
    g_widget_log = &log;

    // 干扰项排在前面的枚举顺序里，选择逻辑仍应命中 Chrome_WidgetWin_1。
    const HWND decoy = createHidden(host, kDecoyClass, 0, 0, 50, 50);
    HWND widget = createHidden(host, kWidgetClass, 40, 30, 200, 150);
    if (!decoy || !widget) {
        std::printf("FAIL: cannot create child windows (error %lu)\n", GetLastError());
        DestroyWindow(host);
        return 1;
    }

    cloudshell::InputBridge bridge(host, cloudshell::InputBridge::Mode::Native);
    bridge.Attach(nullptr);
    check(GetForegroundWindow() != host, "window creation did not bring the shell to the foreground");

    clearLog();
    bridge.HandleMessage(WM_ACTIVATE, WA_ACTIVE, 0);
    pumpMessages();
    {
        const Recorded* record = findLast(WM_ACTIVATE);
        check(record && record->wparam == WA_ACTIVE, "WM_ACTIVATE forwarded with original wParam");
        check(recordCount(WM_ACTIVATE) == 1, "WM_ACTIVATE reaches the widget exactly once");
    }
    if (GetForegroundWindow() == host) {
        printWindow("foreground after activation", GetForegroundWindow());
    }
    check(GetForegroundWindow() != host, "forwarded WM_ACTIVATE did not bring the shell to the foreground");

    clearLog();
    bridge.HandleMessage(WM_MOUSEMOVE, 0, MAKELPARAM(100, 80));
    pumpMessages();
    checkMousePoint(findLast(WM_MOUSEMOVE), 60, 50, "WM_MOUSEMOVE converted from host to widget client coordinates");

    clearLog();
    bridge.HandleMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 80));
    bridge.HandleMessage(WM_LBUTTONUP, 0, MAKELPARAM(100, 80));
    pumpMessages();
    checkMousePoint(findLast(WM_LBUTTONDOWN), 60, 50, "WM_LBUTTONDOWN converted to widget client coordinates");
    checkMousePoint(findLast(WM_LBUTTONUP), 60, 50, "WM_LBUTTONUP converted to widget client coordinates");

    clearLog();
    POINT screen { 100, 80 };
    ClientToScreen(host, &screen);
    bridge.HandleMessage(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(screen.x, screen.y));
    pumpMessages();
    {
        const Recorded* record = findLast(WM_MOUSEWHEEL);
        check(record && GET_X_LPARAM(record->lparam) == screen.x && GET_Y_LPARAM(record->lparam) == screen.y,
              "WM_MOUSEWHEEL keeps screen coordinates");
        check(record && GET_WHEEL_DELTA_WPARAM(record->wparam) == WHEEL_DELTA, "WM_MOUSEWHEEL keeps wheel delta");
        const auto& messages = g_widget_log->messages;
        const bool pre_move = messages.size() >= 2 && messages[messages.size() - 2].message == WM_MOUSEMOVE
                              && messages.back().message == WM_MOUSEWHEEL;
        check(!pre_move, "WM_MOUSEWHEEL does not synthesize a move");
    }

    clearLog();
    bridge.HandleMessage(WM_KEYDOWN, VK_F1, 0);
    bridge.HandleMessage(WM_KEYUP, VK_F1, 0);
    bridge.HandleMessage(WM_CHAR, L'A', 0);
    pumpMessages();
    {
        const Recorded* down = findLast(WM_KEYDOWN);
        const Recorded* up = findLast(WM_KEYUP);
        const Recorded* character = findLast(WM_CHAR);
        check(down && down->wparam == VK_F1, "WM_KEYDOWN forwarded");
        check(up && up->wparam == VK_F1, "WM_KEYUP forwarded");
        check(character && character->wparam == L'A', "WM_CHAR forwarded");
    }

    // WebView2 的输入子窗口重建后，下一次输入应重新定位到新窗口。
    DestroyWindow(widget);
    pumpMessages();
    widget = createHidden(host, kWidgetClass, 70, 50, 200, 150);
    if (!widget) {
        std::printf("FAIL: cannot recreate widget window (error %lu)\n", GetLastError());
        DestroyWindow(host);
        return 1;
    }
    clearLog();
    bridge.HandleMessage(WM_MOUSEMOVE, 0, MAKELPARAM(100, 80));
    pumpMessages();
    checkMousePoint(findLast(WM_MOUSEMOVE), 30, 30, "recreated widget is located and receives converted coordinates");

    // 子窗口暂时消失时原生路径失败，但不应崩溃；重建后自动恢复。
    DestroyWindow(widget);
    pumpMessages();
    clearLog();
    bridge.HandleMessage(WM_MOUSEMOVE, 0, MAKELPARAM(100, 80));
    pumpMessages();
    check(recordCount(WM_MOUSEMOVE) == 0, "missing widget does not receive input");
    widget = createHidden(host, kWidgetClass, 10, 20, 200, 150);
    if (!widget) {
        std::printf("FAIL: cannot recreate widget after removal (error %lu)\n", GetLastError());
        DestroyWindow(host);
        return 1;
    }
    clearLog();
    bridge.HandleMessage(WM_MOUSEMOVE, 0, MAKELPARAM(100, 80));
    pumpMessages();
    checkMousePoint(findLast(WM_MOUSEMOVE), 90, 60, "input resumes after the widget is recreated");

    // Cdp 模式不向子窗口转发原生消息。
    cloudshell::InputBridge cdp_bridge(host, cloudshell::InputBridge::Mode::Cdp);
    cdp_bridge.Attach(nullptr);
    clearLog();
    cdp_bridge.HandleMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 80));
    pumpMessages();
    check(recordCount(WM_LBUTTONDOWN) == 0, "cdp mode does not forward native mouse messages");

    POINT cursor_after {};
    GetCursorPos(&cursor_after);
    const HWND foreground_after = GetForegroundWindow();
    // 本机运行时用户可能同时移动鼠标；CI 与显式设置的验证环境要求光标完全不动。
    if (envFlag(L"CI") || envFlag(L"CLOUDSHELL_TEST_REQUIRE_CURSOR_STABLE")) {
        check(cursor_after.x == cursor_before.x && cursor_after.y == cursor_before.y, "system cursor did not move");
    }
    else {
        std::printf("[SKIP] system cursor stability (set CI=1 to require)\n");
    }
    if (foreground_after != foreground_before) {
        printWindow("foreground before", foreground_before);
        printWindow("foreground after", foreground_after);
    }
    check(foreground_after != host, "input forwarding did not bring the shell to the foreground");

    DestroyWindow(widget);
    DestroyWindow(decoy);
    DestroyWindow(host);
    pumpMessages();

    g_widget_log = nullptr;
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
