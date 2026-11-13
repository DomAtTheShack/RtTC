## Radio to Text Connector (RtTC)

Send and receive text over ham radio as AX.25 packets, through direwolf: 300, 1200 or
9600 baud, with FX.25 error correction. For VHF/UHF.

RtTC is the radio side of a larger system. A separate program, called the **main
program** in these docs (not released yet), answers the people on the radio, e.g. an
LLM chat. Any packet station connects to `RadioConnector` with AX.25 connected mode, and
the connector passes what people type to the main program and its answers back. The main
program never touches the radio. Any program that speaks the JSON protocol in
[docs/json-format.md](docs/json-format.md) can take its place, and `RadioConnectorCli`
works on its own without one.

RTTY isn't supported. An older, unmaintained version that does RTTY through minimodem
is on the `archive/minimodem` branch.

PTT is VOX, a CAT command (Hamlib), or the RTS/DTR line of a serial port. That can be a
second port: some radios' USB connection shows two serial ports, one for CAT and one for
PTT. An AIOC cable (for handhelds with a Kenwood-style K plug, such as Baofengs) does PTT
and audio in one: pick **AIOC cable** in Settings > PTT, and its serial port and sound card
are found for you ([docs/radios.md](docs/radios.md#aioc-cable)). The Settings menu lists common Icom, Yaesu, Kenwood, Elecraft and Xiegu radios; any
other Hamlib model works by its number, or through `rigctld`. Which radios are tested, which
should work and which setups won't: [docs/radios.md](docs/radios.md).

Radios with Bluetooth audio (BTech UV-Pro and UV-50Pro, Radioddity GA-5WB and DB50-B
Mini, Vero VR-N75/N76/N7500/N7600) can skip the sound card and cable: set **Bluetooth
radio** in Settings ([docs/bluetooth-audio.md](docs/bluetooth-audio.md)).

### The programs

| Program | What it is |
| --- | --- |
| `RadioConnectorCli` | The menu program: pick settings, then send and receive text by hand. Also the tool for testing a connector over the air |
| `RadioConnector` | Headless: connects one radio to the main program, for connected-mode sessions ([below](#radioconnector)) |
| `RadioTests` | Every test, run with no arguments ([below](#tests)) |

### RadioConnectorCli

Run `RadioConnectorCli`:

- **Start** runs with the current settings.
- **Settings** changes them. They're saved to:
  - Windows: `%APPDATA%\RtTC\settings.ini`
  - macOS: `~/Library/Application Support/RtTC/settings.ini`
  - Linux: `~/.config/rttc/settings.ini`
- **Test tools**: loopback self-test, test files, tuning tone (a steady tone at the AFSK
  tones, to set audio levels and check the radio keys; any key stops it), test message,
  CAT check, and WAV files: decode the packets in a recording (`atest`), or write typed
  packets to a new one (`gen_packets`).

Menu keys: Up/Down (j/k), PgUp/PgDn, Home/End, Enter to choose, Esc/q to go back, or type
an item's number.

`RadioConnectorCli file.wav` decodes the packets in `file.wav`, then quits.

Several copies can run at once, e.g. one per radio or sound card. Each one uses its own
settings file: the first uses `settings.ini`, a second `settings-2.ini`, a third
`settings-3.ini`, and so on. The menu shows which file it's using when it isn't
`settings.ini`. A new file starts as a copy of `settings.ini` with a different SSID
(N0CALL-2, N0CALL-3, ...; -1 is RadioConnector's), so each copy is a different station.
`--settings FILE` picks the file instead.

#### Sessions

Set **Packet speed** and **Callsign**, then **Mode**:

- **Duplex**: UI packets, chat style. Enter sends a line as a packet, once, with no ack,
  and every packet heard on the frequency is shown. For testing and watching the channel.
- **Connected**: AX.25 connected-mode chat, the way a user reaches a RadioConnector
  ([docs/connected-mode.md](docs/connected-mode.md#radioconnectorcli)). Text is acked and
  resent until it gets through, and only your sessions are shown. Also a range test
  between two RadioConnectorCli stations.

Both need the Rx and Tx sound cards and PTT set up.

**Send to** (Duplex) picks who typed packets are addressed to: empty for `CQ`
(everyone), or a station's callsign, e.g. a RadioConnector's `N0CALL-1` when testing it.
Packets addressed to someone other than `CQ` show as `FROM>TO:`.

Esc waits for queued packets, then stops; Esc again stops now.

**FX.25** (Auto unless changed) adds Reed-Solomon error correction,
so packets survive some bad bytes. Stations without FX.25 still decode them as plain AX.25.
Receiving always decodes FX.25. Packets are shorter with it on (179 bytes of text, or 139
with 64 check bytes), so long lines take more packets.

**Transmit delay** (500 ms unless changed) is how long direwolf sends flags after keying
up, before the packet, while the radio comes up to power (direwolf's TXDELAY). If the
station at the other end misses the start of your packets, raise it: Bluetooth radios and
VOX often need 750 ms or more. direwolf's config is written fresh at every start, so set
it here, not in a `direwolf.conf`.

### RadioConnector

`RadioConnector --main 127.0.0.1:9000` runs headless: it connects one radio (direwolf,
AX.25 with FX.25) to the main program, which sends it text and config as JSON lines over
TCP. Any packet station can connect with **AX.25 connected mode** and chat with the main
program, several at once, with long replies sent a page at a time
([docs/connected-mode.md](docs/connected-mode.md)).

- It dials the main program and retries every 5 s; the radio keeps running meanwhile. Pick a
  port nothing else uses (8080 is often a web server's).
- It keeps its own settings in `connector.ini`, next to `settings.ini`, so it is a different
  station from RadioConnectorCli. The first run copies `settings.ini` and takes the next SSID
  as its callsign (N0CALL becomes N0CALL-1). Edit the file by hand, or let the main program
  send config.
- It needs a callsign and both sound cards before direwolf starts.

| Option | |
| --- | --- |
| `--main host:port` | Where the main program listens (saved in `connector.ini`) |
| `--settings file` | Use this settings file instead of `connector.ini` |
| `--verbose` | Log every session frame (`agw out`, `agw in`) and every JSON line (`to main`, `from main`) |
| `--direwolf` | Also show direwolf's own output, as `direwolf:` lines |
| `--quit-on-disconnect` | Exit when the main program hangs up (RadioTests uses it) |

Logs go to stderr.

### Docs

- [docs/radios.md](docs/radios.md): radios that are tested, that should work, and setups
  that won't.
- [docs/connected-mode.md](docs/connected-mode.md): connected-mode sessions, how several
  users share the channel, and RadioConnectorCli's Connected mode for testing by hand.
- [docs/connector-protocol.md](docs/connector-protocol.md): the Radio Connector and its
  protocol, in overview.
- [docs/json-format.md](docs/json-format.md): the JSON format, message by message and field
  by field, for writing a main program.
- [docs/bluetooth-audio.md](docs/bluetooth-audio.md): audio over Bluetooth to the radios
  that support it, how to set it up, and how the radio's protocol works.

### Installing direwolf

- **Windows**: the build compiles direwolf with Hamlib (the official build has no CAT
  PTT). Put the source in `direwolf/`
  (`git clone --branch 1.8.1 https://github.com/wb2osz/direwolf.git`), or build with
  `-DRTC_DIREWOLF=OFF`. Allow direwolf through Windows Firewall if asked.
- **Linux**: `sudo apt install direwolf` (see `src/APT-PACKAGES.txt`).
- **macOS**: `brew install direwolf pulseaudio && brew services start pulseaudio`.
  direwolf uses the default sound devices; PulseAudio plays the tuning tone.

The programs look on the PATH, in `/usr/local/bin`, `/opt/homebrew/bin` and next to
themselves.

### Building

CMake 4.1 or newer and a C++20 compiler (MinGW on Windows).

```
cmake -S . -B build
cmake --build build
```

- Windows: Hamlib's Windows build goes in `hamlib-w64-4.7.2/` and direwolf's source in
  `direwolf/`. The programs, direwolf and Hamlib's DLLs end up together in `build/bin`.
- Linux and macOS: Hamlib installed (`make install`, or Homebrew). `src/APT-PACKAGES.txt`
  lists the Ubuntu packages.
- Options: `-DRTC_TESTS=OFF` skips RadioTests, `-DRTC_DIREWOLF=OFF` (Windows) skips
  building direwolf, `-DRTC_BLUETOOTH=OFF` leaves out Bluetooth audio (on Linux it needs
  `libbluetooth-dev` and `libpulse-dev`, and is left out with a warning without them).
- `-DRTC_STATIC=ON` (Linux and macOS; what the releases use) links Hamlib, libusb and the
  C++ runtime into the programs. It needs a static Hamlib that pkg-config finds:
  `./configure --disable-shared --enable-static --without-cxx-binding`, then
  `make install`; `.github/workflows/build.yml` does this. Windows builds always use the
  bundled Hamlib DLLs.

Source layout:

| Folder | What's in it |
| --- | --- |
| `src/` | RadioConnectorCli (`main.cpp`; the Duplex and Connected screens in `session.cpp`), settings, the OS layer (`backend.cpp`), test cases |
| `src/Radio/` | direwolf's command lines and config, test audio (`fsk.cpp`), and CAT radio control |
| `src/Connector/` | RadioConnector (`radio_connector.cpp`), connected-mode sessions (`sessions.cpp`) over direwolf's AGW port (`agw.cpp`), the JSON envelope (`connector.cpp`), config, and AX.25/KISS (`packet.cpp`) |
| `src/Bluetooth/` | Bluetooth audio: the radio's protocol (`bt_protocol.cpp`), connections (`rfcomm_*`), the virtual sound card (`virtual_audio_*`) and the bridge (`bt_audio.cpp`) |
| `src/third_party/` | nlohmann/json and Google's libsbc, copied in |
| `src/tests/` | RadioTests (`radio_tests.cpp`) and the test audio |
| `docs/` | The protocol and testing docs |

### Tests

`RadioTests` runs every test with no arguments (`ctest` runs it too). A group that can't
run on this computer is skipped, not failed:

- `unit`: settings, command lines, AX.25/KISS, FX.25, scoring, the connector's config and
  connected-mode sessions, the Bluetooth protocol
- `audio`: decodes each packet recording in `src/tests/audio` (see its README) and checks
  the text. Skipped without atest
- `cat`: frequency, mode and PTT through Hamlib's dummy rig. `--cat-port COM5` tests a
  real radio instead (it is keyed for half a second)
- `air`: starts RadioConnector and a stand-in station on the sound cards from
  `settings.ini` (e.g. VB-Cable looped back), then checks a session in with a two-page
  reply, two users at once, and a session the connector opens. Skipped without sound
  cards, or when PTT keys a radio (CAT, RTS, DTR or AIOC)

`--only unit,cat` or `--skip air` picks groups; `RadioTests --help` lists every option.
To regenerate the test audio: `RadioTests --generate ../src/tests/audio` from the build folder.
