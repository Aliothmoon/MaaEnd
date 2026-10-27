// 云终末地的页面策略，按输入来源区分控制器输入与用户的物理输入。外壳调用：
//   __maaShellExpect(x, y)         派发控制器的鼠标事件前登记坐标；派发按键前不带参数调用
//   __maaShellSetForeground(bool)  外壳窗口是否在前台
(() => {
    // 只覆盖真正最小化：Chromium 会把页面标记为隐藏，SDK 随即停发心跳；遮挡由浏览器启动参数处理
    for (const [name, value] of [["hidden", false], ["visibilityState", "visible"]]) {
        Object.defineProperty(Document.prototype, name, {configurable: true, get: () => value});
    }

    // WebGL2 画质增强的像素随显卡而变，还会替换下面按键修复依赖的原始视频
    const getContext = HTMLCanvasElement.prototype.getContext;
    HTMLCanvasElement.prototype.getContext = function (type, ...args) {
        return type === "webgl2" ? null : getContext.call(this, type, ...args);
    };

    const kExpectTtlMs = 1000;
    const expected = [];
    let foreground = true;
    let lastAutomatedAt = -Infinity;
    let zeroMove = null;

    window.__maaShellExpect = (x, y) => {
        lastAutomatedAt = performance.now();
        if (x === undefined) return;
        expected.push({x, y, time: lastAutomatedAt, look: false});
        if (expected.length > 64) expected.shift();
    };
    // 镜头位移：这一拍要的是一次带 movementX 的移动，不能按“自动化定位”清零。
    // 同一次位移会派发 pointerrawupdate/pointermove/mousemove 等多个事件，
    // 条目在 TTL 内不消费，保证每种事件都能按同一坐标认出来。
    window.__maaShellLook = (x, y) => {
        lastAutomatedAt = performance.now();
        expected.push({x, y, time: lastAutomatedAt, look: true});
        if (expected.length > 64) expected.shift();
    };
    // 回位移动：零掉这组事件的位移。首个事件的位置在 500ms 内对同组事件生效。
    window.__maaShellZeroNext = () => {
        zeroMove = {x: null, y: null, until: performance.now() + 500};
    };
    window.__maaShellSetForeground = (value) => {
        foreground = !!value;
    };

    // 按事件到达时间判过期：CDP 合成事件的时间戳与 performance.now 不同源。
    function matchExpected(event) {
        const now = performance.now();
        while (expected.length && now - expected[0].time > kExpectTtlMs) expected.shift();
        return expected.find((item) => item.x === event.clientX && item.y === event.clientY) || null;
    }

    // 点击始终放行：在后台窗口上点击属于用户的主动操作。SDK 在游戏元素上同时监听了鼠标与指针事件
    const kHoverTypes = [
        "mousemove", "mouseover", "mouseout", "mouseenter", "mouseleave", "wheel", "mousewheel",
        "pointermove", "pointerrawupdate", "pointerover", "pointerout", "pointerenter", "pointerleave",
    ];
    const kMoveTypes = new Set(["mousemove", "pointermove", "pointerrawupdate"]);
    for (const type of [...kHoverTypes, "mousedown"]) {
        window.addEventListener(
            type,
            (event) => {
                if (!event.isTrusted) return;
                const moveType = kMoveTypes.has(type);
                if (zeroMove) {
                    const now = performance.now();
                    if (now > zeroMove.until) {
                        zeroMove = null;
                    }
                    else {
                        if (zeroMove.x === null) {
                            zeroMove.x = event.clientX;
                            zeroMove.y = event.clientY;
                        }
                        if (event.clientX === zeroMove.x && event.clientY === zeroMove.y) {
                            if (moveType) {
                                Object.defineProperties(event, {movementX: {value: 0}, movementY: {value: 0}});
                            }
                            return;
                        }
                    }
                }

                const entry = matchExpected(event);
                if (type === "mousedown") {
                    // 原生转发与 CDP 合成事件都属于自动化输入，不再按物理点击放行指针锁定
                    return;
                }

                if (entry && entry.look) {
                    return;
                }

                // 无登记项的事件来自宿主转交的原生消息：放行，但不让无按键的定位移动带动镜头。
                // 拖动（buttons 非零）保留位移；CDP 的 expected 移动同样按此处理。
                if (moveType && !event.buttons) {
                    Object.defineProperties(event, {movementX: {value: 0}, movementY: {value: 0}});
                }
            },
            {capture: true, passive: false},
        );
    }

    // CloudShell 是自动化宿主：原生转发与 CDP 输入都无法与真实鼠标可靠区分，指针锁定一律拒绝，
    // 避免锁定后真实鼠标在窗口外仍被游戏接收。必须保持可写：SDK 在严格模式下会先赋值，
    // 只读属性会让赋值抛异常并被 SDK 吞掉。
    Element.prototype.requestPointerLock = function () {
        return Promise.reject(new DOMException("Pointer lock is disabled in the CloudShell automation host.", "NotAllowedError"));
    };

    // 鼠标离开视频或窗口失焦后 SDK 不再发送按键，直到视频收到 mouseenter；
    // 在同一个 keydown 里、SDK 的 document 监听之前补发
    window.addEventListener(
        "keydown",
        (event) => {
            if (!event.isTrusted) return;
            const video = document.getElementById("WelinkGameVideo");
            if (video && video.readyState >= 2 && !video.paused) {
                const target = document.getElementById("gamePlayLayer") || video;
                target.dispatchEvent(new MouseEvent("mouseenter", {view: window}));
            }
        },
        true,
    );
})();
