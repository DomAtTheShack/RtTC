// Main menu, settings and test tools.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ansi.hpp"
#include "backend.h"
#include "Radio/aioc.h"
#include "Radio/cat.h"
#include "Radio/fsk.h"
#include "Radio/modem.h"
#include "options.h"
#include "Connector/connector_config.h"
#include "Connector/packet.h"
#include "session.h"
#include "settings.h"
#include "testcases.h"
#ifdef RTC_BLUETOOTH
#include "Bluetooth/bt_audio.h"
#include "Bluetooth/bt_protocol.h"
#endif

using std::string;
using std::vector;
namespace fs = std::filesystem;

static_assert(defaultRigModel == RIG_MODEL_X6100, "settings.h's default radio must be the X6100");

namespace {

const char* const programName = "Radio to Text Connector";

const ansi::Color titleColor = ansi::Color::BrightRed;
const ansi::Color infoColor  = ansi::Color::BrightGreen;
const ansi::Color warnColor  = ansi::Color::BrightYellow;
const ansi::Color failColor  = ansi::Color::BrightRed;
const ansi::Color dimColor   = ansi::Color::Gray;

// Helpers

void showMessage(const string& title, const string& text) {
    ansi::clearScreen();
    ansi::print(titleColor, ansi::Bold, "%s\n\n", title.c_str());
    std::printf("%s\n", text.c_str());
    ansi::waitKey();
}

// settings.ini, settings-2.ini, ... (whichever no other running copy has), or --settings
string settingsFile;

void save(const Settings& settings) {
    if (!saveSettingsTo(settings, settingsFile))
        showMessage("Settings not saved", "Could not write " + settingsFile);
}

// Sets settingsFile and loads it. given: --settings, else "". Each running copy locks its
// file (file.lock), so a second copy takes settings-2.ini, a third settings-3.ini, ...
// A new one starts as a copy of settings.ini with the next free SSID (-1 is the Radio
// Connector's), so it's a different station
Settings loadOwnSettings(const string& given) {
    if (!given.empty()) {
        settingsFile = given;
        os::lockForLife(given + ".lock");   // So an auto-picking copy skips it
        return loadSettingsFrom(given);
    }
    std::error_code ec;
    fs::create_directories(fs::path(settingsPath()).parent_path(), ec);
    for (int copy = 1; copy <= 15; copy++) {
        string path = settingsPath(copy);
        if (!os::lockForLife(path + ".lock")) continue;
        settingsFile = path;
        bool found = false;
        Settings settings = loadSettingsFrom(path, &found);
        if (found || copy == 1) return settings;
        settings = loadSettings();
        settings.callsign = packet::otherSsid(settings.callsign, copy);
        saveSettingsTo(settings, path);   // If it can't, save() says so later
        return settings;
    }
    // Locks unavailable: share settings.ini, as before
    settingsFile = settingsPath();
    return loadSettings();
}

// Pads name so values line up in a column
string padded(const string& name, size_t width = 16) {
    return name.size() >= width ? name + " " : name + string(width - name.size(), ' ');
}

template <size_t N>
int nearestOption(const Option (&options)[N], float value) {
    int best = 0;
    for (int i = 1; i < static_cast<int>(N); i++)
        if (std::fabs(options[i].value - value) < std::fabs(options[best].value - value)) best = i;
    return best;
}

// Picks one of options into value. extra(i) adds a note to a label. false on Esc
template <size_t N>
bool pickOption(const string& title, const Option (&options)[N], float& value,
                const std::function<string(int)>& extra = {}, const string& subtitle = "") {
    int current = nearestOption(options, value);
    bool exact = std::fabs(options[current].value - value) < 0.01f;
    int choice = ansi::selectWith(title, static_cast<int>(N), [&](int i) {
        string label = options[i].label;
        if (extra) label += extra(i);
        if (exact && i == current) label += "   <- current";
        return label;
    }, current, subtitle);
    if (choice < 0) return false;
    value = options[choice].value;
    return true;
}

// Sound cards

vector<os::AudioDevice> findAudioDevices(os::AudioDirection direction) {
    ansi::clearScreen();
    ansi::print(dimColor, "Looking for sound cards...\n");
    ansi::flush();
    return os::listAudioDevices(direction);
}

// false on Esc or if there are none
bool pickAudioDevice(Settings& settings, os::AudioDirection direction,
                     const vector<os::AudioDevice>& devices, const string& note = "") {
    bool playback = direction == os::AudioDirection::Playback;
    string& name = playback ? settings.txAudio : settings.rxAudio;
    string& desc = playback ? settings.txAudioDesc : settings.rxAudioDesc;
    if (devices.empty()) {
        showMessage("No sound cards", string("No ") + (playback ? "playback" : "capture") +
                    " devices found.");
        return false;
    }
    int current = 0;
    for (int i = 0; i < static_cast<int>(devices.size()); i++)
        if (devices[i].name == name) current = i;
    const string& subtitle = note;
    int choice = ansi::selectWith(
        playback ? "Tx sound card" : "Rx sound card",
        static_cast<int>(devices.size()), [&](int i) {
            string label = devices[i].desc + "  [" + devices[i].name + "]";
            if (devices[i].name == name) label += "   <- current";
            return label;
        }, current, subtitle);
    if (choice < 0) return false;
    name = devices[choice].name;
    desc = devices[choice].desc;
    return true;
}

// The saved sound card, asking for one if it's missing. nullopt on Esc
std::optional<string> resolveAudioName(Settings& settings, bool playback) {
    auto direction = playback ? os::AudioDirection::Playback : os::AudioDirection::Capture;
    const string& name = playback ? settings.txAudio : settings.rxAudio;
    vector<os::AudioDevice> devices = findAudioDevices(direction);
    bool found = false;
    for (const auto& device : devices) found = found || device.name == name;
    if (!found) {
        string note = name.empty() ? "" : name + " not found";
        if (!pickAudioDevice(settings, direction, devices, note)) return std::nullopt;
        save(settings);
    }
    return playback ? settings.txAudio : settings.rxAudio;
}

// Bluetooth

// The radio's Bluetooth audio for one session (docs/bluetooth-audio.md). It never starts
// in builds without Bluetooth
class BluetoothSession {
public:
    // false (after saying why) if it can't connect
    bool start(const Settings& settings) {
#ifdef RTC_BLUETOOTH
        ansi::clearScreen();
        ansi::print(dimColor, "Connecting to %s over Bluetooth...\n",
                    settings.bluetoothName.empty() ? settings.bluetooth.c_str() : settings.bluetoothName.c_str());
        ansi::flush();
        auto say = [](const string& line) {
            ansi::print(dimColor, "%s\n", line.c_str());
            ansi::flush();
        };
        if (bridge.start(settings.bluetooth, settings.btPlayAudio, settings.btRecordAudio, say)) {
            setGain(settings.btRxGain, settings.btTxGain);
            return true;
        }
        showMessage("Bluetooth audio didn't start", bridge.error() +
                    "\n\nCheck the radio is on, paired and in range, and that nothing else (e.g. "
                    "HTCommander or a phone app) is connected to it.");
        return false;
#else
        (void)settings;
        showMessage("No Bluetooth", "This build has no Bluetooth audio. Turn it off in Settings.");
        return false;
#endif
    }

