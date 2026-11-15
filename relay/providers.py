"""Provider profiles and adapters for the DSChat relay.

Every adapter exposes the same coroutine generator:

    async for text in adapter.stream(history, user_text, result): ...

* `history` is the adapter-native message list kept by the relay (it is reset
  whenever a client switches profile, so formats never mix).
* It yields ONLY final-answer text (reasoning/thinking output is dropped).
* On success it sets result["stop"] ("end_turn" | "max_tokens") and
  result["assistant"] (the history entry to append after the user turn).
* On failure it raises ReplyFailed with a short ASCII message for the DS;
  the relay then leaves the history untouched (rollback).
"""

from __future__ import annotations

import asyncio
import logging
import os
import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, AsyncIterator

log = logging.getLogger("relay")

ADAPTERS = ("anthropic", "openai")
NAME_RE = re.compile(r"^[a-z0-9_-]{1,16}$")


class ReplyFailed(Exception):
    """Raised with a short ASCII message to send to the DS as FAIL."""


class ConfigError(Exception):
    pass


# --------------------------------------------------------------------------
# Profiles
# --------------------------------------------------------------------------

@dataclass
class Profile:
    name: str
    adapter: str
    model: str
    label: str = ""
    base_url: str | None = None
    api_key_env: str | None = None          # NAME of the env var, never the key
    max_tokens: int | None = None           # overrides MAX_TOKENS from .env
    max_tokens_param: str = "max_tokens"    # openai: or "max_completion_tokens"
    system_role: str = "system"             # openai: or "developer"
    params: dict[str, Any] = field(default_factory=dict)  # extra request body

    def api_key(self) -> str | None:
        return os.environ.get(self.api_key_env) if self.api_key_env else None

    @property
    def needs_key(self) -> bool:
        return self.api_key_env is not None

    @property
    def has_key(self) -> bool:
        return not self.needs_key or bool(self.api_key())


def load_profiles(path: Path) -> dict[str, Profile]:
    try:
        data = tomllib.loads(path.read_text())
    except (OSError, tomllib.TOMLDecodeError) as e:
        raise ConfigError(f"{path}: {e}") from e
    raw = data.get("profiles")
    if not isinstance(raw, dict) or not raw:
        raise ConfigError(f"{path}: no [profiles.<name>] tables")
    known = set(Profile.__dataclass_fields__) - {"name"}
    profiles: dict[str, Profile] = {}
    for name, p in raw.items():
        where = f"{path.name}: profile '{name}'"
        if not NAME_RE.match(name):
            raise ConfigError(f"{where}: name must match {NAME_RE.pattern}")
        unknown = set(p) - known
        if unknown:
            raise ConfigError(f"{where}: unknown keys {sorted(unknown)}")
        if p.get("adapter") not in ADAPTERS:
            raise ConfigError(f"{where}: adapter must be one of {ADAPTERS}")
        if not p.get("model") or " " in p["model"]:
            raise ConfigError(f"{where}: 'model' missing or contains spaces")
        if p["adapter"] == "openai" and not p.get("base_url"):
            raise ConfigError(f"{where}: openai adapter needs base_url")
        if p.get("max_tokens_param", "max_tokens") not in ("max_tokens", "max_completion_tokens"):
            raise ConfigError(f"{where}: bad max_tokens_param")
        if p.get("system_role", "system") not in ("system", "developer"):
            raise ConfigError(f"{where}: bad system_role")
        profiles[name] = Profile(name=name, **p)
    return profiles


# --------------------------------------------------------------------------
# Reasoning filter for OpenAI-compatible text streams
# --------------------------------------------------------------------------

class ThinkFilter:
    """Removes <think>...</think> style blocks from a streamed text.

    Some providers/models put their reasoning inline in `content` (Groq
    reasoning_format=raw, DeepSeek-R1 distills on Ollama, Gemini with
    include_thoughts, ...). Tags can be split across stream chunks.
    An unterminated block at the end of the stream is dropped.
    """

    TAGS = ("think", "thinking", "thought", "reasoning")

    def __init__(self) -> None:
        self.buf = ""
        self.closing: str | None = None   # set while inside a block
        self._opens = [f"<{t}>" for t in self.TAGS]

    def feed(self, chunk: str) -> str:
        self.buf += chunk
        out: list[str] = []
        while self.buf:
            if self.closing:
                i = self.buf.lower().find(self.closing)
                if i < 0:
                    # keep a possible partial closing tag, drop the rest
                    keep = len(self.closing) - 1
                    self.buf = self.buf[-keep:] if keep else ""
                    break
                self.buf = self.buf[i + len(self.closing):]
                self.closing = None
                continue
            i = self.buf.find("<")
            if i < 0:
                out.append(self.buf)
                self.buf = ""
                break
            out.append(self.buf[:i])
            rest = self.buf[i:]
            lower = rest.lower()
            opened = next((t for t in self._opens if lower.startswith(t)), None)
            if opened:
                self.closing = "</" + opened[1:]
                self.buf = rest[len(opened):]
            elif any(t.startswith(lower) for t in self._opens):
                self.buf = rest            # could still become a tag
                break
            else:
                out.append("<")
                self.buf = rest[1:]
        return "".join(out)

    def flush(self) -> str:
        rest, self.buf = ("" if self.closing else self.buf), ""
        return rest


