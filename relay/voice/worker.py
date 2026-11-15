#!/usr/bin/env python3
"""DSChat voice worker: speech to text for the relay (protocol v4).

Started by relay.py with relay/voice/.venv/bin/python (faster-whisper lives in
its own venv, so the relay's venv stays small and voice stays optional). One
request at a time over stdin/stdout; it never writes audio anywhere and never
prints a transcript except in its answer.

  stdout, once:  {"ready": true, "model": "small", "load_s": 0.8}   (or "error")
  stdin:         {"id": 1, "lang": "en", "samples": N, "hints": "..."}\\n + N*2 bytes s16le 16 kHz
  stdout:        {"id": 1, "text": "...", "seconds": 1.9}             (or "error")

Pipeline (from the mictest experiment, docs/TECHNICAL-NOTES.md): the DS has
already dropped the amplifier's 0.3 s power-up burst; here a 100 Hz high-pass
removes DC, rumble and the DS's 60 Hz hum, then Whisper runs with the
language given by the DS (auto-detection guessed Polish/Latin/Slovenian for
English on DS audio), the voice-activity filter on (silence otherwise became
"Thank you for watching!") and the user's VOICE_HINTS as the initial prompt.
"""
import argparse
import json
import sys
import time
from pathlib import Path


def highpass(x, rate=16000, cutoff=100.0):
    import numpy as np
    if len(x) == 0:
        return x
    spec = np.fft.rfft(x)
    spec[np.fft.rfftfreq(len(x), 1 / rate) < cutoff] = 0
    return np.fft.irfft(spec, len(x)).astype(np.float32)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model-dir", required=True)
    ap.add_argument("--model", default="small")
    ap.add_argument("--threads", type=int, default=0)
    args = ap.parse_args()
    out = sys.stdout
    t = time.monotonic()
    try:
        import numpy as np
        from faster_whisper import WhisperModel
        path = Path(args.model_dir) / f"faster-whisper-{args.model}"
        if not (path / "model.bin").exists():
            raise FileNotFoundError(f"model {args.model} not found in {args.model_dir}")
        model = WhisperModel(str(path), device="cpu", compute_type="int8",
                             cpu_threads=args.threads, local_files_only=True)
    except Exception as e:                      # noqa: BLE001 - reported to the relay
        out.write(json.dumps({"ready": False, "error": f"{e.__class__.__name__}: {e}"}) + "\n")
        out.flush()
        return 1
    out.write(json.dumps({"ready": True, "model": args.model,
                          "load_s": round(time.monotonic() - t, 2)}) + "\n")
    out.flush()

    inp = sys.stdin.buffer
    while True:
        line = inp.readline()
        if not line:
            return 0
        try:
            req = json.loads(line)
            n = int(req["samples"])
            raw = inp.read(n * 2)
            if len(raw) != n * 2:
                return 0
            t = time.monotonic()
            x = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768
            segs, _ = model.transcribe(highpass(x), language=req["lang"], beam_size=5,
                                       vad_filter=True, condition_on_previous_text=False,
                                       initial_prompt=req.get("hints") or None)
            text = " ".join(s.text.strip() for s in segs).strip()
            ans = {"id": req.get("id"), "text": text, "seconds": round(time.monotonic() - t, 3)}
        except Exception as e:                  # noqa: BLE001 - one bad request must not kill it
            ans = {"id": None, "error": f"{e.__class__.__name__}: {e}"}
        out.write(json.dumps(ans) + "\n")
        out.flush()


if __name__ == "__main__":
    sys.exit(main())
