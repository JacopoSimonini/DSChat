"""mictest receiver helpers: framing, WAV writing, level analysis.

Pure standard library, so the unit tests (test_miclib.py) and the receiver's
--no-transcribe mode need nothing installed. (mictest diagnostic)
"""
from __future__ import annotations

import array
import io
import math
import struct
import sys
import unicodedata
import wave

HEADER_LEN = 10
MAX_PAYLOAD = 4096


class FrameError(Exception):
    pass


def encode_frame(ftype: str, payload: bytes | str = b"") -> bytes:
    if isinstance(payload, str):
        payload = payload.encode("ascii")
    if len(ftype) != 4 or len(payload) > MAX_PAYLOAD:
        raise FrameError("bad frame")
    return f"{ftype} {len(payload):04d}\n".encode("ascii") + payload


def parse_header(hdr: bytes) -> tuple[str, int]:
    """'TYPE LLLL\\n' -> (type, length). Raises FrameError."""
    if len(hdr) != HEADER_LEN:
        raise FrameError("short header")
    ftype, sp, digits, nl = hdr[0:4], hdr[4:5], hdr[5:9], hdr[9:10]
    if not (ftype.isalpha() and ftype.isupper() and sp == b" " and digits.isdigit()
            and nl == b"\n"):
        raise FrameError("bad frame header")
    n = int(digits)
    if n > MAX_PAYLOAD:
        raise FrameError("frame too long")
    return ftype.decode("ascii"), n


class FrameParser:
    """Incremental parser: feed() bytes, get complete (type, payload) frames."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes) -> list[tuple[str, bytes]]:
        self.buf += data
        out = []
        while len(self.buf) >= HEADER_LEN:
            ftype, n = parse_header(bytes(self.buf[:HEADER_LEN]))
            if len(self.buf) < HEADER_LEN + n:
                break
            out.append((ftype, bytes(self.buf[HEADER_LEN:HEADER_LEN + n])))
            del self.buf[:HEADER_LEN + n]
        return out


def parse_meta(line: str) -> dict:
    """'k=v k2=v2' -> dict (ints where possible)."""
    meta = {}
    for part in line.split():
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        try:
            meta[k] = int(v)
        except ValueError:
            meta[k] = v
    if isinstance(meta.get("touch_ms"), int):
        meta["touch_ms"] = [meta["touch_ms"]]
    elif isinstance(meta.get("touch_ms"), str):
        meta["touch_ms"] = [int(x) for x in meta["touch_ms"].split(",") if x.isdigit()]
    return meta


def samples_from_raw(raw: bytes, bits: int) -> array.array:
    """DS samples -> signed ints. bits=8: signed int8; bits=16: s16le
    (12-bit ADC value shifted up 4 bits)."""
    if bits == 8:
        return array.array("b", raw)
    a = array.array("h")
    a.frombytes(raw[: len(raw) // 2 * 2])
    if sys.byteorder != "little":
        a.byteswap()
    return a


def wav_bytes(raw: bytes, bits: int, rate: int) -> bytes:
    """A mono PCM WAV. 8-bit WAV is unsigned, so the signed DS bytes are
    shifted by 128; 16-bit is written as is (little-endian)."""
    bio = io.BytesIO()
    with wave.open(bio, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1 if bits == 8 else 2)
        w.setframerate(rate)
        if bits == 8:
            w.writeframes(bytes((b + 128) & 0xFF for b in raw))
        else:
            w.writeframes(raw[: len(raw) // 2 * 2])
    return bio.getvalue()


def _db(x: float) -> float:
    return 20 * math.log10(x) if x > 0 else -120.0


def analyse(raw: bytes, bits: int, rate: int, touch_ms=(), win_ms: int = 20) -> dict:
    """Levels in dBFS: peak, RMS (DC removed), noise floor (10th percentile of
    20 ms window RMS), speech level (95th percentile), clipped %, DC offset,
    and the level around stylus taps compared with the median window."""
    s = samples_from_raw(raw, bits)
    n = len(s)
    full = 128.0 if bits == 8 else 32768.0
    clip_hi = 127 if bits == 8 else 32752
    clip_lo = -128 if bits == 8 else -32768
    if n == 0:
        return {"samples": 0, "duration_s": 0.0}
    dc = sum(s) / n
    peak = max(abs(v) for v in s)
    clipped = sum(1 for v in s if v >= clip_hi or v <= clip_lo)
    rms = math.sqrt(sum((v - dc) ** 2 for v in s) / n)
    w = max(1, rate * win_ms // 1000)
    wins = []
    for i in range(0, n - w + 1, w):
        seg = s[i:i + w]
        wins.append(math.sqrt(sum((v - dc) ** 2 for v in seg) / w))
    wins_sorted = sorted(wins) or [rms]

    def pct(p):
        return wins_sorted[min(len(wins_sorted) - 1, int(p * len(wins_sorted)))]

    noise, speech, median = pct(0.10), pct(0.95), pct(0.50)
    res = {
        "samples": n,
        "duration_s": round(n / rate, 3),
        "dc_offset_pct": round(dc / full * 100, 2),
        "peak_dbfs": round(_db(peak / full), 1),
        "rms_dbfs": round(_db(rms / full), 1),
        "noise_floor_dbfs": round(_db(noise / full), 1),
        "speech_dbfs": round(_db(speech / full), 1),
        "snr_db": round(_db(speech / full) - _db(noise / full), 1),
        "clipped_pct": round(clipped / n * 100, 2),
    }
    taps = []
    for t in touch_ms or ():
        idx = int(t * rate / 1000) // w
        near = wins[max(0, idx - 2): idx + 3]
        if near:
            taps.append(round(_db(max(near) / full) - _db(median / full), 1))
    if taps:
        res["tap_db_above_median"] = taps
    return res


def sanitize(text: str) -> str:
    """Printable ASCII for the DS (accents dropped, others -> '?')."""
    out = []
    for ch in text:
        if 0x20 <= ord(ch) <= 0x7E:
            out.append(ch)
            continue
        base = "".join(c for c in unicodedata.normalize("NFKD", ch)
                       if not unicodedata.combining(c))
        out.append(base if base and all(0x20 <= ord(c) <= 0x7E for c in base) else
                   (" " if ch.isspace() else "?"))
    return "".join(out)
