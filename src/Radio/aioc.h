// The AIOC (All-In-One-Cable, github.com/skuep/AIOC): a USB cable for handhelds with a
// Kenwood-style two-pin plug (K plug), such as Baofengs. It's a sound card and a serial
// port in one. The radio transmits while the port's DTR line is on and RTS is off.
#ifndef RTC_AIOC_H
#define RTC_AIOC_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../backend.h"
#include "../settings.h"

namespace aioc {

// The AIOC's USB ID (pid.codes)
constexpr uint16_t usbVendorId = 0x1209;
constexpr uint16_t usbProductId = 0x7388;

// What's plugged in. Empty for the parts not found
struct Cable {
    std::string port;
    std::optional<os::AudioDevice> capture;
    std::optional<os::AudioDevice> playback;
};

// Whether a listed sound card is the cable's. It calls itself "All-In-One-Cable"
// (ALSA's card ID drops the dashes: AllInOneCable)
bool isCableAudio(const os::AudioDevice& device);
// The cable's sound card from a list. On Linux, plughw: first: it converts the rate
std::optional<os::AudioDevice> pickCableAudio(const std::vector<os::AudioDevice>& devices);

// Looks for a plugged-in cable
Cable find();

// Puts the cable's serial port and sound cards into settings (they can change each time
// it's plugged in). "" if done, else what's wrong. Leaves settings as they were on an error
std::string useCable(Settings& settings, const Cable& cable);

} // namespace aioc

#endif //RTC_AIOC_H
