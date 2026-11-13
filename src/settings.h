// Settings and their file.
#ifndef RTC_SETTINGS_H
#define RTC_SETTINGS_H

#include <string>
#include <vector>

// RadioConnectorCli's Start. Duplex: UI packets sent and heard, chat style. Connected: AX.25
// connected-mode chat (docs/connected-mode.md), as a packet station that connects to a
// Radio Connector or another RadioConnectorCli
enum class Operation { Duplex, Connected };
// How the transmitter is keyed: VOX, a CAT command, the RTS/DTR line of a serial port
// (which can be a second serial port, on radios whose USB connection has two), or an AIOC
// cable (src/Radio/aioc.h: DTR on and RTS off on its serial port, which is found each run
// along with its sound card)
enum class PttType { Vox, Cat, Rts, Dtr, Aioc };

// RIG_MODEL_X6100, without needing Hamlib's headers here
constexpr int defaultRigModel = 3087;
// RIG_MODEL_NETRIGCTL: the user's own rigctld, with catPort = host:port
constexpr int ownRigctldModel = 2;

// direwolf's TXDELAY is in 10 ms steps, up to 255
constexpr int maxTxDelayMs = 2550;

struct Settings {
    Operation operation = Operation::Duplex;
    int packetBaud = 1200;     // 300, 1200 or 9600
    int fx25 = 1;              // FX.25 transmit: 0 off, 1 auto, 16/32/64 check bytes.
                               // Receiving always decodes FX.25
    int txDelayMs = 500;       // direwolf's TXDELAY: flags sent after keying, before the
                               // packet, while the radio comes up to power. 0 to 2550
    std::string callsign;
    std::string rxAudio;       // AudioDevice::name; empty = ask
    std::string txAudio;
    std::string rxAudioDesc;
    std::string txAudioDesc;
    // The radio's CAT port: a serial port, or host:port with ownRigctldModel. Empty = none
    std::string catPort;
    int catBaud = 19200;
    int rigModel = defaultRigModel;   // Hamlib model number
    PttType ptt = PttType::Vox;
    std::string pttPort;       // RTS/DTR: the serial port. Empty = the CAT port. AIOC: the
                               // cable's port when last found
    // Bluetooth audio (docs/bluetooth-audio.md): the radio's address, empty = off. The modems
    // then use a virtual sound card instead of rxAudio/txAudio, except on Windows, where
    // rxAudio/txAudio are the ends of virtual cables and the bridge uses the other ends,
    // btPlayAudio (it plays the radio into) and btRecordAudio (it records the modems from)
    std::string bluetooth;
    std::string bluetoothName;
    std::string btPlayAudio;
    std::string btRecordAudio;

    // Radio Connector only; RadioConnectorCli keeps them but doesn't use them
    std::vector<std::string> path;   // Digipeaters for session.open, e.g. WIDE1-1
    std::string mainAddress;         // host:port of the main program
    // Connected mode (docs/connected-mode.md)
    bool connectedEnabled = true;
    int maxSessions = 4;
    int idleMinutes = 10;            // 0 = never
    int pageBytes = 1024;            // 0 = no paging
    std::string busyText = "Busy, try again in a few minutes";

    // Interactive program only: who typed packets go to. Empty = CQ. Connected mode: who
    // to connect to at the start. Empty = wait for calls
    std::string sendTo;
    // Interactive program only, Connected mode: the radio is tuned here over CAT at the
    // start. Hz, 0 = leave it
    long long frequency = 0;

    bool hasCat()    const { return !catPort.empty(); }
    bool keysRadio() const { return ptt != PttType::Vox; }
    bool usesBluetooth() const { return !bluetooth.empty(); }
    // The serial port RTS/DTR or AIOC keying uses
    const std::string& keyingPort() const {
        return pttPort.empty() && ptt != PttType::Aioc ? catPort : pttPort;
    }
};

// Whether this build has Bluetooth audio, and whether the modems then use the saved sound
// cards (Windows: they're virtual cables) or a virtual sound card the bridge makes
bool bluetoothBuiltIn();
bool bluetoothUsesSavedAudio();

// "" if the PTT and CAT settings work together, else what's wrong
std::string pttProblem(const Settings& settings);

// key=value lines. Unknown keys and bad values keep the default
std::string formatSettings(const Settings& settings);
Settings parseSettings(const std::string& text);

//   Windows  %APPDATA%\RtTC\settings.ini
//   macOS    ~/Library/Application Support/RtTC/settings.ini
//   Linux    $XDG_CONFIG_HOME/rttc/settings.ini (~/.config by default)
// The first time, the files from the old folder (RTTY-Connector, rtty-connector on Linux)
// are copied in
std::string settingsPath();
// More copies of RadioConnectorCli running at once: settings-2.ini, settings-3.ini, ... in
// the same folder. 1 is settingsPath()
std::string settingsPath(int copy);
Settings loadSettings();
bool saveSettings(const Settings& settings);

// The Radio Connector keeps its own file, connector.ini, in the same folder, so it can
// run as a different station from RadioConnectorCli
std::string connectorSettingsPath();
// found is set to whether the file was there; a missing file gives the defaults
Settings loadSettingsFrom(const std::string& path, bool* found = nullptr);
bool saveSettingsTo(const Settings& settings, const std::string& path);

const char* operationName(Operation operation);
// "Off", "Auto", "16 check bytes", ...
std::string fx25Name(int fx25);
// "vox", "cat", "rts", "dtr", "aioc": the settings file and the connector's JSON
const char* pttKey(PttType ptt);
bool parsePttKey(const std::string& text, PttType& ptt);

#endif //RTC_SETTINGS_H