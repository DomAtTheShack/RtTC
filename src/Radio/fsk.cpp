#include "fsk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fsk {

namespace {

constexpr double pi = 3.14159265358979323846;
// Phase-continuous tones
class Synth {
public:
    explicit Synth(int rate) { audio.rate = rate; }

    void tone(double hz, double seconds) {
        // Against total time, so fractional bit lengths don't drift
        elapsed += seconds;
        auto end = static_cast<size_t>(std::llround(elapsed * audio.rate));
        while (audio.samples.size() < end) {
            audio.samples.push_back(static_cast<float>(0.5 * std::sin(phase)));
            phase = std::fmod(phase + 2 * pi * hz / audio.rate, 2 * pi);
        }
    }

    void silence(double seconds) {
        elapsed += seconds;
        auto end = static_cast<size_t>(std::llround(elapsed * audio.rate));
        audio.samples.resize(end, 0.0f);
        lastLevel = 0;
    }

    // Baseband level with a raised-cosine step (for 9600)
    void level(double target, double seconds) {
        elapsed += seconds;
        auto end = static_cast<size_t>(std::llround(elapsed * audio.rate));
        size_t begin = audio.samples.size();
        double from = lastLevel;
        for (size_t n = begin; n < end; n++) {
            double t = static_cast<double>(n - begin + 1) / static_cast<double>(end - begin);
            audio.samples.push_back(static_cast<float>(from + (target - from) * (1 - std::cos(pi * t)) / 2));
        }
        lastLevel = target;
    }

    Audio audio;

private:
    double phase = 0;
    double elapsed = 0;
    double lastLevel = 0;
};

uint32_t nextRandom(uint32_t& state) {   // xorshift32
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// Approximately normal, mean 0, sd 1
float gaussian(uint32_t& state) {
    float sum = 0;
    for (int i = 0; i < 12; i++) sum += static_cast<float>(nextRandom(state)) / 4294967296.0f;
    return sum - 6.0f;
}

void putLE(std::FILE* f, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; i++) std::fputc(static_cast<int>((value >> (8 * i)) & 0xFF), f);
}

} // namespace

uint16_t ax25Fcs(const std::vector<uint8_t>& frame) {
    uint16_t crc = 0xFFFF;
    for (uint8_t byte : frame) {
        crc ^= byte;
        for (int i = 0; i < 8; i++) crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408) : crc >> 1;
    }
    return static_cast<uint16_t>(crc ^ 0xFFFF);
}

Audio ax25(const std::vector<std::vector<uint8_t>>& frames, int baud, int rate) {
    // HDLC bits before NRZI
    auto hdlcBits = [&](const std::vector<uint8_t>& frame) {
        std::vector<int> bits;
        auto flag = [&] {
            for (int i = 0; i < 8; i++) bits.push_back((0x7E >> i) & 1);
        };
        // ~0.3 s of flags, like direwolf's TXDELAY
        int preamble = std::max(8, baud * 3 / 80);
        for (int i = 0; i < preamble; i++) flag();
        std::vector<uint8_t> data = frame;
        uint16_t fcs = ax25Fcs(frame);
        data.push_back(static_cast<uint8_t>(fcs & 0xFF));
        data.push_back(static_cast<uint8_t>(fcs >> 8));
        int ones = 0;
        for (uint8_t byte : data) {
            for (int i = 0; i < 8; i++) {
                int bit = (byte >> i) & 1;
                bits.push_back(bit);
                ones = bit ? ones + 1 : 0;
                if (ones == 5) {               // Bit stuffing
                    bits.push_back(0);
                    ones = 0;
                }
            }
        }
        for (int i = 0; i < std::max(4, baud / 400); i++) flag();
        return bits;
    };

    Synth synth(rate);
    synth.silence(0.2);
    double bitTime = 1.0 / baud;
    double mark = baud == 300 ? 1600 : 1200, space = baud == 300 ? 1800 : 2200;
    int level = 1;                             // NRZI: a 0 changes the level, a 1 keeps it
    uint32_t lfsr = 0;                         // G3RUH scrambler, x^17 + x^12 + 1
    for (const auto& frame : frames) {
        for (int bit : hdlcBits(frame)) {
            if (bit == 0) level ^= 1;
            if (baud == 9600) {
                // Same as direwolf's gen_tone.c
                int scrambled = static_cast<int>((static_cast<uint32_t>(level) ^ (lfsr >> 16) ^ (lfsr >> 11)) & 1);
                lfsr = (lfsr << 1) | static_cast<uint32_t>(scrambled);
                synth.level(scrambled ? 0.5 : -0.5, bitTime);
            } else {
                synth.tone(level ? mark : space, bitTime);
            }
        }
        synth.silence(0.3);
    }
    return synth.audio;
}

Audio tone(float hz, float seconds, int rate) {
    Synth synth(rate);
    synth.tone(hz, seconds);
    return synth.audio;
}

Audio noise(float seconds, int rate, uint32_t seed) {
    Audio audio;
    audio.rate = rate;
    audio.samples.assign(static_cast<size_t>(seconds * rate), 0.0f);
    uint32_t state = seed ? seed : 1;
    for (float& s : audio.samples) s = 0.2f * gaussian(state);
    return audio;
}

void addNoise(Audio& audio, float snrDb, uint32_t seed) {
    double power = 0;
    for (float s : audio.samples) power += static_cast<double>(s) * s;
    if (audio.samples.empty() || power == 0) return;
    power /= static_cast<double>(audio.samples.size());
    auto sigma = static_cast<float>(std::sqrt(power / std::pow(10.0, snrDb / 10.0)));
    uint32_t state = seed ? seed : 1;
    for (float& s : audio.samples) s += sigma * gaussian(state);
}

bool writeWav(const std::string& path, const Audio& audio) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto dataBytes = static_cast<uint32_t>(audio.samples.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    putLE(f, 36 + dataBytes, 4);
    std::fwrite("WAVEfmt ", 1, 8, f);
    putLE(f, 16, 4);                             // fmt chunk size
    putLE(f, 1, 2);                              // PCM
    putLE(f, 1, 2);                              // Mono
    putLE(f, static_cast<uint32_t>(audio.rate), 4);
    putLE(f, static_cast<uint32_t>(audio.rate) * 2, 4);
    putLE(f, 2, 2);                              // Bytes per sample frame
    putLE(f, 16, 2);                             // Bits per sample
    std::fwrite("data", 1, 4, f);
    putLE(f, dataBytes, 4);
    for (float s : audio.samples) {
        float clipped = s > 1 ? 1 : (s < -1 ? -1 : s);
        auto value = static_cast<int16_t>(std::lround(clipped * 32767));
        putLE(f, static_cast<uint16_t>(value), 2);
    }
    bool ok = std::ferror(f) == 0;
    return std::fclose(f) == 0 && ok;
}

} // namespace fsk
