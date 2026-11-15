#!/usr/bin/env python3
"""PC test client for the DSChat relay (docs/PROTOCOL.md). Stdlib only.

  python3 tools/client.py                       # interactive, 127.0.0.1:4242
  python3 tools/client.py --host 192.168.77.1   # through the AP address
  python3 tools/client.py -m "Hello"            # one message, then exit
  python3 tools/client.py --selftest            # protocol checks (use relay --fake)
  python3 tools/client.py --profile gemini      # switch profile, then chat
  python3 tools/client.py --check gemini        # one real round trip (validates the key)
  python3 tools/client.py --check all           # every profile the relay knows

Interactive commands: /ping  /reset  /profiles  /use NAME  /quit
(anything else is sent as SEND; Ctrl-C during a reply sends CNCL)
"""

from __future__ import annotations

import argparse
import socket
import sys
import time
import unicodedata

HEADER_LEN = 10


def frame(ftype: str, payload: bytes = b"") -> bytes:
    return f"{ftype} {len(payload):04d}\n".encode("ascii") + payload


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("relay closed the connection")
        buf += chunk
    return buf


def read_frame(sock: socket.socket) -> tuple[str, bytes]:
    hdr = recv_exact(sock, HEADER_LEN)
    if hdr[4:5] != b" " or hdr[9:10] != b"\n" or not hdr[5:9].isdigit():
        raise ValueError(f"bad header from relay: {hdr!r}")
    n = int(hdr[5:9])
    return hdr[:4].decode("ascii"), recv_exact(sock, n) if n else b""


def connect(host: str, port: int) -> socket.socket:
    """Open the TCP connection; turns the usual failures into a one-line hint."""
    try:
        return socket.create_connection((host, port), timeout=10)
    except OSError as e:
        reason = {ConnectionRefusedError: "connection refused",
                  TimeoutError: "timed out"}.get(type(e), e.strerror or str(e))
        raise SystemExit(
            f"client: cannot connect to the relay at {host}:{port} ({reason}).\n"
            f"        Is it running? relay/.venv/bin/python relay/relay.py"
            f"{' --bind local' if host.startswith('127.') else ''}") from None


def check_ascii(payload: bytes) -> None:
    bad = [b for b in payload if not (0x20 <= b <= 0x7E or b == 0x0A)]
    if bad:
        raise ValueError(f"non-printable bytes in payload: {bad[:8]}")


def chat(sock: socket.socket, text: str, raw: bool = False) -> tuple[str, str]:
    """Send one message; print the streamed reply. Returns (end type, text)."""
    # The protocol is ASCII-only (the DS keyboard is too): drop accents.
    ascii_text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore")
    sock.sendall(frame("SEND", ascii_text))
    t0 = time.monotonic()
    first = None
    parts: list[str] = []
    cancel_sent = False
    while True:
        try:
            ftype, payload = read_frame(sock)
        except KeyboardInterrupt:
            if cancel_sent:
                raise
            # Protocol v3: ask the relay to stop; the reply still ends with
            # FAIL cancelled (or DONE if it finished first).
            sock.sendall(frame("CNCL"))
            cancel_sent = True
            print("\n[cancel requested]")
            continue
        check_ascii(payload)
        if raw:
            print(f"[{ftype} {len(payload)}] {payload!r}")
        if ftype == "TEXT":
            if first is None:
                first = time.monotonic() - t0
            parts.append(payload.decode())
            if not raw:
                print(payload.decode(), end="", flush=True)
        elif ftype in ("DONE", "FAIL"):
            if not raw:
                tail = f"[{ftype} {payload.decode()}]"
                print(("\n" if parts else "") + tail)
                print(f"(first chunk {first if first is not None else -1:.2f}s, "
                      f"total {time.monotonic() - t0:.2f}s, "
                      f"{sum(map(len, parts))} chars)")
            return ftype, "".join(parts)
        else:
            raise ValueError(f"unexpected frame during reply: {ftype}")


