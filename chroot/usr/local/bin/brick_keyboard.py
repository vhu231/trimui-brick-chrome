"""D-pad driven on-screen keyboard for the Brick's X11 session.

matchbox-keyboard expects a pointer and draws grey-on-black, neither of which
suits a handheld.  This draws its own keyboard with PIL, uploads it to the X
server as pixmaps, and moves a selection box with the D-pad; the selected key is
typed with XTEST so it lands in whatever window holds the input focus.
"""

import os

from PIL import Image, ImageDraw, ImageFont
from Xlib import X, XK

BACKGROUND = (22, 22, 24)
KEY_FACE = (48, 48, 52)
KEY_BORDER = (72, 72, 78)
KEY_TEXT = (255, 255, 255)
SELECTED_FACE = (10, 132, 255)
SELECTED_BORDER = (120, 190, 255)
LATCHED_FACE = (94, 92, 230)
STATUS_TEXT = (255, 255, 255)
STATUS_BACKGROUND = (16, 40, 72)
STATUS_DETAIL = (168, 205, 255)

ROW_GAP = 6
KEY_GAP = 6
EDGE = 8
STRIP_ROWS = 24          # upload height per PutImage request

BACKSPACE = "\x08"
RETURN = "\n"
SHIFT = "\x01"
CLOSE = "\x02"
SPEECH = "\x03"


class Key:
    def __init__(self, label, shift_label, value, shift_value, units=1):
        self.label = label
        self.shift_label = shift_label
        self.value = value
        self.shift_value = shift_value
        self.units = units
        self.box = (0, 0, 0, 0)

    def text(self, shift):
        return self.shift_label if shift else self.label

    def output(self, shift):
        return self.shift_value if shift else self.value


def _plain(row):
    return [Key(a, b, a, b) for a, b in row]


def _layout():
    return [
        _plain([("1", "!"), ("2", "@"), ("3", "#"), ("4", "$"), ("5", "%"),
                ("6", "^"), ("7", "&"), ("8", "*"), ("9", "("), ("0", ")")]),
        _plain([("q", "Q"), ("w", "W"), ("e", "E"), ("r", "R"), ("t", "T"),
                ("y", "Y"), ("u", "U"), ("i", "I"), ("o", "O"), ("p", "P")]),
        _plain([("a", "A"), ("s", "S"), ("d", "D"), ("f", "F"), ("g", "G"),
                ("h", "H"), ("j", "J"), ("k", "K"), ("l", "L")])
        + [Key("退格", "退格", BACKSPACE, BACKSPACE)],
        [Key("上档", "上档", SHIFT, SHIFT)]
        + _plain([("z", "Z"), ("x", "X"), ("c", "C"), ("v", "V"), ("b", "B"),
                  ("n", "N"), ("m", "M"), (".", ">"), ("/", "?")]),
        [Key("-", "_", "-", "_"), Key(":", ";", ":", ";"),
         Key("空格", "空格", " ", " ", units=3),
         Key(".com", ".com", ".com", ".com"),
         Key("语音", "语音", SPEECH, SPEECH),
         Key("回车", "回车", RETURN, RETURN, units=2),
         Key("关闭", "关闭", CLOSE, CLOSE)],
    ]


def _find_font(*names):
    for directory, _, files in os.walk("/usr/share/fonts"):
        for name in names:
            if name in files:
                return os.path.join(directory, name)
    return None


def _put_image(drawable, gc, image, depth):
    """Send BGRA rather than going through put_pil_image, which encodes RGB as
    "BGRX" and leaves the fourth byte at zero.  The Brick's display engine treats
    that byte as alpha, so a zero there composites the drawing away and it shows
    up as a solid black bar -- Chrome renders fine next to it precisely because
    its own pixels carry alpha 255.

    Strips, because one request for a full-width image would be well over a
    megabyte."""
    width, height = image.size
    opaque = image.convert("RGBA")
    for top in range(0, height, STRIP_ROWS):
        rows = min(STRIP_ROWS, height - top)
        strip = opaque.crop((0, top, width, top + rows))
        drawable.put_image(gc, 0, top, width, rows, X.ZPixmap, depth, 0,
                           strip.tobytes("raw", "BGRA"))


