# DSChat wire protocol (v4)

Plain TCP between the DS (client) and the relay (server). Designed so the C
side needs no line scanning, no escaping and no dynamic allocation.

Version history:

* **v1**: `PING`/`PONG`, `SEND`/`TEXT`/`DONE`/`FAIL`, `RSET`/`OKAY`.
* **v2**: adds the profile frames `PLST`, `PGET`, `PSET` and the `INFO`
  reply. Nothing in v1 changed, so a v1 client works against a v2 relay
  unchanged. A v2 client can detect a v1 relay from `PONG` (`DSChat relay 1`)
  or from `FAIL unknown frame type` in answer to a profile frame.
* **v3**: adds `CNCL` (cancel the reply in progress) and `LIVE` (liveness
  probe answered at once, even during a reply), and key=value fields in the
  `PONG` payload (`max_msg`, `boot`). Nothing in v1/v2 changed. A client must
  check the version in `PONG` before sending `CNCL` or `LIVE`: a v2 relay
  would only answer them after the current reply, with `FAIL unknown frame
  type`, which looks like the end of that reply.
* **v4**: adds voice input: `VBEG`, `VDAT`, `VEND` (client) and `VTXT`
  (relay), plus the `PONG` field `voice=1`. Nothing older changed.
  * A client may send voice frames **only if `PONG` says `voice=1`**. An
    older relay would answer each of them with `FAIL unknown frame type`,
    which would desynchronise the client's request queue.
  * A v4 relay offers `voice=1` only while its speech-to-text engine is
    running.
  * Older clients ignore the new `PONG` field.

## Framing

Every message is a **frame**: a fixed 10-byte ASCII header, then a payload.

```
offset 0..3  TYPE   4 uppercase ASCII letters
offset 4     ' '    a single space (0x20)
offset 5..8  LEN    payload length in bytes, 4 decimal digits, zero-padded
offset 9     '\n'   newline (0x0A)
offset 10..  payload, exactly LEN bytes (LEN may be 0000)
```

Example: `PING 0000\n` (no payload), `SEND 0005\nHello`.

To read a frame in C: read exactly 10 bytes, check `hdr[4]==' ' && hdr[9]=='\n'`,
parse 4 digits, then read exactly LEN bytes. Any malformed header is a fatal
protocol error: the relay answers `FAIL` and closes the connection.

Payload text is **printable ASCII (0x20-0x7E) plus `\n`**. The relay
guarantees this for everything it sends (it transliterates or replaces
anything else, see "Sanitising" below). The relay also sanitises what it
receives. There is no NUL terminator on the wire.

## Limits

| Limit | Value | Notes |
|---|---|---|
| Max payload of any frame | 4096 bytes | larger LEN = protocol error |
| Max `SEND` payload | 1024 bytes (`MAX_MSG` in `.env`) | larger = `FAIL`, connection stays open |
| Max `TEXT` payload sent by the relay | 256 bytes | API deltas are split if longer |
| Idle timeout | 600 s with no frame from the client | relay closes the socket |
| First-frame timeout | 20 s for a new connection to send its first frame | relay closes the socket (silent connections cannot hold the slots) |
| Message rate | 10 `SEND`s per client per minute (`MAX_SEND_PER_MIN`, 0 = off; off with `--fake`) | over it: `FAIL too many messages, wait a minute` |
| Conversations kept | 16 clients (per IP) | the oldest idle one is dropped |
| Frame timeout | 15 s to receive the rest of a frame once its first byte arrived | |
| Reply timeout | 120 s total for one reply | relay sends `FAIL` |
| Concurrent connections | 4 | extra connections get `FAIL` and are closed |
| Profile name | 1-16 chars `[a-z0-9_-]` | model IDs contain no spaces |

## Frames

Client to relay:

| TYPE | Payload | Meaning | Answer |
|---|---|---|---|
| `PING` | empty (anything is ignored) | liveness check | `PONG` |
| `SEND` | user message text | ask the AI; starts a reply | `TEXT`* then `DONE` or `FAIL` |
| `RSET` | empty | forget the conversation history | `OKAY` (empty) |
| `PLST` | empty | list the AI profiles *(v2)* | `INFO` |
| `PGET` | empty | current profile of this client *(v2)* | `INFO` |
| `PSET` | profile name | switch profile; **resets the conversation** *(v2)* | `OKAY` or `FAIL` |
| `CNCL` | empty | cancel the reply in progress *(v3)* | none of its own; the reply ends with `FAIL cancelled` |
| `LIVE` | empty | liveness probe *(v3)* | `LIVE` at once, even in the middle of a reply |
| `VBEG` | `codec=ima-adpcm rate=16000 block=1025 lang=en` *(v4)* | start a voice stream | none (problems are reported in the answer to `VEND`) |
| `VDAT` | one or more whole ADPCM blocks *(v4)* | audio, streamed while the user speaks | none |
| `VEND` | empty *(v4)* | end of the stream: transcribe it | `VTXT` or `FAIL` (may take seconds; `LIVE` is answered meanwhile) |

