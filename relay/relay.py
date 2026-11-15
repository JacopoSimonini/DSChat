#!/usr/bin/env python3
"""DSChat relay: plain-TCP (docs/PROTOCOL.md) <-> AI provider bridge for the DS.

The DS speaks a tiny framed protocol over TCP to this process; this process
holds the API keys, keeps the conversation history and streams the reply
back as sanitised ASCII chunks. Providers are configured as profiles in
providers.toml (adapters: native Anthropic SDK, or any OpenAI-compatible API).

Run:  .venv/bin/python relay.py               # bind to the AP address from net/ap.conf
      .venv/bin/python relay.py --bind local  # 127.0.0.1, for melonDS / PC tests
      .venv/bin/python relay.py --fake        # no API calls, canned replies
      .venv/bin/python relay.py --profile gemini   # override ACTIVE_PROFILE
      .venv/bin/python relay.py --check-config     # list profiles + key status, exit

Logs never contain API keys (redacted as a safety net) nor message text;
-v / --debug adds the user's messages (still no keys).
"""

from __future__ import annotations

import argparse
import asyncio
import errno
import ipaddress
import logging
import os
import re
import secrets
import stat
import time
import sys
from collections import deque
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path

from dotenv import load_dotenv

from providers import (Adapter, ConfigError, Profile, ReplyFailed, load_profiles,
                       make_adapter)
from voice_engine import VoiceEngine, VoiceStream

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
AP_CONF = REPO / "net" / "ap.conf"
PROVIDERS_TOML = HERE / "providers.toml"

PROTO_VERSION = 4            # 2 = profile frames (PLST/PGET/PSET), 3 = CNCL, LIVE, PONG info,
                             # 4 = voice input (VBEG/VDAT/VEND -> VTXT, PONG voice=1)
# Changes on every relay start: a client that sees a new boot id knows the
# relay restarted and its conversation history (kept in memory) is gone.
BOOT_ID = secrets.token_hex(4)

HEADER_LEN = 10
MAX_PAYLOAD = 4096          # any frame
MAX_TEXT_CHUNK = 256        # TEXT frames we send
IDLE_TIMEOUT = 600.0        # s without a frame from the client
HELLO_TIMEOUT = 20.0        # s for a NEW connection to send its first frame
                            # (so silent connections cannot hold all the slots)
FRAME_TIMEOUT = 15.0        # s to finish a frame once it started
WRITE_TIMEOUT = 30.0        # s for the client to accept our data
REPLY_TIMEOUT = 120.0       # s for one whole reply
MAX_CONNECTIONS = 4
MAX_CONVERSATIONS = 16      # per-IP histories kept; the oldest idle one is dropped
DEFAULT_RATE = 10           # SENDs per client per minute (MAX_SEND_PER_MIN, 0 = off)

DEFAULT_PORT = 4242
DEFAULT_PROFILE = "claude"

SYSTEM_PROMPT = """\
You are DSChat, an AI assistant that the user talks to from a Nintendo DS \
Lite. The DS is only the screen and keyboard: you run on a remote AI service, \
reached through a relay on the user's computer. The user reads your replies \
on a tiny 32x24 character text screen and types on a slow on-screen keyboard.

How to write:
- Keep replies short: usually 1 to 4 sentences, at most about 500 characters, \
unless the user explicitly asks for something longer.
- Plain text only. No Markdown: no asterisks, no #, no backticks, no tables, \
no links syntax. No emoji.
- Use only basic ASCII characters. If the user's language uses accented \
letters, write them without the accent.
- For lists, put each item on its own line starting with "- ".
- Answer in the language the user writes in.
- Be direct; skip greetings and filler."""

log = logging.getLogger("relay")


# --------------------------------------------------------------------------
# Log redaction: a safety net so an API key can never reach the log, even if
# a provider echoes it in an error body or a traceback contains it.
# --------------------------------------------------------------------------

_KEY_PATTERNS = re.compile(
    r"(sk-ant-[A-Za-z0-9_-]{8,}|sk-[A-Za-z0-9_-]{16,}|AIza[0-9A-Za-z_-]{20,}"
    r"|Bearer\s+[A-Za-z0-9._-]{16,})")


class RedactingFormatter(logging.Formatter):
    def __init__(self, *a, secrets_=(), **kw):
        super().__init__(*a, **kw)
        self.secrets = sorted({x for x in secrets_ if len(x) >= 8}, key=len, reverse=True)

    def format(self, record: logging.LogRecord) -> str:
        return redact(super().format(record), self.secrets)


