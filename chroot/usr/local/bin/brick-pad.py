#!/usr/bin/python3
"""Map the Brick Pro's built-in controls to an X11 pointer, keys and session control."""

import os
import queue
import select
import signal
import sys
import threading
import time
import traceback

sys.path.insert(0, "/usr/local/bin")

from evdev import InputDevice, ecodes
from Xlib import X, XK, display
from Xlib import error as xerror
from Xlib.ext import xtest

import brick_speech
from brick_keyboard import OnScreenKeyboard, StatusBar
from brick_typing import Typist


running = True


def stop(_signum, _frame):
    global running
    running = False


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

pad = InputDevice("/dev/input/event3")
for _ in range(50):
    try:
        xdisplay = display.Display(os.environ.get("DISPLAY", ":1"))
        break
    except Exception:
        time.sleep(0.1)
else:
    raise SystemExit("X11 display did not start")

root = xdisplay.screen().root
width = xdisplay.screen().width_in_pixels
height = xdisplay.screen().height_in_pixels
keyboard = OnScreenKeyboard(xdisplay, width, height)
status = StatusBar(xdisplay, width)
typist = Typist(xdisplay)

axis = {ecodes.ABS_X: 0, ecodes.ABS_Y: 0, ecodes.ABS_RY: 0,
        ecodes.ABS_Z: 0, ecodes.ABS_RZ: 0,
        ecodes.ABS_HAT0X: 0, ecodes.ABS_HAT0Y: 0}
held = set()
last_scroll = 0.0
last_focus = 0.0
kill_deadline = None
FIT_TOLERANCE = 8

SPEECH_LANGUAGE = os.environ.get("BRICK_SPEECH_LANG", "zh-CN")
try:
    POINTER_SPEED = float(os.environ.get("BRICK_POINTER_SPEED", "12"))
except ValueError:
    POINTER_SPEED = 12.0
DEADZONE = 6000
pointer_carry_x = 0.0
pointer_carry_y = 0.0
MENU_HOLD_SECONDS = 0.35
speech_results = queue.Queue()
speech_release = threading.Event()
speech_active = False
speech_stopping = False
menu_pressed_at = None
status_until = 0.0


def report(message):
    traceback.print_exc()
    sys.stderr.write("  (while %s)\n" % message)
    sys.stderr.flush()


# --------------------------------------------------------------------- keys

def tap_keycode(keycode, pressed):
    xtest.fake_input(xdisplay, X.KeyPress if pressed else X.KeyRelease, keycode)


def key(name, pressed):
    keycode = xdisplay.keysym_to_keycode(XK.string_to_keysym(name))
    if keycode:
        tap_keycode(keycode, pressed)
        xdisplay.sync()


def tap(name):
    key(name, True)
    key(name, False)


def combo(modifier, name):
    key(modifier, True)
    tap(name)
    key(modifier, False)


# Unicode output needs the keymap borrowed, which brick_typing batches.
send_keysym = typist.send_keysym
send_char = typist.send_char
type_text = typist.type


# ------------------------------------------------------------------- speech

def _speech_worker(language):
    """Owns the whole push-to-talk lifecycle on one thread: start, poll for
    partial text, then stop when the button comes up.  Never touches X --
    python-xlib connections are not thread-safe -- so it reports via a queue."""
    session = brick_speech.Session()
    try:
        session.start(language)
    except brick_speech.SpeechError as exc:
        speech_results.put(("err", str(exc)))
        return
    except Exception as exc:
        speech_results.put(("err", exc.__class__.__name__))
        return
    # Do not invite the user to speak until the engine says capture is running:
    # start() only means the request was accepted.
    ready = False
    deadline = time.monotonic() + 45
    last_poll = 0.0
    while not speech_release.is_set() and time.monotonic() < deadline:
        time.sleep(0.03)
        if time.monotonic() - last_poll < (0.12 if not ready else 0.5):
            continue
        last_poll = time.monotonic()
        state = session.state()
        if state is None:
            continue
        error = state.get("error")
        if error and not ready and error not in ("no-speech", "aborted"):
            speech_results.put(("err", brick_speech.describe(error)))
            session.close()
            return
        if not ready and state.get("ready"):
            ready = True
            speech_results.put(("live", ""))
        partial = state.get("text")
        if partial:
            speech_results.put(("interim", partial))
    try:
        speech_results.put(("ok", session.stop()))
    except brick_speech.SpeechError as exc:
        speech_results.put(("err", str(exc)))
    except Exception as exc:
        speech_results.put(("err", exc.__class__.__name__))


