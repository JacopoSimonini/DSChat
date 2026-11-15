# Technical notes

Findings from building DSChat that are not obvious from the code: what the
DS Lite, DSWiFi, the microphone, Whisper and melonDS actually do. Measured
on real hardware unless marked otherwise.

## Wi-Fi and networking on the DS (DSWiFi / lwIP)

* **A DHCP server must send a DNS server (option 6).**
  * DSWiFi 0.4.2 (BlocksDS v1.25.0, `source/arm9/access_point.c`, state
    `WIFI_CONNECT_DHCPING`) leaves "Acquiring DHCP" only when lwIP has an
    IPv4 address *and* a DNS server, and the state has no timeout. Without
    option 6 the DS hangs, although it bound the lease (it answers ARP for
    its address).
  * melonDS's built-in AP sends a DNS server, so the emulator hides the
    problem.
  * DSChat's dnsmasq always sends router and DNS = the AP address. DNS queries
    are then dropped by the firewall, since the DS connects to the relay by IP.
* The DS's DHCP client requests options 1, 3, 28 and 6, does not set the
  broadcast flag (unicast replies work), and does no ARP probe after the ACK.
  dnsmasq's own ping check delays the first OFFER by about 3 s.
* lwIP reports a refused TCP connect (RST) as `ECONNRESET` with
  POLLERR|POLLHUP, not `ECONNREFUSED`.
* `Wifi_AssocStatus()` keeps returning "associated" after the link is gone:
  DSWiFi has no link-lost signal. The app detects loss itself: keepalive PING
  after 20 s idle, a LIVE probe during long replies, a probe when the lid
  opens, and an RX watchdog that power-cycles the radio after 8 s without a
  received packet.
* `Wifi_Deinit()` busy-waits for the ARM7. With a stuck radio it never
  returned and froze the UI, so the app doesn't use it.
* The DS Lite does 802.11b only, open or WEP (no WPA), 2.4 GHz. The AP uses
  `hw_mode=b` and basic rates 1 and 2 Mbps (the DS cannot receive 5.5/11 Mbps
  broadcasts), long preamble, no 11n/WMM. Unicast at normal rates works.

## The upload stall, and why the app paces its sends

* **Symptom:** the DS uploaded at about 10 KB/s (4-50 KB/s) on a link that
  carries about 165 KB/s while data flows.
* **What a packet capture showed on the laptop's AP interface:**
  * about 7% of the DS's 1260-byte TCP segments never arrived;
  * the laptop answered with duplicate ACKs within ~5 ms, so delayed ACKs
    were not the problem;
  * the DS had only 1-2 segments in flight, so lwIP never collected the 3
    duplicate ACKs a fast retransmit needs. Every loss waited for the
    retransmission timeout (~1.26 s), and these stalls were ~90% of the
    upload time.
* **The send buffer is not the cause:**
  * DSWiFi's 24 KB TX ring refused 0 packets (`WSTAT_TXQUEUEDREJECTED`);
  * per its source, it never drops queued packets silently.
* **The loss happens at or after the radio, and only when frames go out back
  to back.**
  * DSWiFi never reads the TX status of data frames, so a frame that fails
    all 7 hardware attempts vanishes without being counted anywhere.
  * `WSTAT_TXPACKETS` counts frames handed to the MAC, including the DS's
    own management and null frames, not retries.
* **Fix (`ds/dschat/source/net.c`):**
  * `TCP_NODELAY`;
  * at most one 1260-byte segment handed to `send()` per frame (60 Hz),
    about 75 KB/s maximum.
* **Measured on hardware with the mictest diagnostic:**

  | Upload | KB/s (two runs) |
  |---|---|
  | burst (send everything at once) | 9.4, 11.6 |
  | paced | 61.5, 61.5, with no retransmission timeouts |

  Pacing removes the losses, not only their cost.

## Microphone (DS Lite, libnds / BlocksDS)

* `soundMicRecord(buffer, len, MicFormat_8Bit | MicFormat_12Bit, rate, cb)`.
  * The ARM7 samples the touchscreen controller's AUX input on ARM7 timer 1.
  * 8-bit samples arrive as signed int8; 12-bit as signed int16 (the 12-bit
    value shifted up 4 bits).
  * The callback runs in IRQ context with each full half of the double
    buffer. The ARM7 writes main RAM, so read it through `memUncached()`.
  * One more, partial buffer arrives after `soundMicOff()`.