def redact(text: str, values=()) -> str:
    for v in values:
        text = text.replace(v, "<redacted>")
    return _KEY_PATTERNS.sub("<redacted>", text)


def secret_values(profiles) -> list[str]:
    """Values of every key variable: the profiles' api_key_env + *_KEY/*_TOKEN."""
    names = {p.api_key_env for p in profiles.values() if p.api_key_env}
    names |= {n for n in os.environ if n.endswith(("_KEY", "_TOKEN", "_SECRET"))}
    return [os.environ[n] for n in names if os.environ.get(n)]


# --------------------------------------------------------------------------
# Text sanitising: everything sent to the DS is printable ASCII plus "\n".
# --------------------------------------------------------------------------

_ASCII_MAP = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'", "′": "'",
    "“": '"', "”": '"', "„": '"', "‟": '"', "″": '"',
    "«": '"', "»": '"', "‹": "'", "›": "'",
    "‐": "-", "‑": "-", "‒": "-", "–": "-", "—": "-",
    "―": "-", "−": "-",
    "…": "...", "•": "*", "·": "*", "‣": "*", "●": "*",
    " ": " ", " ": " ", " ": " ", " ": " ", " ": " ",
    "​": "", "‌": "", "‍": "", "﻿": "",
    "ß": "ss", "æ": "ae", "Æ": "AE", "œ": "oe", "Œ": "OE",
    "ø": "o", "Ø": "O", "ł": "l", "Ł": "L", "đ": "d",
    "Đ": "D", "ð": "d", "þ": "th", "Þ": "Th",
    "€": "EUR", "£": "GBP", "°": " deg", "×": "x",
    "÷": "/", "→": "->", "←": "<-", "≤": "<=", "≥": ">=",
    "≠": "!=", "±": "+/-", "©": "(c)", "®": "(R)",
    "™": "(TM)", "¿": "?", "¡": "!",
    "\t": " ",
}


def sanitize(text: str) -> str:
    """Map arbitrary text to printable ASCII (0x20-0x7E) plus newline."""
    out: list[str] = []
    for ch in text:
        o = ord(ch)
        if 0x20 <= o <= 0x7E or ch == "\n":
            out.append(ch)
            continue
        if ch == "\r":
            continue                      # \r\n -> \n, lone \r dropped
        if ch in _ASCII_MAP:
            out.append(_ASCII_MAP[ch])
            continue
        decomposed = unicodedata.normalize("NFKD", ch)
        base = "".join(c for c in decomposed if not unicodedata.combining(c))
        if base and all(0x20 <= ord(c) <= 0x7E for c in base):
            out.append(base)             # e.g. e-acute -> e, ligatures, fullwidth
        elif unicodedata.combining(ch):
            continue                      # stray combining mark (split delta)
        else:
            out.append("?")
    return "".join(out)


# --------------------------------------------------------------------------
# Framing
# --------------------------------------------------------------------------

class ProtocolError(Exception):
    pass


def encode_frame(ftype: str, payload: str | bytes = b"") -> bytes:
    if isinstance(payload, str):
        payload = sanitize(payload).encode("ascii")
    assert len(ftype) == 4 and len(payload) <= MAX_PAYLOAD
    return f"{ftype} {len(payload):04d}\n".encode("ascii") + payload


async def read_frame(reader: asyncio.StreamReader,
                     idle: float = IDLE_TIMEOUT) -> tuple[str, bytes] | None:
    """Read one frame. Returns None on clean EOF before a frame starts."""
    try:
        first = await asyncio.wait_for(reader.readexactly(1), idle)
    except asyncio.IncompleteReadError:
        return None
    rest = await asyncio.wait_for(reader.readexactly(HEADER_LEN - 1), FRAME_TIMEOUT)
    hdr = first + rest
    ftype, sp, digits, nl = hdr[0:4], hdr[4:5], hdr[5:9], hdr[9:10]
    if not (ftype.isalpha() and ftype.isupper() and sp == b" "
            and digits.isdigit() and nl == b"\n"):
        raise ProtocolError("bad frame header")
    length = int(digits)
    if length > MAX_PAYLOAD:
        raise ProtocolError("frame too long")
    payload = b""
    if length:
        payload = await asyncio.wait_for(reader.readexactly(length), FRAME_TIMEOUT)
    return ftype.decode("ascii"), payload


