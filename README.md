## Radio to Text Connector

Send and receive text over ham radio as AX.25 packets, through direwolf: 300, 1200 or
9600 baud, with FX.25 error correction. One packet per line, with your callsign and error
checking.

The minimodem version (RTTY and Bell202 as well) is kept on the `archive/minimodem`
branch. It's frozen and doesn't merge back.

PTT is VOX, a CAT command (Hamlib), or the RTS/DTR line of a serial port, which can be a
second port (e.g. the FTDX3000's Standard port). Common Icom, Yaesu, Kenwood, Elecraft and
Xiegu radios are listed; any other Hamlib model works by number, or through `rigctld`.

Radios with Bluetooth audio (BTech UV-Pro and UV-50Pro, Radioddity GA-5WB, Vero
VR-N75/N76/N7500/N7600) can skip the sound card and cable: set **Bluetooth radio** in
Settings ([docs/bluetooth-audio.md](docs/bluetooth-audio.md)).

### The programs

| Program | What it is |
| --- | --- |
| `RadioConnectorCli` | The menu program: pick settings, then send and receive text by hand. Also the tool for testing a connector over the air |
| `RadioConnector` | Headless: connects one radio to the suite's main program, with acks and retries ([below](#radioconnector)) |
| `RadioTests` | Every test, run with no arguments ([below](#tests)) |

### RadioConnectorCli

Run `RadioConnectorCli`:

- **Start** runs with the current settings.
- **Settings** changes them. They're saved to:
  - Windows: `%APPDATA%\RtTC\settings.ini`
  - macOS: `~/Library/Application Support/RtTC/settings.ini`
  - Linux: `~/.config/rttc/settings.ini`

  The first run copies the files from the old folder (`RTTY-Connector`, or
  `rtty-connector` on Linux), which is left as it was.
- **Test tools**: loopback self-test, test files, tuning tone (a steady tone at the AFSK
  tones, to set audio levels and check the radio keys; any key stops it), test message,
  CAT check.

Menu keys: Up/Down (j/k), PgUp/PgDn, Home/End, Enter to choose, Esc/q to go back, or type
an item's number.

`RadioConnectorCli [file.wav]`: `file.wav` is decoded (Receive) or written (Transmit).

#### Sessions

Set **Packet speed** and **Callsign**, then **Mode**:

- **Receive**: shows packets as they arrive.
- **Transmit**: Enter sends a packet, Tab finishes.
- **Duplex**: send and receive at once, chat style.

**Send to** (Transmit and Duplex) picks who typed packets are addressed to: empty for `CQ`
(everyone), or a station's callsign, e.g. a RadioConnector's `N0CALL-1` when testing it.
Packets addressed to someone other than `CQ` show as `FROM>TO:`.

Esc waits for queued packets, then stops; Esc again stops now. direwolf always needs the
Rx sound card. With an audio file, `atest` decodes and `gen_packets` writes.

**FX.25** (Transmit and Duplex, Auto unless changed) adds Reed-Solomon error correction,
so packets survive some bad bytes. Stations without FX.25 still decode them as plain AX.25.
Receiving always decodes FX.25. Packets are shorter with it on (179 bytes of text, or 139
with 64 check bytes), so long lines take more packets.

### RadioConnector

`RadioConnector --main 127.0.0.1:9000` runs headless: it connects one radio (direwolf,
AX.25 with FX.25) to the suite's main program, which sends it text and config as JSON
lines over TCP. The connector delivers the text to another station's connector, splitting
it into parts, with acks and retries, and reports each message's state. The main program
never deals with the radio: every connector speaks the same JSON, whatever it connects to.

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
| `--verbose` | Log every frame sent (`tx`) and heard (`rx`), and every JSON line (`to main`, `from main`) |
| `--direwolf` | Also show direwolf's own output, as `direwolf:` lines |
| `--quit-on-disconnect` | Exit when the main program hangs up (RadioTests uses it) |

Logs go to stderr.

### Docs

- [docs/connector-protocol.md](docs/connector-protocol.md): the protocol. JSON between the
  main program and a connector, and the `RC1` air format between connectors.
- [docs/ack-guide.md](docs/ack-guide.md): how the acks work, when each is sent, and the code
  that does it.
- [docs/rc1-test-messages.md](docs/rc1-test-messages.md): `RC1` lines to type in
  RadioConnectorCli to test a RadioConnector's acks by hand.
- [docs/bluetooth-audio.md](docs/bluetooth-audio.md): audio over Bluetooth to UV-Pro-style
  radios, how to set it up, and how the radio's protocol works.

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
| `src/` | RadioConnectorCli (`main.cpp`, `session.cpp`), settings, the OS layer (`backend.cpp`), test cases |
| `src/Radio/` | direwolf's command lines and config, test audio (`fsk.cpp`), and CAT radio control |
| `src/Connector/` | RadioConnector (`radio_connector.cpp`), its ack engine (`connector.cpp`), config, and AX.25/KISS (`packet.cpp`) |
| `src/Bluetooth/` | Bluetooth audio: the radio's protocol (`bt_protocol.cpp`), connections (`rfcomm_*`), the virtual sound card (`virtual_audio_*`) and the bridge (`bt_audio.cpp`) |
| `src/third_party/` | nlohmann/json and Google's libsbc, copied in |
| `src/tests/` | RadioTests (`radio_tests.cpp`) and the test audio |
| `docs/` | The protocol and testing docs |

### Tests

`RadioTests` runs every test with no arguments (`ctest` runs it too). A group that can't
run on this computer is skipped, not failed:

- `unit`: settings, command lines, AX.25/KISS, FX.25, scoring, the connector's protocol and
  ack logic, the Bluetooth protocol
- `audio`: decodes each packet recording in `src/tests/audio` (see its README) and checks
  the text. Skipped without atest
- `cat`: frequency, mode and PTT through Hamlib's dummy rig. `--cat-port COM5` tests a
  real radio instead (it is keyed for half a second)
- `air`: starts RadioConnector and a stand-in station on the sound cards from
  `settings.ini` (e.g. VB-Cable looped back), then checks receiving, a missing part, and
  sending with acks. Skipped without sound cards, or when PTT keys a radio (CAT, RTS or DTR)

`--only unit,cat` or `--skip air` picks groups; `RadioTests --help` lists every option.
To regenerate the test audio: `RadioTests --generate ../src/tests/audio` from the build folder.
