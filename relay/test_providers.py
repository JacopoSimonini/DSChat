"""Offline tests for the provider layer. Run: .venv/bin/python -m unittest -v

The adapter tests point the real openai / anthropic SDKs at a local mock HTTP
server, so streaming parsing and error mapping are exercised without any
network access or API keys.
"""

import asyncio
import json
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from providers import (AnthropicAdapter, OpenAICompatAdapter, Profile, ReplyFailed,
                       ThinkFilter, answer_text, load_profiles)

HERE = Path(__file__).resolve().parent


class ThinkFilterTest(unittest.TestCase):
    def run_chunks(self, chunks):
        f = ThinkFilter()
        return "".join(f.feed(c) for c in chunks) + f.flush()

    def test_plain(self):
        self.assertEqual(self.run_chunks(["a < b", " and c>d"]), "a < b and c>d")

    def test_block_split_everywhere(self):
        text = "<think>secret\nstuff</think>Answer <b>ok</b>"
        for size in range(1, len(text) + 1):
            chunks = [text[i:i + size] for i in range(0, len(text), size)]
            self.assertEqual(self.run_chunks(chunks), "Answer <b>ok</b>", size)

    def test_other_tags_and_case(self):
        self.assertEqual(self.run_chunks(["<Thought>x</Thought>y<thinking>z</thinking>!"]), "y!")

    def test_unterminated_block_dropped(self):
        self.assertEqual(self.run_chunks(["hi <think>never closed"]), "hi ")

    def test_partial_open_tag_at_end_kept(self):
        self.assertEqual(self.run_chunks(["x <thi"]), "x <thi")


class AnswerTextTest(unittest.TestCase):
    def test_variants(self):
        self.assertEqual(answer_text(None), "")
        self.assertEqual(answer_text("hi"), "hi")
        mistral = [{"type": "thinking", "thinking": [{"type": "text", "text": "no"}]},
                   {"type": "text", "text": "yes"}]
        self.assertEqual(answer_text(mistral), "yes")


class ProfilesFileTest(unittest.TestCase):
    def test_shipped_profiles_load(self):
        profiles = load_profiles(HERE / "providers.toml")
        for name in ("claude", "gemini", "qwen", "openai", "ollama"):
            self.assertIn(name, profiles)
        self.assertEqual(profiles["claude"].adapter, "anthropic")
        self.assertFalse(profiles["ollama"].needs_key)
        for p in profiles.values():
            self.assertNotIn("key", json.dumps(p.params).lower())   # no secrets in params


# --------------------------------------------------------------------------
# Mock provider server
# --------------------------------------------------------------------------

def sse(events):
    return "".join(f"data: {json.dumps(e)}\n\n" for e in events) + "data: [DONE]\n\n"


def chunk(delta, finish=None):
    return {"id": "x", "object": "chat.completion.chunk", "created": 0, "model": "m",
            "choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}


OPENAI_STREAMS = {
    # DeepSeek / Qwen style: reasoning_content field, then content
    "reasoning-field": [chunk({"role": "assistant", "reasoning_content": "secret"}),
                        chunk({"reasoning_content": " more"}),
                        chunk({"content": "Hel"}), chunk({"content": "lo"}),
                        chunk({}, "stop")],
    # OpenRouter style reasoning field
    "reasoning-or": [chunk({"reasoning": "secret", "reasoning_details": [{"type": "reasoning.text", "text": "secret"}]}),
                     chunk({"content": "Hello"}), chunk({}, "stop")],
    # Groq raw / Ollama R1-style inline tags split across chunks
    "inline-think": [chunk({"content": "<thi"}), chunk({"content": "nk>secret</th"}),
                     chunk({"content": "ink>\n\nHello"}), chunk({}, "stop")],
    # Mistral reasoning: typed chunk list, then plain string
    "mistral": [chunk({"content": [{"type": "thinking", "thinking": [{"type": "text", "text": "secret"}]}]}),
                chunk({"content": "Hello"}), chunk({}, "stop")],
    "length": [chunk({"content": "Hel"}), chunk({}, "length")],
    "filtered": [chunk({"content": "x"}), chunk({}, "content_filter")],
    "only-thinking": [chunk({"content": "<think>never ends"}), chunk({}, "length")],
}
OPENAI_ERRORS = {
    "e401": (401, {"error": {"message": "Incorrect API key provided", "code": "invalid_api_key"}}),
    "e429": (429, {"error": {"message": "Rate limit reached"}}),
    "e404": (404, {"error": {"message": "The model does not exist"}}),
    "e400key": (400, [{"error": {"code": 400, "message": "API key not valid. Please pass a valid API key.", "status": "INVALID_ARGUMENT"}}]),
    "e400model": (400, {"error": {"message": "Model not found: foo"}}),
    "e400other": (400, {"error": {"message": "temperature out of range"}}),
    "e500": (500, {"error": {"message": "boom"}}),
}
SEEN = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        SEEN.append((self.path, req, dict(self.headers)))
        model = req.get("model")
        if self.path.endswith("/chat/completions"):
            if model in OPENAI_ERRORS:
                code, body = OPENAI_ERRORS[model]
                return self._send(code, body)
            return self._send(200, sse(OPENAI_STREAMS[model]), "text/event-stream")
        if self.path.endswith("/v1/messages"):
            if model == "e401":
                return self._send(401, {"type": "error", "error": {"type": "authentication_error", "message": "invalid x-api-key"}})
            events = [
                ("message_start", {"type": "message_start", "message": {"id": "m", "type": "message", "role": "assistant", "model": model, "content": [], "stop_reason": None, "stop_sequence": None, "usage": {"input_tokens": 5, "output_tokens": 0}}}),
                ("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": "", "signature": ""}}),
                ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "signature_delta", "signature": "sig"}}),
                ("content_block_stop", {"type": "content_block_stop", "index": 0}),
                ("content_block_start", {"type": "content_block_start", "index": 1, "content_block": {"type": "text", "text": ""}}),
                ("content_block_delta", {"type": "content_block_delta", "index": 1, "delta": {"type": "text_delta", "text": "Hel"}}),
                ("content_block_delta", {"type": "content_block_delta", "index": 1, "delta": {"type": "text_delta", "text": "lo"}}),
                ("content_block_stop", {"type": "content_block_stop", "index": 1}),
                ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "refusal" if model == "refuse" else "end_turn", "stop_sequence": None}, "usage": {"output_tokens": 3}}),
                ("message_stop", {"type": "message_stop"}),
            ]
            body = "".join(f"event: {n}\ndata: {json.dumps(d)}\n\n" for n, d in events)
            return self._send(200, body, "text/event-stream")
        self._send(404, {"error": "no route"})


class AdapterTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.url = f"http://127.0.0.1:{cls.server.server_address[1]}"
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def openai_adapter(self, model, **kw):
        p = Profile(name="t", adapter="openai", model=model, base_url=self.url + "/v1",
                    api_key_env="DSCHAT_TEST_KEY", **kw)
        a = OpenAICompatAdapter(p, system_prompt="SYS", max_tokens=123, timeout=10)
        a.client = a.client.with_options(max_retries=0)
        return a

    def run_adapter(self, adapter, history=None):
        async def go():
            result, parts = {}, []
            async for piece in adapter.stream(history or [], "hi", result):
                parts.append(piece)
            return "".join(parts), result
        return asyncio.run(go())

    def test_reasoning_never_reaches_output(self):
        for model in ("reasoning-field", "reasoning-or", "inline-think", "mistral"):
            text, result = self.run_adapter(self.openai_adapter(model))
            self.assertEqual(text.strip(), "Hello", model)
            self.assertEqual(result["assistant"], {"role": "assistant", "content": text}, model)
            self.assertEqual(result["stop"], "end_turn")

    def test_request_shape(self):
        SEEN.clear()
        a = self.openai_adapter("reasoning-field", max_tokens_param="max_completion_tokens",
                                system_role="developer", params={"reasoning_effort": "none"})
        hist = [{"role": "user", "content": "a"}, {"role": "assistant", "content": "b"}]
        self.run_adapter(a, hist)
        _, req, _ = SEEN[-1]
        self.assertEqual(req["messages"][0], {"role": "developer", "content": "SYS"})
        self.assertEqual(req["messages"][1:], hist + [{"role": "user", "content": "hi"}])
        self.assertEqual(req["max_completion_tokens"], 123)
        self.assertNotIn("max_tokens", req)
        self.assertEqual(req["reasoning_effort"], "none")
        self.assertTrue(req["stream"])

    def test_length_and_filters(self):
        _, result = self.run_adapter(self.openai_adapter("length"))
        self.assertEqual(result["stop"], "max_tokens")
        for model, msg in (("filtered", "declined"), ("only-thinking", "empty reply")):
            with self.assertRaises(ReplyFailed) as cm:
                self.run_adapter(self.openai_adapter(model))
            self.assertIn(msg, str(cm.exception))

    def test_error_mapping(self):
        expect = {"e401": "invalid API key", "e429": "rate limited",
                  "e404": "unknown model e404", "e400key": "invalid API key",
                  "e400model": "unknown model e400model", "e400other": "bad request",
                  "e500": "API error 500"}
        for model, msg in expect.items():
            result = {}
            with self.assertRaises(ReplyFailed, msg=model) as cm:
                self.run_adapter(self.openai_adapter(model))
            self.assertIn(msg, str(cm.exception), model)
            self.assertNotIn("assistant", result)

    def test_connection_error(self):
        p = Profile(name="t", adapter="openai", model="x", base_url="http://127.0.0.1:9/v1")
        a = OpenAICompatAdapter(p, system_prompt="S", max_tokens=10, timeout=3)
        a.client = a.client.with_options(max_retries=0)
        with self.assertRaises(ReplyFailed) as cm:
            self.run_adapter(a)
        self.assertIn("cannot reach", str(cm.exception))

    def anthropic_adapter(self, model):
        p = Profile(name="c", adapter="anthropic", model=model, base_url=self.url,
                    api_key_env="DSCHAT_TEST_KEY", params={"output_config": {"effort": "low"}})
        a = AnthropicAdapter(p, system_prompt="SYS", max_tokens=50, timeout=10)
        a.client = a.client.with_options(max_retries=0, api_key="test")
        return a

    def test_anthropic(self):
        SEEN.clear()
        text, result = self.run_adapter(self.anthropic_adapter("claude-test"))
        self.assertEqual(text, "Hello")
        types = [b.type for b in result["assistant"]["content"]]
        self.assertEqual(types, ["thinking", "text"])      # thinking kept for replay
        _, req, _ = SEEN[-1]
        self.assertEqual(req["output_config"], {"effort": "low"})
        self.assertEqual(req["system"], "SYS")
        with self.assertRaises(ReplyFailed) as cm:
            self.run_adapter(self.anthropic_adapter("refuse"))
        self.assertIn("declined", str(cm.exception))
        with self.assertRaises(ReplyFailed) as cm:
            self.run_adapter(self.anthropic_adapter("e401"))
        self.assertIn("invalid API key", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
