#include "InputBridge.h"

#include <algorithm>
#include <cwchar>
#include <format>
#include <iterator>

#include <windowsx.h>
#include <wrl.h>

#include "KeyMap.h"
#include "Log.h"
#include "Resources.h"

namespace cloudshell
{

namespace
{

constexpr int kModifierAlt = 1;
constexpr int kModifierCtrl = 2;
constexpr int kModifierMeta = 4;
constexpr int kModifierShift = 8;

constexpr int kButtonLeft = 1;
constexpr int kButtonRight = 2;
constexpr int kButtonMiddle = 4;
constexpr int kButtonBack = 8;
constexpr int kButtonForward = 16;

// 页面对话框或渲染进程卡死时 CDP 命令可能永远不返回，超时就丢弃，后面的输入才能继续
constexpr UINT kCommandTimeoutMs = 3000;

// 页面视口固定 1280x720（README「与 MaaFramework 的约定」），转向锚点即视口中线
constexpr int kViewportW = 1280;
constexpr int kViewportH = 720;
constexpr int kLookAnchorX = kViewportW / 2;
constexpr int kLookAnchorY = kViewportH / 2;

// 未发出的移动只保留最新一次：队列尾部连续 kMoveBatch 条移动会被替换掉
constexpr size_t kMoveBatch = 2;

std::string jsonEscape(std::string_view src)
{
    std::string out;
    out.reserve(src.size() + 2);
    for (const char ch : src) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        }
        else if (static_cast<unsigned char>(ch) < 0x20) {
            out += std::format("\\u{:04x}", static_cast<unsigned char>(ch));
        }
        else {
            out += ch;
        }
    }
    return out;
}

const char* buttonName(int button)
{
    switch (button) {
    case kButtonLeft:
        return "left";
    case kButtonRight:
        return "right";
    case kButtonMiddle:
        return "middle";
    case kButtonBack:
        return "back";
    case kButtonForward:
        return "forward";
    default:
        return "none";
    }
}

// 系统交换了左右键时 MaaFramework 把 contact 0 投递成右键消息，换回来
int unswapButton(int button)
{
    if (GetSystemMetrics(SM_SWAPBUTTON) == 0) {
        return button;
    }
    if (button == kButtonLeft) {
        return kButtonRight;
    }
    if (button == kButtonRight) {
        return kButtonLeft;
    }
    return button;
}

// 页面里的 KeyboardEvent.keyCode 对左右修饰键报的是泛指键码
unsigned genericVirtualKey(unsigned vk)
{
    switch (vk) {
    case VK_LSHIFT:
    case VK_RSHIFT:
        return VK_SHIFT;
    case VK_LCONTROL:
    case VK_RCONTROL:
        return VK_CONTROL;
    case VK_LMENU:
    case VK_RMENU:
        return VK_MENU;
    default:
        return vk;
    }
}

int modifierBit(unsigned vk)
{
    switch (genericVirtualKey(vk)) {
    case VK_SHIFT:
        return kModifierShift;
    case VK_CONTROL:
        return kModifierCtrl;
    case VK_MENU:
        return kModifierAlt;
    case VK_LWIN:
    case VK_RWIN:
        return kModifierMeta;
    default:
        return 0;
    }
}

} // namespace

InputBridge::InputBridge(HWND hwnd, Mode mode)
    : hwnd_(hwnd)
    , mode_(mode)
{
}

void InputBridge::Attach(ICoreWebView2* webview)
{
    webview_ = webview;
    if (mode_ != Mode::Cdp) {
        native_ = ensureWidget();
    }
    logNativeMode();
}

void InputBridge::Detach()
{
    if (watchdog_timer_ != 0) {
        KillTimer(hwnd_, watchdog_timer_);
        watchdog_timer_ = 0;
    }
    widget_ = nullptr;
    native_ = false;
    webview_.Reset();
    queue_.clear();
}

