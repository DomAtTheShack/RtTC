// The simulated radio path for RadioTests' channel group: narrow FM (VHF/UHF),
// with the radios' own filtering plus noise.
#ifndef RTC_CHANNEL_H
#define RTC_CHANNEL_H

#include <cstdint>
#include <limits>

#include "../Radio/fsk.h"

namespace sim {

constexpr float noNoise = std::numeric_limits<float>::infinity();

// Audio in, through an FM transmitter, the RF path and an FM receiver, audio out.
// Near and below the FM threshold (about 10 dB CNR) the receiver makes clicks and
// dropouts on its own, as real ones do
struct Nfm {
    // true: microphone in (pre-emphasis, 300-3000 Hz) and speaker out (de-emphasis,
    // 300-3000 Hz). false: flat data ports, as 9600 needs
    bool micPath = true;
    float deviationHz = 3000;     // Peak
    float ifBandwidthHz = 12500;  // Receiver IF filter
    float cnrDb = noNoise;        // Carrier to noise, in the IF bandwidth
    uint32_t seed = 1;
};
fsk::Audio nfm(const fsk::Audio& audio, const Nfm& radio);

} // namespace sim

#endif //RTC_CHANNEL_H
