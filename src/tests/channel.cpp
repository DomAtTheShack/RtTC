#include "channel.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sim {

namespace {

constexpr double pi = 3.14159265358979323846;

// Same numbers on every OS (std::normal_distribution differs between standard libraries)
class Random {
public:
    explicit Random(uint32_t seed) : state_(seed ? seed : 1) {}
    double uniform() {   // (0, 1)
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return (state_ + 0.5) / 4294967296.0;
    }
    double gaussian() { return std::sqrt(-2 * std::log(uniform())) * std::cos(2 * pi * uniform()); }

private:
    uint32_t state_;
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    double run(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// RBJ cookbook high- or low-pass
Biquad pass(bool high, double rate, double hz, double q) {
    double w = 2 * pi * hz / rate, c = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    Biquad f;
    f.b0 = (high ? (1 + c) : (1 - c)) / 2 / a0;
    f.b1 = (high ? -(1 + c) : (1 - c)) / a0;
    f.b2 = f.b0;
    f.a1 = -2 * c / a0;
    f.a2 = (1 - alpha) / a0;
    return f;
}

// First order (1 + s/zero) / (1 + s/pole), scaled to gain 1 at 1 kHz
Biquad shelf(double rate, double zeroHz, double poleHz) {
    double k = 2 * rate, wz = 2 * pi * zeroHz, wp = 2 * pi * poleHz, w = 2 * pi * 1000;
    double gain = (wp / wz) * std::sqrt((w * w + wz * wz) / (w * w + wp * wp));
    double g = wp / wz / gain, a0 = k + wp;
    Biquad f;
    f.b0 = g * (k + wz) / a0;
    f.b1 = g * (wz - k) / a0;
    f.a1 = (wp - k) / a0;
    return f;
}

class Filter {
public:
    // Butterworth, order 4 or 8
    void butterworth(bool high, double rate, double hz, int order) {
        static const double q4[] = {0.5412, 1.3066}, q8[] = {0.5098, 0.6013, 0.9000, 2.5629};
        const double* q = order == 8 ? q8 : q4;
        for (int i = 0; i < order / 2; i++) stages_.push_back(pass(high, rate, hz, q[i]));
    }
    void add(const Biquad& stage) { stages_.push_back(stage); }
    double run(double x) {
        for (Biquad& stage : stages_) x = stage.run(x);
        return x;
    }

private:
    std::vector<Biquad> stages_;
};

// A radio's audio filter
Filter audioBand(double rate, double lowHz, double highHz) {
    Filter f;
    f.butterworth(true, rate, lowHz, 4);
    f.butterworth(false, rate, highHz, 4);
    return f;
}

// 750 us, the land mobile standard: +6 dB per octave from 212 Hz
constexpr double emphasisHz = 1e6 / (2 * pi * 750);
constexpr double emphasisTopHz = 8000;   // Where the transmitter's emphasis stops rising

} // namespace

fsk::Audio nfm(const fsk::Audio& audio, const Nfm& radio) {
    const double rate = audio.rate;
    std::vector<double> tx(audio.samples.begin(), audio.samples.end());
    if (radio.micPath) {
        Filter mic = audioBand(rate, 300, 3000);
        mic.add(shelf(rate, emphasisHz, emphasisTopHz));
        for (double& x : tx) x = mic.run(x);
    }
    // Audio level set so the loudest moment is full deviation
    double peak = 0;
    for (double x : tx) peak = std::max(peak, std::fabs(x));
    if (peak > 0)
        for (double& x : tx) x /= peak;

    // Complex noise over the whole sample rate, so the IF bandwidth gets the asked-for CNR
    double sigma = std::isfinite(radio.cnrDb)
        ? std::sqrt(rate / (radio.ifBandwidthHz * std::pow(10.0, radio.cnrDb / 10)) / 2) : 0;
    Random random(radio.seed);
    Filter ifI, ifQ;
    ifI.butterworth(false, rate, radio.ifBandwidthHz / 2, 8);
    ifQ.butterworth(false, rate, radio.ifBandwidthHz / 2, 8);
    Filter speaker = audioBand(rate, 300, 3000);
    speaker.add(shelf(rate, emphasisTopHz, emphasisHz));

    fsk::Audio out;
    out.rate = audio.rate;
    out.samples.reserve(tx.size());
    double phase = 0, lastI = 1, lastQ = 0;
    for (double x : tx) {
        phase = std::fmod(phase + 2 * pi * radio.deviationHz * x / rate, 2 * pi);
        double i = ifI.run(std::cos(phase) + sigma * random.gaussian());
        double q = ifQ.run(std::sin(phase) + sigma * random.gaussian());
        // Discriminator: the phase step between samples, as a fraction of full deviation
        double d = std::atan2(q * lastI - i * lastQ, i * lastI + q * lastQ) * rate /
                   (2 * pi * radio.deviationHz);
        lastI = i;
        lastQ = q;
        if (radio.micPath) d = speaker.run(d);
        out.samples.push_back(static_cast<float>(0.5 * d));
    }
    return out;
}

} // namespace sim
