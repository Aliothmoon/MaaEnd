// InputBridge 原生转交的窗口级集成测试。
// 用隐藏的宿主窗口与 Chrome_WidgetWin_1 子窗口模拟 WebView2 的客户区，验证
// WM_ACTIVATE、客户区坐标转换、滚轮屏幕坐标转换、键盘/字符转交，以及子窗口重建后的重新定位。
// 另外用 LookCommands/TrimQueue/ArmWatchdog 这三个不依赖 WebView2 的函数覆盖 look 命令组、
// 队列上限裁剪与看门狗启动失败（N-04）的纯逻辑。pump() 本体需要真实 ICoreWebView2，
// 仍只能靠运行期验证。
// 测试只投递窗口消息，不移动系统光标，也不改变前台窗口。

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <string>
#include <utility>
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

void appendLook(std::deque<cloudshell::InputBridge::Command>& queue, uint64_t group)
{
    for (auto& command : cloudshell::InputBridge::LookCommands(10, 10, 0, group)) {
        queue.push_back(std::move(command));
    }
}

// 队列里的每个 look 组都必须完整（一次 look 的命令数），组号单调递增
bool lookGroupsIntact(const std::deque<cloudshell::InputBridge::Command>& queue, uint64_t oldest_group)
{
    bool intact = true;
    uint64_t previous = oldest_group - 1;
    for (size_t index = 0; index < queue.size();) {
        const uint64_t group = queue[index].group;
        size_t end = index + 1;
        while (end < queue.size() && queue[end].group == group) {
            ++end;
        }
        if (group != 0) {
            constexpr size_t kLookCommands = 4;
            intact = intact && end - index == kLookCommands && group > previous;
            previous = group;
        }
        index = end;
    }
    return intact;
}

bool contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

void testLookGroup()
{
    const auto commands = cloudshell::InputBridge::LookCommands(120, -80, 0, 7);
    check(commands.size() == 4, "one look enqueues four CDP commands");
    bool same_group = !commands.empty();
    for (const auto& command : commands) {
        same_group = same_group && command.group == 7;
    }
    check(same_group, "all look commands share one group id");
    check(
        commands.size() == 4 && commands[0].method == "Runtime.evaluate" && commands[1].method == "Input.dispatchMouseEvent"
            && commands[2].method == "Runtime.evaluate" && commands[3].method == "Input.dispatchMouseEvent",
        "look group order is register/move/zero-next/move-back");
    check(
        commands.size() == 4 && contains(commands[1].params, "\"x\":760") && contains(commands[1].params, "\"y\":280"),
        "look displacement is applied relative to the viewport anchor");
    check(
        commands.size() == 4 && contains(commands[2].params, "__maaShellZeroNext"),
        "look group resets the page-side zero-next marker");
    check(
        commands.size() == 4 && contains(commands[3].params, "\"x\":640") && contains(commands[3].params, "\"y\":360"),
        "look group returns the virtual pointer to the anchor");

    // 协议上限 ±640/±360：更大的转向由发送方拆条，壳侧只钳到视口内
    const auto clamped = cloudshell::InputBridge::LookCommands(5000, -5000, 0, 8);
    check(clamped.size() == 4 && contains(clamped[1].params, "\"x\":1279"), "look dx clamps to viewport width - 1");
    check(clamped.size() == 4 && contains(clamped[1].params, "\"y\":0"), "look dy clamps to viewport top");
    check(clamped.size() == 4 && contains(clamped[3].params, "\"x\":640"), "clamped look still returns to the anchor");
}

