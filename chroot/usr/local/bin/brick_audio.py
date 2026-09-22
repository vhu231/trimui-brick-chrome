"""Receive text over the air as sound, for typing without a keyboard.

The sender (tools/audio-send.py, run on a computer) plays the text as a series
of tones; this records them off the Brick's microphone and decodes them back.

Modulation is 16-tone MFSK -- one tone per 4-bit nibble -- plus a 17th tone used
as the preamble.  The frequencies are chosen so that every one of them lands
exactly on a Goertzel bin at the receiver's window length:

    bin width = 16000 / 960 = 16.667 Hz
    1200 Hz -> bin 72,  200 Hz spacing -> 12 bins,  4400 Hz -> bin 264

all integers, so there is no spectral leakage between adjacent tones and a plain
Goertzel filter per tone is enough.  No numpy on this device, and none needed.

Frame:  preamble x10 | length (2 nibbles) | payload | CRC-8 (2 nibbles)

The CRC is what makes this safe to wire to a keyboard: a misheard frame is
rejected rather than typed as garbage.
"""

import math
import struct
import subprocess
import time

SAMPLE_RATE = 16000
SYMBOL_SAMPLES = 960              # 60 ms
TONE_BASE = 1200.0
TONE_STEP = 200.0
TONE_COUNT = 16                   # 4 bits per symbol
SYNC_FREQ = 4400.0
PREAMBLE_SYMBOLS = 10

# Trim this much off each end of a symbol before analysing it: the edges carry
# the transition and any timing slop between the two machines' clocks.  What is
# left must be a multiple of 80 samples, because 16000/200 = 80 -- that is what
# puts every tone exactly on a Goertzel bin and keeps neighbouring tones from
# leaking into each other.  960-2*160 = 640: 1200 Hz is bin 48, the spacing is 8.
SYMBOL_MARGIN = 160

# Shorter window for the preamble hunt: 4400 * 240 / 16000 = 66, also integer.
SCAN_SAMPLES = 240
SCAN_STEP = 120

# Finer window for pinning down where the preamble stops: 4400 * 80 / 16000 = 22.
EDGE_SAMPLES = 80
EDGE_STEP = 20

TONES = [TONE_BASE + TONE_STEP * i for i in range(TONE_COUNT)]
CAPTURE_DEVICE = "hw:0,0"


class AudioError(Exception):
    pass