def simple(sock: socket.socket, ftype: str, expect: str) -> bytes:
    sock.sendall(frame(ftype))
    got, payload = read_frame(sock)
    if got != expect:
        raise ValueError(f"{ftype}: expected {expect}, got {got} {payload!r}")
    return payload


def list_profiles(sock: socket.socket) -> list[tuple[str, str, str]]:
    """PLST -> [(flag, name, model)]; flag '*' current, '+' ready, '!' no key."""
    out = []
    for line in simple(sock, "PLST", "INFO").decode().splitlines():
        flag, name, model = line.split(" ", 2)
        out.append((flag, name, model))
    return out


def use_profile(sock: socket.socket, name: str) -> str:
    """PSET; returns 'name model'. Raises ValueError with the relay's FAIL text."""
    sock.sendall(frame("PSET", name.encode("ascii")))
    t, p = read_frame(sock)
    if t != "OKAY":
        raise ValueError(f"PSET {name}: {t} {p.decode()}")
    return p.decode()


CHECK_PROMPT = "This is a connection test. Reply with exactly: OK"


def check(host: str, port: int, which: str) -> int:
    """One real round trip per profile, to validate keys/models from the PC."""
    with connect(host, port) as s:
        s.settimeout(150)
        print("relay:", simple(s, "PING", "PONG").decode())
        profiles = list_profiles(s)
        names = [n for _, n, _ in profiles] if which == "all" else [which]
        failed = 0
        for name in names:
            print(f"\n=== {name} ===")
            try:
                print("profile:", use_profile(s, name))
            except ValueError as e:
                print(f"SKIP/FAIL  {e}")
                failed += 1
                continue
            end, text = chat(s, CHECK_PROMPT)
            ok = end == "DONE" and text.strip() != ""
            print(f"{'OK' if ok else 'FAIL'}  {name}")
            failed += not ok
        return 1 if failed else 0


# --- protocol v4 voice: IMA ADPCM encoder, a port of ds/dschat/source/adpcm.c --
_STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
          60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
          337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
          1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
          5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
          20350, 22385, 24623, 27086, 29794, 32767]
_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]
VOICE_BLOCK = 1025


def adpcm_encode(pcm: list[int], block: int = VOICE_BLOCK) -> list[bytes]:
    """16 kHz int16 samples -> self-contained IMA ADPCM blocks (PROTOCOL.md v4)."""
    out, idx = [], 0
    for off in range(0, len(pcm), block):
        part = pcm[off:off + block]
        pred = part[0]
        b = bytearray(int(pred).to_bytes(2, "little", signed=True) + bytes([idx, 0]))
        codes = []
        for x in part[1:]:
            st = _STEPS[idx]
            diff, code = x - pred, 0
            if diff < 0:
                code, diff = 8, -diff
            for bit, sh in ((4, 0), (2, 1), (1, 2)):
                if diff >= st >> sh:
                    code |= bit
                    diff -= st >> sh
            d = st >> 3
            d += st if code & 4 else 0
            d += st >> 1 if code & 2 else 0
            d += st >> 2 if code & 1 else 0
            pred = max(-32768, min(32767, pred - d if code & 8 else pred + d))
            idx = max(0, min(88, idx + _INDEX[code & 7]))
            codes.append(code)
        for i in range(0, len(codes), 2):
            b.append(codes[i] | ((codes[i + 1] << 4) if i + 1 < len(codes) else 0))
        out.append(bytes(b))
    return out


def tone(seconds: float, freq: float = 440.0, amp: int = 8000) -> list[int]:
    import math
    return [int(amp * math.sin(2 * math.pi * freq * i / 16000)) for i in range(int(16000 * seconds))]


def voice_frames(pcm: list[int], lang: str = "en", vbeg: bytes | None = None) -> bytes:
    blocks = adpcm_encode(pcm)
    data = frame("VBEG", vbeg if vbeg is not None else
                 f"codec=ima-adpcm rate=16000 block={VOICE_BLOCK} lang={lang}".encode())
    for i in range(0, len(blocks), 2):
        data += frame("VDAT", b"".join(blocks[i:i + 2]))
    return data