class FrameReader:
    """Reads frames from one connection, at most one read in flight.

    A reply is streamed while the next frame is read concurrently (to catch
    CNCL). Cancelling a half-done read would lose bytes, so the read task is
    kept and reused by whoever reads next; non-CNCL frames that arrive during
    a reply are queued and handled after it, in order.
    """

    def __init__(self, reader: asyncio.StreamReader):
        self.reader = reader
        self.task: asyncio.Task | None = None
        self.queued: deque = deque()
        self.first = True

    def pending(self) -> asyncio.Task:
        if self.task is None:
            idle = HELLO_TIMEOUT if self.first else IDLE_TIMEOUT
            self.first = False
            self.task = asyncio.create_task(read_frame(self.reader, idle))
        return self.task

    def take(self):
        """Result of the finished read task (may raise ProtocolError etc.)."""
        t, self.task = self.task, None
        return t.result()

    async def next(self):
        if self.queued:
            return self.queued.popleft()
        await asyncio.wait({self.pending()})
        return self.take()

    def close(self) -> None:
        if self.task is not None:
            self.task.cancel()
            self.task = None


# --------------------------------------------------------------------------
# Conversations
# --------------------------------------------------------------------------

@dataclass
class Conversation:
    profile: str
    messages: list = field(default_factory=list)   # adapter-native format
    busy: bool = False
    last_used: float = 0.0
    sends: deque = field(default_factory=deque)     # times of recent SENDs

    @property
    def turns(self) -> int:
        return len(self.messages) // 2


@dataclass
class Config:
    max_tokens: int
    max_msg: int
    max_turns: int
    fake: bool
    rate: int = DEFAULT_RATE


