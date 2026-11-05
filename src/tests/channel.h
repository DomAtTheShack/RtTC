// Simulated radio paths for RadioTests' channel group: narrow FM (VHF/UHF)
// and SSB (HF), with the radios' own filtering plus noise, mistuning and fading.
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

// Audio in, through an SSB transmitter and receiver (300-2700 Hz), with the receiver's
// AGC levelling the output
struct Ssb {
    float snrDb = noNoise;        // Average signal to noise, in the 2400 Hz passband
    float mistuneHz = 0;          // Receiver off frequency: every tone moves by this
    // Fading (Watterson model): two equal paths, each wandering with this Doppler spread,
    // the second delayed. 0 = no fading
    float dopplerHz = 0;
    float delayMs = 0;
    uint32_t seed = 1;
};
fsk::Audio ssb(const fsk::Audio& audio, const Ssb& radio);

} // namespace sim

#endif //RTC_CHANNEL_H