class OnScreenKeyboard:
    def __init__(self, display, screen_width, screen_height, height=330):
        self.display = display
        self.screen = display.screen()
        self.width = screen_width
        self.height = height
        self.top = screen_height - height
        self.rows = _layout()
        self.row = 1
        self.column = 0
        self.shift = False
        self.visible = False
        self.window = None
        self.pixmaps = {}
        self.on_speech = None
        self._showing_status = False
        self._measure()

    # ---------------------------------------------------------------- layout

    def _measure(self):
        columns = max(sum(key.units for key in row) for row in self.rows)
        unit = (self.width - 2 * EDGE) / columns
        rows = len(self.rows)
        row_height = (self.height - 2 * EDGE - (rows - 1) * ROW_GAP) / rows
        for index, row in enumerate(self.rows):
            y = EDGE + index * (row_height + ROW_GAP)
            x = EDGE
            for key in row:
                span = key.units * unit
                key.box = (round(x), round(y), round(x + span - KEY_GAP),
                           round(y + row_height))
                x += span

    # --------------------------------------------------------------- drawing

    def _font_paths(self):
        latin = _find_font("DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf")
        return latin, _find_font("wqy-microhei.ttc", "wqy-zenhei.ttc") or latin

    def _fonts(self):
        latin, cjk = self._font_paths()
        size = max(20, int((self.height - 2 * EDGE) / len(self.rows) * 0.46))
        return (ImageFont.truetype(latin, size),
                ImageFont.truetype(cjk, int(size * 0.8)))

    def _render(self, shift, highlighted):
        latin_font, cjk_font = self._fonts()
        image = Image.new("RGB", (self.width, self.height), BACKGROUND)
        draw = ImageDraw.Draw(image)
        for row in self.rows:
            for key in row:
                face = SELECTED_FACE if highlighted else KEY_FACE
                border = SELECTED_BORDER if highlighted else KEY_BORDER
                if not highlighted and shift and key.value == SHIFT:
                    face = LATCHED_FACE
                draw.rounded_rectangle(key.box, radius=9, fill=face,
                                       outline=border, width=2)
                label = key.text(shift)
                font = cjk_font if any(ord(c) > 127 for c in label) else latin_font
                left, top, right, bottom = draw.textbbox((0, 0), label, font=font)
                x = key.box[0] + (key.box[2] - key.box[0] - (right - left)) / 2 - left
                y = key.box[1] + (key.box[3] - key.box[1] - (bottom - top)) / 2 - top
                draw.text((x, y), label, font=font, fill=KEY_TEXT)
        return image

    def _blit(self, drawable, gc, image):
        _put_image(drawable, gc, image, self.screen.root_depth)

    def _upload(self, image):
        pixmap = self.window.create_pixmap(self.width, self.height,
                                           self.screen.root_depth)
        gc = pixmap.create_gc()
        self._blit(pixmap, gc, image)
        gc.free()
        return pixmap

    def _pixmap(self, shift, highlighted):
        cached = self.pixmaps.get((shift, highlighted))
        if cached is None:
            cached = self._upload(self._render(shift, highlighted))
            self.pixmaps[(shift, highlighted)] = cached
        return cached

    # ------------------------------------------------------------ x11 window

    def _ensure_window(self):
        if self.window is not None:
            return
        # Chrome raises its own window when it takes focus, which occludes this
        # one.  Without backing store the server throws the covered contents
        # away and repaints the background when it is exposed again, so keep the
        # contents, and repaint on Expose for the case where it declines to.
        self.window = self.screen.root.create_window(
            0, self.top, self.width, self.height, 0, self.screen.root_depth,
            X.InputOutput, X.CopyFromParent,
            background_pixel=self.screen.black_pixel,
            override_redirect=1, backing_store=X.Always,
            event_mask=X.ExposureMask)
        self.gc = self.window.create_gc()

    def _paint(self):
        self._showing_status = False
        base = self._pixmap(self.shift, False)
        self.window.copy_area(self.gc, base, 0, 0, self.width, self.height, 0, 0)
        self._paint_selection()

    def _paint_key(self, key, highlighted):
        source = self._pixmap(self.shift, highlighted)
        x, y, right, bottom = key.box
        self.window.copy_area(self.gc, source, x, y, right - x, bottom - y, x, y)

    def _paint_selection(self):
        self._paint_key(self.current(), True)
        self.display.sync()

    # ------------------------------------------------------------ public api

    def current(self):
        row = self.rows[self.row]
        return row[min(self.column, len(row) - 1)]

    def show(self):
        self._ensure_window()
        self.window.map()
        self.window.configure(stack_mode=X.Above)
        self.visible = True
        self._paint()

    def hide(self):
        if self.window is not None:
            self.window.unmap()
            self.display.sync()
        self.visible = False
        self._showing_status = False

    def toggle(self):
        self.hide() if self.visible else self.show()

    def raise_above(self):
        if not self.visible or self.window is None:
            return
        self.window.configure(stack_mode=X.Above)
        if not self._showing_status:
            self._paint()

    def handle_event(self, event):
        """Repaint when the server hands the window back after an occlusion."""
        if (self.visible and self.window is not None
                and event.type == X.Expose and event.count == 0
                and getattr(event, "window", None) is not None
                and event.window.id == self.window.id
                and not self._showing_status):
            self._paint()

    def show_status(self, message, detail=""):
        """Replace the keys with a centred message while voice input runs."""
        if not self.visible or self.window is None:
            return
        _, cjk_path = self._font_paths()
        image = Image.new("RGB", (self.width, self.height), BACKGROUND)
        draw = ImageDraw.Draw(image)
        for text, size, offset in ((message, 58, -30), (detail, 30, 46)):
            if not text:
                continue
            font = ImageFont.truetype(cjk_path, size)
            left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
            draw.text(((self.width - (right - left)) / 2 - left,
                       (self.height - (bottom - top)) / 2 - top + offset),
                      text, font=font, fill=STATUS_TEXT)
        self._blit(self.window, self.gc, image)
        self.display.sync()
        self._showing_status = True

    def clear_status(self):
        if self.visible and self.window is not None and self._showing_status:
            self._paint()

    def move(self, dx, dy):
        if not self.visible or self._showing_status:
            return
        previous = self.current()
        if dy:
            centre = (previous.box[0] + previous.box[2]) / 2
            self.row = (self.row + dy) % len(self.rows)
            row = self.rows[self.row]
            self.column = min(
                range(len(row)),
                key=lambda i: abs((row[i].box[0] + row[i].box[2]) / 2 - centre))
        if dx:
            row = self.rows[self.row]
            self.column = (min(self.column, len(row) - 1) + dx) % len(row)
        self._paint_key(previous, False)
        self._paint_selection()

    def press(self, send_char, send_keysym):
        """Returns False when the key closed the keyboard."""
        if not self.visible or self._showing_status:
            return True
        value = self.current().output(self.shift)
        if value == CLOSE:
            self.hide()
            return False
        if value == SHIFT:
            self.shift = not self.shift
            self._paint()
            return True
        if value == SPEECH:
            if self.on_speech is not None:
                self.on_speech()
            return True
        if value == BACKSPACE:
            send_keysym(XK.XK_BackSpace)
        elif value == RETURN:
            send_keysym(XK.XK_Return)
        else:
            for character in value:
                send_char(character)
        return True


