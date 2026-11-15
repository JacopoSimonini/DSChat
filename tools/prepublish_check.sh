#!/usr/bin/env bash
# DSChat: privacy check before publishing a copy of this repository.
#
#   tools/prepublish_check.sh [options] [REPO_DIR]
#     --patterns FILE      your own literal strings to forbid, one per line,
#                          case-insensitive (# comments): your name, hostname,
#                          device MACs, hotspot/network names, ... Keep this
#                          file OUTSIDE the published tree.
#     --secrets-from DIR   a private checkout: the values of *_KEY/*_TOKEN/
#                          *_SECRET in DIR/relay/.env and WEP_KEY in
#                          DIR/net/dschat.conf must not appear (never printed)
#     --author "N <E>"     every commit's author and committer must be this
#
# Built-in checks (no personal data in this script):
#   * files that should never be published: photos/video/audio, packet
#     captures, logs, .env, dschat.conf, allowed_macs.txt, settings.txt,
#     local/, net/logs/ (only ds/dschat/icon.png is allowed as an image)
#   * MAC addresses (except the placeholder 02:00:00:00:00:01), e-mail
#     addresses (except example.com/.org), /home/<user> paths,
#     key-shaped strings (sk-..., AIza..., Bearer ..., ghp_...), IPv4
#     addresses outside 192.168.77.0/24, 10.64.0.0/24 (melonDS), loopback,
#     masks and the documentation ranges, and a hardcoded laptop Wi-Fi
#     interface name (the scripts detect it at run time, net/iface.sh)
#   * commit messages, with the same rules
# Exit status 1 if anything is found. Findings show file:line and the rule,
# never the matched secret.
set -euo pipefail
PATTERNS="" SECRETS_DIR="" AUTHOR="" DIR=""
while (( $# )); do
    case $1 in
        --patterns) PATTERNS=$2; shift ;;
        --secrets-from) SECRETS_DIR=$2; shift ;;
        --author) AUTHOR=$2; shift ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) DIR=$1 ;;
    esac
    shift
done
DIR=${DIR:-$(cd "$(dirname "$0")/.." && pwd)}
exec python3 - "$DIR" "$PATTERNS" "$SECRETS_DIR" "$AUTHOR" <<'PY'
import ipaddress, os, re, subprocess, sys
root, patterns_file, secrets_dir, author = sys.argv[1:]
os.chdir(root)
problems = []

def files():
    if os.path.isdir(".git"):
        out = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard"],
                             capture_output=True, text=True, check=True).stdout
        return [f for f in out.splitlines() if os.path.isfile(f)]
    res = []
    for dp, dn, fn in os.walk("."):
        dn[:] = [d for d in dn if d not in (".git", ".venv", "build", "models", "__pycache__")]
        res += [os.path.relpath(os.path.join(dp, f)) for f in fn]
    return res

BAD_FILE = re.compile(r"(\.(jpe?g|png|gif|bmp|webp|heic|mp4|mov|avi|webm|wav|mp3|m4a|ogg|flac|"
                      r"pcap|pcapng|cap|log|bak)$)|(^|/)(\.env|dschat\.conf|allowed_macs\.txt|"
                      r"settings\.txt|CLAUDE\.md|HISTORY\.md)$|(^|/)(local|net/logs)/", re.I)
ALLOWED_FILES = {"ds/dschat/icon.png"}

RULES = [
    ("MAC address", re.compile(r"\b(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}\b", re.I),
     lambda m: m.group(0).lower() != "02:00:00:00:00:01"),
    ("e-mail address", re.compile(r"[\w.+-]+@[\w-]+(?:\.[\w-]+)+"),
     lambda m: not re.search(r"@example\.(com|org)$", m.group(0))),
    ("home path", re.compile(r"/home/[A-Za-z0-9_.-]+"), lambda m: True),
    ("key-shaped secret", re.compile(r"sk-ant-[\w-]{8,}|sk-[\w-]{16,}|AIza[\w-]{20,}|"
                                     r"Bearer\s+[\w.-]{16,}|ghp_\w{20,}|xox[bp]-[\w-]{10,}"),
     lambda m: True),
    ("hardcoded Wi-Fi interface name", re.compile(r"\bwl" + r"o1\b"), lambda m: True),
]
IP = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
OK_NETS = [ipaddress.ip_network(n) for n in (
    "192.168.77.0/24", "10.64.0.0/24", "127.0.0.0/8", "0.0.0.0/32", "224.0.0.0/4",
    "192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24", "255.0.0.0/8")]

