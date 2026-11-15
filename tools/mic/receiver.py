#!/usr/bin/env python3
"""mictest receiver (diagnostic): takes push-to-talk
clips from ds/mictest/mictest.nds, saves them, measures them and transcribes
them with faster-whisper (CPU, int8), base + small multilingual models.

    relay/voice/.venv/bin/python tools/mic/receiver.py          # on 192.168.77.1:4243
    relay/voice/.venv/bin/python tools/mic/receiver.py --bind local  # melonDS (127.0.0.1)
    ... --no-transcribe        save + analyse only (works with plain python3)
    ... --models base          only one model
    ... --gemini               also ask Gemini (GEMINI_API_KEY from relay/.env; uses quota)

It NEVER downloads anything: models are loaded from relay/voice/models/ (fetch
them with relay/voice/setup.sh, which asks first). Clips go to local/mic/
(gitignored): clipNNN_<time>.wav + .json with the DS metadata, the analysis,
upload timing and the transcripts.

Speaks the DSChat framing; answers PING/PGET/LIVE like the relay so the DS
reuses dschat's net.c. Port 4243 must be opened on the AP for the DS:
sudo net/mic-port.sh open   (runtime-only; close with: sudo net/mic-port.sh close)
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
import secrets
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
import miclib  # noqa: E402

OUT = REPO / "local" / "mic"
MODELS = REPO / "relay" / "voice" / "models"   # shared with the relay (relay/voice/setup.sh)
MAX_CLIP = 16000 * 2 * 12           # a little over 10 s of 16 kHz 16-bit
BOOT = secrets.token_hex(4)

whisper = {}                        # name -> WhisperModel
gemini_key = None


def set_out(path: Path) -> None:
    global OUT
    OUT = path.resolve()


def log(msg: str) -> None:
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def ap_addr() -> str:
    for line in (REPO / "net" / "ap.conf").read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line.startswith("AP_ADDR="):
            return line.split("=", 1)[1].strip().strip('"')
    raise SystemExit("AP_ADDR not found in net/ap.conf")


def load_models(names: list[str]) -> None:
    try:
        from faster_whisper import WhisperModel
    except ImportError:
        raise SystemExit("faster-whisper is not installed: run relay/voice/setup.sh "
                         "(it shows the download size and asks first), or use --no-transcribe")
    for name in names:
        path = MODELS / f"faster-whisper-{name}"
        if not (path / "model.bin").exists():
            raise SystemExit(f"model '{name}' not downloaded ({path}): run relay/voice/setup.sh")
        t = time.monotonic()
        whisper[name] = WhisperModel(str(path), device="cpu", compute_type="int8",
                                     local_files_only=True)
        log(f"loaded whisper {name} in {time.monotonic() - t:.1f}s")


def read_gemini_key() -> str | None:
    env = REPO / "relay" / ".env"
    if not env.exists():
        return None
    for line in env.read_text().splitlines():
        if line.strip().startswith("GEMINI_API_KEY="):
            return line.split("=", 1)[1].strip().strip('"').strip("'") or None
    return None


def load_audio_16k(wav: Path):
    """WAV -> float32 mono at 16 kHz for Whisper. Done here instead of by
    faster-whisper's decode_audio(): faster-whisper 1.2.1 passes
    av.open(metadata_errors=...), which PyAV 19 no longer accepts.
    Resampling (8 kHz clips) is FFT-based (band-limited, exact for 2x)."""
    import wave

    import numpy as np
    with wave.open(str(wav)) as w:
        rate, width = w.getframerate(), w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if width == 1:
        x = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128) / 128
    else:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768
    if rate != 16000 and len(x):
        m = int(round(len(x) * 16000 / rate))
        spec = np.fft.rfft(x)
        out = np.zeros(m // 2 + 1, dtype=complex)
        k = min(len(spec), len(out))
        out[:k] = spec[:k]
        x = (np.fft.irfft(out, m) * (m / len(x))).astype(np.float32)
    return x


def transcribe_whisper(name: str, wav: Path) -> dict:
    t = time.monotonic()
    audio = load_audio_16k(wav)
    segments, info = whisper[name].transcribe(audio, beam_size=5, vad_filter=False)
    text = " ".join(s.text.strip() for s in segments).strip()   # (runs the decoding)
    return {"engine": f"whisper-{name}", "seconds": round(time.monotonic() - t, 2),
            "language": info.language, "language_p": round(info.language_probability, 2),
            "text": text}


def transcribe_gemini(wav: Path, model: str) -> dict:
    body = {"contents": [{"parts": [
        {"text": "Transcribe this audio exactly, in its original language. "
                 "Reply with the transcript only."},
        {"inline_data": {"mime_type": "audio/wav",
                         "data": base64.b64encode(wav.read_bytes()).decode()}}]}]}
    req = urllib.request.Request(
        f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent",
        data=json.dumps(body).encode(), method="POST",
        headers={"Content-Type": "application/json", "x-goog-api-key": gemini_key})
    t = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            data = json.load(r)
        parts = data["candidates"][0]["content"]["parts"]
        text = " ".join(p.get("text", "") for p in parts).strip()
    except urllib.error.HTTPError as e:
        text = f"(error: HTTP {e.code})"           # body may echo the request: not printed
    except Exception as e:                          # noqa: BLE001 - experiment, report and go on
        text = f"(error: {e.__class__.__name__})"
    return {"engine": f"gemini:{model}", "seconds": round(time.monotonic() - t, 2),
            "language": "", "language_p": 0, "text": text}


class Clip:
    def __init__(self, meta: dict):
        self.meta = meta
        self.data = bytearray()
        self.t_first = self.t_last = None
        self.path: Path | None = None
        self.record: dict = {}


class Session:
    def __init__(self, args, reader, writer):
        self.args, self.reader, self.writer = args, reader, writer
        self.peer = "%s:%s" % (writer.get_extra_info("peername") or ("?", 0))
        self.clip: Clip | None = None
        self.queue: asyncio.Queue = asyncio.Queue()

    def send(self, ftype: str, payload: bytes | str = b"") -> None:
        if isinstance(payload, str):
            payload = miclib.sanitize(payload)[:miclib.MAX_PAYLOAD]
        self.writer.write(miclib.encode_frame(ftype, payload))

    async def read_loop(self) -> None:
        """Reads frames; LIVE is answered at once (even while transcribing),
        everything else is handled in order by work_loop()."""
        parser = miclib.FrameParser()
        try:
            while True:
                data = await asyncio.wait_for(self.reader.read(65536), 600)
                if not data:
                    break
                now = time.monotonic()
                for ftype, payload in parser.feed(data):
                    if ftype == "LIVE":
                        self.send("LIVE")
                    else:
                        await self.queue.put((ftype, payload, now))
                await self.writer.drain()
        except (miclib.FrameError, asyncio.TimeoutError, ConnectionError) as e:
            log(f"{self.peer} closed: {e.__class__.__name__} {e}")
        finally:
            await self.queue.put(None)

    async def work_loop(self) -> None:
        while (item := await self.queue.get()) is not None:
            ftype, payload, t_rx = item
            try:
                await self.handle(ftype, payload, t_rx)
                await self.writer.drain()
            except ConnectionError:
                break

    async def handle(self, ftype: str, payload: bytes, t_rx: float) -> None:
        if ftype == "PING":
            self.send("PONG", f"DSChat relay 3 max_msg=4096 boot={BOOT}")
        elif ftype == "PGET":
            self.send("INFO", "mictest receiver")
        elif ftype == "CNCL":
            pass
        elif ftype == "MBEG":
            meta = miclib.parse_meta(payload.decode("ascii", "replace"))
            if meta.get("bits") not in (8, 16) or not isinstance(meta.get("rate"), int) \
                    or not 4000 <= meta["rate"] <= 48000:
                self.send("FAIL", "bad clip metadata")
                return
            self.clip = Clip(meta)
            self.send("OKAY")
        elif ftype == "MDAT":
            c = self.clip
            if c is None:
                self.send("FAIL", "MDAT before MBEG")
                return
            if len(c.data) + len(payload) > MAX_CLIP:
                self.send("FAIL", "clip too long")
                self.clip = None
                return
            c.t_first = c.t_first or t_rx
            c.t_last = t_rx
            c.data += payload
            self.send("OKAY", str(len(c.data)))
        elif ftype == "MEND":
            c = self.clip
            if c is None:
                self.send("FAIL", "MEND before MBEG")
                return
            ms = int(((c.t_last or 0) - (c.t_first or 0)) * 1000)
            self.send("OKAY", f"{len(c.data)} {ms}")
            self.save(c, ms)
        elif ftype == "MSTA":
            # upload stats from the DS (stall analysis): TX ring refusals, pacing
            st = miclib.parse_meta(payload.decode("ascii", "replace"))
            self.send("OKAY")
            if self.clip is not None and self.clip.path is not None:
                self.clip.record["ds_upload"] = st
                self.write_json(self.clip)
            log(f"  DS: TX ring refusals {st.get('rej')}, pacing {st.get('pace') or 'off (burst)'}, "
                f"upload {st.get('up_ms')} ms on the DS")
        elif ftype == "SEND":
            await self.transcribe()
        else:
            self.send("FAIL", "unknown frame type")

    def save(self, c: Clip, upload_ms: int) -> None:
        m = c.meta
        OUT.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        n = len(list(OUT.glob("clip*.wav"))) + 1
        c.path = OUT / f"clip{n:03d}_{stamp}.wav"
        c.path.write_bytes(miclib.wav_bytes(bytes(c.data), m["bits"], m["rate"]))
        a = miclib.analyse(bytes(c.data), m["bits"], m["rate"], m.get("touch_ms", []))
        kbps = len(c.data) / 1024 / (upload_ms / 1000) if upload_ms > 0 else 0
        c.record = {"file": c.path.name, "ds_meta": m, "bytes": len(c.data),
                    "upload_ms_laptop": upload_ms, "upload_kbps_laptop": round(kbps, 1),
                    "analysis": a, "transcripts": []}
        if m.get("rec_ms"):
            # audio shorter than the time Y was held = samples lost on the DS
            c.record["audio_vs_held_pct"] = round(a["duration_s"] * 1000 / m["rec_ms"] * 100, 1)
        self.write_json(c)
        log(f"clip {c.path.name}: {m.get('bits')}-bit {m.get('rate')} Hz gain x{m.get('gain')} "
            f"traffic={m.get('traffic')} taps={m.get('touches')}")
        log(f"  {a['duration_s']:.2f}s audio (held {m.get('rec_ms', 0) / 1000:.2f}s)  "
            f"peak {a['peak_dbfs']} dBFS  rms {a['rms_dbfs']}  noise {a['noise_floor_dbfs']}  "
            f"speech {a['speech_dbfs']}  snr {a['snr_db']} dB  clipped {a['clipped_pct']}%  "
            f"dc {a['dc_offset_pct']}%")
        if a.get("tap_db_above_median"):
            log(f"  taps: {a['tap_db_above_median']} dB above the median 20 ms window")
        log(f"  upload {len(c.data)} B in {upload_ms} ms = {kbps:.1f} KB/s (laptop side)")

    def write_json(self, c: Clip) -> None:
        c.path.with_suffix(".json").write_text(json.dumps(c.record, indent=2) + "\n")

    async def transcribe(self) -> None:
        c = self.clip
        if c is None or c.path is None:
            self.send("FAIL", "no clip")
            return
        a = c.record["analysis"]
        self.send("TEXT", f"laptop: {a['duration_s']:.1f}s peak {a['peak_dbfs']:.0f} "
                          f"rms {a['rms_dbfs']:.0f} noise {a['noise_floor_dbfs']:.0f} dBFS, "
                          f"snr {a['snr_db']:.0f} dB, clip {a['clipped_pct']}%")
        await self.writer.drain()
        loop = asyncio.get_running_loop()
        jobs = [(f"whisper {n}", transcribe_whisper, (n, c.path)) for n in whisper]
        if gemini_key:
            jobs.append(("gemini", transcribe_gemini, (c.path, self.args.gemini_model)))
        if not jobs:
            self.send("TEXT", "(transcription off: --no-transcribe)")
        for label, fn, fargs in jobs:
            r = await loop.run_in_executor(None, fn, *fargs)
            c.record["transcripts"].append(r)
            self.write_json(c)
            lang = f" [{r['language']} {r['language_p']:.2f}]" if r["language"] else ""
            log(f"  {r['engine']:<26} {r['seconds']:5.1f}s{lang}: {r['text']}")
            self.send("TEXT", f"{label} {r['seconds']:.1f}s{lang}: {r['text'] or '(nothing)'}")
            await self.writer.drain()
        self.send("DONE", "end_turn")


async def main_async(args) -> None:
    async def handle(reader, writer):
        s = Session(args, reader, writer)
        log(f"{s.peer} connected")
        await asyncio.gather(s.read_loop(), s.work_loop())
        writer.close()
        log(f"{s.peer} disconnected")

    server = await asyncio.start_server(handle, args.host, args.port, reuse_address=True)
    log(f"listening on {args.host}:{args.port}; clips -> {OUT}/  (Ctrl-C stops)")
    async with server:
        await server.serve_forever()


def main() -> int:
    global gemini_key
    ap = argparse.ArgumentParser(description="mictest receiver (experiment)")
    ap.add_argument("--bind", default="ap", help="'ap' (AP_ADDR, default), 'local' or an IP")
    ap.add_argument("--port", type=int, default=4243)
    ap.add_argument("--models", default="base,small", help="whisper models (default base,small)")
    ap.add_argument("--no-transcribe", action="store_true")
    ap.add_argument("--gemini", action="store_true",
                    help="also transcribe with Gemini (GEMINI_API_KEY in relay/.env; uses quota)")
    ap.add_argument("--gemini-model", default="gemini-3.5-flash-lite")
    ap.add_argument("--out", type=Path, default=OUT, help="clip folder (default local/mic)")
    args = ap.parse_args()
    set_out(args.out)
    args.host = {"ap": None, "local": "127.0.0.1"}.get(args.bind, args.bind) or ap_addr()
    if not args.no_transcribe:
        load_models([m for m in args.models.split(",") if m])
    if args.gemini:
        gemini_key = read_gemini_key()
        if not gemini_key:
            raise SystemExit("--gemini: GEMINI_API_KEY not found in relay/.env")
        log(f"Gemini comparison ON ({args.gemini_model}; uses your free quota)")
    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        log("bye")
    except OSError as e:
        raise SystemExit(f"cannot listen on {args.host}:{args.port}: {e}. Is the AP up "
                         f"(./dschat start)? For melonDS use --bind local.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
