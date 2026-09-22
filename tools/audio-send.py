#!/usr/bin/env python3
"""Play text at the Brick as sound, for typing without a keyboard.

    ./audio-send.py "https://example.com/some/long/url"
    echo "文本" | ./audio-send.py
    ./audio-send.py --save out.wav "text"      # write instead of playing

Point the laptop's speaker at the handheld, press the keyboard's 声波 key, then
run this.  The modulation lives in brick_audio.py, which both ends import, so
the two cannot drift apart.

Nothing here is macOS-specific except the preferred player; it falls back to
aplay, then to just writing a file and telling you to play it.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import wave

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "chroot", "usr", "local", "bin"))
sys.path.insert(0, _HERE)

import brick_audio


def write_wav(path, pcm, sample_rate):
    with wave.open(path, "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(sample_rate)
        handle.writeframes(pcm)


def play(path):
    for player in ("afplay", "aplay", "paplay"):
        if shutil.which(player):
            subprocess.run([player, path], check=False)
            return player
    return None


def main():
    parser = argparse.ArgumentParser(description="Send text to the Brick as sound")
    parser.add_argument("text", nargs="?", help="text to send; omit to read stdin")
    parser.add_argument("--rate", type=int, default=48000,
                        help="playback sample rate (default 48000)")
    parser.add_argument("--volume", type=float, default=0.6,
                        help="0.1-1.0, default 0.6; raise it in a noisy room")
    parser.add_argument("--repeat", type=int, default=1,
                        help="send the frame N times; the receiver takes the "
                             "first one that passes CRC")
    parser.add_argument("--save", metavar="FILE", help="write a WAV instead of playing")
    arguments = parser.parse_args()

    text = arguments.text
    if text is None:
        text = sys.stdin.read()
    text = text.strip()
    if not text:
        parser.error("nothing to send")

    payload = text.encode("utf-8")
    if len(payload) > 255:
        parser.error("too long: %d bytes, the frame carries at most 255"
                     % len(payload))

    pcm = brick_audio.encode(text, sample_rate=arguments.rate,
                             amplitude=max(0.1, min(1.0, arguments.volume)))
    gap = b"\0\0" * int(arguments.rate * 0.4)
    stream = (pcm + gap) * max(1, arguments.repeat)

    seconds = len(stream) / 2.0 / arguments.rate
    symbols = brick_audio.PREAMBLE_SYMBOLS + 4 + len(payload) * 2
    print("%d 字节 -> %d 符号，约 %.1f 秒%s"
          % (len(payload), symbols, seconds,
             " x%d" % arguments.repeat if arguments.repeat > 1 else ""))

    if arguments.save:
        write_wav(arguments.save, stream, arguments.rate)
        print("已写入 %s" % arguments.save)
        return

    handle, path = tempfile.mkstemp(suffix=".wav")
    os.close(handle)
    try:
        write_wav(path, stream, arguments.rate)
        print("播放中 —— 把扬声器对准掌机，先按掌机键盘上的「声波」键")
        if play(path) is None:
            print("找不到播放器；用 --save 存成文件自己播", file=sys.stderr)
            sys.exit(1)
    finally:
        os.unlink(path)


if __name__ == "__main__":
    main()
