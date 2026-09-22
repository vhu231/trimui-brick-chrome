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

SAMPLE_RATE = 16000
SYMBOL_SAMPLES = 960              # 60 ms
TONE_BASE = 1200.0
TONE_STEP = 200.0
TONE_COUNT = 16                   # 4 bits per symbol
SYNC_FREQ = 4400.0
PREAMBLE_SYMBOLS = 10

# Shorter window for the preamble hunt: 4400 * 240 / 16000 = 66, also integer.
SCAN_SAMPLES = 240
SCAN_STEP = 120

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


def _find_preamble(samples):
    """Start of the first symbol after a run of preamble tones."""
    bin_index = int(round(SYNC_FREQ * SCAN_SAMPLES / SAMPLE_RATE))
    needed = int(PREAMBLE_SYMBOLS * SYMBOL_SAMPLES * 0.6 / SCAN_STEP)
    run_start = None
    run = 0
    limit = len(samples) - SCAN_SAMPLES
    position = 0
    while position < limit:
        energy = goertzel(samples, position, SCAN_SAMPLES, bin_index)
        # Compare against a couple of data tones so that loud broadband noise
        # does not look like a preamble.
        rival = max(goertzel(samples, position, SCAN_SAMPLES,
                             int(round(f * SCAN_SAMPLES / SAMPLE_RATE)))
                    for f in (TONES[0], TONES[8], TONES[15]))
        if energy > rival * 4.0 and energy > 1e6:
            if run_start is None:
                run_start = position
            run += 1
        else:
            if run >= needed and run_start is not None:
                return run_start + run * SCAN_STEP + SCAN_SAMPLES
            run_start = None
            run = 0
        position += SCAN_STEP
    if run >= needed and run_start is not None:
        return run_start + run * SCAN_STEP + SCAN_SAMPLES
    raise AudioError("没有听到起始信号")


def _read_symbol(samples, start):
    # Skip the edges of the symbol: they carry the transition and any timing
    # slop between the two machines' clocks.
    margin = SYMBOL_SAMPLES // 5
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


def decode(pcm):
    samples = _to_samples(pcm)
    position = _find_preamble(samples)

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


# ------------------------------------------------------------------ capture

class Receiver:
    """One capture, abortable.

    arecord blocks for the whole window, so the process handle is kept and
    cancel() kills it -- otherwise there is no way out of a listen the user
    started by mistake short of waiting it out."""

    def __init__(self, device=CAPTURE_DEVICE):
        self.device = device
        self._process = None
        self._cancelled = False

    def cancel(self):
        self._cancelled = True
        process = self._process
        if process is not None:
            try:
                process.terminate()
            except Exception:
                pass

    def record(self, seconds):
        if self._cancelled:
            raise AudioError("已取消")
        command = ["/usr/bin/arecord", "-D", self.device, "-f", "S16_LE",
                   "-r", str(SAMPLE_RATE), "-c", "1", "-d", str(seconds),
                   "-t", "raw", "-q"]
        try:
            self._process = subprocess.Popen(
                command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            pcm, _ = self._process.communicate(timeout=seconds + 15)
        except Exception as exc:
            self.cancel()
            raise AudioError("录音失败 (%s)" % exc.__class__.__name__)
        finally:
            self._process = None
        if self._cancelled:
            raise AudioError("已取消")
        if not pcm:
            raise AudioError("没有录到声音")
        return pcm

    def receive(self, seconds=12):
        pcm = self.record(seconds)
        if self._cancelled:
            raise AudioError("已取消")
        return decode(pcm)


def receive(seconds=12, device=CAPTURE_DEVICE):
    return Receiver(device).receive(seconds)