* **Gain:** the hardware amplifier has ×20, ×40, ×80 and ×160.
  * The stock ARM7 core sets ×160 at every recording start, and the ARM9 has
    no command to change it. mictest ships its own ARM7 with a FIFO handler
    calling `micSetAmp()`; ×40 measured 7-10 dB below ×160.
  * `soundMicOff()` switches the amplifier off.
* **What the signal looks like** (12-bit, 16 kHz):
  * every recording starts with ~220-250 ms of full-scale spikes every
    2.6 ms while the amplifier powers up. Drop the first 0.3 s;
  * a 60 Hz hum (the console's own frame rate, not mains) around −56 dBFS,
    sub-150 Hz rumble and a 2-3% DC offset. A 100 Hz high-pass removes them;
  * the positive rail is about +32448, not 32752;
  * at about 20 cm, speech peaks around −15 dBFS with 25-34 dB SNR at ×160.
    Close to the microphone it is ~5 dB louder and touches the rail now and
    then.
  * No samples are lost while Wi-Fi traffic flows or the touchscreen is
    used; taps raise the local level by only 1-6 dB.

## Speech to text (faster-whisper on the laptop)

Measured on 26 short read-aloud clips recorded on a DS Lite.
* **Never auto-detect the language on DS audio.** Detection guessed unrelated
  languages for English speech, which also caused 9-50 s outliers and a
  "translation" into the wrong language. Pass the language explicitly.
* **Use the voice-activity filter.** Without it, silence turns into invented
  text ("Thank you for watching!").
* With the first 0.3 s dropped, a 100 Hz high-pass, the language set and VAD
  on, `small` (int8, CPU) averaged 9.3% WER at ~2 s per clip on a laptop CPU;
  `base`, 16.7% at ~0.8 s.
* **IMA ADPCM** (4 bits/sample, 8 KB/s at 16 kHz) costs nothing measurable:

  | | WER |
  |---|---|
  | uncompressed 16 kHz | 9.3% |
  | ADPCM 16 kHz | 8.5% |
  | ADPCM 8 kHz | 12.3% |
* Proper names are the weak point. Passing them as Whisper's initial prompt
  (`VOICE_HINTS`) fixed the names in the tests.
* faster-whisper 1.2.1's `decode_audio()` passes an argument that PyAV 19
  rejects, so the relay decodes the audio itself and hands Whisper a float
  array.

## Rendering on the DS

* **Never clear an area and then redraw it in VRAM.** Redrawing the whole
  keyboard took longer than VBlank, and the LCD showed the half-cleared
  keyboard for one frame: a visible dimming on every key press and release.
  * The bottom screen is drawn into a RAM back buffer with per-row dirty
    flags and DMA-copied right after VBlank starts.
  * Only keys whose look changed are redrawn.
* The real DS Lite LCD is much dimmer and lower contrast than an emulator, so
  judge colours by contrast. Status bars are black on near-white.
* Flashcards with a working DLDI driver can create, write, rename and read
  back files with BlocksDS's FAT. DSChat writes its settings file as temp
  file, remove, rename, then read back and compare.

## Laptop side

* **One radio, one channel.** Most Wi-Fi cards that support a managed and an
  AP interface at the same time allow only one channel:
  `iw list` shows `#channels <= 1`. The AP must then use the hotspot's
  channel, and the hotspot must be on 2.4 GHz.
  * If the hotspot moves, the AP must follow: `./dschat`'s watcher restarts
    it on the new channel.
  * It also steps aside when the hotspot is on 5 GHz or gone, so the laptop
    can rejoin.
* **NetworkManager** must ignore the AP interface. Some distributions set
  `unmanaged-devices=none` in a conf.d file, so the runtime file must sort
  last and use `+=`.
* **ufw's default-deny** lives in the iptables table. An accept rule in a
  separate nftables table does not override it, so DSChat adds two iptables
  ACCEPT rules too (tagged `dschat`, removed on stop).
* Processes started from a terminal (hostapd, dnsmasq) die when the desktop
  session ends. `./dschat` runs them in transient systemd units instead.

## melonDS (development only)

* In indirect (libslirp) Wi-Fi mode the emulated DS gets 10.64.0.x, and its
  gateway 10.64.0.1 reaches the host's 127.0.0.1: run the relay with
  `--bind local`.
* melonDS 1.1's emulated Wi-Fi stops sending and receiving about 65-70 s
  after the ROM starts. It's time-based, not traffic-based, and emulator-only:
  a real DS Lite passed a 3-minute soak test. Keep test runs short.
* DLDI FolderSync writes the emulated SD card back to the host folder only on
  a clean quit.
* Run melonDS for automated tests in a nested display (Xephyr). Driving the
  real desktop with synthetic input is risky.