def ip_bad(s):
    try:
        a = ipaddress.ip_address(s)
    except ValueError:
        return False          # not an address (e.g. a parser test string)
    return not any(a in n for n in OK_NETS)

private = []
if patterns_file:
    for line in open(patterns_file, encoding="utf-8"):
        line = line.strip()
        if line and not line.startswith("#"):
            private.append(line.lower())

secret_values = []
if secrets_dir:
    for rel, rx in (("relay/.env", r"^\s*([A-Z0-9_]*(?:KEY|TOKEN|SECRET))\s*=\s*(.*)$"),
                    ("net/dschat.conf", r"^\s*(WEP_KEY)\s*=\s*(.*)$")):
        path = os.path.join(secrets_dir, rel)
        if os.path.exists(path):
            for line in open(path, encoding="utf-8", errors="replace"):
                m = re.match(rx, line)
                if m:
                    v = m.group(2).strip().strip('"').strip("'")
                    if len(v) >= 5:
                        secret_values.append((f"{rel}:{m.group(1)}", v))

def scan_text(text, where, is_license=False):
    for n, line in enumerate(text.splitlines(), 1):
        low = line.lower()
        for name, rx, keep in RULES:
            for m in rx.finditer(line):
                if keep(m):
                    shown = "" if name == "key-shaped secret" else f" ({m.group(0)})"
                    problems.append(f"{where}:{n}: {name}{shown}")
        for m in IP.finditer(line):
            if ip_bad(m.group(0)):
                problems.append(f"{where}:{n}: IPv4 outside the allowed ranges ({m.group(0)})")
        if not (is_license and re.match(r"^Copyright \(c\) \d{4} ", line)):
            for p in private:
                if p in low:
                    problems.append(f"{where}:{n}: private pattern #{private.index(p) + 1}")
        for label, v in secret_values:
            if v in line:
                problems.append(f"{where}:{n}: the real value of {label}")

checked = 0
for f in files():
    if BAD_FILE.search(f) and f not in ALLOWED_FILES:
        problems.append(f"{f}: file type/name that must not be published")
        continue
    data = open(f, "rb").read()
    if b"\0" in data[:8192]:
        continue              # binary (the icon)
    checked += 1
    scan_text(data.decode("utf-8", "replace"), f, is_license=(f == "LICENSE"))

if os.path.isdir(".git"):
    ids = subprocess.run(["git", "log", "--all", "--format=%an <%ae>%n%cn <%ce>"],
                         capture_output=True, text=True).stdout.splitlines()
    if author:
        for who in sorted(set(ids)):
            if who != author:
                problems.append(f"git: commit identity '{who}' is not '{author}'")
    msgs = subprocess.run(["git", "log", "--all", "--format=%B"], capture_output=True, text=True).stdout
    keep_private, private[:] = private[:], []          # the author line itself is allowed
    scan_text("\n".join(l for l in msgs.splitlines()
                        if not l.startswith("Co-Authored-By:")), "git commit messages")
    private[:] = keep_private
    for l in msgs.splitlines():
        if any(p in l.lower() for p in private):
            problems.append("git commit messages: private pattern")

print(f"prepublish_check: {checked} text files checked in {root}"
      f"{', ' + str(len(private)) + ' private patterns' if private else ''}"
      f"{', ' + str(len(secret_values)) + ' secret values' if secret_values else ''}")
for p in problems:
    print("  FOUND", p)
print("prepublish_check:", "FAILED" if problems else "OK, nothing found")
sys.exit(1 if problems else 0)
PY