def start_speech():
    global speech_active, speech_stopping, status_until
    if speech_active:
        return
    speech_active = True
    speech_stopping = False
    status_until = 0.0
    speech_release.clear()
    status.show("等待中…", "正在启动语音识别")
    threading.Thread(target=_speech_worker, args=(SPEECH_LANGUAGE,),
                     daemon=True).start()


def finish_speech():
    """Called from the main loop, so touching X here is fine."""
    global speech_stopping
    if not speech_active or speech_stopping:
        return
    speech_stopping = True
    speech_release.set()
    status.show("识别中…", "请稍候")


def drain_speech(now):
    """Apply whatever the worker has reported; all X work happens here, on the
    main thread."""
    global speech_active, speech_stopping, status_until
    while True:
        try:
            outcome, payload = speech_results.get_nowait()
        except queue.Empty:
            return
        # Once the button is up the worker's last poll can still land; do not
        # let it paint "listening" over "recognising".
        if speech_stopping and outcome in ("live", "interim"):
            continue
        if outcome == "live":
            status.show("可以说话了", "说完松开 MENU")
        elif outcome == "interim":
            status.show("正在聆听…", payload[-24:])
        elif outcome == "ok":
            speech_active = False
            speech_stopping = False
            if payload:
                status.show("正在输入…", payload[:24])
                type_text(payload)
                status_until = now + 0.6
            else:
                status.show("没有听清", "再按住 MENU 试一次")
                status_until = now + 1.8
        else:
            speech_active = False
            speech_stopping = False
            status.show(payload[:14], payload[14:44])
            status_until = now + 2.5


# ------------------------------------------------------------------ pointer

def mouse(button, pressed):
    xtest.fake_input(xdisplay, X.ButtonPress if pressed else X.ButtonRelease, button)
    xdisplay.sync()


def wheel(button):
    mouse(button, True)
    mouse(button, False)


def navigate(back):
    combo("Alt_L", "Left" if back else "Right")


# ------------------------------------------------------------------ browser

def browser_window():
    """Largest viewable top-level window belonging to Chrome."""
    best = None
    best_area = -1
    try:
        children = root.query_tree().children
    except xerror.ConnectionClosedError:
        raise
    except Exception:
        return None
    for window in children:
        try:
            names = window.get_wm_class()
            geometry = window.get_geometry()
            attributes = window.get_attributes()
        except Exception:
            continue
        if not names or attributes.map_state != X.IsViewable:
            continue
        if not any("chrom" in name.lower() for name in names):
            continue
        area = geometry.width * geometry.height
        if area > best_area:
            best, best_area = window, area
    return best


def focus_browser():
    """Without a window manager X keeps focus on the pointer, which would send
    typed characters nowhere useful.  Pin focus to Chrome, and keep the
    frameless window matched to the panel."""
    window = browser_window()
    if window is None:
        return
    try:
        geometry = window.get_geometry()
        # Chrome settles a pixel or two short of the panel; only step in when the
        # window genuinely does not fit, or we would fight it every pass.
        if (geometry.width > width or geometry.height > height
                or geometry.width < width - FIT_TOLERANCE
                or geometry.height < height - FIT_TOLERANCE):
            window.configure(x=0, y=0, width=width, height=height)
        xdisplay.set_input_focus(window, X.RevertToPointerRoot, X.CurrentTime)
        keyboard.raise_above()
        status.raise_above()
        xdisplay.sync()
    except xerror.ConnectionClosedError:
        raise
    except Exception:
        pass