class Relay:
    def __init__(self, cfg: Config, profiles: dict[str, Profile], default: str,
                 voice: VoiceEngine | None = None):
        self.cfg = cfg
        self.voice = voice or VoiceEngine(fake=False)
        self.profiles = profiles
        self.default = default
        self.conversations: dict[str, Conversation] = {}
        self.adapters: dict[str, Adapter] = {}
        self.active = 0
        # Newest connection per client IP. The DS uses one connection at a
        # time; after a Wi-Fi drop or sleep its old connection may linger
        # half-open (its FIN never arrived), so a new one replaces it.
        self.by_ip: dict[str, tuple[asyncio.StreamWriter, asyncio.Task]] = {}

    @property
    def server_info(self) -> str:
        # PONG payload: "DSChat relay <version>" first (v1/v2 clients only
        # display it), then key=value fields for v3+ clients; voice=1 only
        # while speech-to-text is actually available.
        return (f"DSChat relay {PROTO_VERSION} max_msg={self.cfg.max_msg} boot={BOOT_ID}"
                + (" voice=1" if self.voice.available else ""))

    # -- profiles ------------------------------------------------------------

    def ready(self, p: Profile) -> bool:
        return self.cfg.fake or p.has_key

    def adapter(self, name: str) -> Adapter:
        if name not in self.adapters:
            self.adapters[name] = make_adapter(
                self.profiles[name], self.cfg.fake, system_prompt=SYSTEM_PROMPT,
                max_tokens=self.cfg.max_tokens, timeout=REPLY_TIMEOUT)
        return self.adapters[name]

    def profile_list(self, current: str) -> str:
        lines = []
        for name, p in self.profiles.items():
            flag = "*" if name == current else ("+" if self.ready(p) else "!")
            lines.append(f"{flag} {name} {p.model}")
        return "\n".join(lines)

    def conversation(self, ip: str) -> Conversation:
        """This client's conversation; at most MAX_CONVERSATIONS are kept."""
        conv = self.conversations.get(ip)
        if conv is None:
            if len(self.conversations) >= MAX_CONVERSATIONS:
                idle = [(c.last_used, k) for k, c in self.conversations.items()
                        if not c.busy and k not in self.by_ip]
                if idle:
                    old = min(idle)[1]
                    del self.conversations[old]
                    log.info("dropped the conversation of %s (oldest of %d)", old,
                             MAX_CONVERSATIONS)
            conv = self.conversations[ip] = Conversation(profile=self.default)
        conv.last_used = time.monotonic()
        return conv

    def rate_ok(self, conv: Conversation) -> bool:
        if self.cfg.rate <= 0:
            return True
        now = time.monotonic()
        while conv.sends and now - conv.sends[0] > 60:
            conv.sends.popleft()
        if len(conv.sends) >= self.cfg.rate:
            return False
        conv.sends.append(now)
        return True

    # -- connection handling -------------------------------------------------

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        peer = writer.get_extra_info("peername") or ("?", 0)
        ip = peer[0]
        tag = f"{peer[0]}:{peer[1]}"

        async def send(ftype: str, payload: str | bytes = b"") -> None:
            writer.write(encode_frame(ftype, payload))
            await asyncio.wait_for(writer.drain(), WRITE_TIMEOUT)

        if self.active >= MAX_CONNECTIONS:
            log.warning("%s rejected: too many connections", tag)
            try:
                await send("FAIL", "too many connections")
            finally:
                writer.close()
            return

        old = self.by_ip.get(ip)
        me = (writer, asyncio.current_task())
        self.by_ip[ip] = me
        if old is not None:
            log.info("%s replaces the older connection from %s", tag, ip)
            old[0].close()
            # let it finish (it rolls back / clears "busy") before we go on
            await asyncio.wait({old[1]}, timeout=3)

        self.active += 1
        log.info("%s connected (%d active)", tag, self.active)
        conv = self.conversation(ip)
        frames = FrameReader(reader)
        vstream: VoiceStream | None = None
        try:
            while True:
                try:
                    frame = await frames.next()
                except ProtocolError as e:
                    log.warning("%s protocol error: %s", tag, e)
                    await send("FAIL", f"protocol error: {e}")
                    break
                if frame is None:
                    break
                ftype, payload = frame

                if ftype == "PING":
                    await send("PONG", self.server_info)
                elif ftype == "LIVE":
                    await send("LIVE")
                elif ftype == "CNCL":
                    # No reply in progress (it may have just ended): ignore,
                    # without an answer, so a CNCL racing a DONE is harmless.
                    # During a voice stream (before VEND) it drops the stream.
                    if vstream is not None:
                        log.info("%s voice input cancelled", tag)
                        vstream = None
                    else:
                        log.debug("%s CNCL with no reply in progress, ignored", tag)
                elif ftype == "VBEG":
                    # no reply; problems are reported in the answer to VEND
                    vstream = VoiceStream.begin(payload)
                    if not self.voice.available:
                        vstream.error = "voice input not available on this relay"
                elif ftype == "VDAT":
                    if vstream is not None:
                        vstream.add(payload, asyncio.get_running_loop().time())
                elif ftype == "VEND":
                    vs, vstream = vstream, None
                    await self.on_vend(tag, conv, vs, send, frames)
                elif ftype == "RSET":
                    if conv.busy:
                        await send("FAIL", "busy")
                    else:
                        conv.messages.clear()
                        log.info("%s conversation reset", tag)
                        await send("OKAY")
                elif ftype == "SEND":
                    await self.on_send(tag, conv, payload, send, frames)
                elif ftype == "PLST":
                    await send("INFO", self.profile_list(conv.profile))
                elif ftype == "PGET":
                    await send("INFO", f"{conv.profile} {self.profiles[conv.profile].model}")
                elif ftype == "PSET":
                    await self.on_pset(tag, conv, payload, send)
                else:
                    await send("FAIL", "unknown frame type")
        except asyncio.TimeoutError:
            log.info("%s timed out", tag)
        except (ConnectionError, asyncio.IncompleteReadError, OSError) as e:
            log.info("%s connection lost: %s", tag, e.__class__.__name__)
        except ProtocolError as e:      # raised while reading during a reply
            log.warning("%s protocol error: %s", tag, e)
            try:
                await send("FAIL", f"protocol error: {e}")
            except Exception:
                pass
        finally:
            frames.close()
            if self.by_ip.get(ip) == me:
                del self.by_ip[ip]
            self.active -= 1
            log.info("%s disconnected (%d active)", tag, self.active)
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass

    async def on_vend(self, tag, conv: Conversation, vs: VoiceStream | None, send,
                      frames: FrameReader) -> None:
        """Transcribe a finished voice stream; answer VTXT or FAIL. Like a
        SEND reply, LIVE is answered and CNCL cancels while it runs."""
        if vs is None:
            await send("FAIL", "no voice input in progress")
            return
        if vs.error:
            await send("FAIL", vs.error)
            return
        if not vs.pcm:
            await send("FAIL", "no audio received")
            return
        if not self.rate_ok(conv):
            log.warning("%s rate limit: more than %d messages a minute", tag, self.cfg.rate)
            await send("FAIL", "too many messages, wait a minute")
            return
        log.info("%s voice: %.1f s of %s audio, %d B, received over %.1f s", tag,
                 vs.audio_ms / 1000, vs.lang, vs.bytes, vs.rx_ms / 1000)
        task = asyncio.create_task(self.voice.transcribe(vs.pcm, vs.lang))
        try:
            while not task.done():
                reading = frames.pending()
                await asyncio.wait({task, reading}, return_when=asyncio.FIRST_COMPLETED)
                if not reading.done():
                    continue
                frame = frames.take()
                if frame is None:
                    raise ConnectionError("client closed during transcription")
                if frame[0] == "LIVE":
                    await send("LIVE")
                    continue
                if frame[0] == "CNCL":
                    log.info("%s voice input cancelled during transcription", tag)
                    await send("FAIL", "cancelled")
                    return          # the worker finishes the job; its result is dropped
                frames.queued.append(frame)
            try:
                text, secs = task.result()
            except RuntimeError as e:
                log.warning("%s voice: %s", tag, e)
                await send("FAIL", str(e))
                return
            text = sanitize(text).replace("\n", " ").strip()
            log.info("%s voice: %d chars in %.1f s", tag, len(text), secs)
            log.debug("%s voice text: %r", tag, text)
            head = (f"ms={int(secs * 1000)} audio={vs.audio_ms} bytes={vs.bytes} "
                    f"rx={vs.rx_ms}\n")
            await send("VTXT", head + text[:MAX_PAYLOAD - len(head)])
        finally:
            if not task.done():
                task.add_done_callback(lambda t: t.exception() if not t.cancelled() else None)

    async def on_pset(self, tag, conv: Conversation, payload: bytes, send) -> None:
        name = payload.decode("ascii", "replace").strip()
        p = self.profiles.get(name)
        if p is None:
            await send("FAIL", "unknown profile")
        elif not self.ready(p):
            await send("FAIL", f"profile {name} has no API key ({p.api_key_env})")
        elif conv.busy:
            await send("FAIL", "busy")
        else:
            conv.profile = name
            conv.messages.clear()          # histories are provider-specific
            log.info("%s profile -> %s (%s), conversation reset", tag, name, p.model)
            await send("OKAY", f"{name} {p.model}")

    async def on_send(self, tag, conv: Conversation, payload: bytes, send,
                      frames: FrameReader) -> None:
        text = sanitize(payload.decode("latin-1")).strip()
        if len(payload) > self.cfg.max_msg:
            await send("FAIL", f"message too long (max {self.cfg.max_msg})")
            return
        if not text:
            await send("FAIL", "empty message")
            return
        if conv.busy:   # same IP on a second connection while a reply streams
            await send("FAIL", "busy")
            return
        if conv.turns >= self.cfg.max_turns:
            await send("FAIL", "history full, send RSET")
            return
        if not self.rate_ok(conv):
            log.warning("%s rate limit: more than %d messages a minute", tag, self.cfg.rate)
            await send("FAIL", "too many messages, wait a minute")
            return
        profile = self.profiles[conv.profile]
        if not self.ready(profile):
            await send("FAIL", f"profile {profile.name} has no API key")
            return

        log.info("%s SEND %d bytes -> %s", tag, len(text), profile.name)
        log.debug("%s user: %r", tag, text)
        conv.busy = True
        loop = asyncio.get_running_loop()
        started = loop.time()
        result: dict = {}

        async def stream() -> None:
            async with asyncio.timeout(REPLY_TIMEOUT):
                pending_ws = True   # strip leading whitespace of the reply
                async for piece in self.adapter(profile.name).stream(
                        conv.messages, text, result):
                    piece = sanitize(piece)
                    if pending_ws:
                        piece = piece.lstrip()
                        if not piece:
                            continue
                        pending_ws = False
                    for i in range(0, len(piece), MAX_TEXT_CHUNK):
                        await send("TEXT", piece[i:i + MAX_TEXT_CHUNK])

        task = asyncio.create_task(stream())
        cancelled = False
        try:
            # Stream the reply while watching the connection for CNCL.
            while not task.done():
                reading = frames.pending()
                await asyncio.wait({task, reading}, return_when=asyncio.FIRST_COMPLETED)
                if not reading.done():
                    continue
                frame = frames.take()           # may raise ProtocolError
                if frame is None:               # client closed the connection
                    raise ConnectionError("client closed during reply")
                if frame[0] == "LIVE":          # liveness probe: answer right away
                    await send("LIVE")
                    continue
                if frame[0] == "CNCL":
                    log.info("%s reply cancelled by client", tag)
                    cancelled = True
                    task.cancel()
                    break
                frames.queued.append(frame)     # handled after this reply
            try:
                await task
            except asyncio.CancelledError:
                if not cancelled:
                    raise
                await send("FAIL", "cancelled")
                return
            # Commit the exchange only after a complete, successful reply.
            conv.messages.append({"role": "user", "content": text})
            conv.messages.append(result["assistant"])
            stop = result.get("stop") or "end_turn"
            if stop == "max_tokens":
                await send("TEXT", " [...]")
            await send("DONE", stop)
            log.info("%s reply done (%s) in %.1fs", tag, stop, loop.time() - started)
        except ReplyFailed as e:
            log.warning("%s reply failed: %s", tag, e)
            await send("FAIL", str(e))
        except TimeoutError:
            log.warning("%s reply timed out", tag)
            await send("FAIL", "reply timed out")
        finally:
            if not task.done():
                task.cancel()
                try:
                    await task
                except BaseException:
                    pass
            conv.busy = False


