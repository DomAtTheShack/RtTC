// Settings and their file.
#ifndef RTC_SETTINGS_H
#define RTC_SETTINGS_H

#include <string>
#include <vector>

enum class Operation { Receive, Transmit, Duplex };
// How the transmitter is keyed: VOX, a CAT command, or the RTS/DTR line of a serial port
// (which can be a second port, e.g. the FTDX3000's Standard port)
enum class PttType { Vox, Cat, Rts, Dtr };

// RIG_MODEL_X6100, without needing Hamlib's headers here
constexpr int defaultRigModel = 3087;
// RIG_MODEL_NETRIGCTL: the user's own rigctld, with catPort = host:port
constexpr int ownRigctldModel = 2;

struct Settings {
    Operation operation = Operation::Receive;
    int packetBaud = 1200;     // 300, 1200 or 9600
    int fx25 = 1;              // FX.25 transmit: 0 off, 1 auto, 16/32/64 check bytes.
                               // Receiving always decodes FX.25
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
    std::string pttPort;       // RTS/DTR: the serial port. Empty = the CAT port
    std::string audioFile;     // Empty = sound card
    // Bluetooth audio (docs/bluetooth-audio.md): the radio's address, empty = off. The modems
    // then use a virtual sound card instead of rxAudio/txAudio, except on Windows, where
    // rxAudio/txAudio are the ends of virtual cables and the bridge uses the other ends,
    // btPlayAudio (it plays the radio into) and btRecordAudio (it records the modems from)
    std::string bluetooth;
    std::string bluetoothName;
    std::string btPlayAudio;
    std::string btRecordAudio;

    // Radio Connector only; RadioConnectorCli keeps them but doesn't use them
    std::vector<std::string> path;   // Digipeaters, e.g. WIDE1-1
    int retryTries = 4;              // Sends of a message before it fails
    int retryExtraWait = 10;         // Seconds to wait for an ack, on top of the airtime
    int holdForMain = 180;           // Seconds a received message waits for the main program
    std::string mainAddress;         // host:port of the main program
    // Connected mode (docs/connected-mode.md): the main way in. RC1 stays for now
    bool connectedEnabled = true;
    int maxSessions = 4;
    int idleMinutes = 10;            // 0 = never
    int pageBytes = 1024;            // 0 = no paging
    std::string busyText = "Busy, try again in a few minutes";

    // Interactive program only: who typed packets go to. Empty = CQ
    std::string sendTo;

    bool transmits() const { return operation != Operation::Receive; }
    bool receives()  const { return operation != Operation::Transmit; }
    bool hasCat()    const { return !catPort.empty(); }
    bool keysRadio() const { return ptt != PttType::Vox; }
    bool usesBluetooth() const { return !bluetooth.empty() && audioFile.empty(); }
    // The serial port RTS/DTR keying uses
    const std::string& keyingPort() const { return pttPort.empty() ? catPort : pttPort; }
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
// "vox", "cat", "rts", "dtr": the settings file and the connector's JSON
const char* pttKey(PttType ptt);
bool parsePttKey(const std::string& text, PttType& ptt);

#endif //RTC_SETTINGS_H