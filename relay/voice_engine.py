"""Voice input for the relay (protocol v4): ADPCM decoding, stream state and
the speech-to-text worker process (relay/voice/worker.py, run with the
faster-whisper venv relay/voice/.venv). Standard library only.
"""
from __future__ import annotations

import array
import asyncio
import json
import logging
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

log = logging.getLogger("relay")

HERE = Path(__file__).resolve().parent
VOICE_DIR = HERE / "voice"
VOICE_PYTHON = VOICE_DIR / ".venv" / "bin" / "python"
WORKER = VOICE_DIR / "worker.py"
MODELS = VOICE_DIR / "models"

RATE = 16000
BLOCK = 1025                     # samples per ADPCM block (the DS's choice)
BLOCK_BYTES = 4 + (BLOCK - 1) // 2
MAX_AUDIO_S = 21                 # 20 s of speech + slack
MAX_SAMPLES = RATE * MAX_AUDIO_S
MAX_BYTES = (MAX_SAMPLES // BLOCK + 2) * BLOCK_BYTES
LANGS = ("en", "it")

_STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
          66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337,
          371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
          1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
          6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
          22385, 24623, 27086, 29794, 32767]
_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_decode_block(block: bytes) -> array.array:
    """One IMA ADPCM block as sent by the DS (ds/dschat/source/adpcm.h):
    int16 LE predictor, step index, 0, then 4-bit codes low nibble first."""
    if len(block) < 4:
        raise ValueError("short ADPCM block")
    pred = int.from_bytes(block[0:2], "little", signed=True)
    idx = min(block[2], 88)
    out = array.array("h", [pred])
    steps, adj = _STEPS, _INDEX
    for byte in block[4:]:
        for code in (byte & 15, byte >> 4):
            st = steps[idx]
            diff = st >> 3
            if code & 4:
                diff += st
            if code & 2:
                diff += st >> 1
            if code & 1:
                diff += st >> 2
            pred = pred - diff if code & 8 else pred + diff
            pred = 32767 if pred > 32767 else -32768 if pred < -32768 else pred
            idx += adj[code & 7]
            idx = 0 if idx < 0 else 88 if idx > 88 else idx
            out.append(pred)
    return out


def parse_params(payload: bytes) -> dict:
    p = {}
    for part in payload.decode("ascii", "replace").split():
        if "=" in part:
            k, v = part.split("=", 1)
            p[k] = v
    return p


@dataclass
class VoiceStream:
    """One VBEG..VEND stream on a connection."""
    lang: str = "en"
    pcm: array.array = field(default_factory=lambda: array.array("h"))
    bytes: int = 0
    t_first: float = 0.0
    t_last: float = 0.0
    error: str = ""

    @staticmethod
    def begin(payload: bytes) -> "VoiceStream":
        p = parse_params(payload)
        s = VoiceStream(lang=p.get("lang", ""))
        if p.get("codec") != "ima-adpcm" or p.get("rate") != str(RATE) or p.get("block") != str(BLOCK):
            s.error = "unsupported audio format"
        elif s.lang not in LANGS:
            s.error = f"language must be one of {', '.join(LANGS)}"
        return s

    def add(self, payload: bytes, now: float) -> None:
        if self.error:
            return
        self.t_first = self.t_first or now
        self.t_last = now
        self.bytes += len(payload)
        if self.bytes > MAX_BYTES:
            self.error = f"audio too long (max {MAX_AUDIO_S - 1} s)"
            self.pcm = array.array("h")
            return
        try:
            for i in range(0, len(payload), BLOCK_BYTES):
                self.pcm.extend(adpcm_decode_block(payload[i:i + BLOCK_BYTES]))
        except ValueError as e:
            self.error = str(e)
        if len(self.pcm) > MAX_SAMPLES:
            self.error = f"audio too long (max {MAX_AUDIO_S - 1} s)"
            self.pcm = array.array("h")

    @property
    def audio_ms(self) -> int:
        return len(self.pcm) * 1000 // RATE

    @property
    def rx_ms(self) -> int:
        return int((self.t_last - self.t_first) * 1000)


class VoiceEngine:
    """The worker process: started once, one transcription at a time."""

    def __init__(self, model: str = "small", hints: str = "", fake: bool = False):
        self.model = model
        self.hints = hints
        self.fake = fake
        self.available = False
        self.reason = "not started"
        self.proc: asyncio.subprocess.Process | None = None
        self.lock = asyncio.Lock()
        self.next_id = 0

    def describe(self) -> str:
        if self.available:
            return f"on ({'fake' if self.fake else self.model}, {'/'.join(LANGS)})"
        return f"off ({self.reason})"

    async def start(self, timeout: float = 120) -> None:
        if self.fake:
            self.available, self.reason = True, ""
            return
        if not VOICE_PYTHON.exists():
            self.reason = "faster-whisper not installed (relay/voice/setup.sh)"
            return
        if not (MODELS / f"faster-whisper-{self.model}" / "model.bin").exists():
            self.reason = f"model '{self.model}' not downloaded (relay/voice/setup.sh)"
            return
        try:
            self.proc = await asyncio.create_subprocess_exec(
                str(VOICE_PYTHON), "-I", str(WORKER), "--model-dir", str(MODELS),
                "--model", self.model, stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE, limit=1 << 20)
            line = await asyncio.wait_for(self.proc.stdout.readline(), timeout)
            ready = json.loads(line or b"{}")
        except (OSError, asyncio.TimeoutError, ValueError) as e:
            self.reason = f"worker did not start: {e.__class__.__name__}"
            await self.close()
            return
        if not ready.get("ready"):
            self.reason = f"worker: {ready.get('error', 'no answer')}"[:120]
            await self.close()
            return
        self.available, self.reason = True, ""
        log.info("voice worker ready: %s model loaded in %ss", self.model, ready.get("load_s"))

    async def transcribe(self, pcm: array.array, lang: str) -> tuple[str, float]:
        """-> (text, seconds). Raises RuntimeError with a short reason."""
        if self.fake:
            await asyncio.sleep(0.2)
            secs = len(pcm) / RATE
            text = "" if secs < 0.5 else f"this is a fake transcript of {secs:.1f} seconds of {lang} audio"
            return text, 0.2
        async with self.lock:                     # one transcription at a time
            if self.proc is None or self.proc.returncode is not None:
                self.available = False
                await self.start()
                if not self.available:
                    raise RuntimeError("voice engine not available")
            self.next_id += 1
            req = {"id": self.next_id, "lang": lang, "samples": len(pcm), "hints": self.hints}
            if sys.byteorder != "little":
                pcm = array.array("h", pcm)
                pcm.byteswap()
            try:
                self.proc.stdin.write(json.dumps(req).encode() + b"\n" + pcm.tobytes())
                await self.proc.stdin.drain()
                ans = json.loads(await asyncio.wait_for(self.proc.stdout.readline(), 110) or b"{}")
            except (OSError, asyncio.TimeoutError, ValueError, ConnectionError) as e:
                await self.close()
                raise RuntimeError(f"voice engine failed ({e.__class__.__name__})") from None
            if "error" in ans or "text" not in ans:
                log.warning("voice worker error: %s", ans.get("error", "no answer"))
                raise RuntimeError("transcription failed")
            return ans["text"], float(ans.get("seconds", 0))

    async def close(self) -> None:
        p, self.proc = self.proc, None
        if p is not None and p.returncode is None:
            p.kill()
            try:
                await p.wait()
            except Exception:                     # noqa: BLE001
                pass