def browser_pids():
    pids = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open("/proc/%s/cmdline" % entry, "rb") as handle:
                argv0 = handle.read().split(b"\0")[0].decode("utf-8", "replace")
        except Exception:
            continue
        if os.path.basename(argv0) in ("chrome", "google-chrome-stable", "google-chrome"):
            pids.append(int(entry))
    return pids


def quit_session():
    """SELECT+START ends the browser; brick-x11.sh then tears down X11."""
    global kill_deadline
    keyboard.hide()
    status.hide()
    for pid in browser_pids():
        try:
            os.kill(pid, signal.SIGTERM)
        except Exception:
            pass
    kill_deadline = time.monotonic() + 6.0


# ------------------------------------------------------------------ buttons

def press_confirm(pressed):
    if speech_active:
        if pressed:
            finish_speech()
        return
    if keyboard.visible:
        if pressed:
            keyboard.press(send_char, send_keysym)
        return
    mouse(1, pressed)


def press_cancel(pressed):
    if speech_active:
        return
    if keyboard.visible:
        if pressed:
            keyboard.hide()
        return
    key("Escape", pressed)


# trimui_inputd presents the pad as an Xbox 360 controller and maps the face
# buttons by POSITION, so the codes follow the Xbox silkscreen while the Brick's
# own silkscreen is Nintendo-style.  Name them by the letter printed on the case.
FACE_A = ecodes.BTN_EAST     # 305, right-hand button
FACE_B = ecodes.BTN_SOUTH    # 304, bottom button
FACE_X = ecodes.BTN_WEST     # 308, top button
FACE_Y = ecodes.BTN_NORTH    # 307, left-hand button

buttons = {
    FACE_A: press_confirm,
    FACE_B: press_cancel,
    FACE_X: lambda pressed: send_keysym(XK.XK_BackSpace) if pressed else None,
    FACE_Y: lambda pressed: mouse(3, pressed),
    ecodes.BTN_START: lambda pressed: key("Return", pressed),
    ecodes.BTN_SELECT: lambda pressed: key("Tab", pressed),
    ecodes.BTN_TL: lambda pressed: key("Prior", pressed),
    ecodes.BTN_TR: lambda pressed: key("Next", pressed),
    ecodes.BTN_THUMBL: lambda pressed: pressed and combo("Control_L", "r"),
    ecodes.BTN_THUMBR: lambda pressed: pressed and combo("Control_L", "l"),
}

keyboard.on_speech = start_speech


def scaled(value):
    """Squared response past the deadzone.

    The old linear ramp jumped straight to a fifth of full speed the moment the
    stick cleared the deadzone, which made fine positioning impossible.  This
    re-normalises so motion starts from zero and squares it, keeping full speed
    available at full deflection."""
    magnitude = abs(value)
    if magnitude < DEADZONE:
        return 0.0
    ramp = (magnitude - DEADZONE) / (32767.0 - DEADZONE)
    ramp = min(1.0, ramp) ** 2
    return ramp if value > 0 else -ramp


def handle_hat(code, value):
    if not value:
        return
    step = 1 if value > 0 else -1
    if keyboard.visible:
        keyboard.move(step if code == ecodes.ABS_HAT0X else 0,
                      step if code == ecodes.ABS_HAT0Y else 0)
    elif code == ecodes.ABS_HAT0X:
        navigate(back=step < 0)


# --------------------------------------------------------------------- loop