class StatusBar:
    """A banner across the top of the screen.

    Separate from the keyboard on purpose: push-to-talk has to give feedback
    whether or not the keyboard is open."""

    def __init__(self, display, screen_width, height=104):
        self.display = display
        self.screen = display.screen()
        self.width = screen_width
        self.height = height
        self.window = None
        self.visible = False

    def _ensure_window(self):
        if self.window is not None:
            return
        self.window = self.screen.root.create_window(
            0, 0, self.width, self.height, 0, self.screen.root_depth,
            X.InputOutput, X.CopyFromParent,
            background_pixel=self.screen.black_pixel,
            override_redirect=1, backing_store=X.Always, event_mask=0)
        self.gc = self.window.create_gc()

    def show(self, message, detail=""):
        self._ensure_window()
        latin = _find_font("DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf")
        cjk = _find_font("wqy-microhei.ttc", "wqy-zenhei.ttc") or latin
        image = Image.new("RGB", (self.width, self.height), STATUS_BACKGROUND)
        draw = ImageDraw.Draw(image)
        for text, size, top, colour in ((message, 46, 10, STATUS_TEXT),
                                        (detail, 26, 64, STATUS_DETAIL)):
            if not text:
                continue
            font = ImageFont.truetype(cjk, size)
            left, _, right, _ = draw.textbbox((0, 0), text, font=font)
            draw.text(((self.width - (right - left)) / 2 - left, top), text,
                      font=font, fill=colour)
        # Map before drawing: anything painted into an unmapped window is
        # discarded, and the map then clears it to background_pixel (black).
        if not self.visible:
            self.window.map()
            self.visible = True
        self.window.configure(stack_mode=X.Above)
        _put_image(self.window, self.gc, image, self.screen.root_depth)
        self.display.sync()

    def raise_above(self):
        if self.visible and self.window is not None:
            self.window.configure(stack_mode=X.Above)

    def hide(self):
        if self.window is not None and self.visible:
            self.window.unmap()
            self.display.sync()
        self.visible = False