def selftest(host: str, port: int) -> int:
    """Protocol conformance checks. Meant for a relay started with --fake."""
    results = []

    def check(name, fn):
        try:
            fn()
            results.append((name, True, ""))
        except Exception as e:  # noqa: BLE001
            results.append((name, False, str(e)))

    def conn():
        s = connect(host, port)
        return s

    def t_ping():
        with conn() as s:
            info = simple(s, "PING", "PONG").decode()
            assert info.startswith("DSChat relay 4 "), info
            fields = dict(f.split("=", 1) for f in info.split()[3:])
            assert int(fields["max_msg"]) > 0 and len(fields["boot"]) == 8, info
            assert fields.get("voice") == "1", "fake relay should offer voice"

    def t_voice():
        with conn() as s:
            s.sendall(voice_frames(tone(2.0), "it") + frame("VEND"))
            t, p = read_frame(s)
            assert t == "VTXT", (t, p)
            head, text = p.decode().split("\n", 1)
            f = dict(x.split("=", 1) for x in head.split())
            assert abs(int(f["audio"]) - 2000) <= 70, f            # whole blocks
            assert int(f["bytes"]) == sum(len(b) for b in adpcm_encode(tone(2.0))), f
            assert "fake transcript of 2.0 seconds of it audio" in text, text
            check_ascii(p)

    def t_voice_short_and_errors():
        with conn() as s:
            s.sendall(voice_frames(tone(0.3)) + frame("VEND"))      # nothing heard
            t, p = read_frame(s)
            assert t == "VTXT" and p.decode().split("\n", 1)[1] == "", (t, p)
            s.sendall(frame("VEND"))                                 # no stream
            assert read_frame(s)[0] == "FAIL"
            s.sendall(voice_frames(tone(1), vbeg=b"codec=mp3 rate=16000 block=1025 lang=en")
                      + frame("VEND"))
            t, p = read_frame(s)
            assert t == "FAIL" and b"format" in p, (t, p)
            s.sendall(voice_frames(tone(1), "fr") + frame("VEND"))
            t, p = read_frame(s)
            assert t == "FAIL" and b"language" in p, (t, p)
            s.sendall(voice_frames(tone(22)) + frame("VEND"))        # over the limit
            t, p = read_frame(s)
            assert t == "FAIL" and b"too long" in p, (t, p)
            assert simple(s, "PING", "PONG")                          # still usable

    def t_voice_cancel():
        with conn() as s:
            s.sendall(voice_frames(tone(1.0)) + frame("CNCL") + frame("PING"))
            assert read_frame(s)[0] == "PONG"                         # CNCL: no reply
            s.sendall(frame("VEND"))                                  # stream is gone
            assert read_frame(s)[0] == "FAIL"
            s.sendall(voice_frames(tone(1.0)) + frame("VEND") + frame("CNCL"))
            t, p = read_frame(s)                                      # fake takes 0.2 s
            assert (t, p) == ("FAIL", b"cancelled") or t == "VTXT", (t, p)
            assert simple(s, "PING", "PONG")

    def t_cancel():
        with conn() as s:
            simple(s, "RSET", "OKAY")
            s.sendall(frame("SEND", b"slow reply please"))   # fake: 20 s delay
            time.sleep(0.5)
            t0 = time.monotonic()
            s.sendall(frame("CNCL"))
            t, p = read_frame(s)
            assert (t, p) == ("FAIL", b"cancelled"), (t, p)
            assert time.monotonic() - t0 < 3, "cancel took too long"
            end, text = chat(s, "after cancel", raw=True)
            assert end == "DONE" and "#1" in text, text       # rolled back

    def t_cancel_idle_ignored():
        with conn() as s:
            s.sendall(frame("CNCL") + frame("PING"))
            t, _ = read_frame(s)
            assert t == "PONG", t                              # no answer to CNCL

    def t_cancel_with_queued_frame():
        with conn() as s:
            s.sendall(frame("SEND", b"slow one") + frame("PING"))
            time.sleep(0.3)
            s.sendall(frame("CNCL"))
            seq = [read_frame(s)[0], read_frame(s)[0]]
            assert seq == ["FAIL", "PONG"], seq                # PING waited

    def t_new_connection_replaces_old():
        old = conn()
        simple(old, "PING", "PONG")
        with conn() as s:
            simple(s, "PING", "PONG")                         # new one works
            old.settimeout(5)
            assert old.recv(1) == b"", "old connection should be closed"
        old.close()

    def t_live():
        with conn() as s:
            assert simple(s, "LIVE", "LIVE") == b""                 # idle: immediate
            s.sendall(frame("SEND", b"slow reply please"))          # fake: 20 s delay
            time.sleep(0.3)
            t0 = time.monotonic()
            s.sendall(frame("LIVE"))
            t, _ = read_frame(s)
            assert t == "LIVE" and time.monotonic() - t0 < 2, t     # answered mid-reply
            s.sendall(frame("CNCL"))
            t, p = read_frame(s)
            assert (t, p) == ("FAIL", b"cancelled"), (t, p)

    def t_long_reply():
        with conn() as s:
            simple(s, "RSET", "OKAY")
            end, text = chat(s, "long reply please", raw=False)
            assert end == "DONE" and len(text) > 2500, len(text)
            assert "Pneumonoultramicroscopicsilicovolcanoconiosis" in text

    def t_profiles():
        with conn() as s:
            profs = list_profiles(s)
            assert len(profs) >= 2, profs
            current = [n for f, n, _ in profs if f == "*"]
            assert len(current) == 1, profs
            got = simple(s, "PGET", "INFO").decode().split(" ")
            assert got[0] == current[0], (got, current)
            other = next(n for f, n, _ in profs if f != "*")
            simple(s, "RSET", "OKAY")
            chat(s, "before switch", raw=True)              # history: 1 turn
            name_model = use_profile(s, other)
            assert name_model.split(" ")[0] == other, name_model
            assert simple(s, "PGET", "INFO").decode().startswith(other + " ")
            assert [n for f, n, _ in list_profiles(s) if f == "*"] == [other]
            end, text = chat(s, "after switch", raw=True)
            assert end == "DONE", end
            assert f"#1 from {other}" in text, text          # switch reset history
            assert "SECRET" not in text and "think>" not in text, text
            s.sendall(frame("PSET", b"no-such-profile"))
            t, _ = read_frame(s)
            assert t == "FAIL", t
            use_profile(s, current[0])                       # restore default

    def t_chat_and_history():
        with conn() as s:
            simple(s, "RSET", "OKAY")
            end, text = chat(s, "hello caf\xe9", raw=True)   # latin-1 e-acute
            assert end == "DONE", end
            assert "#1" in text, text
            end, text = chat(s, "again", raw=True)
            assert end == "DONE" and "#2" in text, text
        with conn() as s:   # history survives reconnect (same IP)
            end, text = chat(s, "third", raw=True)
            assert "#3" in text, text
            simple(s, "RSET", "OKAY")
            end, text = chat(s, "after reset", raw=True)
            assert "#1" in text, text

    def t_fail_rolls_back():
        with conn() as s:
            simple(s, "RSET", "OKAY")
            end, _ = chat(s, "fail please", raw=True)
            assert end == "FAIL", end
            end, text = chat(s, "ok", raw=True)
            assert end == "DONE" and "#1" in text, text

    def t_unknown_type():
        with conn() as s:
            s.sendall(frame("WHAT"))
            t, _ = read_frame(s)
            assert t == "FAIL", t
            simple(s, "PING", "PONG")   # connection still usable

    def t_too_long_message():
        with conn() as s:
            s.sendall(frame("SEND", b"x" * 2000))
            t, p = read_frame(s)
            assert t == "FAIL" and b"too long" in p, (t, p)
            simple(s, "PING", "PONG")

    def t_empty_message():
        with conn() as s:
            s.sendall(frame("SEND", b"   "))
            t, _ = read_frame(s)
            assert t == "FAIL", t

    def t_bad_header_closes():
        with conn() as s:
            s.sendall(b"hello world\n")
            t, _ = read_frame(s)
            assert t == "FAIL", t
            assert s.recv(1) == b"", "connection should be closed"

    def t_oversize_frame_closes():
        with conn() as s:
            s.sendall(b"SEND 9999\n")
            t, p = read_frame(s)
            assert t == "FAIL" and b"too long" in p, (t, p)

    def t_pipelined_requests_wait():
        with conn() as s:
            # Two SENDs back to back: the second must wait for the first reply.
            s.sendall(frame("RSET") + frame("SEND", b"one") + frame("PING"))
            seq = []
            while True:
                t, _ = read_frame(s)
                seq.append(t)
                if t == "PONG":
                    break
            assert seq[0] == "OKAY" and seq[-2] == "DONE", seq
            assert "PONG" not in seq[:-1], seq

    for name, fn in [("ping (v4 info, voice=1)", t_ping), ("profiles list/get/switch", t_profiles),
                     ("voice: stream + transcript", t_voice),
                     ("voice: empty result, format/language/length errors", t_voice_short_and_errors),
                     ("voice: cancel mid-stream and during transcription", t_voice_cancel),
                     ("cancel (CNCL)", t_cancel), ("CNCL when idle is ignored", t_cancel_idle_ignored),
                     ("CNCL with a queued frame", t_cancel_with_queued_frame),
                     ("long reply", t_long_reply), ("LIVE probe (idle and mid-reply)", t_live),
                     ("new connection replaces the old one", t_new_connection_replaces_old),
                     ("chat + history", t_chat_and_history),
                     ("fail rolls back history", t_fail_rolls_back),
                     ("unknown type", t_unknown_type),
                     ("too long message", t_too_long_message),
                     ("empty message", t_empty_message),
                     ("bad header closes", t_bad_header_closes),
                     ("oversize frame closes", t_oversize_frame_closes),
                     ("pipelined requests are serialised", t_pipelined_requests_wait)]:
        check(name, fn)

    print()
    ok = True
    for name, passed, err in results:
        print(f"{'PASS' if passed else 'FAIL'}  {name}  {err}")
        ok &= passed
    return 0 if ok else 1