    // Percent, on what the radio hears and what the modems send
    void setGain(int rxPercent, int txPercent) {
#ifdef RTC_BLUETOOTH
        bridge.setGain(rxPercent, txPercent);
#else
        (void)rxPercent;
        (void)txPercent;
#endif
    }

    // Sound card names for the modems, or "" for the saved ones
    string capture() const {
#ifdef RTC_BLUETOOTH
        return bridge.modemCaptureDevice();
#else
        return "";
#endif
    }
    string playback() const {
#ifdef RTC_BLUETOOTH
        return bridge.modemPlaybackDevice();
#else
        return "";
#endif
    }

private:
#ifdef RTC_BLUETOOTH
    bt::BluetoothAudio bridge;
#endif
};

string bluetoothDescription(const Settings& s) {
    if (s.bluetooth.empty()) return "Off (sound cards)";
    return s.bluetoothName.empty() ? s.bluetooth : s.bluetoothName + "  [" + s.bluetooth + "]";
}

// Pick a paired radio, or turn Bluetooth audio off. false on Esc
bool pickBluetooth(Settings& settings) {
#ifdef RTC_BLUETOOTH
    ansi::clearScreen();
    ansi::print(dimColor, "Looking for paired Bluetooth devices...\n");
    ansi::flush();
    vector<bt::Device> devices = bt::pairedDevices();
    vector<string> options = {"Off (use sound cards)"};
    int current = 0;
    for (const bt::Device& device : devices) {
        if (device.address == settings.bluetooth) current = static_cast<int>(options.size());
        options.push_back(device.name + "  [" + device.address + "]");
    }
    options.push_back("Type an address...");
    int choice = ansi::select("Bluetooth radio", options, current,
                              "UV-Pro, UV-50Pro, GA-5WB, VR-N75/N76/N7500/N7600. Pair it first; radios list first");
    if (choice < 0) return false;
    if (choice == 0) {
        settings.bluetooth.clear();
        settings.bluetoothName.clear();
        return true;
    }
    if (choice == static_cast<int>(options.size()) - 1) {
        string address = settings.bluetooth;
        for (;;) {
            if (!ansi::inputLine("Radio's Bluetooth address", "e.g. 38:D2:00:01:C0:AB", address)) return false;
            string normalized = bt::normalizeAddress(address);
            if (!normalized.empty()) {
                settings.bluetooth = normalized;
                settings.bluetoothName.clear();
                break;
            }
            showMessage("Not an address", "Six pairs of hex digits, e.g. 38:D2:00:01:C0:AB");
        }
    } else {
        const bt::Device& device = devices[static_cast<size_t>(choice - 1)];
        settings.bluetooth = device.address;
        settings.bluetoothName = device.name;
    }
    // Sound keys the radio, so there's nothing else to key it with
    settings.ptt = PttType::Vox;
    return true;
#else
    (void)settings;
    return false;
#endif
}

// Windows: the virtual cable end the bridge plays the radio into, or records the modems from
void pickBluetoothCable(Settings& settings, bool play) {
    auto direction = play ? os::AudioDirection::Playback : os::AudioDirection::Capture;
    vector<os::AudioDevice> devices = findAudioDevices(direction);
    if (devices.empty()) {
        showMessage("No sound cards", "Install a virtual cable such as VB-Cable first.");
        return;
    }
    string& name = play ? settings.btPlayAudio : settings.btRecordAudio;
    int current = 0;
    for (int i = 0; i < static_cast<int>(devices.size()); i++)
        if (devices[i].name == name) current = i;
    int choice = ansi::selectWith(
        play ? "Cable the radio's audio goes into" : "Cable the transmit audio comes from",
        static_cast<int>(devices.size()), [&](int i) {
            string label = devices[i].desc + "  [" + devices[i].name + "]";
            if (devices[i].name == name) label += "   <- current";
            return label;
        }, current,
        play ? "e.g. CABLE Input. Set Rx sound card to its other end (CABLE Output)"
             : "e.g. CABLE-B Output. Set Tx sound card to its other end (CABLE-B Input)");
    if (choice >= 0) name = devices[choice].name;
}

// Radio

string radioDescription(int model) {
    string name = radioName(model);
    return name.empty() ? "Hamlib model " + std::to_string(model) : name;
}

string radioControlDescription(const Settings& settings) {
    if (!settings.hasCat()) return "None";
    string text = radioDescription(settings.rigModel) + " on " + settings.catPort;
    if (settings.rigModel != ownRigctldModel) text += " at " + std::to_string(settings.catBaud) + " baud";
    return text;
}

string pttDescription(const Settings& settings) {
    if (!settings.bluetooth.empty()) return "the radio keys itself on Bluetooth audio";
    switch (settings.ptt) {
        case PttType::Vox: return "VOX";
        case PttType::Cat: return "CAT command";
        case PttType::Rts:
        case PttType::Dtr: {
            string text = settings.ptt == PttType::Rts ? "RTS" : "DTR";
            if (settings.pttPort.empty()) return text + " on the CAT port (" + settings.catPort + ")";
            return text + " on " + settings.pttPort;
        }
        case PttType::Aioc:
            return settings.pttPort.empty() ? "AIOC cable" : "AIOC cable on " + settings.pttPort;
    }
    return "?";
}

// Opens what keys the radio. Leaves radio empty for VOX. false if it can't be opened
bool openRadio(const Settings& settings, std::unique_ptr<Cat>& radio) {
    try {
        radio = openPtt(settings);
        return true;
    } catch (const std::exception& e) {
        showMessage("Cannot open the radio", string(e.what()) + "\n\nPTT: " + pttDescription(settings) +
                    "\nRadio (CAT): " + radioControlDescription(settings) +
                    "\n\nCheck Settings > PTT and Radio (CAT).");
        return false;
    }
}

// On Windows direwolf is killed rather than stopped, so it may leave the radio keyed
void unkeyAfterDirewolf(const Settings& settings) {
    try {
        if (auto radio = openPtt(settings)) radio->setPtt(false);
    } catch (const std::exception& e) {
        ansi::print(failColor, "Couldn't unkey the radio (%s). Check it isn't transmitting!\n", e.what());
    }
}

// AIOC: finds the cable's serial port and sound card again, since they can change each
// time it's plugged in. false (after saying why) if it isn't there
bool findAiocCable(Settings& settings) {
    if (settings.ptt != PttType::Aioc) return true;
    ansi::clearScreen();
    ansi::print(dimColor, "Looking for the AIOC cable...\n");
    ansi::flush();
    if (string problem = aioc::useCable(settings, aioc::find()); !problem.empty()) {
        showMessage("AIOC cable not found", problem + "\n\nPlug it in, or change Settings > PTT.");
        return false;
    }
    save(settings);
    return true;
}

// Running

void printSessionHeader(const Settings& settings, bool onAir) {
    ansi::clearScreen();
    ansi::print(infoColor, ansi::Bold, "%s\n", describe(settings).c_str());
    if (onAir) {
        ansi::print(infoColor, "PTT: %s\n", pttDescription(settings).c_str());
        if (settings.ptt == PttType::Cat)
            ansi::print(infoColor, "Radio: %s\n", radioControlDescription(settings).c_str());
    }
    if (settings.operation == Operation::Connected) {
        if (settings.sendTo.empty()) ansi::print(infoColor, "Waiting for calls\n");
        else ansi::print(infoColor, "Connecting to %s\n", settings.sendTo.c_str());
    } else if (!settings.sendTo.empty()) {
        ansi::print(infoColor, "Sending to %s\n", settings.sendTo.c_str());
    }
    ansi::flush();
}

// "145.0500"
string mhzText(long long hz) {
    char text[32];
    std::snprintf(text, sizeof text, "%.4f", static_cast<double>(hz) / 1e6);
    return text;
}

// "145.0500 MHz", or "Leave the radio as it is" for 0
string frequencyDescription(long long hz) {
    return hz > 0 ? mhzText(hz) + " MHz" : "Leave the radio as it is";
}

// The frequency Connected mode tunes to, in MHz. false on Esc
bool pickFrequency(Settings& settings) {
    string text = settings.frequency > 0 ? mhzText(settings.frequency) : "";
    string prompt = "MHz, e.g. 145.050. Tuned over CAT at the start. Empty = leave the radio";
    for (;;) {
        if (!ansi::inputLine("Frequency", prompt, text)) return false;
        if (text.empty()) {
            settings.frequency = 0;
            return true;
        }
        char* end = nullptr;
        double mhz = std::strtod(text.c_str(), &end);
        if (end != text.c_str() && *end == '\0' && mhz >= 0.1 && mhz <= 10000) {
            settings.frequency = std::llround(mhz * 1e6);
            return true;
        }
        prompt = "\"" + text + "\" isn't a frequency in MHz, e.g. 145.050";
    }
}

// settings.sendTo: who Duplex addresses typed packets to (empty = CQ), or who Connected
// mode connects to at the start (empty = wait for calls). false on Esc
bool pickSendTo(Settings& settings) {
    bool connected = settings.operation == Operation::Connected;
    const char* title = connected ? "Connect to" : "Send to";
    string ifEmpty = connected ? "Empty = wait for calls" : "Empty = CQ (everyone)";
    string call = settings.sendTo;
    string prompt = "Callsign, e.g. a Radio Connector's N0CALL-1. " + ifEmpty;
    for (;;) {
        if (!ansi::inputLine(title, prompt, call)) return false;
        string normalized = packet::normalizeCallsign(call);
        if (call.empty() || !normalized.empty()) {
            settings.sendTo = normalized;
            return true;
        }
        prompt = "\"" + call + "\" isn't a valid callsign. " + ifEmpty;
    }
}

// false on Esc
bool pickCallsign(Settings& settings) {
    string call = settings.callsign;
    string prompt = "e.g. N0CALL or N0CALL-7";
    for (;;) {
        if (!ansi::inputLine("Callsign", prompt, call)) return false;
        string normalized = packet::normalizeCallsign(call);
        if (!normalized.empty()) {
            settings.callsign = normalized;
            return true;
        }
        prompt = "\"" + call + "\" isn't a valid callsign";
    }
}

// Who typed packets go to: a station for testing a Radio Connector, else everyone
string sendToAddress(const Settings& settings) {
    return settings.sendTo.empty() ? packet::groupAddress : settings.sendTo;
}

// Typed lines -> gen_packets input (direwolf monitor format)
TransmitFormat genPacketsFormat(const string& callsign, const string& destination, int fx25) {
    TransmitFormat format;
    format.name = "gen_packets";
    format.hideOutput = true;           // It echoes every packet
    size_t maxInfo = packet::maxInfoFor(fx25);
    format.format = [callsign, destination, maxInfo](const string& typed) {
        string out;
        size_t start = 0;
        while (start < typed.size()) {
            size_t end = typed.find('\n', start);
            if (end == string::npos) end = typed.size();
            string line = typed.substr(start, end - start);
            start = end + 1;
            if (line.empty()) continue;
            for (const string& packetLine : packet::monitorLines(callsign, destination, line, maxInfo))
                out += packetLine + "\n";
        }
        return out;
    };
    return format;
}

// direwolf truncates -c paths at 99 characters, so it runs from the config's folder
// Writes direwolf's config to the temp folder, named after this run's port so two copies
// of the program don't share one. Empty (after saying why) if it can't
fs::path writeDirewolfConfig(const Settings& settings, const string& rxName, const string& txName,
                             int kissPort, int agwPort = 0, int paclen = 0) {
    fs::path config = fs::temp_directory_path() /
        ("radio-to-text-direwolf-" + std::to_string(agwPort > 0 ? agwPort : kissPort) + ".conf");
    std::ofstream out(config, std::ios::trunc);
    bool cable = settings.ptt == PttType::Aioc;
    out << direwolfConfig(settings, direwolfDevice(rxName, false, cable), direwolfDevice(txName, true, cable),
                          kissPort, agwPort, paclen);
    if (!out) {
        ansi::print(failColor, "Could not write %s\n", config.string().c_str());
        return {};
    }
    return config;
}

// After a direwolf session: the radio unkeyed, the config gone, and why it failed if
// direwolf is missing. Returns ok
bool afterDirewolf(const Settings& settings, const fs::path& config, bool transmitted, bool ok) {
    if (transmitted && settings.keysRadio()) unkeyAfterDirewolf(settings);
    std::error_code ec;
    fs::remove(config, ec);
    if (!ok && !direwolfAvailable())
        ansi::print(warnColor, "direwolf not found at %s\n", os::direwolfPath());
    return ok;
}

// Duplex: direwolf with a KISS port, and the packet screen
bool runDirewolf(const Settings& settings, const string& rxName, const string& txName,
                 PacketSession session) {
    int kissPort = os::freeTcpPort();
    if (kissPort == 0) {
        ansi::print(failColor, "No free TCP port for direwolf.\n");
        return false;
    }
    fs::path config = writeDirewolfConfig(settings, rxName, txName, kissPort);
    if (config.empty()) return false;
    session.direwolfArgs = direwolfArgs(config.filename().string());
    session.workingDir = config.parent_path().string();
    session.kissPort = kissPort;
    session.callsign = settings.callsign;
    session.destination = sendToAddress(settings);
    session.baud = settings.packetBaud;
    session.txDelayMs = settings.txDelayMs;
    session.turnaroundWaitMs = settings.turnaroundWaitMs;
    session.fx25 = settings.fx25;
    bool transmitted = false;
    bool ok = runPacketSession(session, transmitted);
    return afterDirewolf(settings, config, transmitted, ok);
}

// Connected mode: direwolf with an AGW port, and the chat screen
bool runConnectedDirewolf(Settings& settings, const string& rxName, const string& txName,
                          BluetoothSession& bluetooth) {
    int agwPort = os::freeTcpPort();
    if (agwPort == 0) {
        ansi::print(failColor, "No free TCP port for direwolf.\n");
        return false;
    }
    fs::path config = writeDirewolfConfig(settings, rxName, txName, 0, agwPort,
                                          connector::direwolfPaclen(settings));
    if (config.empty()) return false;
    ConnectedSession session;
    session.direwolfArgs = direwolfArgs(config.filename().string());
    session.workingDir = config.parent_path().string();
    session.agwPort = agwPort;
    session.callsign = settings.callsign;
    session.connectTo = settings.sendTo;
    session.fx25 = settings.fx25;
    session.maxSessions = settings.maxSessions;
    if (settings.usesBluetooth()) {
        session.bluetooth = true;
        session.btRxGain = settings.btRxGain;
        session.btTxGain = settings.btTxGain;
        session.setGain = [&](int rx, int tx) {
            bluetooth.setGain(rx, tx);
            settings.btRxGain = rx;
            settings.btTxGain = tx;
            save(settings);
        };
    }
    bool transmitted = false;
    bool ok = runConnectedSession(session, transmitted);
    return afterDirewolf(settings, config, transmitted, ok);
}

// Tunes the radio to settings.frequency over CAT. false (after saying why) if it can't
bool tuneRadio(const Settings& settings) {
    if (settings.frequency <= 0) return true;
    string frequency = frequencyDescription(settings.frequency);
    if (!settings.hasCat()) {
        showMessage("Can't tune the radio", "Frequency is " + frequency + ", but there's no CAT port. "
                    "Set Settings > Radio (CAT), or clear Frequency and tune the radio yourself.");
        return false;
    }
    ansi::clearScreen();
    ansi::print(dimColor, "Tuning to %s...\n", frequency.c_str());
    ansi::flush();
    try {
        Cat radio(settings.rigModel, settings.catPort, settings.catBaud);
        radio.setFrequency(static_cast<double>(settings.frequency));
        return true;
    } catch (const std::exception& e) {
        showMessage("Can't tune the radio", string(e.what()) + "\n\nRadio (CAT): " +
                    radioControlDescription(settings) + "\n\nCheck Settings > Radio (CAT) and Frequency.");
        return false;
    }
}

// false (after saying why) if direwolf can't reach a Bluetooth radio here
bool bluetoothPacketWorks(const Settings& settings) {
#ifdef __APPLE__
    if (settings.usesBluetooth()) {
        // Homebrew's direwolf uses the Mac's default sound card, which the bridge can't be
        showMessage("Bluetooth packet needs Linux or Windows",
                    "On macOS direwolf always uses the system's default sound card, so it can't "
                    "reach the Bluetooth radio.");
        return false;
    }
#else
    (void)settings;
#endif
    return true;
}

// The sound cards for direwolf: the Bluetooth bridge's, or the saved ones (asking for any
// that are missing). false (after saying why) on Esc or a problem
bool radioAudio(Settings& settings, BluetoothSession& bluetooth, string& rxName, string& txName) {
    if (!findAiocCable(settings)) return false;
    if (!bluetoothPacketWorks(settings)) return false;
    if (settings.usesBluetooth() && !bluetooth.start(settings)) return false;
    rxName = bluetooth.capture();
    txName = bluetooth.playback();
    for (bool playback : {false, true}) {
        string& name = playback ? txName : rxName;
        if (!name.empty()) continue;
        auto saved = resolveAudioName(settings, playback);
        if (!saved) return false;
        name = *saved;
    }
    return true;
}

// Another direwolf on the same radio answers every frame too, and the two keep resetting
// each other's links. It's only asked about: another copy of this program on another radio
// is fine. false on Back or Esc
bool noOtherDirewolf() {
    int running = os::countProcesses("direwolf");
    if (running <= 0) return true;
    string title = running == 1 ? "Another direwolf is already running"
                                : std::to_string(running) + " other direwolfs are already running";
    int choice = ansi::select(title, {"Back (close it first)", "Start anyway (it's on a different radio)"}, 0,
                              "On the same radio both answer every frame and keep resetting the links");
    return choice == 1;
}

// false on Esc
bool haveCallsign(Settings& settings) {
    if (!settings.callsign.empty()) return true;
    if (!pickCallsign(settings)) return false;
    save(settings);
    return true;
}

void startConnectedSession(Settings& settings) {
    if (!haveCallsign(settings) || !pickSendTo(settings)) return;
    if (!settings.sendTo.empty() && settings.sendTo == packet::normalizeCallsign(settings.callsign)) {
        showMessage("Can't connect", settings.sendTo + " is this station's own callsign.");
        return;
    }
    save(settings);
    if (!noOtherDirewolf()) return;
    if (!tuneRadio(settings)) return;
    BluetoothSession bluetooth;
    string rxName, txName;
    if (!radioAudio(settings, bluetooth, rxName, txName)) return;

    printSessionHeader(settings, true);
    if (settings.frequency > 0)
        ansi::print(infoColor, "Frequency: %s\n", frequencyDescription(settings.frequency).c_str());
    if (!runConnectedDirewolf(settings, rxName, txName, bluetooth))
        ansi::print(failColor, "\nStopped with an error.\n");
    ansi::waitKey();
}

// Duplex: UI packets sent and heard, chat style
void startPacketSession(Settings& settings) {
    if (!haveCallsign(settings) || !noOtherDirewolf()) return;
    BluetoothSession bluetooth;
    string rxName, txName;
    if (!radioAudio(settings, bluetooth, rxName, txName)) return;

    printSessionHeader(settings, true);
    PacketSession session;
    session.showReceived = true;
    session.canType = true;
    if (!runDirewolf(settings, rxName, txName, session))
        ansi::print(failColor, "\nStopped with an error.\n");
    ansi::waitKey();
}

void startSession(Settings& settings) {
    if (settings.operation == Operation::Connected) startConnectedSession(settings);
    else startPacketSession(settings);
}

// Settings menu

// false on Esc
bool pickRadio(Settings& settings) {
    const auto& radios = commonRadios();
    int count = static_cast<int>(radios.size());
    int current = count;   // "Other" unless it's in the list
    for (int i = 0; i < count; i++)
        if (static_cast<int>(radios[i].model) == settings.rigModel) current = i;
    int choice = ansi::selectWith("Radio", count + 1, [&](int i) {
        string label = i < count ? string(radios[i].name) : "Other...";
        if (i < count) label = padded(label, 30) + "[" + std::to_string(radios[i].model) + "]";
        if (i == current) label += "   <- current";
        return label;
    }, current);
    if (choice < 0) return false;
    if (choice < count) {
        settings.rigModel = static_cast<int>(radios[choice].model);
        return true;
    }
    string number = std::to_string(settings.rigModel);
    string prompt = "Model number from `rigctl -l`";
    for (;;) {
        if (!ansi::inputLine("Hamlib model", prompt, number)) return false;
        int model = std::atoi(number.c_str());
        if (model > 0 && !radioName(model).empty()) {
            settings.rigModel = model;
            return true;
        }
        prompt = "Unknown model \"" + number + "\"";
    }
}

// The radio's control (CAT) port, model and speed. Esc leaves them as they were
void pickRadioControl(Settings& settings) {
    vector<string> ports = os::listSerialPorts();
    vector<string> labels = {"None"};
    for (const string& port : ports) labels.push_back(port);
    labels.emplace_back("Type a port...");
    int current = settings.catPort.empty() ? 0 : static_cast<int>(labels.size()) - 1;
    for (int i = 0; i < static_cast<int>(ports.size()); i++)
        if (ports[i] == settings.catPort) current = i + 1;

    int choice = ansi::selectWith("Radio CAT port", static_cast<int>(labels.size()), [&](int i) {
        string label = labels[i];
        if (i == current) label += "   <- current";
        return label;
    }, current);
    if (choice < 0) return;

    Settings changed = settings;
    string port;
    if (choice == static_cast<int>(labels.size()) - 1) {
        port = settings.catPort;
        if (!ansi::inputLine("CAT port", "e.g. COM5, /dev/ttyACM0, or localhost:4532 for your own rigctld. "
                             "Empty = none", port)) return;
    } else if (choice > 0) {
        port = ports[choice - 1];
    }
    if (!port.empty()) {
        if (!pickRadio(changed)) return;
        if (changed.rigModel != ownRigctldModel) {
            float baud = static_cast<float>(changed.catBaud);
            if (!pickOption("CAT baud rate", catBaudRates, baud)) return;
            changed.catBaud = static_cast<int>(baud);
        }
    }
    changed.catPort = port;
    // Keying through a radio that's gone falls back to VOX
    if (!changed.hasCat() && changed.ptt == PttType::Cat) changed.ptt = PttType::Vox;
    if (!changed.hasCat() && changed.keysRadio() && changed.pttPort.empty() && changed.ptt != PttType::Aioc)
        changed.ptt = PttType::Vox;
    if (string problem = pttProblem(changed); !problem.empty()) {
        showMessage("Can't use that", "PTT is " + pttDescription(changed) + ", and " + problem + ".");
        return;
    }
    settings = changed;
}

// The serial port for RTS/DTR keying. "" = the CAT port. false on Esc
bool pickKeyingPort(Settings& settings, PttType type) {
    string line = type == PttType::Rts ? "RTS" : "DTR";
    vector<string> labels, values;
    if (settings.hasCat() && settings.rigModel != ownRigctldModel) {
        labels.push_back("The radio's CAT port (" + settings.catPort + ")");
        values.emplace_back();
    }
    for (const string& port : os::listSerialPorts()) {
        if (port == settings.catPort) continue;
        labels.push_back(port);
        values.push_back(port);
    }
    labels.emplace_back("Type a port...");
    int current = 0;
    for (int i = 0; i < static_cast<int>(values.size()); i++)
        if (values[i] == settings.pttPort) current = i;

    // Radios with two USB ports (e.g. FTDX3000) key on the second: Yaesu calls it "Standard"
    int choice = ansi::selectWith(line + " on which port? (two-port radios: the second, e.g. Yaesu's "
                                  "Standard port)", static_cast<int>(labels.size()), [&](int i) {
        return labels[i];
    }, current);
    if (choice < 0) return false;
    if (choice < static_cast<int>(values.size())) {
        settings.pttPort = values[choice];
        return true;
    }
    string port = settings.pttPort;
    if (!ansi::inputLine("PTT port", "e.g. COM6 or /dev/ttyUSB1", port) || port.empty()) return false;
    settings.pttPort = port;
    return true;
}

void pickPtt(Settings& settings) {
    vector<string> labels = {
        "VOX                the radio keys itself when it hears audio",
        "CAT command        sent over the radio's CAT port",
        "RTS line           on a serial port (e.g. Digirig, IC-7300 USB SEND, FTDX3000 Standard port)",
        "DTR line           on a serial port",
        "AIOC cable         USB cable for K-plug radios (e.g. Baofeng): its PTT and sound card",
    };
    int choice = ansi::select("PTT", labels, static_cast<int>(settings.ptt));
    if (choice < 0) return;
    Settings changed = settings;
    changed.ptt = static_cast<PttType>(choice);
    if (changed.ptt == PttType::Aioc) {
        ansi::clearScreen();
        ansi::print(dimColor, "Looking for the AIOC cable...\n");
        ansi::flush();
        if (string problem = aioc::useCable(changed, aioc::find()); !problem.empty()) {
            showMessage("AIOC cable not found", problem);
            return;
        }
        if (string problem = pttProblem(changed); !problem.empty()) {
            showMessage("Can't use that", "PTT " + pttDescription(changed) + ": " + problem + ".");
            return;
        }
        showMessage("AIOC cable found",
                    "PTT: " + changed.pttPort + "\nRx sound card: " + changed.rxAudioDesc +
                    "\nTx sound card: " + changed.txAudioDesc +
                    "\n\nThey're found again each time you start, in case they change.");
        settings = changed;
        return;
    }
    if (changed.ptt == PttType::Cat && !changed.hasCat()) {
        pickRadioControl(changed);
        if (!changed.hasCat()) return;
        changed.ptt = PttType::Cat;
    }
    if (changed.ptt == PttType::Rts || changed.ptt == PttType::Dtr) {
        if (!pickKeyingPort(changed, changed.ptt)) return;
    } else {
        changed.pttPort.clear();
    }
    if (string problem = pttProblem(changed); !problem.empty()) {
        showMessage("Can't use that", "PTT " + pttDescription(changed) + ": " + problem + ".");
        return;
    }
    settings = changed;
}

void pickPacketBaud(Settings& settings) {
    const int speeds[] = {1200, 300, 9600};
    vector<string> labels;
    int current = 0;
    for (int i = 0; i < 3; i++) {
        labels.push_back(padded(std::to_string(speeds[i]) + " baud", 11) + packetModemName(speeds[i]));
        if (speeds[i] == settings.packetBaud) current = i;
    }
    int choice = ansi::select("Packet speed", labels, current);
    if (choice >= 0) settings.packetBaud = speeds[choice];
}

void pickFx25(Settings& settings) {
    const int modes[] = {0, 1, 16, 32, 64};
    const char* notes[] = {
        "plain AX.25",
        "16-64 check bytes, picked by packet size",
        "fixes up to 8 bad bytes",
        "fixes up to 16 bad bytes",
        "fixes up to 32 bad bytes, shortest packets",
    };
    vector<string> labels;
    int current = 0;
    for (int i = 0; i < 5; i++) {
        labels.push_back(padded(fx25Name(modes[i]), 16) + notes[i]);
        if (modes[i] == settings.fx25) current = i;
    }
    int choice = ansi::select("FX.25 error correction (receivers without it still decode)",
                              labels, current);
    if (choice >= 0) settings.fx25 = modes[choice];
}

void pickTxDelay(Settings& settings) {
    const int delays[] = {300, 500, 750, 1000, 1500, 2000};
    const char* notes[] = {
        "direwolf's default, for radios that key quickly",
        "most handhelds",
        "slow radios, Bluetooth radios",
        "",
        "",
        "",
    };
    vector<string> labels;
    int current = 1;   // A value from settings.ini that isn't listed starts on 500
    for (int i = 0; i < 6; i++) {
        labels.push_back(padded(std::to_string(delays[i]) + " ms", 10) + notes[i]);
        if (delays[i] == settings.txDelayMs) current = i;
    }
    int choice = ansi::select("Transmit delay: from keying up to the packet", labels, current,
                              "Raise it if the start of packets gets cut off");
    if (choice >= 0) settings.txDelayMs = delays[choice];
}

void pickTurnaroundWait(Settings& settings) {
    const int waits[] = {0, 300, 500, 750, 1000, 1500};
    const char* notes[] = {
        "answer as soon as the channel is quiet",
        "the other radio is usually back on receive",
        "for answering Bluetooth radios",
        "",
        "",
        "",
    };
    vector<string> labels;
    int current = 1;   // A value from settings.ini that isn't listed starts on 300
    for (int i = 0; i < 6; i++) {
        labels.push_back(padded(std::to_string(waits[i]) + " ms", 10) + notes[i]);
        if (waits[i] == settings.turnaroundWaitMs) current = i;
    }
    int choice = ansi::select("Turnaround wait: after the channel goes quiet, before transmitting",
                              labels, current, "Raise it if your replies to a station keep getting lost");
    if (choice >= 0) settings.turnaroundWaitMs = waits[choice];
}

string audioValue(const string& name, const string& desc) {
    if (name.empty()) return "(not set)";
    return desc.empty() ? name : desc + "  [" + name + "]";
}

void settingsMenu(Settings& settings) {
    struct Item {
        string name;
        string value;
        std::function<void()> change;
    };
    string cursorName;
    for (;;) {
        Settings& s = settings;
        vector<Item> items;
        items.push_back({"Mode", operationName(s.operation), [&] {
            int choice = ansi::select("Mode", {"Duplex       UI packets, chat style",
                                               "Connected    AX.25 connected-mode"},
                                      static_cast<int>(s.operation));
            if (choice >= 0) s.operation = static_cast<Operation>(choice);
        }});
        items.push_back({"Packet speed", std::to_string(s.packetBaud) + " baud  (" +
                         packetModemName(s.packetBaud) + ")", [&] { pickPacketBaud(s); }});
        items.push_back({"Callsign", s.callsign.empty() ? "(not set)" : s.callsign,
                         [&] { pickCallsign(s); }});
        if (s.operation == Operation::Connected) {
            items.push_back({"Connect to", s.sendTo.empty() ? "(wait for calls)" : s.sendTo,
                             [&] { pickSendTo(s); }});
            items.push_back({"Frequency", frequencyDescription(s.frequency), [&] { pickFrequency(s); }});
        } else {
            items.push_back({"Send to", s.sendTo.empty() ? "CQ (everyone)" : s.sendTo,
                             [&] { pickSendTo(s); }});
        }
        // For sending: direwolf decodes FX.25 whenever it hears it
        items.push_back({"FX.25", fx25Name(s.fx25), [&] { pickFx25(s); }});
        items.push_back({"Transmit delay", std::to_string(s.txDelayMs) + " ms", [&] { pickTxDelay(s); }});
        items.push_back({"Turnaround wait", std::to_string(s.turnaroundWaitMs) + " ms",
                         [&] { pickTurnaroundWait(s); }});
        if (bluetoothBuiltIn() || !s.bluetooth.empty())
            items.push_back({"Bluetooth radio", bluetoothDescription(s), [&] { pickBluetooth(s); }});
        // Bluetooth on Linux and macOS brings its own sound card. On Windows the modems use
        // virtual cables, and the bridge the cables' other ends
        bool bluetoothCard = !s.bluetooth.empty() && !bluetoothUsesSavedAudio();
        if (!s.bluetooth.empty() && bluetoothUsesSavedAudio()) {
            items.push_back({"Radio audio into", s.btPlayAudio.empty() ? "(not set)" : s.btPlayAudio,
                             [&] { pickBluetoothCable(s, true); }});
            items.push_back({"Transmit audio from", s.btRecordAudio.empty() ? "(not set)" : s.btRecordAudio,
                             [&] { pickBluetoothCable(s, false); }});
        }
        if (!bluetoothCard) {
            items.push_back({"Rx sound card", audioValue(s.rxAudio, s.rxAudioDesc), [&] {
                auto devices = findAudioDevices(os::AudioDirection::Capture);
                pickAudioDevice(s, os::AudioDirection::Capture, devices);
            }});
            items.push_back({"Tx sound card", audioValue(s.txAudio, s.txAudioDesc), [&] {
                auto devices = findAudioDevices(os::AudioDirection::Playback);
                pickAudioDevice(s, os::AudioDirection::Playback, devices);
            }});
        }
        if (s.bluetooth.empty()) {
            items.push_back({"PTT", pttDescription(s), [&] { pickPtt(s); }});
            items.push_back({"Radio (CAT)", radioControlDescription(s), [&] { pickRadioControl(s); }});
        }
        items.push_back({"Reset to defaults", "", [&] {
            if (ansi::select("Reset all settings?", {"Reset", "Cancel"}, 1) == 0)
                s = Settings{};
        }});
        items.push_back({"Back", "", {}});

        int cursor = 0;
        for (int i = 0; i < static_cast<int>(items.size()); i++)
            if (items[i].name == cursorName) cursor = i;
        int choice = ansi::selectWith("Settings", static_cast<int>(items.size()), [&](int i) {
            return items[i].value.empty() ? items[i].name : padded(items[i].name) + items[i].value;
        }, cursor);
        if (choice < 0 || !items[choice].change) return;
        cursorName = items[choice].name;
        items[choice].change();
        save(settings);
    }
}

// Test tools

void loopbackTest(const Settings& current) {
    Settings settings = current;
    ansi::clearScreen();
    ansi::print(titleColor, ansi::Bold, "Loopback self-test\n\n");
    string text = testMessage;
    fs::path path = fs::temp_directory_path() / "radio-to-text-selftest.wav";
    if (settings.callsign.empty()) settings.callsign = testCallsign;
    fsk::Audio audio = fsk::ax25(packetFrames(text, settings.callsign), settings.packetBaud, 48000);
    if (!fsk::writeWav(path.string(), audio)) {
        showMessage("Self-test failed", "Could not write " + path.string());
        return;
    }
    ansi::print(infoColor, "Settings: %s\n", describe(settings).c_str());
    ansi::print(infoColor, "Sent:     %s\n", text.c_str());
    ansi::print(dimColor, "Decoding...\n");
    ansi::flush();
    CaseResult result = decodeFile(settings, path.string(), text, 0.9f);
    std::error_code ec;
    fs::remove(path, ec);

    if (!result.error.empty()) {
        ansi::print(failColor, "\nFAIL: %s\n", result.error.c_str());
        if (!atestAvailable()) ansi::print(warnColor, "atest not found at %s\n", os::atestPath());
    } else {
        ansi::print(infoColor, "Decoded:  %s\n\n", result.decoded.c_str());
        ansi::print(result.passed ? infoColor : failColor, ansi::Bold, "%s  (%.0f%%)\n",
                    result.passed ? "PASS" : "FAIL", result.accuracy * 100);
    }
    ansi::waitKey();
}

void runStandardTests() {
    string dir = findTestAudioDir();
    if (dir.empty()) {
        showMessage("Test files not found", "No test-audio or tests/audio folder found.");
        return;
    }
    if (!atestAvailable()) {
        showMessage("atest not found", string("Install direwolf first (atest comes with it). Looked at ") +
                    os::atestPath());
        return;
    }
    ansi::clearScreen();
    ansi::print(titleColor, ansi::Bold, "Standard test files\n");
    ansi::print(dimColor, "%s\n\n", dir.c_str());
    int passed = 0;
    const auto& cases = standardCases();
    for (const AudioCase& c : cases) {
        ansi::print(dimColor, "  ....  %s", c.file.c_str());
        ansi::flush();
        CaseResult result = runCase(c, dir);
        ansi::clearLine();
        if (result.passed) passed++;
        ansi::print(result.passed ? infoColor : failColor, "  %s  ", result.passed ? "PASS" : "FAIL");
        ansi::print(dimColor, "%3.0f%%  ", result.accuracy * 100);
        std::printf("%-26s %s\n", c.file.c_str(), c.description.c_str());
        if (!result.error.empty()) ansi::print(failColor, "        %s\n", result.error.c_str());
        else if (!result.passed)
            ansi::print(failColor, "        decoded: %s\n", result.decoded.substr(0, 100).c_str());
    }
    int ran = static_cast<int>(cases.size());
    ansi::print(passed == ran ? infoColor : failColor, ansi::Bold, "\n%d of %d passed\n", passed, ran);
    ansi::waitKey();
}

// One packet with the test message, then stops
void packetTestMessage(Settings& settings) {
    if (!haveCallsign(settings)) return;
    BluetoothSession bluetooth;
    string rxName, txName;
    if (!radioAudio(settings, bluetooth, rxName, txName)) return;
    Settings onAir = settings;
    onAir.operation = Operation::Duplex;   // A UI packet, whatever the mode
    printSessionHeader(onAir, true);
    PacketSession session;
    session.showReceived = false;
    session.canType = true;
    session.text = testMessage;
    session.finishAfterText = true;
    if (!runDirewolf(onAir, rxName, txName, session))
        ansi::print(failColor, "\nStopped with an error.\n");
    ansi::waitKey();
}

// A steady tone for setting levels and checking the radio keys, until a key is pressed
void tuningTone(Settings& settings) {
    // AFSK's two tones, the centre between them, and a level-setting reference
    const struct { int hz; const char* label; } tones[] = {
        {1700, "1700 Hz  centre of 1200 baud (1200/2200)"},
        {1200, "1200 Hz  1200 baud mark"},
        {2200, "2200 Hz  1200 baud space"},
        {1700, "1700 Hz  centre of 300 baud HF (1600/1800)"},
        {1000, "1000 Hz  reference"},
    };
    vector<string> labels;
    for (const auto& t : tones) labels.emplace_back(t.label);
    int choice = ansi::select("Tuning tone", labels, settings.packetBaud == 300 ? 3 : 0,
                              "It transmits until a key is pressed");
    if (choice < 0) return;
    int hz = tones[choice].hz;
    if (!findAiocCable(settings)) return;

    BluetoothSession bluetooth;
    if (settings.usesBluetooth() && !bluetooth.start(settings)) return;
    string device = bluetooth.playback();
    if (device.empty()) {
        auto name = resolveAudioName(settings, true);
        if (!name) return;
        device = *name;
    }
    std::unique_ptr<Cat> radio;   // Keys PTT; empty for VOX and Bluetooth
    if (!openRadio(settings, radio)) return;

    printSessionHeader(settings, true);
    ansi::print(infoColor, "Tone: %d Hz  (any key stops)\n", hz);
    ansi::flush();

    // A second of whole cycles at 48 kHz (32 kHz for Bluetooth, its own rate), so it
    // loops without a click. Half scale: plenty for setting levels
    int rate = bluetooth.playback().empty() ? 48000 : 32000;
    fsk::Audio second = fsk::tone(static_cast<float>(hz), 1.0f, rate);
    vector<int16_t> samples(second.samples.size());
    for (size_t i = 0; i < samples.size(); i++)
        samples[i] = static_cast<int16_t>(std::lround(second.samples[i] * 32767));

    string error;
    {
        std::optional<PttGuard> keyed;
        try {
            if (radio) keyed.emplace(*radio);
        } catch (const std::exception& e) {
            error = string("can't key the radio: ") + e.what();
        }
        ansi::setRawMode(true);
        if (error.empty()) {
            error = os::playTone(device, samples, rate, [] {
                if (!os::waitForInput(0)) return true;
                os::readInputByte();
                return false;
            });
        }
        ansi::setRawMode(false);
    }
    // PttGuard swallows errors; this one must be seen
    if (radio) {
        try {
            radio->setPtt(false);
        } catch (const std::exception& e) {
            ansi::print(failColor, "Couldn't unkey the radio (%s). Check it isn't transmitting!\n", e.what());
        }
    }
    if (!error.empty()) ansi::print(failColor, "\n%s\n", error.c_str());
    else ansi::print(dimColor, "\nStopped.\n");
    ansi::waitKey();
}

void checkCat(const Settings& settings) {
    if (settings.catPort.empty()) {
        showMessage("No CAT port", "Set one in Settings > Radio (CAT).");
        return;
    }
    ansi::clearScreen();
    ansi::print(titleColor, ansi::Bold, "CAT check\n\n");
    ansi::print(dimColor, "Connecting to %s on %s...\n",
                radioDescription(settings.rigModel).c_str(), settings.catPort.c_str());
    ansi::flush();
    try {
        Cat radio(settings.rigModel, settings.catPort, settings.catBaud);
        double hz = radio.frequency();
        const char* mode = rig_strrmode(radio.mode());
        ansi::print(infoColor, ansi::Bold, "\nOK: ");
        ansi::print(infoColor, "%.3f MHz, %s\n", hz / 1e6, mode);
    } catch (const std::exception& e) {
        ansi::print(failColor, ansi::Bold, "\nFAIL: ");
        ansi::print(failColor, "%s\n", e.what());
    }
    ansi::waitKey();
}

// The last WAV file a tool used, offered next time
string lastWavFile;

// Asks for a WAV file's path. false on Esc or an empty answer
bool pickWavFile(const string& title, const string& prompt, string& file) {
    file = lastWavFile;
    if (!ansi::inputLine(title, prompt, file) || file.empty()) return false;
    lastWavFile = file;
    return true;
}

// Decodes the packets in a WAV file with atest. Asks for the file if it's empty
void decodeWavFile(const Settings& settings, string file = "") {
    if (file.empty() && !pickWavFile("Decode a WAV file", "Path to a WAV recording", file)) return;
    if (!os::fileReadable(file)) {
        showMessage("Can't decode", "Can't read " + file);
        return;
    }
    ansi::clearScreen();
    ansi::print(titleColor, ansi::Bold, "Decode a WAV file\n");
    ansi::print(infoColor, "%s\n", describe(settings).c_str());
    ansi::print(dimColor, "Decoding %s...\n\n", file.c_str());
    ansi::flush();
    string error;
    string packets = decodePacketsFile(settings, file, error);
    if (!error.empty()) {
        ansi::print(failColor, "%s\n", error.c_str());
        if (!atestAvailable()) ansi::print(warnColor, "atest not found at %s\n", os::atestPath());
    } else if (packets.empty()) {
        ansi::print(warnColor, "No packets found.\n");
    } else {
        ansi::print(infoColor, "%s", packets.c_str());
    }
    ansi::waitKey();
}

// Typed lines become packets in a WAV file, with gen_packets
void writeWavFile(Settings& settings) {
    if (!haveCallsign(settings)) return;
    string file;
    if (!pickWavFile("Write packets to a WAV file", "Path for the new WAV file", file)) return;
    if (os::fileExists(file)) {
        if (ansi::select(file + " already exists", {"Overwrite it", "Cancel"}) != 0) return;
        std::error_code ec;
        fs::remove(file, ec);
    }
    Settings toFile = settings;
    toFile.operation = Operation::Duplex;   // UI packets, whatever the mode
    printSessionHeader(toFile, false);
    ansi::print(infoColor, "Writing %s\n", file.c_str());
    bool ok = runFileTransmit(genPacketsArgs(settings, file),
                              genPacketsFormat(settings.callsign, sendToAddress(settings), settings.fx25));
    if (!ok) ansi::print(failColor, "\nStopped with an error.\n");
    ansi::waitKey();
}

void testMenu(Settings& settings) {
    int cursor = 0;
    for (;;) {
        vector<string> items = {
            "Loopback self-test",
            "Standard test files",
            "Tuning tone",
            "Test message",
            "CAT check",
            "Decode a WAV file",
            "Write packets to a WAV file",
            "Back",
        };
        int choice = ansi::select("Test tools", items, cursor, describe(settings));
        if (choice < 0 || choice == 7) return;
        cursor = choice;
        switch (choice) {
            case 0: loopbackTest(settings);         break;
            case 1: runStandardTests();             break;
            case 2: tuningTone(settings);           break;
            case 3: packetTestMessage(settings);    break;
            case 4: checkCat(settings);             break;
            case 5: decodeWavFile(settings);        break;
            case 6: writeWavFile(settings);         break;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    string wavFile, givenSettings;
    bool badArgs = false;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--settings" && i + 1 < argc) givenSettings = argv[++i];
        else if (arg[0] != '-' && wavFile.empty()) wavFile = arg;
        else badArgs = true;
    }
    if (badArgs) {
        std::fprintf(stderr, "Usage: %s [--settings FILE] [file.wav]\n"
                     "  --settings FILE  use this settings file. By default it's settings.ini,\n"
                     "                   or settings-2.ini, -3, ... when other copies are running\n"
                     "  file.wav         decode the packets in this file, then quit\n", argv[0]);
        return EXIT_FAILURE;
    }
    Settings settings = loadOwnSettings(givenSettings);

#ifndef _WIN32
    // Bigger PulseAudio buffer so direwolf's Tx audio doesn't gap (WSLg, virtual cables)
    setenv("PULSE_LATENCY_MSEC", "100", 0);
#endif

    if (!wavFile.empty()) {
        decodeWavFile(settings, wavFile);
        ansi::clearScreen();
        return EXIT_SUCCESS;
    }

    bool haveDirewolf = direwolfAvailable();
    // Shown when it isn't the usual settings.ini, so two copies can be told apart
    string settingsNote;
    if (settingsFile != settingsPath())
        settingsNote = "Settings: " + (givenSettings.empty()
                                           ? fs::path(settingsFile).filename().string()
                                           : settingsFile);
    int cursor = 0;
    for (;;) {
        string status = settingsNote;
        if (!haveDirewolf)
            status += (status.empty() ? "" : "   ") + string("direwolf not found at ") +
                      os::direwolfPath();
        vector<string> items = {
            "Start        " + describe(settings),
            "Settings",
            "Test tools",
            "Quit",
        };
        int choice = ansi::select(programName, items, cursor, status);
        if (choice < 0 || choice == 3) break;
        cursor = choice;
        switch (choice) {
            case 0: startSession(settings); break;
            case 1: settingsMenu(settings); break;
            case 2: testMenu(settings);     break;
        }
    }
    ansi::clearScreen();
    return EXIT_SUCCESS;
}