Relay to client:

| TYPE | Payload | Meaning |
|---|---|---|
| `PONG` | server info, see below (`DSChat relay 3 ...` today) | answer to `PING` |
| `TEXT` | a chunk of the reply text (1..256 bytes) | streamed as it arrives from the API |
| `DONE` | stop reason: `end_turn` or `max_tokens` | end of reply; the reply is complete |
| `FAIL` | human-readable error text (ASCII, short) | the request failed; ends the current reply if one was in progress |
| `OKAY` | empty after `RSET`; `<name> <model>` after `PSET` | success |
| `INFO` | data, see below *(v2)* | answer to `PLST` / `PGET` |
| `LIVE` | empty *(v3)* | answer to `LIVE`; may arrive between `TEXT` frames |
| `VTXT` | stats line + transcript, see below *(v4)* | answer to `VEND` |

### PONG payload

`DSChat relay <version>` followed, from v3, by space-separated `key=value`
fields (clients ignore keys they do not know):

* `max_msg=<n>`: the largest `SEND` payload the relay accepts (`MAX_MSG`).
* `boot=<8 hex>`: random id chosen when the relay starts. A client that sees
  a different boot id after reconnecting knows the relay restarted and its
  conversation history (kept in relay memory) is gone.

* `voice=1` *(v4)*: voice input (`VBEG`/`VDAT`/`VEND`) is available. Absent,
  or any other value: do not send voice frames.

Example: `DSChat relay 4 max_msg=1024 boot=1a2b3c4d voice=1`.

### Voice input (v4)

The user holds a button and speaks; the client streams the audio while
recording, then asks for the transcript. The transcript is text for the
client to show or edit: it is **not** a chat message and does not touch the
conversation history.

* **`VBEG`** parameters (`key=value`, space-separated, all required):
  * `codec=ima-adpcm`;
  * `rate=16000` (Hz);
  * `block=1025` (samples per block);
  * `lang=en` or `lang=it`: the spoken language. It is never auto-detected:
    on DS audio, detection guessed Polish, Latin or Slovenian for English.

  Anything else makes `VEND` fail with `unsupported audio format` or
  `language must be one of en, it`.
* **`VDAT`** carries whole blocks of IMA ADPCM, mono:
  * a block of n samples (n = 1025, the last block may be shorter) is 4
    header bytes then the codes;
  * header: bytes 0-1 = the first sample (int16 little-endian), byte 2 = the
    step index (0-88), byte 3 = 0;
  * the codes: (n - 1) 4-bit codes, two per byte, low nibble first;
  * a full block is 516 bytes;
  * every block restarts the decoder, so blocks are self-contained;
  * standard IMA/DVI step and index tables (`ds/dschat/source/adpcm.c`).
* The client should already have dropped audio it knows is garbage. The DS
  drops its first 0.3 s, the microphone amplifier's power-up burst.
* **`CNCL`** while a stream is open (before `VEND`) drops the stream with no
  answer. `CNCL` while the relay is transcribing ends `VEND` with
  `FAIL cancelled`.
* **`VTXT`** payload: one line of `key=value` statistics, `\n`, then the
  transcript (printable ASCII, may be empty = nothing was heard):
  * `ms`: transcription time on the relay;
  * `audio`: milliseconds of audio decoded;
  * `bytes`: ADPCM bytes received;
  * `rx`: first to last `VDAT` at the relay, in milliseconds.

  Example: `ms=2010 audio=3200 bytes=25600 rx=3150\nTell me the capital of France.`
* **`FAIL` answers to `VEND`:**
  * `no voice input in progress`;
  * `no audio received`;
  * `audio too long (max 20 s)`;
  * `unsupported audio format`;
  * `language must be one of en, it`;
  * `voice input not available on this relay`;
  * `too many messages, wait a minute` (`VEND` counts toward the rate limit);
  * `transcription failed`;
  * `cancelled`.
* Limits: 21 s of audio, about 170 KB of ADPCM, one transcription at a time
  on the relay. The relay never writes audio to disk and logs transcripts
  only with `--debug`.

### Profile payloads (v2)

A *profile* is a named AI provider configuration on the relay
(`relay/providers.toml`), e.g. `claude`, `gemini`, `qwen`, `openai`, `ollama`.

`PLST` answer: one line per profile, lines separated by `\n` (no trailing
newline), each line three fields separated by single spaces:

```
<flag> <name> <model>
```