def interactive(host: str, port: int, raw: bool, profile: str | None) -> int:
    with connect(host, port) as s:
        s.settimeout(180)
        print("PONG:", simple(s, "PING", "PONG").decode())
        if profile:
            print("profile:", use_profile(s, profile))
        else:
            print("profile:", simple(s, "PGET", "INFO").decode())
        print("Type a message. /ping /reset /profiles /use NAME /quit")
        while True:
            try:
                line = input("> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                return 0
            if not line:
                continue
            if line == "/quit":
                return 0
            if line == "/ping":
                print("PONG:", simple(s, "PING", "PONG").decode())
            elif line == "/reset":
                simple(s, "RSET", "OKAY")
                print("[conversation reset]")
            elif line == "/profiles":
                for flag, name, model in list_profiles(s):
                    print(f" {flag} {name:<12} {model}")
                print(" (* current, + ready, ! no API key)")
            elif line.startswith("/use "):
                try:
                    print("[switched, conversation reset]", use_profile(s, line[5:].strip()))
                except ValueError as e:
                    print(e)
            else:
                chat(s, line, raw)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4242)
    ap.add_argument("-m", "--message", help="send one message and exit")
    ap.add_argument("--raw", action="store_true", help="print every frame")
    ap.add_argument("--selftest", action="store_true",
                    help="run protocol checks (start the relay with --fake)")
    ap.add_argument("-p", "--profile", help="switch to this relay profile first")
    ap.add_argument("--check", metavar="PROFILE",
                    help="one real round trip with PROFILE ('all' = every profile)")
    a = ap.parse_args()
    if a.selftest:
        return selftest(a.host, a.port)
    if a.check:
        return check(a.host, a.port, a.check)
    if a.message:
        with connect(a.host, a.port) as s:
            s.settimeout(180)
            if a.profile:
                print("profile:", use_profile(s, a.profile))
            end, _ = chat(s, a.message, a.raw)
            return 0 if end == "DONE" else 1
    return interactive(a.host, a.port, a.raw, a.profile)


if __name__ == "__main__":
    sys.exit(main())