while running:
    # First X call of the pass, so it doubles as the check for the server going
    # away at the end of the session -- otherwise the loop spins on tracebacks.
    try:
        pending = xdisplay.pending_events()
    except xerror.ConnectionClosedError:
        break
    while pending:
        try:
            keyboard.handle_event(xdisplay.next_event())
        except Exception:
            report("handling an X event")
        pending -= 1

    ready, _, _ = select.select([pad.fd], [], [], 0.02)
    if ready:
        try:
            events = list(pad.read())
        except OSError:
            events = []
        for event in events:
            if event.type == ecodes.EV_ABS and event.code in axis:
                old = axis[event.code]
                axis[event.code] = event.value
                if event.code in (ecodes.ABS_HAT0X, ecodes.ABS_HAT0Y) and event.value != old:
                    handle_hat(event.code, event.value)
                elif event.code in (ecodes.ABS_Z, ecodes.ABS_RZ) and old < 128 <= event.value:
                    combo("Control_L", "minus" if event.code == ecodes.ABS_Z else "plus")
            elif event.type == ecodes.EV_KEY:
                if event.value:
                    held.add(event.code)
                else:
                    held.discard(event.code)
                if ecodes.BTN_SELECT in held and ecodes.BTN_START in held:
                    quit_session()
                    continue
                if event.code == ecodes.BTN_MODE:
                    # Tap toggles the keyboard, hold talks.
                    if event.value:
                        menu_pressed_at = time.monotonic()
                    else:
                        if speech_active:
                            finish_speech()
                        elif (menu_pressed_at is not None
                              and time.monotonic() - menu_pressed_at < MENU_HOLD_SECONDS):
                            keyboard.toggle()
                        menu_pressed_at = None
                    continue
                if event.code in buttons:
                    try:
                        buttons[event.code](event.value != 0)
                    except Exception:
                        # A raise here used to kill the whole daemon, leaving no
                        # pointer and no way to quit the session.
                        report("handling button %d" % event.code)

    # Carry the fraction between passes, or anything under half a pixel per
    # frame would round to zero and the slow end of the curve would be dead.
    pointer_carry_x += POINTER_SPEED * scaled(axis[ecodes.ABS_X])
    pointer_carry_y += POINTER_SPEED * scaled(axis[ecodes.ABS_Y])
    dx = int(pointer_carry_x)
    dy = int(pointer_carry_y)
    pointer_carry_x -= dx
    pointer_carry_y -= dy
    if dx or dy:
        pointer = root.query_pointer()
        x = max(0, min(width - 1, pointer.root_x + dx))
        y = max(0, min(height - 1, pointer.root_y + dy))
        xtest.fake_input(xdisplay, X.MotionNotify, x=x, y=y)
        xdisplay.sync()

    now = time.monotonic()

    if (menu_pressed_at is not None and not speech_active
            and now - menu_pressed_at >= MENU_HOLD_SECONDS):
        # Cleared here, not on release: otherwise a session that ends while the
        # button is still down immediately starts another one.
        menu_pressed_at = None
        try:
            start_speech()
        except Exception:
            speech_active = False
            speech_stopping = False
            report("starting voice input")

    try:
        drain_speech(now)
    except Exception:
        speech_active = False
        speech_stopping = False
        status_until = now + 0.1
        report("applying voice input")

    if status_until and now >= status_until:
        status_until = 0.0
        try:
            status.hide()
        except Exception:
            report("hiding the status bar")

    scroll = scaled(axis[ecodes.ABS_RY])
    if not keyboard.visible:
        scroll = axis[ecodes.ABS_HAT0Y] or scroll
    if scroll and now - last_scroll >= 0.15:
        wheel(5 if scroll > 0 else 4)
        last_scroll = now

    if now - last_focus >= 2.0:
        focus_browser()
        last_focus = now

    if kill_deadline is not None and now >= kill_deadline:
        for pid in browser_pids():
            try:
                os.kill(pid, signal.SIGKILL)
            except Exception:
                pass
        kill_deadline = None

keyboard.hide()
status.hide()
pad.close()
os._exit(0)
