#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool toInt(const std::string& s, int low, int high, int& out) {
    char* end = nullptr;
    long value = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || value < low || value > high) return false;
    out = static_cast<int>(value);
    return true;
}

// Where the folder goes, and its name now and before the program was RtTC
struct ConfigLocation {
    fs::path base;
    const char* name;
    const char* oldName;
};

ConfigLocation configLocation() {
#ifdef _WIN32
    const wchar_t* appData = _wgetenv(L"APPDATA");
    return {appData ? fs::path(appData) : fs::path("."), "RtTC", "RTTY-Connector"};
#else
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::path(".");
  #ifdef __APPLE__
    return {base / "Library" / "Application Support", "RtTC", "RTTY-Connector"};
  #else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) base = fs::path(xdg);
    else base /= ".config";
    return {base, "rttc", "rtty-connector"};
  #endif
#endif
}

fs::path configDir() {
    static const fs::path dir = [] {
        ConfigLocation where = configLocation();
        fs::path now = where.base / where.name, before = where.base / where.oldName;
        // First run since the rename: copy (not move), so an older build still finds its files
        std::error_code ec;
        if (!fs::exists(now, ec) && fs::is_directory(before, ec)) {
            fs::create_directories(now, ec);
            for (const char* file : {"settings.ini", "connector.ini"})
                if (fs::exists(before / file, ec)) fs::copy_file(before / file, now / file, ec);
        }
        return now;
    }();
    return dir;
}

} // namespace

const char* operationName(Operation operation) {
    switch (operation) {
        case Operation::Duplex:    return "Duplex";
        case Operation::Connected: return "Connected";
    }
    return "?";
}

std::string fx25Name(int fx25) {
    if (fx25 <= 0) return "Off";
    if (fx25 == 1) return "Auto";
    return std::to_string(fx25) + " check bytes";
}

const char* pttKey(PttType ptt) {
    switch (ptt) {
        case PttType::Vox: return "vox";
        case PttType::Cat: return "cat";
        case PttType::Rts: return "rts";
        case PttType::Dtr: return "dtr";
        case PttType::Aioc: return "aioc";
    }
    return "vox";
}

bool parsePttKey(const std::string& text, PttType& ptt) {
    for (PttType type : {PttType::Vox, PttType::Cat, PttType::Rts, PttType::Dtr, PttType::Aioc}) {
        if (text == pttKey(type)) {
            ptt = type;
            return true;
        }
    }
    return false;
}

