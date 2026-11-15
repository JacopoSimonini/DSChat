"""Drive melonDS (flatpak) for DSChat tests - ONLY inside a nested Xephyr display.

Never send synthetic input to your real display: xdotool on a live desktop
can disturb it (one run coincided with an X server crash). Here every X
client (Xephyr's children: melonDS, xdotool, import) uses DISPLAY=:2.

    from melon import Melon
    with Melon("ds/dschat/dschat_melonAP.nds", sd_files={"dschat/settings.txt": "..."}) as m:
        m.press("select"); m.touch(128, 40); m.type("hello"); m.shot("name")

Buttons are keyboard keys (melonDS mapping), touch is a mouse click on the
bottom screen at DS coordinates. Screenshots go to SHOTS. The user's
melonDS.toml is backed up and restored; DLDI (the emulated SD card) is pointed
at a scratch folder (SD_DIR) for the run. melonDS ignores SIGTERM, so it is
SIGKILLed (which also stops it rewriting its config).
"""
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SHOTS = Path(os.environ.get("MELON_SHOTS", "/tmp/dschat-melon-shots"))
CFG = Path.home() / ".var/app/net.kuribo64.melonDS/config/melonDS/melonDS.toml"
BACKUP = HERE / "melonDS.toml.bak"            # gitignored (tools/melontest/*.bak)
SD_DIR = REPO / "ds/dschat/build/melon_sd"     # gitignored build dir
BUTTON = {"start": "Return", "a": "l", "b": "k", "x": "i", "y": "j", "select": "v",
          "l": "e", "r": "r", "up": "w", "down": "s", "left": "a", "right": "d"}
NESTED = ":2"
REAL_DISPLAY = os.environ.get("DISPLAY", ":0")
os.environ["DISPLAY"] = NESTED


def sh(*args, check=True):
    return subprocess.run(args, check=check, capture_output=True, text=True).stdout


def keys():
    """Key centres from the app's own layout code (tools/melontest/keys_dump.c)."""
    out = SHOTS / "keys_dump"
    SHOTS.mkdir(parents=True, exist_ok=True)
    src = REPO / "ds/dschat/source"
    subprocess.run(["cc", "-I", str(src), "-o", str(out), str(HERE / "keys_dump.c"),
                    str(src / "kbd.c")], check=True)
    return json.loads(subprocess.run([str(out)], check=True, capture_output=True,
                                     text=True).stdout)


def kill_melon():
    for pid in sh("pgrep", "-x", "melonDS", check=False).split():
        os.kill(int(pid), 9)
    for _ in range(20):
        if not sh("pgrep", "-x", "melonDS", check=False).strip():
            return
        time.sleep(0.25)
    raise RuntimeError("melonDS still running")