# --------------------------------------------------------------------------
# Startup
# --------------------------------------------------------------------------

def read_ap_conf() -> dict[str, str]:
    conf: dict[str, str] = {}
    if AP_CONF.exists():
        for line in AP_CONF.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                conf[k.strip()] = v.strip().strip('"').strip("'")
    return conf


def main() -> int:
    ap = argparse.ArgumentParser(description="DSChat relay")
    ap.add_argument("--bind", default="ap",
                    help="'ap' (AP_ADDR from net/ap.conf, default), 'local' "
                         "(127.0.0.1, for melonDS/PC tests) or an IP address")
    ap.add_argument("--port", type=int, help="override PORT from .env")
    ap.add_argument("--profile", help="override ACTIVE_PROFILE from .env")
    ap.add_argument("--providers", type=Path, default=PROVIDERS_TOML,
                    help="profiles file (default relay/providers.toml)")
    ap.add_argument("--fake", action="store_true",
                    help="do not call any API; stream canned replies")
    ap.add_argument("-v", "--verbose", "--debug", action="store_true",
                    help="debug logging: adds the user's message text (never keys)")
    ap.add_argument("--no-voice", action="store_true",
                    help="do not offer voice input (same as VOICE=off in .env)")
    ap.add_argument("--check-config", action="store_true",
                    help="print the profiles and which have a key, then exit "
                         "(status 2 if the default profile has none)")
    args = ap.parse_args()

    handler = logging.StreamHandler()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        handlers=[handler])
    # The SDKs' own debug logs dump whole requests (conversation text):
    # never enabled, even with --debug.
    for noisy in ("httpx", "httpx2", "anthropic", "openai", "httpcore"):
        logging.getLogger(noisy).setLevel(logging.WARNING)

    env_file = HERE / ".env"
    if env_file.exists():
        mode = env_file.stat().st_mode
        if mode & (stat.S_IRWXG | stat.S_IRWXO):
            log.warning("relay/.env is readable by other users (mode %o): run "
                        "chmod 600 relay/.env", stat.S_IMODE(mode))
    load_dotenv(env_file)
    apconf = read_ap_conf()

    try:
        profiles = load_profiles(args.providers)
    except ConfigError as e:
        log.error("%s", e)
        return 2
    handler.setFormatter(RedactingFormatter("%(asctime)s %(levelname)s %(message)s",
                                            datefmt="%H:%M:%S",
                                            secrets_=secret_values(profiles)))
    for old in ("MODEL", "EFFORT"):
        if os.environ.get(old):
            log.warning("%s in .env is no longer used: models and options live "
                        "in providers.toml", old)

    default = args.profile or os.environ.get("ACTIVE_PROFILE") or DEFAULT_PROFILE
    if default not in profiles:
        log.error("profile '%s' not in %s (have: %s)", default, args.providers.name,
                  ", ".join(profiles))
        return 2

    port = args.port or int(os.environ.get("PORT") or apconf.get("RELAY_PORT")
                            or DEFAULT_PORT)
    if apconf.get("RELAY_PORT") and int(apconf["RELAY_PORT"]) != port:
        log.warning("PORT %d differs from RELAY_PORT %s in net/ap.conf: the AP "
                    "firewall will block it", port, apconf["RELAY_PORT"])

    if args.bind == "ap":
        host = apconf.get("AP_ADDR")
        if not host:
            log.error("AP_ADDR not found in %s", AP_CONF)
            return 2
    elif args.bind == "local":
        host = "127.0.0.1"
    else:
        host = args.bind
        try:
            addr = ipaddress.ip_address(host)
        except ValueError:
            log.error("--bind must be 'ap', 'local' or an IP address")
            return 2
        if addr.is_unspecified:
            log.warning("binding %s exposes the relay (and your API keys' quota) on "
                        "EVERY network of this laptop, not just the DS's AP", host)

    cfg = Config(
        max_tokens=int(os.environ.get("MAX_TOKENS") or 2048),
        max_msg=min(int(os.environ.get("MAX_MSG") or 1024), MAX_PAYLOAD),
        max_turns=int(os.environ.get("MAX_TURNS") or 40),
        fake=args.fake,
        # (fake mode costs nothing: no limit unless set explicitly)
        rate=int(os.environ.get("MAX_SEND_PER_MIN") or (0 if args.fake else DEFAULT_RATE)),
    )
    voice_mode = "off" if args.no_voice else (os.environ.get("VOICE") or "auto").lower()
    voice = VoiceEngine(model=os.environ.get("VOICE_MODEL") or "small",
                        hints=(os.environ.get("VOICE_HINTS") or "").strip()[:400],
                        fake=args.fake and voice_mode != "off")
    if voice_mode == "off":
        voice.reason = "VOICE=off" if not args.no_voice else "--no-voice"
    elif voice.model not in ("small", "base"):
        voice.reason = "VOICE_MODEL must be small or base"
        voice_mode = "off"
    relay = Relay(cfg, profiles, default, voice)

    if args.check_config:
        for name, p in profiles.items():
            print(f"{name:<10} {p.model:<26} "
                  f"{'no key needed' if not p.needs_key else 'key ok' if p.has_key else 'NO KEY (' + p.api_key_env + ')'}"
                  f"{'   <- default' if name == default else ''}")
        from voice_engine import MODELS, VOICE_PYTHON
        if voice_mode == "off":
            print(f"voice: off ({voice.reason})")
        elif not VOICE_PYTHON.exists():
            print("voice: off (faster-whisper not installed: relay/voice/setup.sh)")
        elif not (MODELS / f"faster-whisper-{voice.model}" / "model.bin").exists():
            print(f"voice: off (model '{voice.model}' not downloaded: relay/voice/setup.sh)")
        else:
            print(f"voice: on ({voice.model}, en/it) if the model loads")
        return 0 if relay.ready(profiles[default]) else 2

    for name, p in profiles.items():
        state = "ready" if relay.ready(p) else f"no key ({p.api_key_env} not set)"
        log.info("profile %-10s %-9s %-24s %s%s", name, p.adapter, p.model, state,
                 "  <- default" if name == default else "")
    if not relay.ready(profiles[default]):
        log.error("default profile '%s' has no API key: set %s in relay/.env, "
                  "pick another ACTIVE_PROFILE, or run with --fake",
                  default, profiles[default].api_key_env)
        return 2

    async def serve():
        if voice_mode != "off":
            await voice.start()
        log.info("voice: %s", voice.describe())
        try:
            server = await asyncio.start_server(relay.handle, host, port,
                                                reuse_address=True)
        except OSError as e:
            if e.errno == errno.EADDRNOTAVAIL:
                log.error("%s is not an address of this machine. Is the AP up "
                          "(sudo net/ap-up.sh)? For local tests use --bind local.",
                          host)
            else:
                log.error("cannot listen on %s:%d: %s", host, port, e)
            return 2
        log.info("listening on %s:%d  default profile=%s%s", host, port, default,
                 "  [FAKE MODE]" if cfg.fake else "")
        try:
            async with server:
                await server.serve_forever()
        finally:
            await voice.close()
        return 0

    try:
        return asyncio.run(serve())
    except KeyboardInterrupt:
        log.info("bye")
        return 0


if __name__ == "__main__":
    sys.exit(main())
