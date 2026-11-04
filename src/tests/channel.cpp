#include "channel.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace sim {

namespace {

constexpr double pi = 3.14159265358979323846;
using Complex = std::complex<double>;

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

// One Watterson path: Gaussian Doppler spectrum, made from a sum of sinusoids
class Fading {
public:
    Fading(double dopplerHz, Random& random) {
        for (int i = 0; i < 16; i++) {
            hz_.push_back(random.gaussian() * dopplerHz / 2);   // Spread = 2 sigma
            phase_.push_back(2 * pi * random.uniform());
        }
    }
    Complex gain(double seconds) const {
        Complex sum;
        for (size_t i = 0; i < hz_.size(); i++) sum += std::polar(1.0, 2 * pi * hz_[i] * seconds + phase_[i]);
        return sum / std::sqrt(static_cast<double>(hz_.size()));
    }

private:
    std::vector<double> hz_, phase_;
};

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

fsk::Audio ssb(const fsk::Audio& audio, const Ssb& radio) {
    const double rate = audio.rate;
    const size_t n = audio.samples.size();
    Filter txFilter = audioBand(rate, 300, 2700);
    std::vector<double> x(n);
    for (size_t i = 0; i < n; i++) x[i] = txFilter.run(audio.samples[i]);

    // Signal power while transmitting, not counting the gaps between frames
    double power = 0;
    size_t active = 0;
    for (double v : x) {
        if (std::fabs(v) < 1e-4) continue;
        power += v * v;
        active++;
    }
    power = active ? power / static_cast<double>(active) : 0;

    // The analytic signal (a Hilbert transformer), so tones can be moved and faded
    const int half = 127;
    std::vector<double> hilbert(2 * half + 1, 0.0);
    for (int k = -half; k <= half; k++) {
        if (k % 2 == 0) continue;
        double window = 0.42 + 0.5 * std::cos(pi * k / half) + 0.08 * std::cos(2 * pi * k / half);
        hilbert[k + half] = 2 / (pi * k) * window;
    }
    std::vector<Complex> analytic(n);
    for (size_t i = 0; i < n; i++) {
        double im = 0;
        for (int k = -half; k <= half; k += 2) {
            long j = static_cast<long>(i) - k;
            if (j >= 0 && j < static_cast<long>(n)) im += hilbert[k + half] * x[j];
        }
        analytic[i] = {x[i], im};
    }

    Random random(radio.seed);
    bool fading = radio.dopplerHz > 0;
    Fading first(radio.dopplerHz, random), second(radio.dopplerHz, random);
    auto delay = static_cast<size_t>(std::lround(radio.delayMs * rate / 1000));
    double sigma = std::isfinite(radio.snrDb) && power > 0
        ? std::sqrt(power / std::pow(10.0, radio.snrDb / 10) * (rate / 2) / 2400) : 0;
    Filter rxFilter = audioBand(rate, 300, 2700);

    std::vector<double> y(n);
    Complex g1 = 1, g2 = 0;
    for (size_t i = 0; i < n; i++) {
        double t = static_cast<double>(i) / rate;
        if (fading && i % 32 == 0) {   // The fading changes slowly; every 32 samples is plenty
            g1 = first.gain(t) * std::sqrt(0.5);
            g2 = second.gain(t) * std::sqrt(0.5);
        }
        Complex s = g1 * analytic[i];
        if (fading && i >= delay) s += g2 * analytic[i - delay];
        s *= std::polar(1.0, 2 * pi * radio.mistuneHz * t);
        y[i] = rxFilter.run(s.real() + sigma * random.gaussian());
    }

    // The receiver's AGC
    double rms = 0;
    for (double v : y) rms += v * v;
    rms = n ? std::sqrt(rms / static_cast<double>(n)) : 0;
    fsk::Audio out;
    out.rate = audio.rate;
    out.samples.reserve(n);
    for (double v : y) out.samples.push_back(static_cast<float>(rms > 0 ? 0.2 * v / rms : 0));
    return out;
}

} // namespace sim
