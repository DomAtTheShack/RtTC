# Bluetooth audio

Radios built on Benshi's Bluetooth design carry audio over Bluetooth as well as
control: BTech UV-Pro and UV-50Pro, Radioddity GA-5WB and DB50-B Mini, Vero VR-N75,
VR-N76, VR-N7500 and VR-N7600. With **Bluetooth radio** set, direwolf uses that instead
of a sound card and cable, at every packet speed, and so does the tuning tone.

The protocol comes from [HTCommander](https://github.com/Ylianst/HTCommander) (Apache
2.0), whose Linux, Windows and macOS builds do Bluetooth audio, and
[UVProTermBT](https://github.com/chengmania/UVProTermBT), whose
`docs/GAIA_AUDIO_SSTV.md` describes it and whose author has sent and received SSTV with
it on the air.

## Setting it up

1. Pair the radio with the computer (Linux: `bluetoothctl`, then `scan on`, `pair`,
   `trust`; or the desktop's Bluetooth settings).
2. In RadioConnectorCli, **Settings > Bluetooth radio**, pick it (radios are listed
   first). PTT changes to VOX: the radio transmits while it gets audio and stops when it
   gets the end frame, so nothing else keys it.
3. **Start** connects to the radio, then runs the modem as usual.

For RadioConnector, set `bluetooth.address` in its config (`connector.ini` has
`bluetooth=38:D2:00:01:C0:AB`), or send `{"bluetooth":{"address":"38:D2:00:01:C0:AB"}}`.

Only one program can use the radio's Bluetooth channels at a time. Close HTCommander,
phone apps and other copies of these programs first.

### Linux

Needs BlueZ, and PulseAudio or PipeWire (with its PulseAudio server, which desktop
installs have). While running, the bridge adds two silent sound cards, `rtc_bt_rx`
(*Bluetooth-radio-receive*) and `rtc_bt_tx` (*Bluetooth-radio-transmit*), and removes
them when it stops. The modems reach them through ALSA's `pulse` device, so the ALSA
PulseAudio plugin has to be installed (`libasound2-plugins` on Debian/Ubuntu,
`alsa-plugins` on Arch, `pipewire-alsa` also works).

### Windows

Windows has no virtual sound cards of its own, so install a virtual cable, e.g. VB-Cable,
plus a second one (VB-Cable A+B) for transmitting. Then in Settings:

| Setting | Example | What it is |
| --- | --- | --- |
| Radio audio into | CABLE Input | The bridge plays what the radio hears into this |
| Rx sound card | CABLE Output | ...and the modems listen on its other end |
| Tx sound card | CABLE-B Input | The modems play into this |
| Transmit audio from | CABLE-B Output | ...and the bridge sends what it hears on the other end |

Untested on a radio: the Bluetooth code follows HTCommander's, through Winsock instead of
WinRT.

### macOS

Not available yet. The code is there (it follows HTCommander's macOS build, through
Homebrew's PulseAudio), but direwolf on macOS only uses the system's default sound card,
so it can't reach the bridge's, and the Bluetooth setting is hidden. (The minimodem
version on the `archive/minimodem` branch could use it.)

## How it works

```
 direwolf                            bridge (src/Bluetooth)                     radio
 ────────                            ──────────────────────                     ─────
 records rtc_bt_rx.monitor  <──  plays decoded audio  <── SBC decode <── audio channel
 plays into rtc_bt_tx       ──>  records its monitor  ──> key on sound,
                                                          SBC encode    ──> audio channel
                                 GAIA handshake, then listens       <──> control channel
```

- **Control channel**: the radio's Serial Port service (RFCOMM, UUID 0x1101) speaks GAIA.
  The bridge sends HTCommander's connect handshake (device info, then registering for
  status notifications). Without it the radio ignores transmit audio. The bridge keeps
  the channel open and reads it, and stops if it closes.
- **Audio channel**: the vendor "BS AOC" service, UUID
  `39144315-32FA-40DB-85ED-FBFEBA2D86E6`, found with SDP.
- **Receiving**: the radio sends SBC frames while its squelch is open. The bridge decodes
  them and plays them into `rtc_bt_rx`.
- **Transmitting**: the modems play into `rtc_bt_tx`, which is digital silence (zeros)
  between transmissions. A sample over 64 (of 32767) starts a transmission; 300 ms of
  quiet ends it with the end frame, so the radio stays keyed through direwolf's and
  the tuning tone's own gaps. The first 250 ms are buffered before anything is sent, so a
  Bluetooth hiccup can't starve the radio part way through a packet. The 300 ms of quiet
  at the end goes out too, like a short TXTAIL.
- **Stopping** always ends any transmission with the end frame.

### Audio channel framing

```
7e <command> <payload> 7e
```

`7e` starts and ends a frame (one `7e` can do both). A `7d` or `7e` inside is sent as
`7d` followed by the byte XOR `20`.

| Command | Meaning |
| --- | --- |
| `00` | SBC audio, both ways |
| `03` | SBC audio from the radio, too |
| `01` | End of audio. `7e 01 00 01 00 00 00 00 00 00 7e` ends a transmission |
| `02` | Ack |
| `09` | Our own transmit audio, sent back (ignored) |

Each transmit frame holds 4 SBC frames (16 ms). The radio ignores much bigger ones
(UVProTermBT found this by comparing Bluetooth captures with HTCommander's).

### SBC

32 kHz, mono, 16 blocks, 8 subbands, **loudness** allocation (the radio rejects SNR
allocation). Frames start `9c 71`. The bridge sends bitpool 40 (88-byte frames, 22 kB/s),
as UVProTermBT does; HTCommander sends 18. Received frames carry their own settings.
The codec is Google's libsbc (Apache 2.0), copied into `src/third_party/libsbc`; it's
the one HTCommander's codec was checked against.

### GAIA

```
ff 01 <flags> <data length> <group: 2 bytes> <command: 2 bytes> <data>
```

Big endian. Flag bit 0 means a checksum byte follows. Replies have `0x8000` set in the
command. Group 2 is the basic group; commands used: 4 (device info), 6 (register
notification), 9 (event notification), 20 (HT status). An HT status notification has
`0x40` set in its first status byte while the radio is transmitting.

## What has been checked

- RadioTests' unit tests: framing, GAIA, SBC (round trip about 60 dB SNR, and the header
  the radio needs), transmit keying and settings.
- On a Linux PC, without a radio: direwolf packets (and, before it was archived, minimodem
  RTTY) through the virtual
  sound cards, the transmit keying, the SBC encoding and back. Text and packets came
  through whole, one transmission each, ended with the end frame. Our SBC also decodes
  with BlueZ's libsbc.
- With a UV-PRO on Linux: both channels connect, and the radio answers the handshake
  with device info, status notifications and its HT status. Transmitting works on the
  air, and so does receiving: direwolf decoded an APRS position beacon from a
  Radioddity GD-88.
- Not yet: Windows and macOS haven't been run on a radio.

Before transmitting for real, try it into a dummy load or on a quiet simplex frequency,
and listen on a second radio for clean audio at the right level.
