"""Type arbitrary text into X clients with XTEST.

XTEST can only press keycodes, and the Brick's keymap has no CJK, so characters
it lacks are typed by borrowing unused keycodes and pointing them at Unicode
keysyms -- the trick xdotool uses.

Two things about Chrome shape this.

Chrome resolves a keycode to a character when it *processes* the event, against
whatever keymap it holds at that moment -- not when the event arrives.  Borrowing
one keycode and re-pointing it at each character in turn therefore loses races
whenever Chrome is slow to drain its queue: a press still sitting in the queue
gets read with the mapping meant for a later character.  "广州今天" arrived as
"广今今天" that way.  So each distinct character gets its own keycode and the
mapping does not change once typing starts.

And Chrome derives keyboard *shortcuts* from the hardware keycode's DomCode,
not from the keysym we put there, so some keycodes act as browser keys whatever
they are mapped to.  The pool below was screened on the device: each keycode was
mapped to a known character, typed into a page, and kept only if that exact
character arrived and the page had not navigated.
"""

import time

from Xlib import X, XK
from Xlib.ext import xtest

SETTLE_SECONDS = 0.06      # once per batch: let Chrome act on MappingNotify
KEY_INTERVAL = 0.004
DRAIN_SECONDS = 0.25       # before reusing a keycode for a different character
UNICODE_BASE = 0x01000000

VERIFIED_KEYCODES = (
    92, 93, 101, 118, 120, 121, 122, 123, 129, 130, 131, 132, 133, 135, 136,
    137, 138, 139, 140, 141, 142, 143, 145, 146, 147, 148, 149, 151, 152, 154,
    155, 157, 158, 159, 163, 165, 168, 169, 171, 172, 173, 175, 177, 179, 180,
)


class Typist:
    def __init__(self, display):
        self.display = display
        self.shift_keycode = display.keysym_to_keycode(XK.XK_Shift_L)
        self._pool = None

    # ------------------------------------------------------------- keystrokes

    def tap_keycode(self, keycode, pressed):
        xtest.fake_input(self.display,
                         X.KeyPress if pressed else X.KeyRelease, keycode)

    def _slot(self, keysym):
        """(keycode, needs_shift) if the current keymap can produce it."""
        for keycode, level in self.display.keysym_to_keycodes(keysym):
            if level <= 1:
                return keycode, bool(level)
        return None

    def send_keysym(self, keysym):
        slot = self._slot(keysym)
        if slot is None:
            return False
        keycode, needs_shift = slot
        if needs_shift:
            self.tap_keycode(self.shift_keycode, True)
        self.tap_keycode(keycode, True)
        self.tap_keycode(keycode, False)
        if needs_shift:
            self.tap_keycode(self.shift_keycode, False)
        self.display.sync()
        return True

    def send_char(self, character):
        return self.send_keysym(ord(character))

    # ----------------------------------------------------------- scratch keys

    def pool(self):
        """Screened keycodes that are still unclaimed in the live keymap."""
        if self._pool is None:
            mapping = self.display.get_keyboard_mapping(8, 248)
            free = {8 + index for index, keysyms in enumerate(mapping)
                    if not any(keysyms)}
            # A keycode this class borrowed earlier still carries its Unicode
            # keysym, so treat the whole screened set as available.
            self._pool = [keycode for keycode in VERIFIED_KEYCODES
                          if keycode in free or keycode in self._borrowed]
        return self._pool

    _borrowed = set()

    def _needs_scratch(self, character, pool):
        """True unless the real keymap can type it.

        A stale borrow from a previous call does not count: this batch may point
        that keycode at a different character."""
        slot = self._slot(ord(character))
        return slot is None or slot[0] in pool

    # ------------------------------------------------------------------- type

    def type(self, text):
        pool = self.pool()
        pool_set = set(pool)
        position = 0
        while position < len(text):
            # Plan a batch: characters the keymap already has go through
            # untouched, the rest each take one keycode from the pool.
            assignment = {}
            end = position
            while end < len(text):
                character = text[end]
                if (character != "\n" and character not in assignment
                        and self._needs_scratch(character, pool_set)):
                    if not pool or len(assignment) == len(pool):
                        break
                    assignment[character] = pool[len(assignment)]
                end += 1
            if end == position:
                position += 1          # nothing typable and no pool: skip it
                continue

            if assignment:
                for character, keycode in assignment.items():
                    keysym = UNICODE_BASE + ord(character)
                    self.display.change_keyboard_mapping(
                        keycode, [[keysym, keysym]])
                    self._borrowed.add(keycode)
                self.display.sync()
                time.sleep(SETTLE_SECONDS)

            for character in text[position:end]:
                if character == "\n":
                    self.send_keysym(XK.XK_Return)
                elif character in assignment:
                    keycode = assignment[character]
                    self.tap_keycode(keycode, True)
                    self.tap_keycode(keycode, False)
                    self.display.sync()
                else:
                    self.send_char(character)
                time.sleep(KEY_INTERVAL)

            position = end
            if position < len(text):
                # The next batch repoints these keycodes, so let Chrome finish
                # reading the presses already queued before their meaning moves.
                time.sleep(DRAIN_SECONDS)

        # The borrows are deliberately left in place.  Restoring them here would
        # be the same race in reverse: presses still queued would resolve to
        # NoSymbol and the tail of the text would vanish.  These keycodes have
        # no physical key behind them, so leaving them mapped costs nothing.