| flag | meaning |
|---|---|
| `*` | the current profile of this client |
| `+` | available |
| `!` | not usable: its API key is missing on the relay |

`PGET` answer: `<name> <model>` (one line).

`PSET <name>`: on success `OKAY <name> <model>`; the conversation history of
this client is cleared (histories are provider-specific). Errors:
`FAIL unknown profile`, `FAIL profile <name> has no API key (<ENV_VAR>)`,
`FAIL busy` (a reply for the same client is streaming on another connection).

The profile choice is kept per client IP like the history, and resets to the
relay's default (`ACTIVE_PROFILE`) when the relay restarts.

## Exchange rules

* Strictly request/response, answered **in order**. A client may pipeline
  requests (send the next one before the previous answer arrived); they are
  handled one after the other. During a reply the relay (v3) keeps reading:
  `CNCL` and `LIVE` act at once, any other frame is queued and handled after
  the reply has ended. (v1/v2 relays did not read during a reply at all.)
* `CNCL` (v3): the relay stops the AI request, ends the reply with
  `FAIL cancelled` and does not add the exchange to the history. If the reply
  had already ended (`CNCL` crossed `DONE`/`FAIL` on the wire), `CNCL` is
  ignored without an answer, so the client simply waits for the end of the
  reply in both cases.
* `LIVE` (v3): answered immediately with `LIVE`, outside the request order.
  The DS uses it while waiting for a slow reply to tell "the AI is still
  thinking" from "the connection is dead".
* **One connection per client IP.** A new connection from an IP replaces
  (closes) the older one from the same IP, rolling back any reply that was
  streaming on it. The DS only ever uses one connection; after a Wi-Fi drop or
  sleep its old connection may linger half-open on the relay.
* A reply is zero or more `TEXT` frames followed by exactly one `DONE` or one
  `FAIL`. After `FAIL` the text already received is incomplete; the relay has
  removed the failed exchange from the history, so the user can simply retry.
* `TEXT` carries only the final answer. Reasoning/thinking output that some
  providers stream (separate reasoning fields, `<think>...</think>` blocks,
  thinking chunks) is never sent to the DS.
* If the reply was cut by the token limit the relay sends a final
  `TEXT " [...]"` and then `DONE max_tokens`.
* Unknown TYPE: `FAIL unknown frame type`, connection stays open.
* Conversation history is kept **per client IP address** in relay memory, so
  it survives a reconnect (e.g. after the DS drops Wi-Fi: dnsmasq gives the
  DS the same IP again). It is lost when the relay restarts (detect it with
  the `boot` id in `PONG`). `RSET` and `PSET` clear it. When the history
  reaches `MAX_TURNS` exchanges, `SEND` answers `FAIL history full, send RSET`.
* Error texts are the same for every provider, e.g. `relay: invalid API key`,
  `rate limited, try again later`, `relay: unknown model <id>`,
  `AI API error <code>, retry`, `relay cannot reach the AI API`,
  `the AI declined to answer that`, `reply timed out`.

## Sanitising

The DS console shows 32x24 characters from a 7-bit font. The relay converts
all text it sends to printable ASCII:

* `\r` is dropped (`\r\n` becomes `\n`); tab becomes a space.
* Accented letters lose the accent (`è` -> `e`), via Unicode NFKD.
* Common typography maps to ASCII: curly quotes -> `'` `"`, dashes -> `-`,
  ellipsis -> `...`, bullet -> `*`, non-breaking space -> space, etc.
* Anything else becomes `?`.

Word wrapping to 32 columns is the client's job.

## Example session

```
C: PING 0000\n
R: PONG 0041\nDSChat relay 3 max_msg=1024 boot=1a2b3c4d
C: SEND 0009\nHi there!
R: TEXT 0005\nHello
R: TEXT 0019\n! I'm Claude, an AI
R: DONE 0008\nend_turn
C: PLST 0000\n
R: INFO 0075\n* claude claude-haiku-5-5\n+ gemini gemini-3.5-flash-lite\n+ ollama gemma3:4b
C: PSET 0006\ngemini
R: OKAY 0028\ngemini gemini-3.5-flash-lite
C: PGET 0000\n
R: INFO 0028\ngemini gemini-3.5-flash-lite
C: RSET 0000\n
R: OKAY 0000\n
C: SEND 0020\nTell me a long story
C: LIVE 0000\n                          (no text yet, is the relay alive?)
R: LIVE 0000\n
R: TEXT 0016\nOnce upon a time
C: CNCL 0000\n
R: FAIL 0009\ncancelled
```

You can talk to the relay by hand with `tools/client.py`
(`/profiles`, `/use NAME` in interactive mode; Ctrl-C during a reply sends
`CNCL`).
