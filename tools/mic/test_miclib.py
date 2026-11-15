"""Unit tests for the mictest receiver helpers (stdlib only).
Run: python3 -m unittest tools/mic/test_miclib.py   (from the repo root)
"""
import io
import math
import struct
import sys
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import miclib  # noqa: E402


def s16(vals):
    return struct.pack(f"<{len(vals)}h", *vals)


class Framing(unittest.TestCase):
    def test_roundtrip_binary(self):
        payload = bytes(range(256)) * 16          # 4096 bytes incl. \n and NUL
        f = miclib.encode_frame("MDAT", payload)
        self.assertEqual(f[:10], b"MDAT 4096\n")
        p = miclib.FrameParser()
        self.assertEqual(p.feed(f), [("MDAT", payload)])

    def test_split_and_pipelined(self):
        data = (miclib.encode_frame("PING") + miclib.encode_frame("MBEG", "bits=16 rate=16000")
                + miclib.encode_frame("MDAT", b"\x00\x01\n\x02"))
        p = miclib.FrameParser()
        got = []
        for i in range(len(data)):                 # one byte at a time
            got += p.feed(data[i:i + 1])
        self.assertEqual(got, [("PING", b""), ("MBEG", b"bits=16 rate=16000"),
                               ("MDAT", b"\x00\x01\n\x02")])

    def test_bad_headers(self):
        for bad in (b"ping 0000\n", b"PING 00x0\n", b"PING 0000 ", b"PING 9999\n"):
            with self.assertRaises(miclib.FrameError):
                miclib.FrameParser().feed(bad)
        with self.assertRaises(miclib.FrameError):
            miclib.encode_frame("MDAT", b"x" * 4097)
        with self.assertRaises(miclib.FrameError):
            miclib.encode_frame("TOOLONG")


class Meta(unittest.TestCase):
    def test_parse(self):
        m = miclib.parse_meta("clip=4 bits=16 rate=16000 gain=160 bytes=64000 rec_ms=2000 "
                              "traffic=1 pings=10 pongs=9 lost=0 touches=2 touch_ms=100,250")
        self.assertEqual(m["bits"], 16)
        self.assertEqual(m["gain"], 160)
        self.assertEqual(m["touch_ms"], [100, 250])
        self.assertEqual(miclib.parse_meta("touch_ms=-")["touch_ms"], [])
        self.assertEqual(miclib.parse_meta("touch_ms=7")["touch_ms"], [7])


class Wav(unittest.TestCase):
    def read(self, data):
        with wave.open(io.BytesIO(data)) as w:
            return w.getnchannels(), w.getsampwidth(), w.getframerate(), w.readframes(10 ** 6)

    def test_16bit(self):
        raw = s16([0, 32752, -32768, 1234])
        ch, sw, rate, frames = self.read(miclib.wav_bytes(raw, 16, 16000))
        self.assertEqual((ch, sw, rate), (1, 2, 16000))
        self.assertEqual(frames, raw)

    def test_8bit_signed_to_unsigned(self):
        raw = bytes([0x00, 0x7F, 0x80, 0xFF])       # 0, 127, -128, -1 (signed)
        ch, sw, rate, frames = self.read(miclib.wav_bytes(raw, 8, 8000))
        self.assertEqual((ch, sw, rate), (1, 1, 8000))
        self.assertEqual(frames, bytes([128, 255, 0, 127]))

    def test_odd_length_16bit(self):
        _, _, _, frames = self.read(miclib.wav_bytes(s16([5, 6]) + b"\x01", 16, 16000))
        self.assertEqual(frames, s16([5, 6]))


class Analysis(unittest.TestCase):
    def test_sine_levels(self):
        rate, amp = 16000, 16384                    # half scale = -6 dBFS peak
        vals = [int(amp * math.sin(2 * math.pi * 440 * i / rate)) for i in range(rate)]
        a = miclib.analyse(s16(vals), 16, rate)
        self.assertEqual(a["samples"], rate)
        self.assertAlmostEqual(a["duration_s"], 1.0)
        self.assertAlmostEqual(a["peak_dbfs"], -6.0, delta=0.1)
        self.assertAlmostEqual(a["rms_dbfs"], -9.0, delta=0.2)   # sine RMS = peak - 3 dB
        self.assertEqual(a["clipped_pct"], 0.0)
        self.assertLess(abs(a["dc_offset_pct"]), 0.1)

    def test_noise_floor_and_snr(self):
        rate = 16000
        quiet = [((i * 7919) % 41) - 20 for i in range(rate)]          # ~-67 dBFS
        loud = [int(8000 * math.sin(2 * math.pi * 300 * i / rate)) for i in range(rate // 4)]
        a = miclib.analyse(s16(quiet + loud), 16, rate)
        self.assertLess(a["noise_floor_dbfs"], -60)
        self.assertGreater(a["speech_dbfs"], -16)
        self.assertGreater(a["snr_db"], 40)

    def test_clipping_8bit(self):
        raw = bytes([0x7F, 0x80, 0x00, 0x10] * 100)     # half the samples at the rails
        a = miclib.analyse(raw, 8, 8000)
        self.assertEqual(a["clipped_pct"], 50.0)
        self.assertEqual(a["peak_dbfs"], 0.0)

    def test_tap_click(self):
        rate = 16000
        vals = [0] * rate
        for i in range(8000, 8080):                      # a click at 500 ms
            vals[i] = 20000 if i % 2 else -20000
        vals = [v + ((i * 31) % 7) - 3 for i, v in enumerate(vals)]
        a = miclib.analyse(s16(vals), 16, rate, touch_ms=[500])
        self.assertGreater(a["tap_db_above_median"][0], 40)

    def test_empty(self):
        self.assertEqual(miclib.analyse(b"", 16, 16000)["samples"], 0)


class Sanitize(unittest.TestCase):
    def test_ascii(self):
        self.assertEqual(miclib.sanitize("Qual è la capitale? Perché!"),
                         "Qual e la capitale? Perche!")
        self.assertEqual(miclib.sanitize("a\tb€"), "a b?")


if __name__ == "__main__":
    unittest.main()