void InputBridge::logNativeMode()
{
    if (mode_ == Mode::Cdp) {
        log::Info("input mode: cdp");
    }
    else if (native_) {
        log::Info("input mode: native (Chrome_WidgetWin_1)");
    }
    else {
        log::Info("input mode: cdp fallback (Chrome_WidgetWin_1 not found)");
    }
}

bool InputBridge::ensureWidget()
{
    if (widget_ && IsWindow(widget_)) {
        return true;
    }
    widget_ = nullptr;
    struct Context
    {
        HWND found = nullptr;
    } context;
    EnumChildWindows(
        hwnd_,
        [](HWND child, LPARAM param) -> BOOL {
            wchar_t cls[64] {};
            if (GetClassNameW(child, cls, static_cast<int>(std::size(cls))) == 0) {
                return TRUE;
            }
            if (wcscmp(cls, L"Chrome_WidgetWin_1") != 0) {
                return TRUE;
            }
            reinterpret_cast<Context*>(param)->found = child;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&context));
    widget_ = context.found;
    return widget_ != nullptr;
}

bool InputBridge::isWheelPointOnShell(LPARAM lparam) const
{
    // 视频/合成页在窗口被遮挡时会让 Chromium 丢弃原生滚轮；只有滚轮屏幕坐标
    // 确实落在本壳体的可见区域时才走原生，否则交给 CDP。
    if (IsIconic(hwnd_) || !IsWindowVisible(hwnd_)) {
        return false;
    }
    const POINT point { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    const HWND hit = WindowFromPoint(point);
    if (!hit) {
        return false;
    }
    const HWND hit_root = GetAncestor(hit, GA_ROOT);
    if (hit_root == hwnd_) {
        return true;
    }
    // 视频合成页的 WindowFromPoint 可能返回 WebView2 的顶层 Chrome 窗口；
    // 无法确认壳体可见时交给 CDP，避免原生消息被 Chromium 静默丢弃。
    return false;
}

void InputBridge::updateModifiers(unsigned vk, bool down)
{
    if (const int bit = modifierBit(vk); bit != 0) {
        modifiers_ = down ? (modifiers_ | bit) : (modifiers_ & ~bit);
    }
}

bool InputBridge::forwardNative(UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (!ensureWidget()) {
        native_ = false;
        return false;
    }

    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        POINT point { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        MapWindowPoints(hwnd_, widget_, &point, 1);
        lparam = MAKELPARAM(point.x, point.y);
        break;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        // WM_MOUSEWHEEL/WM_MOUSEHWHEEL 的 lParam 始终是屏幕坐标，Chromium 也按屏幕坐标命中；
        // 转成客户区坐标会让滚轮落到错误位置或被丢弃。保持原 lParam 直接转交。
        break;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        updateModifiers(static_cast<unsigned>(wparam), msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        break;
    default:
        break;
    }

    if (!PostMessageW(widget_, msg, wparam, lparam)) {
        log::Warn("native input forward failed: msg=0x{:04x}", msg);
        native_ = false;
        return false;
    }
    return true;
}

std::optional<LRESULT> InputBridge::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam)
{
    const auto try_native = [&](UINT message) {
        if (mode_ == Mode::Cdp) {
            return false;
        }
        if (!native_ && !ensureWidget()) {
            return false;
        }
        if (!forwardNative(message, wparam, lparam)) {
            return false;
        }
        native_ = true;
        return true;
    };

    switch (msg) {
    case WM_ACTIVATE:
        // 只处理 Run 中截获的投递激活；WndProc 里的真实激活不会走到这里
        try_native(msg);
        return 0;

    case WM_MOUSEMOVE:
        if (try_native(msg)) {
            return 0;
        }
        // 控制器投递的移动总带着按键标志；不带的是偶尔落到顶层窗口上的真实鼠标移动
        if (wparam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) {
            onMouseMove(lparam);
        }
        return 0;

    case kLookMessage:
        dispatchLook(static_cast<int16_t>(LOWORD(wparam)), static_cast<int16_t>(LOWORD(lparam)));
        return 0;

    case WM_TIMER:
        if (wparam != watchdog_timer_) {
            return std::nullopt;
        }
        onWatchdog();
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        if (try_native(msg)) {
            return 0;
        }
        onMouseButton(kButtonLeft, msg == WM_LBUTTONDOWN, lparam);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        if (try_native(msg)) {
            return 0;
        }
        onMouseButton(kButtonRight, msg == WM_RBUTTONDOWN, lparam);
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        if (try_native(msg)) {
            return 0;
        }
        onMouseButton(kButtonMiddle, msg == WM_MBUTTONDOWN, lparam);
        return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        if (try_native(msg)) {
            return TRUE;
        }
        onMouseButton(GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? kButtonBack : kButtonForward, msg == WM_XBUTTONDOWN, lparam);
        return TRUE;

    case WM_MOUSEWHEEL: {
        const bool native_allowed = mode_ == Mode::Native || (mode_ == Mode::Auto && isWheelPointOnShell(lparam));
        if (native_allowed && try_native(msg)) {
            return 0;
        }
        if (mode_ == Mode::Auto && !native_allowed) {
            log::Info("native wheel fallback: point not over shell");
        }
        onMouseWheel(false, wparam, lparam);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        const bool native_allowed = mode_ == Mode::Native || (mode_ == Mode::Auto && isWheelPointOnShell(lparam));
        if (native_allowed && try_native(msg)) {
            return 0;
        }
        if (mode_ == Mode::Auto && !native_allowed) {
            log::Info("native wheel fallback: point not over shell");
        }
        onMouseWheel(true, wparam, lparam);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP:
        if (try_native(msg)) {
            return 0;
        }
        onKey(msg == WM_KEYDOWN, wparam, lparam);
        return 0;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (try_native(msg)) {
            return 0;
        }
        return std::nullopt;
    case WM_CHAR:
        // 控制器 input_text 投递的 WM_CHAR lParam 为 0；TranslateMessage 产生的重复字符不转交
        if (lparam == 0 && try_native(msg)) {
            return 0;
        }
        onChar(wparam, lparam);
        return 0;

    default:
        return std::nullopt;
    }
}

void InputBridge::SetForeground(bool foreground)
{
    if (foreground_ == foreground) {
        return;
    }
    foreground_ = foreground;
    log::Info("foreground={}", foreground_);
    SyncPageState();
}

void InputBridge::SyncPageState()
{
    evaluate(std::format("window.__maaShellSetForeground&&window.__maaShellSetForeground({})", foreground_));
}

void InputBridge::onMouseButton(int raw_button, bool down, LPARAM lparam)
{
    const int button = unswapButton(raw_button);

    // 抬起消息带的是控制器全局最后一次的坐标，不一定属于这个按键，沿用当前指针位置
    if (down) {
        pointer_ = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        buttons_ |= button;
    }
    else {
        buttons_ &= ~button;
    }

    log::Info("mouse {} {} ({}, {})", buttonName(button), down ? "down" : "up", pointer_.x, pointer_.y);
    dispatchMouse(down ? "mousePressed" : "mouseReleased", buttonName(button), 1);
}

void InputBridge::onMouseMove(LPARAM lparam)
{
    pointer_ = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    // button 取按住的第一个键，即最低位
    dispatchMouse("mouseMoved", buttonName(buttons_ & -buttons_), 0);
}

// 只在页面里产生一次带位移的 mousemove，再以零位移回位；不移动系统光标，也不改窗口或焦点
void InputBridge::dispatchLook(int dx, int dy)
{
    const int x = std::clamp(kLookAnchorX + dx, 0, kViewportW - 1);
    const int y = std::clamp(kLookAnchorY + dy, 0, kViewportH - 1);
    const auto make_move = [&](int mx, int my) {
        return std::format(
            R"({{"type":"mouseMoved","x":{},"y":{},"button":"none","buttons":0,"clickCount":0,"modifiers":{}}})",
            mx,
            my,
            modifiers_);
    };
    enqueue(
        "Runtime.evaluate",
        std::format(
            R"js({{"expression":"window.__maaShellLook&&window.__maaShellLook({},{})"}})js",
            x,
            y));
    enqueue("Input.dispatchMouseEvent", make_move(x, y));
    enqueue("Runtime.evaluate", R"js({"expression":"window.__maaShellZeroNext&&window.__maaShellZeroNext()"})js");
    enqueue("Input.dispatchMouseEvent", make_move(kLookAnchorX, kLookAnchorY));
}

void InputBridge::onMouseWheel(bool horizontal, WPARAM wparam, LPARAM lparam)
{
    // 滚轮消息的坐标是屏幕坐标
    POINT point { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    if (ScreenToClient(hwnd_, &point)) {
        pointer_ = point;
    }

    // 滚轮正值向上，CDP 的 deltaY 正值向下；水平方向一致
    const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
    log::Info("wheel horizontal={} delta={} ({}, {})", horizontal, delta, pointer_.x, pointer_.y);
    dispatchMouse("mouseWheel", "none", 0, horizontal ? delta : 0, horizontal ? 0 : -delta);
}

void InputBridge::onKey(bool down, WPARAM wparam, LPARAM lparam)
{
    const auto vk = static_cast<unsigned>(wparam);
    const bool extended = ((lparam >> 24) & 1) != 0;
    const auto info = LookupKey(vk, extended);
    if (!info) {
        log::Warn("unsupported key vk={}", vk);
        return;
    }

    if (const int bit = modifierBit(vk); bit != 0) {
        modifiers_ = down ? (modifiers_ | bit) : (modifiers_ & ~bit);
    }

    const std::string& key = (modifiers_ & kModifierShift) ? info->shifted_key : info->key;

    // 文本统一走 WM_CHAR → Input.insertText，按键用不产生文本的 rawKeyDown；
    // 回车例外，表单提交依赖带文本的 keypress
    std::string type = down ? "rawKeyDown" : "keyUp";
    std::string text_fields;
    if (down && vk == VK_RETURN) {
        type = "keyDown";
        text_fields = R"(,"text":"\r","unmodifiedText":"\r")";
    }

    const unsigned js_vk = genericVirtualKey(vk);
    std::string params = std::format(
        R"({{"type":"{}","modifiers":{},"windowsVirtualKeyCode":{},"nativeVirtualKeyCode":{},"code":"{}","key":"{}","location":{}{}{}}})",
        type,
        modifiers_,
        js_vk,
        js_vk,
        jsonEscape(info->code),
        jsonEscape(key),
        info->location,
        // TODO(#6841): location == 3 应改为 KeyLocation::Numpad 枚举比较，枚举由 #6841 的 KeyMap 补丁引入
        info->location == 3 ? R"(,"isKeypad":true)" : "",
        text_fields);

    log::Info("key vk={} {} {}", vk, down ? "down" : "up", info->code);
    // 页面策略会在 keydown 里补发 mouseenter，SDK 可能随之申请指针锁定，先记为控制器输入
    if (down) {
        evaluate("window.__maaShellExpect&&window.__maaShellExpect()");
    }
    enqueue("Input.dispatchKeyEvent", std::move(params));
}

void InputBridge::onChar(WPARAM wparam, LPARAM lparam)
{
    // TranslateMessage 会把投递的 WM_KEYDOWN 再翻译出带扫描码的 WM_CHAR，与 keydown 重复；
    // 控制器 input_text 投递的 WM_CHAR lParam 为 0
    if (lparam != 0) {
        return;
    }

    const auto ch = static_cast<wchar_t>(wparam);
    std::wstring text;
    if (IS_HIGH_SURROGATE(ch)) {
        pending_high_surrogate_ = ch;
        return;
    }
    if (IS_LOW_SURROGATE(ch)) {
        if (pending_high_surrogate_ == 0) {
            return;
        }
        text = { pending_high_surrogate_, ch };
    }
    else if (ch >= 0x20 && ch != 0x7f) {
        text = ch;
    }
    pending_high_surrogate_ = 0;

    if (text.empty()) {
        return;
    }

    enqueue("Input.insertText", std::format(R"({{"text":"{}"}})", jsonEscape(WideToUtf8(text))));
}

void InputBridge::dispatchMouse(std::string_view type, std::string_view button, int click_count, int delta_x, int delta_y)
{
    std::string params = std::format(
        R"({{"type":"{}","x":{},"y":{},"button":"{}","buttons":{},"clickCount":{},"modifiers":{})",
        type,
        pointer_.x,
        pointer_.y,
        button,
        buttons_,
        click_count,
        modifiers_);
    if (type == "mouseWheel") {
        params += std::format(R"(,"deltaX":{},"deltaY":{})", delta_x, delta_y);
    }
    params += '}';

    // 拖动时控制器每 10ms 投递一次移动，渲染进程按帧确认，命令会越积越多；还没发出的移动只保留最新的
    const bool move = type == "mouseMoved";
    if (move && queue_.size() >= kMoveBatch && queue_.back().move && queue_[queue_.size() - kMoveBatch].move) {
        queue_.pop_back();
        queue_.pop_back();
    }

    // 页面策略按坐标认出控制器的事件
    const std::string expect = std::format("window.__maaShellExpect&&window.__maaShellExpect({},{})", pointer_.x, pointer_.y);
    queue_.push_back(Command { .method = "Runtime.evaluate", .params = std::format(R"({{"expression":"{}"}})", jsonEscape(expect)), .move = move });
    queue_.push_back(Command { .method = "Input.dispatchMouseEvent", .params = std::move(params), .move = move });
    pump();
}

void InputBridge::evaluate(std::string_view expression)
{
    enqueue("Runtime.evaluate", std::format(R"({{"expression":"{}"}})", jsonEscape(expression)));
}

void InputBridge::enqueue(std::string method, std::string params)
{
    queue_.push_back(Command { .method = std::move(method), .params = std::move(params) });
    pump();
}

void InputBridge::pump()
{
    using Microsoft::WRL::Callback;
    using Handler = ICoreWebView2CallDevToolsProtocolMethodCompletedHandler;

    while (!busy_ && !queue_.empty()) {
        if (!webview_) {
            queue_.clear();
            return;
        }

        Command command = std::move(queue_.front());
        queue_.pop_front();

        const std::wstring method = Utf8ToWide(command.method);
        const std::wstring params = Utf8ToWide(command.params);
        const uint64_t generation = ++generation_;
        busy_ = true;
        if (watchdog_timer_ != 0) {
            KillTimer(hwnd_, watchdog_timer_);
        }
        watchdog_timer_ = static_cast<UINT_PTR>(generation);
        SetTimer(hwnd_, watchdog_timer_, kCommandTimeoutMs, nullptr);
        const HRESULT hr = webview_->CallDevToolsProtocolMethod(
            method.c_str(),
            params.c_str(),
            Callback<Handler>([this, generation, name = command.method](HRESULT error, LPCWSTR) -> HRESULT {
                if (generation != generation_) {
                    return S_OK;
                }
                if (FAILED(error)) {
                    log::Warn("CDP {} failed: 0x{:08x}", name, static_cast<unsigned>(error));
                }
                busy_ = false;
                pump();
                if (!busy_) {
                    KillTimer(hwnd_, watchdog_timer_);
                    watchdog_timer_ = 0;
                }
                return S_OK;
            }).Get());

        if (FAILED(hr)) {
            log::Error("CDP {} dispatch failed: 0x{:08x}", command.method, static_cast<unsigned>(hr));
            KillTimer(hwnd_, watchdog_timer_);
            watchdog_timer_ = 0;
            busy_ = false;
        }
    }
}

void InputBridge::onWatchdog()
{
    KillTimer(hwnd_, watchdog_timer_);
    watchdog_timer_ = 0;
    if (!busy_) {
        return;
    }
    log::Warn("CDP command timed out after {}ms, skipped", kCommandTimeoutMs);
    ++generation_;
    busy_ = false;
    pump();
}

} // namespace cloudshell