def answer_text(content: Any) -> str:
    """Final-answer text from an OpenAI-style delta.content.

    Usually a string; Mistral reasoning models stream a list of typed chunks
    ({"type": "thinking", ...} / {"type": "text", "text": ...}) instead.
    """
    if content is None:
        return ""
    if isinstance(content, str):
        return content
    parts = []
    if isinstance(content, list):
        for c in content:
            ctype = c.get("type") if isinstance(c, dict) else getattr(c, "type", None)
            text = c.get("text") if isinstance(c, dict) else getattr(c, "text", None)
            if ctype == "text" and isinstance(text, str):
                parts.append(text)
    return "".join(parts)


# --------------------------------------------------------------------------
# Error mapping (both SDKs are Stainless-generated: same exception names)
# --------------------------------------------------------------------------

def map_error(sdk, e: Exception, profile: Profile) -> ReplyFailed:
    # Some providers (Gemini) wrap errors in a JSON list, so the SDK message
    # is just "error code: 400": look at the raw body too.
    msg = f"{getattr(e, 'message', '') or e} {getattr(e, 'body', '') or ''}".lower()
    rid = "?"
    try:
        rid = e.response.headers.get("request-id") or e.response.headers.get("x-request-id") or "?"
    except Exception:
        pass
    if isinstance(e, sdk.AuthenticationError):
        return ReplyFailed("relay: invalid API key")
    if isinstance(e, sdk.PermissionDeniedError):
        return ReplyFailed("relay: API key not allowed")
    if isinstance(e, sdk.NotFoundError):
        return ReplyFailed(f"relay: unknown model {profile.model}")
    if isinstance(e, sdk.RateLimitError):
        return ReplyFailed("rate limited, try again later")
    if isinstance(e, sdk.BadRequestError):
        # Some providers report a bad key / unknown model as HTTP 400.
        if "api key" in msg or "api_key" in msg:
            return ReplyFailed("relay: invalid API key")
        if "model" in msg and any(w in msg for w in
                                  ("not found", "not exist", "invalid", "unknown",
                                   "not supported", "unsupported", "no such")):
            return ReplyFailed(f"relay: unknown model {profile.model}")
        log.error("[%s] bad request: %s (request-id %s)", profile.name, msg[:300], rid)
        return ReplyFailed("relay: bad request (see relay log)")
    if isinstance(e, sdk.APIStatusError):
        log.error("[%s] API error %s: %s (request-id %s)", profile.name,
                  e.status_code, msg[:300], rid)
        return ReplyFailed(f"AI API error {e.status_code}, retry")
    if isinstance(e, sdk.APITimeoutError):
        return ReplyFailed("AI API timed out")
    if isinstance(e, sdk.APIConnectionError):
        return ReplyFailed("relay cannot reach the AI API")
    log.exception("[%s] unexpected error", profile.name)
    return ReplyFailed("relay: internal error")


# --------------------------------------------------------------------------
# Adapters
# --------------------------------------------------------------------------

class Adapter:
    def __init__(self, profile: Profile, system_prompt: str, max_tokens: int,
                 timeout: float):
        self.profile = profile
        self.system_prompt = system_prompt
        self.max_tokens = profile.max_tokens or max_tokens
        self.timeout = timeout

    def stream(self, history: list, text: str, result: dict) -> AsyncIterator[str]:
        raise NotImplementedError


