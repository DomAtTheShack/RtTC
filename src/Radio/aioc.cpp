#include "aioc.h"

namespace aioc {

bool isCableAudio(const os::AudioDevice& device) {
    for (const std::string& text : {device.name, device.desc})
        if (text.find("All-In-One-Cable") != std::string::npos ||
            text.find("AllInOneCable") != std::string::npos)
            return true;
    return false;
}

std::optional<os::AudioDevice> pickCableAudio(const std::vector<os::AudioDevice>& devices) {
    std::optional<os::AudioDevice> first;
    for (const os::AudioDevice& device : devices) {
        if (!isCableAudio(device)) continue;
        if (device.name.starts_with("plughw:")) return device;
        if (!first) first = device;
    }
    return first;
}

Cable find() {
    Cable cable;
    cable.port = os::findUsbSerialPort(usbVendorId, usbProductId);
    cable.capture = pickCableAudio(os::listAudioDevices(os::AudioDirection::Capture));
    cable.playback = pickCableAudio(os::listAudioDevices(os::AudioDirection::Playback));
    return cable;
}

std::string useCable(Settings& settings, const Cable& cable) {
    if (cable.port.empty() && !cable.capture && !cable.playback)
        return "No AIOC cable found. Is it plugged in?";
    if (cable.port.empty())
        return "The AIOC cable's sound card is there, but not its serial port, which keys the radio";
    if (!cable.capture || !cable.playback)
        return "The AIOC cable's serial port (" + cable.port + ") is there, but not its sound card";
    settings.pttPort = cable.port;
    settings.rxAudio = cable.capture->name;
    settings.rxAudioDesc = cable.capture->desc;
    settings.txAudio = cable.playback->name;
    settings.txAudioDesc = cable.playback->desc;
    return "";
}

} // namespace aioc
