import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";

const policy = readFileSync(new URL("./cloud_policy.js", import.meta.url), "utf8");

function makeHarness() {
    let now = 1000;
    const listeners = new Map();
    class Document {}
    class HTMLCanvasElement {}
    class Element {}
    class MouseEvent {}
    const window = {
        addEventListener(type, handler) {
            listeners.set(type, handler);
        },
    };
    const document = {
        getElementById() {
            return null;
        },
    };
    const context = {
        window,
        document,
        Document,
        HTMLCanvasElement,
        Element,
        MouseEvent,
        DOMException,
        performance: {
            now() {
                return now;
            },
            advance(ms) {
                now += ms;
            },
        },
    };
    vm.runInNewContext(policy, context, { filename: "cloud_policy.js" });
    return { window, listeners, Element, context };
}

function trustedEvent(fields = {}) {
    const event = {
        isTrusted: true,
        clientX: 0,
        clientY: 0,
        buttons: 0,
        cancelable: true,
        stopped: false,
        prevented: false,
        stopImmediatePropagation() {
            this.stopped = true;
        },
        preventDefault() {
            this.prevented = true;
        },
        ...fields,
    };
    return event;
}

test("requestPointerLock is always rejected in the automation host", async () => {
    const { Element } = makeHarness();
    const element = new Element();
    await assert.rejects(element.requestPointerLock(), /disabled in the CloudShell automation host/);
});

test("native no-entry movement without buttons is zeroed; look movement is preserved", () => {
    const { window, listeners } = makeHarness();
    const move = listeners.get("mousemove");
    assert.equal(typeof move, "function");

    const look = trustedEvent({ clientX: 100, clientY: 200 });
    window.__maaShellLook(100, 200);
    move(look);
    assert.equal(Object.getOwnPropertyDescriptor(look, "movementX"), undefined);

    const nativeMove = trustedEvent({ clientX: 300, clientY: 400 });
    move(nativeMove);
    assert.equal(nativeMove.movementX, 0);
    assert.equal(nativeMove.movementY, 0);
});

test("zero-next return move is zeroed at the anchor", () => {
    const { window, listeners } = makeHarness();
    const move = listeners.get("mousemove");

    window.__maaShellLook(640, 360);
    move(trustedEvent({ clientX: 640, clientY: 360 }));

    window.__maaShellZeroNext();
    const back = trustedEvent({ clientX: 640, clientY: 360 });
    move(back);
    assert.equal(back.movementX, 0);
    assert.equal(back.movementY, 0);
});

test("native mousedown is allowed and does not block propagation", () => {
    const { listeners } = makeHarness();
    const down = listeners.get("mousedown");
    assert.equal(typeof down, "function");
    const event = trustedEvent({ clientX: 10, clientY: 20, buttons: 1 });
    down(event);
    assert.equal(event.stopped, false);
    assert.equal(event.prevented, false);
});