class AnthropicAdapter(Adapter):
    """Native Anthropic SDK (Messages API, streaming)."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        import anthropic
        self.sdk = anthropic
        self.client = anthropic.AsyncAnthropic(
            api_key=self.profile.api_key(),
            base_url=self.profile.base_url,      # None = default endpoint
            timeout=anthropic.Timeout(self.timeout, connect=10.0),
            max_retries=2,
        )

    async def stream(self, history, text, result):
        p = self.profile
        try:
            async with self.client.messages.stream(
                model=p.model,
                max_tokens=self.max_tokens,
                system=self.system_prompt,
                messages=history + [{"role": "user", "content": text}],
                extra_body=p.params or None,
            ) as stream:
                async for delta in stream.text_stream:   # text blocks only
                    yield delta
                final = await stream.get_final_message()
        except self.sdk.APIError as e:
            raise map_error(self.sdk, e, p) from e

        log.info("[%s] usage: in=%s out=%s stop=%s", p.name, final.usage.input_tokens,
                 final.usage.output_tokens, final.stop_reason)
        if final.stop_reason == "refusal":
            raise ReplyFailed("the AI declined to answer that")
        if not any(b.type == "text" and b.text for b in final.content):
            raise ReplyFailed("empty reply, try again")
        # Full content (thinking blocks included) is replayed unchanged next turn.
        result["assistant"] = {"role": "assistant", "content": final.content}
        result["stop"] = "max_tokens" if final.stop_reason == "max_tokens" else "end_turn"


class OpenAICompatAdapter(Adapter):
    """Any OpenAI-compatible Chat Completions endpoint (streaming)."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        import openai
        self.sdk = openai
        self.client = openai.AsyncOpenAI(
            # Local servers (Ollama) need no key, but the SDK requires a string.
            api_key=self.profile.api_key() or "no-key",
            base_url=self.profile.base_url,
            timeout=openai.Timeout(self.timeout, connect=10.0),
            max_retries=2,
        )

    async def stream(self, history, text, result):
        p = self.profile
        messages = ([{"role": p.system_role, "content": self.system_prompt}]
                    + history + [{"role": "user", "content": text}])
        body = dict(p.params)
        body[p.max_tokens_param] = self.max_tokens
        think = ThinkFilter()
        parts: list[str] = []
        finish = None
        try:
            stream = await self.client.chat.completions.create(
                model=p.model, messages=messages, stream=True, extra_body=body)
            async with stream:
                async for chunk in stream:
                    if not chunk.choices:
                        continue                       # e.g. usage-only chunk
                    choice = chunk.choices[0]
                    # Only delta.content is answer text; reasoning arrives in
                    # reasoning_content / reasoning / reasoning_details (ignored).
                    piece = think.feed(answer_text(choice.delta.content))
                    if piece:
                        parts.append(piece)
                        yield piece
                    if choice.finish_reason:
                        finish = choice.finish_reason
            tail = think.flush()
            if tail:
                parts.append(tail)
                yield tail
        except self.sdk.APIError as e:
            raise map_error(self.sdk, e, p) from e

        log.info("[%s] finish=%s chars=%d", p.name, finish, sum(map(len, parts)))
        if finish == "content_filter":
            raise ReplyFailed("the AI declined to answer that")
        answer = "".join(parts)
        if not answer.strip():
            raise ReplyFailed("empty reply, try again")
        result["assistant"] = {"role": "assistant", "content": answer}
        result["stop"] = "max_tokens" if finish == "length" else "end_turn"


class FakeAdapter(Adapter):
    """--fake: no network. Exercises the same filtering path as real adapters.

    Triggers in the user message: "fail..." -> ReplyFailed; "slow" -> 20 s
    before the first text; "long" -> about 3 KB of text.
    """

    SLOW_DELAY = 20.0

    async def stream(self, history, text, result):
        p = self.profile
        low = text.lower()
        if low.startswith("fail"):
            raise ReplyFailed("fake failure requested")
        turn = len(history) // 2 + 1
        if "slow" in low:                       # like a slow provider (Gemini ~20 s)
            await asyncio.sleep(self.SLOW_DELAY)
        # The <think> block must never reach the DS (checked by the selftest).
        raw = (f"<think>SECRET reasoning of {p.name}</think>"
               f"Fake reply #{turn} from {p.name} ({p.model}) to: “{text}”. "
               "Accents è à ù, dash — and ellipsis… "
               "are sanitised.\nSecond line.")
        if "long" in low:                       # several screens of text
            raw += "\n\n" + " ".join(
                f"Sentence {i} of a long fake reply, written to test word wrap "
                f"and scrolling on the DS." for i in range(1, 41))
            raw += ("\nA word longer than a screen line: "
                    "Pneumonoultramicroscopicsilicovolcanoconiosis-and-more. The end.")
        think = ThinkFilter()
        parts = []
        for i in range(0, len(raw), 7):        # split tags across chunks too
            await asyncio.sleep(0.01)
            piece = think.feed(raw[i:i + 7])
            if piece:
                parts.append(piece)
                yield piece
        tail = think.flush()
        if tail:
            parts.append(tail)
            yield tail
        result["assistant"] = {"role": "assistant", "content": "".join(parts)}
        result["stop"] = "end_turn"


def make_adapter(profile: Profile, fake: bool, **kw) -> Adapter:
    if fake:
        return FakeAdapter(profile, **kw)
    if profile.adapter == "anthropic":
        return AnthropicAdapter(profile, **kw)
    return OpenAICompatAdapter(profile, **kw)