class Melon:
    def __init__(self, rom, dldi=True, sd_files=None, calibrate=True, mic_wav=None):
        self.rom = str(rom)
        # mic_wav: feed this WAV to the emulated microphone ([Mic] InputType 3)
        # while the mic hotkey (M) is held - see hold(). Without it the user's
        # mic setting is left alone (never used by these tests).
        self.mic_wav = mic_wav
        self.dldi = dldi
        self.sd_files = sd_files       # {relative path: text} placed on the SD; None = keep
        self.do_calibrate = calibrate
        self.wid = None
        self.geom = None
        self.xephyr = None
        self.k = keys()

    def __enter__(self):
        try:
            return self._start()
        except BaseException:
            self.__exit__()
            raise

    def _start(self):
        kill_melon()
        if not BACKUP.exists():
            shutil.copy(CFG, BACKUP)
        text = CFG.read_text()
        m = re.search(r"(?ms)^\[DLDI\]\n(.*?)(?=^\[|\Z)", text)
        sec = m.group(1)
        sec = re.sub(r"(?m)^Enable = .*$", f"Enable = {'true' if self.dldi else 'false'}", sec)
        sec = re.sub(r"(?m)^FolderPath = .*$", f'FolderPath = "{SD_DIR}"', sec)
        sec = re.sub(r"(?m)^ImagePath = .*$", f'ImagePath = "{SD_DIR.parent / "melon_sd.bin"}"', sec)
        text = text[:m.start(1)] + sec + text[m.end(1):]
        if self.mic_wav:
            text = re.sub(r"(?ms)(^\[Mic\]\n.*?)^InputType = .*?$", r"\1InputType = 3", text, count=1)
            text = re.sub(r"(?ms)(^\[Mic\]\n.*?)^WavPath = .*?$",
                          lambda mm: mm.group(1) + f'WavPath = "{self.mic_wav}"', text, count=1)
            text = re.sub(r"(?ms)(^\[Instance0\.Keyboard\]\n.*?)^HK_Mic = .*?$", r"\1HK_Mic = 77",
                          text, count=1)
        CFG.write_text(text)
        if self.sd_files is not None:
            shutil.rmtree(SD_DIR, ignore_errors=True)
            (SD_DIR.parent / "melon_sd.bin").unlink(missing_ok=True)
            SD_DIR.mkdir(parents=True)
            for rel, content in self.sd_files.items():
                p = SD_DIR / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(content)
        SD_DIR.mkdir(parents=True, exist_ok=True)
        SHOTS.mkdir(parents=True, exist_ok=True)

        env = dict(os.environ, DISPLAY=REAL_DISPLAY)
        self.xephyr = subprocess.Popen(["Xephyr", NESTED, "-screen", "1100x1700", "-ac", "-br",
                                        "-noreset", "-title", "DSChat melonDS test"],
                                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(40):
            if subprocess.run(["xdpyinfo"], capture_output=True).returncode == 0:
                break
            time.sleep(0.25)
        else:
            raise RuntimeError("Xephyr did not start")
        subprocess.Popen(["flatpak", "run", "net.kuribo64.melonDS", self.rom],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(60):
            out = sh("xdotool", "search", "--onlyvisible", "--name", "melonDS", check=False).split()
            if out:
                self.wid = out[-1]
                break
            time.sleep(0.5)
        if self.wid is None:
            raise RuntimeError("melonDS window not found")
        time.sleep(2)
        if self.do_calibrate:
            self.calibrate()
        else:
            self.geom = (0, 21, 1.0)       # 1x window under Xephyr (no window manager)
        return self

    def quit(self, wait=8):
        """Clean quit (Ctrl+Q), so melonDS syncs the emulated SD back to SD_DIR."""
        self.focus()
        sh("xdotool", "key", "ctrl+q", check=False)
        for _ in range(int(wait * 4)):
            if not sh("pgrep", "-x", "melonDS", check=False).strip():
                return True
            time.sleep(0.25)
        return False

    def __exit__(self, *exc):
        kill_melon()
        if self.xephyr:
            self.xephyr.kill()
            self.xephyr.wait()
        if BACKUP.exists():
            shutil.copy(BACKUP, CFG)
            BACKUP.unlink()

    def shot(self, name):
        path = SHOTS / f"{name}.png"
        sh("import", "-window", self.wid, str(path))
        return path

    def calibrate(self):
        """Find the two stacked DS screens: the non-black area below the menu bar."""
        for _ in range(30):
            img = Image.open(self.shot("_cal")).convert("RGB")
            w, h = img.size
            px = img.load()
            top = next((y for y in range(5, h) if sum(px[2, y]) < 400), None)
            if top is not None:
                xs, ys = [], []
                for y in range(top, h, 2):
                    for x in range(0, w, 2):
                        if max(px[x, y]) > 4:
                            xs.append(x)
                            ys.append(y)
                if xs:
                    x0, x1, y0, y1 = min(xs), max(xs) + 2, min(ys), max(ys) + 2
                    scale = (x1 - x0) / 256
                    if abs((y1 - y0) - 384 * scale) < 3 * scale:
                        self.geom = (x0, y0, scale)
                        return
            time.sleep(1)
        raise RuntimeError("could not find the DS screens")

    def focus(self):
        # no window manager on the nested display: set the input focus directly
        sh("xdotool", "windowfocus", self.wid, check=False)

    def press(self, button, hold=0.1):
        self.focus()
        key = BUTTON[button]
        sh("xdotool", "keydown", key)
        time.sleep(hold)
        sh("xdotool", "keyup", key)
        time.sleep(0.12)

    def hold(self, buttons, seconds, mic=False):
        """Hold DS buttons (and the mic hotkey M) together for `seconds`."""
        self.focus()
        keys = [BUTTON[b] for b in buttons] + (["m"] if mic else [])
        for k in keys:
            sh("xdotool", "keydown", k)
        time.sleep(seconds)
        for k in reversed(keys):
            sh("xdotool", "keyup", k)
        time.sleep(0.12)

    def touch(self, x, y, hold=0.1):
        """Touch the bottom screen at DS coordinates (x, y)."""
        x0, y0, s = self.geom
        self.focus()
        sh("xdotool", "mousemove", "--window", self.wid,
           str(int(x0 + (x + 0.5) * s)), str(int(y0 + (192 + y + 0.5) * s)))
        sh("xdotool", "mousedown", "1")
        time.sleep(hold)
        sh("xdotool", "mouseup", "1")
        time.sleep(0.12)

    def key(self, name):
        self.touch(*self.k[name])

    def type(self, text):
        """Type on the on-screen keyboard."""
        shifted = dict(zip('~!@#$%^&*()_+{}|:"<>?', "`1234567890-=[]\\;',./"))
        for ch in text:
            if ch in shifted:
                self.key("SHIFT")
                self.key(f"c{ord(shifted[ch])}")
            elif ch == " ":
                self.key("SPACE")
            elif ch.isupper():
                self.key("SHIFT")
                self.key(f"c{ord(ch.lower())}")
            else:
                self.key(f"c{ord(ch)}")


def sheet(out, shots, geom=(0, 21, 1.0)):
    """Side-by-side crops of the DS screens (2x) for quick review."""
    x0, y0, s = geom
    tiles = [Image.open(p).convert("RGB").crop((int(x0), int(y0), int(x0 + 256 * s), int(y0 + 384 * s)))
             .resize((512, 768), Image.NEAREST) for p in shots]
    img = Image.new("RGB", (len(tiles) * 522, 768), (255, 255, 255))
    for i, t in enumerate(tiles):
        img.paste(t, (i * 522, 0))
    img.save(out)
    return out


def relay(log, fake=True, extra=()):
    """Start the relay on 127.0.0.1 (melonDS reaches it as 10.64.0.1)."""
    args = [str(REPO / "relay/.venv/bin/python"), str(REPO / "relay/relay.py"), "--bind", "local"]
    if fake:
        args.append("--fake")
    return subprocess.Popen(args + list(extra), stdout=log, stderr=log, cwd=REPO)