bool bluetoothBuiltIn() {
    // macOS has the code, but direwolf there only uses the default sound card, so nothing
    // can use the bridge's
#if defined(RTC_BLUETOOTH) && !defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

bool bluetoothUsesSavedAudio() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::string pttProblem(const Settings& s) {
    // The radio transmits while it gets audio over Bluetooth, and nothing else keys it
    if (!s.bluetooth.empty() && s.ptt != PttType::Vox) return "Bluetooth audio keys the radio itself: use VOX";
    switch (s.ptt) {
        case PttType::Vox:
            return "";
        case PttType::Cat:
            return s.hasCat() ? "" : "CAT PTT needs a radio control (CAT) port";
        case PttType::Rts:
        case PttType::Dtr:
            if (s.keyingPort().empty()) return "RTS/DTR PTT needs a serial port";
            if (s.pttPort.empty() && s.rigModel == ownRigctldModel)
                return "RTS/DTR PTT needs a serial port, not a rigctld address";
            return "";
        case PttType::Aioc:
            // The cable's port is found when it starts
            return "";
    }
    return "";
}

std::string formatSettings(const Settings& s) {
    std::ostringstream out;
    out << "# Radio to Text Connector settings\n";
    out << "operation=" << (s.operation == Operation::Connected ? "connected" : "duplex") << "\n";
    out << "packet_baud=" << s.packetBaud << "\n";
    out << "fx25=" << (s.fx25 <= 0 ? std::string("off") : s.fx25 == 1 ? std::string("auto")
                                                                         : std::to_string(s.fx25)) << "\n";
    out << "tx_delay=" << s.txDelayMs << "\n";
    out << "callsign=" << s.callsign << "\n";
    out << "rx_audio=" << s.rxAudio << "\n";
    out << "tx_audio=" << s.txAudio << "\n";
    out << "rx_audio_desc=" << s.rxAudioDesc << "\n";
    out << "tx_audio_desc=" << s.txAudioDesc << "\n";
    out << "cat_port=" << s.catPort << "\n";
    out << "cat_baud=" << s.catBaud << "\n";
    out << "rig_model=" << s.rigModel << "\n";
    out << "ptt=" << pttKey(s.ptt) << "\n";
    out << "ptt_port=" << s.pttPort << "\n";
    out << "bluetooth=" << s.bluetooth << "\n";
    out << "bluetooth_name=" << s.bluetoothName << "\n";
    out << "bt_play_audio=" << s.btPlayAudio << "\n";
    out << "bt_record_audio=" << s.btRecordAudio << "\n";
    std::string path;
    for (const std::string& hop : s.path) path += (path.empty() ? "" : ",") + hop;
    out << "path=" << path << "\n";
    out << "main=" << s.mainAddress << "\n";
    out << "connected=" << (s.connectedEnabled ? "on" : "off") << "\n";
    out << "max_sessions=" << s.maxSessions << "\n";
    out << "idle_minutes=" << s.idleMinutes << "\n";
    out << "page_bytes=" << s.pageBytes << "\n";
    out << "busy_text=" << s.busyText << "\n";
    out << "send_to=" << s.sendTo << "\n";
    out << "frequency=" << s.frequency << "\n";
    return out.str();
}

Settings parseSettings(const std::string& text) {
    Settings s;
    bool pttGiven = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));

        // receive and transmit were modes once; they load as duplex
        if (key == "operation") {
            if (value == "receive" || value == "transmit" || value == "duplex") s.operation = Operation::Duplex;
            if (value == "connected") s.operation = Operation::Connected;
        }
        // Keys that are gone still load, and are ignored: minimodem's (modem, protocol,
        // baud, centre, shift, squelch, quiet), audio_file (now the Test tools' WAV file
        // tools) and RC1's (retry_tries, retry_extra_wait, hold_for_main)
        else if (key == "packet_baud") {
            int baud = 0;
            if (toInt(value, 300, 9600, baud) && (baud == 300 || baud == 1200 || baud == 9600))
                s.packetBaud = baud;
        }
        else if (key == "fx25") {
            if (value == "off")  s.fx25 = 0;
            if (value == "auto") s.fx25 = 1;
            if (value == "16" || value == "32" || value == "64") s.fx25 = std::stoi(value);
        }
        else if (key == "tx_delay")     toInt(value, 0, maxTxDelayMs, s.txDelayMs);
        else if (key == "callsign")     s.callsign = value;
        else if (key == "rig_model")    toInt(value, 1, 99999, s.rigModel);
        else if (key == "rx_audio")     s.rxAudio = value;
        else if (key == "tx_audio")     s.txAudio = value;
        else if (key == "rx_audio_desc") s.rxAudioDesc = value;
        else if (key == "tx_audio_desc") s.txAudioDesc = value;
        else if (key == "cat_port")     s.catPort = value;
        else if (key == "cat_baud")     toInt(value, 300, 1000000, s.catBaud);
        else if (key == "ptt")          pttGiven = parsePttKey(value, s.ptt);
        else if (key == "ptt_port")     s.pttPort = value;
        else if (key == "bluetooth")    s.bluetooth = value;
        else if (key == "bluetooth_name") s.bluetoothName = value;
        else if (key == "bt_play_audio")  s.btPlayAudio = value;
        else if (key == "bt_record_audio") s.btRecordAudio = value;
        else if (key == "path") {
            s.path.clear();
            std::istringstream hops(value);
            std::string hop;
            while (std::getline(hops, hop, ','))
                if (!trim(hop).empty()) s.path.push_back(trim(hop));
        }
        else if (key == "main")             s.mainAddress = value;
        else if (key == "connected")        s.connectedEnabled = value == "on";
        else if (key == "max_sessions")     toInt(value, 1, 16, s.maxSessions);
        else if (key == "idle_minutes")     toInt(value, 0, 1440, s.idleMinutes);
        else if (key == "page_bytes") {
            int bytes = 0;
            if (toInt(value, 0, 65536, bytes) && (bytes == 0 || bytes >= 200)) s.pageBytes = bytes;
        }
        else if (key == "busy_text" && !value.empty() && value.size() <= 200) s.busyText = value;
        else if (key == "send_to")          s.sendTo = value;
        else if (key == "frequency") {
            char* end = nullptr;
            long long hz = std::strtoll(value.c_str(), &end, 10);
            if (end != value.c_str() && *end == '\0' && hz >= 0 && hz <= 10'000'000'000LL) s.frequency = hz;
        }
    }
    // Files from before ptt= keyed over CAT whenever a CAT port was set
    if (!pttGiven && s.hasCat()) s.ptt = PttType::Cat;
    return s;
}

std::string settingsPath() {
    return (configDir() / "settings.ini").string();
}

std::string settingsPath(int copy) {
    if (copy <= 1) return settingsPath();
    return (configDir() / ("settings-" + std::to_string(copy) + ".ini")).string();
}

std::string connectorSettingsPath() {
    return (configDir() / "connector.ini").string();
}

Settings loadSettings() {
    return loadSettingsFrom(settingsPath());
}

bool saveSettings(const Settings& settings) {
    return saveSettingsTo(settings, settingsPath());
}

Settings loadSettingsFrom(const std::string& path, bool* found) {
    std::ifstream in{fs::path(path)};
    if (found) *found = static_cast<bool>(in);
    if (!in) return Settings{};
    std::stringstream text;
    text << in.rdbuf();
    return parseSettings(text.str());
}

bool saveSettingsTo(const Settings& settings, const std::string& pathText) {
    std::error_code ec;
    fs::path path = fs::path(pathText);
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    // Write to a temp file and rename, so a crash can't leave a half-written file
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) return false;
        out << formatSettings(settings);
        if (!out) return false;
    }
    fs::rename(temp, path, ec);
    if (ec) {
        // Some Windows runtimes won't rename over an existing file
        fs::remove(path, ec);
        fs::rename(temp, path, ec);
    }
    return !ec;
}
