#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>
#include <wrl/client.h>

#include <WebView2.h>

namespace cloudshell
{

// 把控制器投递到顶层窗口的鼠标、键盘消息交给 WebView2：
// - native：转交给宿主内的 Chrome_WidgetWin_1（后台原生消息，优先）
// - cdp：转成 CDP Input 事件（兼容回退，保留）
// 转向通道始终走 CDP，只产生页面内位移，不移动真实光标。
class InputBridge
{
public:
    enum class Mode
    {
        Auto,
        Native,
        Cdp,
    };

    // 控制器转向通道：不移动真实光标，只把位移交给页面。wParam=dx，lParam=dy（int16）
    static constexpr UINT kLookMessage = WM_APP + 0x51;

    explicit InputBridge(HWND hwnd, Mode mode = Mode::Auto);

    void Attach(ICoreWebView2* webview);
    void Detach();

    std::optional<LRESULT> HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);

    void SetForeground(bool foreground);
    void SyncPageState();

private:
    struct Command
    {
        std::string method;
        std::string params;
        bool move = false;
    };

    bool ensureWidget();
    bool forwardNative(UINT msg, WPARAM wparam, LPARAM lparam);
    bool isWheelPointOnShell(LPARAM lparam) const;
    void updateModifiers(unsigned vk, bool down);
    void logNativeMode();

    void onMouseButton(int raw_button, bool down, LPARAM lparam);
    void onMouseMove(LPARAM lparam);
    void onMouseWheel(bool horizontal, WPARAM wparam, LPARAM lparam);
    void onKey(bool down, WPARAM wparam, LPARAM lparam);
    void onChar(WPARAM wparam, LPARAM lparam);

    void dispatchMouse(std::string_view type, std::string_view button, int click_count, int delta_x = 0, int delta_y = 0);
    void dispatchLook(int dx, int dy);

    // CDP 命令串行执行：页面里登记坐标的 Runtime.evaluate 必须先于 Input.dispatchMouseEvent 完成
    void enqueue(std::string method, std::string params);
    void evaluate(std::string_view expression);
    void pump();
    void onWatchdog();

private:
    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;

    Mode mode_ = Mode::Auto;
    // 宿主内 WebView2 的原生输入窗口；native 模式失败时自动回退 CDP
    HWND widget_ = nullptr;
    bool native_ = false;

    std::deque<Command> queue_;
    bool busy_ = false;
    // 看门狗丢弃超时命令后，用它认出迟到的完成回调
    uint64_t generation_ = 0;
    // 每条命令独立 timer id，避免上一条命令已投递的 WM_TIMER 命中下一条
    UINT_PTR watchdog_timer_ = 0;

    // 控制器消息里的按键状态与坐标不可靠（按下前的 WM_MOUSEMOVE 已带 MK_LBUTTON），以这里记录的为准
    int buttons_ = 0;
    POINT pointer_ {};
    int modifiers_ = 0;
    wchar_t pending_high_surrogate_ = 0;
    bool foreground_ = true;
};

} // namespace cloudshell
