# Third-party software

DSChat's own code is MIT-licensed (see `LICENSE`). This repository contains
**no vendored third-party code**, with one exception, listed first. Everything
else is downloaded by you when you build or install, under its own licence.

## In this repository

| File | Origin | Licence |
|---|---|---|
| `ds/mictest/arm7/source/main.c` | BlocksDS SDK `sys/arm7/main_core/source/main.c` (v1.25.0), modified: adds one FIFO handler for the microphone gain | zlib; the original copyright header is kept and the change is marked in the file |

## Linked into the DS ROMs you build (`.nds`)

The ROMs are built with the BlocksDS toolchain (Docker image
`skylyrac/blocksds:slim-latest`) and contain code from:

| Component | Licence | Notes |
|---|---|---|
| libnds (BlocksDS) | zlib | includes FatFs (ChaN's FatFs licence, BSD-style) |
| DSWiFi (BlocksDS) | MIT | |
| lwIP (inside DSWiFi) | BSD-3-Clause | |
| picolibc (C library) | BSD-3-Clause and similar | |
| BlocksDS start-up files and linker scripts (`sys/crts`) | zlib / MPL-2.0 / FSF all-permissive | |
| BlocksDS default ARM7 core (dschat, wifitest) | zlib | |

Mbed TLS (shipped with DSWiFi) is not linked into these ROMs: they make no
TLS connections.

If you **redistribute built `.nds` files**, the MIT and BSD licences above
require their copyright notices to come with the binary. Include this file,
plus the licence texts from the BlocksDS image:
* `libs/libnds/licenses/`;
* `libs/dswifi/COPYING`, `libs/dswifi/COPYING.lwip`;
* `sys/crts/COPYING.*`.

Distributing only the source (this repository) has no such requirement.

Build tools (ndstool, grit, the GCC toolchain) are not part of the output.
Their licences (GPL for some) do not apply to the ROMs.

## Used by the laptop side (installed by you with pip, not vendored)

| Package | Licence |
|---|---|
| anthropic, openai | Apache-2.0 |
| python-dotenv | BSD-3-Clause |
| faster-whisper, CTranslate2, onnxruntime | MIT |
| Whisper models (Systran/faster-whisper-small, -base; converted from OpenAI Whisper) | MIT |
| Silero VAD model (bundled in faster-whisper) | MIT |
| tokenizers, huggingface_hub, hf-xet, flatbuffers | Apache-2.0 |
| numpy | BSD-3-Clause (and bundled permissive licences) |
| PyAV (`av`) | BSD-3-Clause; its wheels bundle FFmpeg libraries under LGPL. DSChat does not call it, but faster-whisper installs it |
| tqdm | MPL-2.0 and MIT (used unmodified) |
| PyYAML, anyio, filelock, h11, truststore | MIT |
| click, httpx2, httpcore2, idna, fsspec, protobuf | BSD-3-Clause |
| packaging | Apache-2.0 or BSD-2-Clause |
| typing_extensions | PSF-2.0 |

## External programs (run, not linked)

hostapd (BSD), dnsmasq (GPL-2.0/3.0), iw (ISC), nftables and iptables
(GPL-2.0), NetworkManager (GPL-2.0+), systemd (LGPL-2.1+) and melonDS (GPL-3.0,
for development only) are separate programs that DSChat's scripts start or
configure. Using them this way places no licence obligations on DSChat.

Licences as checked on 2026-11-15 (BlocksDS v1.25.0 image, PyPI metadata,
Hugging Face model cards). Check them again before relying on this file.
