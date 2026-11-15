"""Voice input (protocol v4) unit tests, synthetic audio only.

* the relay's Python ADPCM decoder against the DS's C encoder/decoder
  (ds/dschat/source/adpcm.c, compiled here with the host cc; skipped if none)
* tools/client.py's Python encoder against the C encoder (bit for bit)
* VoiceStream limits and parameter checks
Run: cd relay && .venv/bin/python -m unittest test_voice -v
"""
import array
import math
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools"))
import voice_engine as ve  # noqa: E402
import client  # noqa: E402

C_MAIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include "adpcm.h"
// stdin: raw s16le; stdout: blocks (1025 samples) then, after a 0xFFFFFFFF
// marker, the C decoder's output for each block
int main(void)
{
    static int16_t pcm[1 << 20]; size_t n = fread(pcm, 2, 1 << 20, stdin);
    static uint8_t enc[1 << 20]; size_t len = 0; adpcm_state st = {0, 0};
    for (size_t off = 0; off < n; off += 1025)
        len += adpcm_encode_block(&st, pcm + off, n - off < 1025 ? n - off : 1025, enc + len);
    fwrite(enc, 1, len, stdout);
    unsigned m = 0xFFFFFFFFu; fwrite(&m, 4, 1, stdout);
    static int16_t dec[1100];
    for (size_t off = 0, b = 0; off < n; off += 1025)
    {
        size_t k = n - off < 1025 ? n - off : 1025, bytes = adpcm_block_bytes(k);
        size_t got = adpcm_decode_block(enc + b, bytes, dec, 1100);
        fwrite(dec, 2, got, stdout); b += bytes;
    }
    return 0;
}
"""


def synth(seconds=1.3):
    return [int(9000 * math.sin(2 * math.pi * 300 * i / 16000) * (0.5 + 0.5 * math.sin(i / 3000))
                + 3000 * math.sin(2 * math.pi * 1700 * i / 16000)) for i in range(int(16000 * seconds))]


@unittest.skipUnless(shutil.which("cc"), "no C compiler")
class AgainstC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "t.c"
        src.write_text(C_MAIN)
        cls.exe = Path(cls.tmp.name) / "t"
        subprocess.run(["cc", "-O2", "-I", str(REPO / "ds/dschat/source"), "-o", str(cls.exe),
                        str(src), str(REPO / "ds/dschat/source/adpcm.c")], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_c(self, pcm):
        raw = array.array("h", pcm).tobytes()
        out = subprocess.run([str(self.exe)], input=raw, capture_output=True, check=True).stdout
        cut = out.index(b"\xff\xff\xff\xff")
        dec = array.array("h")
        dec.frombytes(out[cut + 4:])
        return out[:cut], dec

    def test_python_decoder_matches_c(self):
        pcm = synth()
        enc, cdec = self.run_c(pcm)
        mine = array.array("h")
        for i in range(0, len(enc), ve.BLOCK_BYTES):
            mine.extend(ve.adpcm_decode_block(enc[i:i + ve.BLOCK_BYTES]))
        # the last block may carry one padding code (odd count): compare the real samples
        self.assertEqual(list(mine[:len(pcm)]), list(cdec[:len(pcm)]))
        err = sum((a - b) ** 2 for a, b in zip(pcm, mine)) / len(pcm)
        sig = sum(a * a for a in pcm) / len(pcm)
        self.assertGreater(10 * math.log10(sig / err), 20)   # decent round trip

    def test_client_encoder_matches_c(self):
        for pcm in (synth(0.5), [0] * 2050, [32767, -32768] * 1500, synth(0.064)[:1]):
            enc, _ = self.run_c(pcm)
            self.assertEqual(b"".join(client.adpcm_encode(pcm)), enc)


class Stream(unittest.TestCase):
    def test_params(self):
        ok = ve.VoiceStream.begin(b"codec=ima-adpcm rate=16000 block=1025 lang=it")
        self.assertEqual((ok.error, ok.lang), ("", "it"))
        self.assertIn("format", ve.VoiceStream.begin(b"codec=pcm rate=16000 block=1025 lang=en").error)
        self.assertIn("format", ve.VoiceStream.begin(b"codec=ima-adpcm rate=8000 block=1025 lang=en").error)
        self.assertIn("language", ve.VoiceStream.begin(b"codec=ima-adpcm rate=16000 block=1025 lang=de").error)

    def test_decode_and_limits(self):
        s = ve.VoiceStream.begin(b"codec=ima-adpcm rate=16000 block=1025 lang=en")
        blocks = client.adpcm_encode(synth(1.0))
        for i in range(0, len(blocks), 2):
            s.add(b"".join(blocks[i:i + 2]), now=float(i))
        self.assertEqual(s.error, "")
        self.assertGreaterEqual(len(s.pcm), 16000)
        self.assertEqual(s.bytes, sum(map(len, blocks)))
        big = ve.VoiceStream.begin(b"codec=ima-adpcm rate=16000 block=1025 lang=en")
        one = client.adpcm_encode([0] * 1025)[0]
        for _ in range(ve.MAX_SAMPLES // 1025 + 3):
            big.add(one, 0.0)
        self.assertIn("too long", big.error)
        self.assertEqual(len(big.pcm), 0)                     # audio dropped, not kept
        bad = ve.VoiceStream.begin(b"codec=ima-adpcm rate=16000 block=1025 lang=en")
        bad.add(b"\x01\x02", 0.0)
        self.assertIn("short", bad.error)

    def test_decoder_clamps(self):
        blk = (32760).to_bytes(2, "little", signed=True) + bytes([88, 0]) + bytes([0x77] * 8)
        out = ve.adpcm_decode_block(blk)
        self.assertEqual(max(out), 32767)
        self.assertEqual(len(out), 17)


class Engine(unittest.TestCase):
    def test_fake_engine(self):
        import asyncio
        eng = ve.VoiceEngine(fake=True)
        asyncio.run(eng.start())
        self.assertTrue(eng.available)
        text, _ = asyncio.run(eng.transcribe(array.array("h", [0] * 32000), "en"))
        self.assertIn("2.0 seconds of en", text)
        text, _ = asyncio.run(eng.transcribe(array.array("h", [0] * 4000), "en"))
        self.assertEqual(text, "")

    def test_missing_install_reports_reason(self):
        import asyncio
        eng = ve.VoiceEngine(model="tiny-not-there")
        asyncio.run(eng.start())
        self.assertFalse(eng.available)
        self.assertIn("relay/voice/setup.sh", eng.reason)


if __name__ == "__main__":
    unittest.main()