def crc8(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def nibbles(payload):
    out = []
    for byte in payload:
        out.append((byte >> 4) & 0xF)
        out.append(byte & 0xF)
    return out


# ------------------------------------------------------------------ encoding

def encode(text, sample_rate=SAMPLE_RATE, amplitude=0.6):
    """Frame and modulate `text`; returns signed 16-bit mono PCM bytes.

    Kept here as well as in the sender so the device can test itself without
    anything being played at it."""
    payload = text.encode("utf-8")
    if len(payload) > 255:
        raise AudioError("最多 255 字节")
    body = [len(payload) >> 4 & 0xF, len(payload) & 0xF]
    body += nibbles(payload)
    check = crc8(payload)
    body += [check >> 4 & 0xF, check & 0xF]

    symbols = [None] * PREAMBLE_SYMBOLS + body
    samples_per_symbol = int(round(sample_rate * SYMBOL_SAMPLES / float(SAMPLE_RATE)))
    pcm = bytearray()
    phase = 0.0
    for symbol in symbols:
        frequency = SYNC_FREQ if symbol is None else TONES[symbol]
        step = 2.0 * math.pi * frequency / sample_rate
        for _ in range(samples_per_symbol):
            # Continuous phase across symbol boundaries: a phase jump would
            # smear energy into neighbouring tones.
            pcm += struct.pack("<h", int(amplitude * 32767 * math.sin(phase)))
            phase += step
            if phase > 2.0 * math.pi:
                phase -= 2.0 * math.pi
    return bytes(pcm)


# ------------------------------------------------------------------ decoding

def goertzel(samples, start, length, bin_index):
    coefficient = 2.0 * math.cos(2.0 * math.pi * bin_index / length)
    s1 = 0.0
    s2 = 0.0
    for index in range(start, start + length):
        s0 = samples[index] + coefficient * s1 - s2
        s2 = s1
        s1 = s0
    return s1 * s1 + s2 * s2 - coefficient * s1 * s2


def _to_samples(pcm):
    count = len(pcm) // 2
    return list(struct.unpack("<%dh" % count, pcm[:count * 2]))


class _PreambleTracker:
    """Sliding preamble detector that keeps its run state between feeds.

    A streaming caller hands it a growing buffer, so each window must be
    examined exactly once and the run of preamble windows has to survive across
    chunk boundaries -- the preamble is 9600 samples, far longer than one read.
    Restarting the scan per chunk, as a stateless search would, never
    accumulates enough consecutive windows to fire."""

    def __init__(self):
        self.position = 0
        self.run = 0
        self.run_start = None
        self.needed = int(PREAMBLE_SYMBOLS * SYMBOL_SAMPLES * 0.6 / SCAN_STEP)
        self.sync_bin = int(round(SYNC_FREQ * SCAN_SAMPLES / SAMPLE_RATE))
        # A few data tones for comparison, so broadband noise does not read as
        # a preamble.
        self.rival_bins = [int(round(f * SCAN_SAMPLES / SAMPLE_RATE))
                           for f in (TONES[0], TONES[8], TONES[15])]

    def _hit(self, samples, position):
        energy = goertzel(samples, position, SCAN_SAMPLES, self.sync_bin)
        if energy <= 1e6:
            return False
        rival = max(goertzel(samples, position, SCAN_SAMPLES, index)
                    for index in self.rival_bins)
        return energy > rival * 4.0

    def feed(self, samples):
        """Estimated frame start once a whole preamble has gone by, else None."""
        limit = len(samples) - SCAN_SAMPLES
        while self.position < limit:
            if self._hit(samples, self.position):
                if self.run_start is None:
                    self.run_start = self.position
                self.run += 1
            else:
                if self.run >= self.needed and self.run_start is not None:
                    start = self.run_start + self.run * SCAN_STEP + SCAN_SAMPLES
                    self.run = 0
                    self.run_start = None
                    self.position += SCAN_STEP
                    return start
                self.run = 0
                self.run_start = None
            self.position += SCAN_STEP
        return None

    def skip_to(self, position):
        self.position = max(self.position, position)
        self.run = 0
        self.run_start = None


def _find_preamble(samples):
    """One-shot search over a complete buffer."""
    tracker = _PreambleTracker()
    found = tracker.feed(samples)
    if found is not None:
        return found
    # The buffer may simply end while still inside the preamble.
    if tracker.run >= tracker.needed and tracker.run_start is not None:
        return tracker.run_start + tracker.run * SCAN_STEP + SCAN_SAMPLES
    return None


def _align(samples, estimate):
    """Pin down where the preamble stops.

    The coarse hunt only locates the edge to within a scan step, and that slop
    is a big share of a symbol -- enough that clock drift between the two
    machines pushes symbols out of their analysis window.  Walk a fine window
    across the estimate and take the point where the preamble tone dies away."""
    bin_index = int(round(SYNC_FREQ * EDGE_SAMPLES / SAMPLE_RATE))
    first = max(0, estimate - SCAN_SAMPLES)
    last = min(len(samples) - EDGE_SAMPLES, estimate + SCAN_SAMPLES)
    energies = []
    position = first
    while position <= last:
        energies.append((position, goertzel(samples, position, EDGE_SAMPLES, bin_index)))
        position += EDGE_STEP
    if not energies:
        return estimate
    peak = max(energy for _, energy in energies)
    if peak <= 0:
        return estimate
    for position, energy in energies:
        if energy < peak * 0.25:
            return position
    return estimate


def _read_symbol(samples, start):
    margin = SYMBOL_MARGIN
    begin = start + margin
    length = SYMBOL_SAMPLES - 2 * margin
    if begin + length > len(samples):
        raise AudioError("信号在中途结束了")
    best = None
    best_energy = -1.0
    for index, frequency in enumerate(TONES):
        bin_index = int(round(frequency * length / SAMPLE_RATE))
        energy = goertzel(samples, begin, length, bin_index)
        if energy > best_energy:
            best, best_energy = index, energy
    return best


def _frame_length(samples, position):
    """Total samples the frame occupies, once its length field is readable."""
    header = position + 2 * SYMBOL_SAMPLES + SYMBOL_MARGIN
    if len(samples) < header:
        return None
    length = (_read_symbol(samples, position) << 4) | \
             _read_symbol(samples, position + SYMBOL_SAMPLES)
    return (2 + length * 2 + 2) * SYMBOL_SAMPLES + SYMBOL_MARGIN


def _decode_from(samples, position):
    def symbol_at(offset):
        return _read_symbol(samples, position + offset * SYMBOL_SAMPLES)

    length = (symbol_at(0) << 4) | symbol_at(1)
    if length == 0:
        raise AudioError("收到空内容")
    payload = bytearray()
    for index in range(length):
        high = symbol_at(2 + index * 2)
        low = symbol_at(3 + index * 2)
        payload.append((high << 4) | low)
    check = (symbol_at(2 + length * 2) << 4) | symbol_at(3 + length * 2)
    if check != crc8(payload):
        raise AudioError("校验失败，请重发")
    try:
        return payload.decode("utf-8")
    except UnicodeDecodeError:
        raise AudioError("收到的不是有效文本")


def decode(pcm):
    samples = _to_samples(pcm)
    position = _find_preamble(samples)
    if position is None:
        raise AudioError("没有听到起始信号")
    return _decode_from(samples, _align(samples, position))


# ------------------------------------------------------------------ capture

class Receiver:
    """One listening session, abortable.

    It decodes as the audio arrives instead of recording a fixed window and only
    then looking at it.  A four-second transmission used to cost a fifteen-second
    wait; now the listen ends the moment a frame checks out.  That also means the
    window can be generous -- waiting longer is free if nothing is being sent."""

    CHUNK_SECONDS = 0.25

    def __init__(self, device=CAPTURE_DEVICE):
        self.device = device
        self._process = None
        self._cancelled = False

    def cancel(self):
        self._cancelled = True
        self._stop()

    def _stop(self):
        process = self._process
        self._process = None
        if process is None:
            return
        try:
            process.terminate()
            process.wait(timeout=2)
        except Exception:
            pass

    def receive(self, seconds=30):
        command = ["/usr/bin/arecord", "-D", self.device, "-f", "S16_LE",
                   "-r", str(SAMPLE_RATE), "-c", "1", "-t", "raw", "-q"]
        if self._cancelled:
            raise AudioError("已取消")
        try:
            self._process = subprocess.Popen(
                command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        except Exception as exc:
            raise AudioError("录音失败 (%s)" % exc.__class__.__name__)

        samples = []
        tracker = _PreambleTracker()
        position = None
        needed = None
        chunk = int(SAMPLE_RATE * self.CHUNK_SECONDS) * 2
        deadline = time.monotonic() + seconds
        stream = self._process.stdout
        try:
            while not self._cancelled and time.monotonic() < deadline:
                data = stream.read(chunk)
                if not data:
                    break
                samples.extend(_to_samples(data))

                if position is None:
                    found = tracker.feed(samples)
                    if found is None:
                        continue
                    position = _align(samples, found)

                if needed is None:
                    needed = _frame_length(samples, position)
                    if needed is None:
                        continue
                if len(samples) - position < needed:
                    continue

                try:
                    return _decode_from(samples, position)
                except AudioError:
                    # A bad frame is not the end: the sender may be repeating.
                    tracker.skip_to(position + needed)
                    position = None
                    needed = None
        finally:
            self._stop()

        if self._cancelled:
            raise AudioError("已取消")
        raise AudioError("没有听到起始信号")


def receive(seconds=30, device=CAPTURE_DEVICE):
    return Receiver(device).receive(seconds)