void testQueueTrim()
{
    // 10 组 look = 40 条，裁到上限 32：整组丢弃最旧的两组
    std::deque<cloudshell::InputBridge::Command> queue;
    for (uint64_t group = 1; group <= 10; ++group) {
        appendLook(queue, group);
    }
    const size_t dropped = cloudshell::InputBridge::TrimQueue(queue, cloudshell::InputBridge::kMaxQueue);
    check(queue.size() <= cloudshell::InputBridge::kMaxQueue, "look queue is trimmed to the hard cap");
    check(dropped == 8, "two oldest look groups are dropped as a whole");
    check(lookGroupsIntact(queue, 3), "remaining look groups stay complete and in order");
    check(!queue.empty() && queue.back().group == 10, "newest look group is kept");

    // 组被 pump 取走前两条后只剩残段：残段永远不能丢（否则页面停在已位移状态）
    std::deque<cloudshell::InputBridge::Command> partial;
    appendLook(partial, 1);
    partial.pop_front();
    partial.pop_front();
    for (uint64_t group = 2; group <= 10; ++group) {
        appendLook(partial, group);
    }
    const size_t partial_dropped = cloudshell::InputBridge::TrimQueue(partial, cloudshell::InputBridge::kMaxQueue);
    check(!partial.empty() && partial.front().group == 1, "partially dispatched look group is never dropped");
    check(partial_dropped == 8, "only complete look groups behind the partial head are dropped");
    check(partial.size() <= cloudshell::InputBridge::kMaxQueue, "queue with a partial head group still fits the cap");

    // 非 look 命令顺序不变，也不会被 look 组裁剪吃掉
    std::deque<cloudshell::InputBridge::Command> mixed;
    mixed.push_back(cloudshell::InputBridge::Command { .method = "Runtime.evaluate", .params = "first" });
    for (uint64_t group = 1; group <= 6; ++group) {
        appendLook(mixed, group);
    }
    mixed.push_back(cloudshell::InputBridge::Command { .method = "Runtime.evaluate", .params = "second" });
    for (uint64_t group = 7; group <= 12; ++group) {
        appendLook(mixed, group);
    }
    cloudshell::InputBridge::TrimQueue(mixed, cloudshell::InputBridge::kMaxQueue);
    std::vector<std::string> non_look;
    for (const auto& command : mixed) {
        if (command.group == 0) {
            non_look.push_back(command.params);
        }
    }
    check(
        non_look.size() == 2 && non_look[0] == "first" && non_look[1] == "second",
        "non-look commands keep their order and are never dropped");
    check(mixed.size() <= cloudshell::InputBridge::kMaxQueue, "mixed queue is trimmed to the cap");
    check(lookGroupsIntact(mixed, 6), "trimmed mixed queue keeps complete look groups in order");

    // 队列里全是非 look 命令时无组可丢：TrimQueue 不做任何事（调用方据此不丢弃控制器输入）
    std::deque<cloudshell::InputBridge::Command> keys;
    for (int index = 0; index < 40; ++index) {
        keys.push_back(cloudshell::InputBridge::Command { .method = "Input.dispatchKeyEvent", .params = std::to_string(index) });
    }
    const size_t key_dropped = cloudshell::InputBridge::TrimQueue(keys, cloudshell::InputBridge::kMaxQueue);
    check(key_dropped == 0 && keys.size() == 40, "trimming never drops non-look commands");
}

void testWatchdogArm(HWND host)
{
    const UINT_PTR timer = cloudshell::InputBridge::ArmWatchdog(host, 1);
    check(timer != 0, "watchdog timer arms on a live window");
    if (timer != 0) {
        KillTimer(host, timer);
    }

    // 失效窗口上 SetTimer 返回 0（error 1400）：pump 需要识别失败并记一次 Error；
    // busy_ 仍保持串行屏障（CDP 命令有严格先后），不能为了“不断流”改成并发投递。
    const HWND doomed = createHidden(host, kDecoyClass, 0, 0, 10, 10);
    if (!doomed) {
        check(false, "cannot create the window used for the watchdog failure case");
        return;
    }
    DestroyWindow(doomed);
    pumpMessages();
    SetLastError(0);
    const UINT_PTR failed = cloudshell::InputBridge::ArmWatchdog(doomed, 2);
    check(failed == 0, "watchdog arming fails on an invalid window");
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

    // 不依赖 WebView2 的纯逻辑：look 命令组、队列裁剪、看门狗定时器启动
    testLookGroup();
    testQueueTrim();
    testWatchdogArm(host);

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
