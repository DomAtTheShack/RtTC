#include "modem.h"

#include <cstdio>

#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "../backend.h"

const char* packetModemName(int baud) {
    switch (baud) {
        case 300:  return "AFSK, HF";
        case 9600: return "G3RUH";
        default:   return "AFSK, VHF/FM";
    }
}

#ifdef __APPLE__
namespace {

// Name of the Mac's default input or output device, e.g. "MacBook Pro Microphone".
// Empty if CoreAudio can't say
std::string defaultCoreAudioName(bool playback) {
    AudioObjectPropertyAddress address = {
        playback ? kAudioHardwarePropertyDefaultOutputDevice
                 : kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof device;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device) != noErr ||
        device == kAudioObjectUnknown)
        return "";

    address.mSelector = kAudioObjectPropertyName;
    CFStringRef cfName = nullptr;
    size = sizeof cfName;
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &cfName) != noErr || !cfName)
        return "";
    char buffer[256] = {};
    bool ok = CFStringGetCString(cfName, buffer, sizeof buffer, kCFStringEncodingUTF8);
    CFRelease(cfName);
    return ok ? buffer : "";
}

} // namespace
#endif

std::string direwolfDevice(const std::string& name, bool playback) {
#ifdef __APPLE__
    // direwolf's PortAudio build only takes exact device names; "default" matches nothing.
    // The app's names are PulseAudio's, so use the system default device's CoreAudio name
    (void)name;
    std::string device = defaultCoreAudioName(playback);
    // Quoted: Mac names have spaces, which would split the ADEVICE line
    return device.empty() ? "default" : "\"" + device + "\"";
#else
    (void)playback;
    // Windows: WinMM index. Linux: ALSA name
    return name.empty() ? "default" : name;
#endif
}

std::string direwolfConfig(const Settings& s, const std::string& rxDevice,
                           const std::string& txDevice, int kissPort, int agwPort, int paclen) {
    std::string call = s.callsign.empty() ? "N0CALL" : s.callsign;
    std::string config =
        "# Temporary config written by Radio to Text Connector\n"
        "ADEVICE " + rxDevice + " " + txDevice + "\n"
        "ARATE 48000\n"
        "ACHANNELS 1\n"
        "CHANNEL 0\n"
        "MYCALL " + call + "\n"
        "MODEM " + std::to_string(s.packetBaud) + "\n";
    // Receiving decodes FX.25 either way
    if (s.fx25 > 0) config += "FX25TX " + std::to_string(s.fx25) + "\n";
    switch (s.ptt) {
        case PttType::Vox:
            break;
        case PttType::Cat:
            // Model 2 (rigctld) takes host:port and no baud rate
            config += "PTT RIG " + std::to_string(s.rigModel) + " " + s.catPort;
            if (s.rigModel != ownRigctldModel) config += " " + std::to_string(s.catBaud);
            config += "\n";
            break;
        case PttType::Rts:
        case PttType::Dtr:
            // The CAT port's own line, or a second port (e.g. the FTDX3000's Standard port)
            config += "PTT " + s.keyingPort() + (s.ptt == PttType::Rts ? " RTS\n" : " DTR\n");
            break;
    }
    config += "AGWPORT " + std::to_string(agwPort > 0 ? agwPort : 0) + "\n";
    if (agwPort > 0 && paclen > 0) config += "PACLEN " + std::to_string(paclen) + "\n";
    config +=
        "KISSPORT 0\n"
        "KISSPORT " + std::to_string(kissPort) + "\n";
    return config;
}

std::vector<std::string> direwolfArgs(const std::string& configPath) {
    // -t 0: no colours. -q hd: quiet; packets are shown from the KISS port
    return {os::direwolfPath(), "-c", configPath, "-t", "0", "-q", "hd"};
}

std::vector<std::string> atestArgs(const Settings& s, const std::string& wavPath) {
    return {os::atestPath(), "-B", std::to_string(s.packetBaud), "-h", wavPath};
}

std::vector<std::string> genPacketsArgs(const Settings& s, const std::string& wavPath) {
    std::vector<std::string> args = {os::genPacketsPath(), "-B", std::to_string(s.packetBaud),
                                     "-r", "48000"};
    if (s.fx25 > 0) {
        args.push_back("-X");
        args.push_back(std::to_string(s.fx25));
    }
    args.insert(args.end(), {"-o", wavPath, "-"});
    return args;
}

std::string describe(const Settings& s) {
    std::string text = std::string(operationName(s.operation)) + " | " + std::to_string(s.packetBaud) +
                       " baud AX.25 (" + packetModemName(s.packetBaud) + ")" +
                       (s.transmits() && s.fx25 > 0 ? " FX.25" : "") + " | " +
                       (s.callsign.empty() ? std::string("no callsign") : s.callsign);
    if (!s.audioFile.empty()) text += " | file " + s.audioFile;
    return text;
}
