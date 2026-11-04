#include "bt_protocol.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// sbc.h puts alignas where C allows it and C++ ignores it; the layout is the same either way
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif
#include "libsbc/sbc.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace bt {

// ---- Services ------------------------------------------------------------------------

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

} // namespace

bool parseUuid(std::string_view text, uint8_t out[16]) {
    std::string hex;
    for (char c : text)
        if (c != '-') hex += c;
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; i++) {
        int hi = hexValue(hex[i * 2]), lo = hexValue(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

bool parseAddress(std::string_view text, uint8_t out[6]) {
    if (text.size() != 17) return false;
    for (int i = 0; i < 6; i++) {
        int hi = hexValue(text[i * 3]), lo = hexValue(text[i * 3 + 1]);
        if (hi < 0 || lo < 0) return false;
        if (i < 5 && text[i * 3 + 2] != ':' && text[i * 3 + 2] != '-') return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

std::string normalizeAddress(std::string_view text) {
    uint8_t bytes[6];
    if (!parseAddress(text, bytes)) return "";
    char buf[18];
    std::snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return buf;
}

// ---- Audio channel framing -------------------------------------------------------------

Bytes audioFrame(uint8_t command, const uint8_t* payload, size_t size) {
    Bytes out;
    out.reserve(size + size / 32 + 4);
    out.push_back(audioFlag);
    auto put = [&](uint8_t b) {
        if (b == audioFlag || b == audioEscape) {
            out.push_back(audioEscape);
            out.push_back(b ^ 0x20);
        } else {
            out.push_back(b);
        }
    };
    put(command);
    for (size_t i = 0; i < size; i++) put(payload[i]);
    out.push_back(audioFlag);
    return out;
}

const Bytes& endAudioFrame() {
    static const Bytes frame = {0x7e, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e};
    return frame;
}

std::vector<AudioFrame> AudioDeframer::feed(const uint8_t* data, size_t size) {
    std::vector<AudioFrame> frames;
    for (size_t i = 0; i < size; i++) {
        uint8_t b = data[i];
        if (b == audioFlag) {
            // One flag ends a frame and starts the next. Empty frames are just flags
            if (started && !escaped && !current.empty())
                frames.push_back({current[0], Bytes(current.begin() + 1, current.end())});
            started = true;
            escaped = false;
            current.clear();
        } else if (!started) {
            continue;
        } else if (escaped) {
            current.push_back(b ^ 0x20);
            escaped = false;
        } else if (b == audioEscape) {
            escaped = true;
        } else {
            current.push_back(b);
        }
    }
    return frames;
}

// ---- GAIA ------------------------------------------------------------------------------

Bytes gaiaFrame(uint16_t group, uint16_t command, const Bytes& data) {
    Bytes out = {0xff, 0x01, 0x00, static_cast<uint8_t>(data.size()),
                 static_cast<uint8_t>(group >> 8), static_cast<uint8_t>(group),
                 static_cast<uint8_t>(command >> 8), static_cast<uint8_t>(command)};
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<Bytes> gaiaHandshake() {
    return {
        gaiaFrame(gaiaBasic, gaiaGetDevInfo, {3}),
        gaiaFrame(gaiaBasic, gaiaRegisterNotification, {notifyHtStatus, notifyFreqMode}),
        gaiaFrame(gaiaBasic, gaiaRegisterNotification, {notifyRadioStatus}),
        gaiaFrame(gaiaBasic, gaiaGetHtStatus),
    };
}

std::vector<GaiaFrame> GaiaDeframer::feed(const uint8_t* data, size_t size) {
    buffer.insert(buffer.end(), data, data + size);
    std::vector<GaiaFrame> frames;
    while (true) {
        // Drop everything before FF 01. A last lone FF stays: the 01 may come next time
        size_t start = 0;
        while (start + 1 < buffer.size() && !(buffer[start] == 0xff && buffer[start + 1] == 0x01))
            start++;
        if (start + 1 == buffer.size() && buffer[start] != 0xff) start = buffer.size();
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(start));
        if (buffer.size() < 8) break;
        size_t length = buffer[3];
        size_t total = 8 + length + (buffer[2] & 1);   // Flag bit 0: a checksum byte follows
        if (buffer.size() < total) break;
        uint16_t command = static_cast<uint16_t>(buffer[6] << 8 | buffer[7]);
        frames.push_back({static_cast<uint16_t>(buffer[4] << 8 | buffer[5]),
                          static_cast<uint16_t>(command & 0x7fff), (command & 0x8000) != 0,
                          Bytes(buffer.begin() + 8, buffer.begin() + 8 + static_cast<long>(length))});
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(total));
    }
    return frames;
}

int transmitting(const GaiaFrame& frame) {
    // HT status: the first status byte has 0x40 set while transmitting (UVProTermBT)
    if (frame.group != gaiaBasic || frame.command != gaiaEventNotification) return -1;
    if (frame.data.size() < 2 || frame.data[0] != notifyHtStatus) return -1;
    return (frame.data[1] & 0x40) ? 1 : 0;
}

// ---- SBC -------------------------------------------------------------------------------

namespace {

sbc_frame radioFrame() {
    sbc_frame frame{};
    frame.msbc = false;
    frame.freq = SBC_FREQ_32K;
    frame.mode = SBC_MODE_MONO;
    frame.bam = SBC_BAM_LOUDNESS;
    frame.nblocks = 16;
    frame.nsubbands = 8;
    frame.bitpool = txBitpool;
    return frame;
}

} // namespace

struct SbcEncoder::State {
    sbc_t sbc;
    sbc_frame frame = radioFrame();
};

SbcEncoder::SbcEncoder() : state(std::make_unique<State>()) { reset(); }
SbcEncoder::~SbcEncoder() = default;

void SbcEncoder::reset() {
    sbc_reset(&state->sbc);
    pending.clear();
}

size_t SbcEncoder::frameSize() const { return sbc_get_frame_size(&state->frame); }

Bytes SbcEncoder::encode(const int16_t* samples, size_t count) {
    pending.insert(pending.end(), samples, samples + count);
    size_t frames = pending.size() / samplesPerSbcFrame;
    size_t size = frameSize();
    Bytes out(frames * size);
    for (size_t i = 0; i < frames; i++) {
        sbc_encode(&state->sbc, pending.data() + i * samplesPerSbcFrame, 1, nullptr, 0,
                   &state->frame, out.data() + i * size, static_cast<unsigned>(size));
    }
    pending.erase(pending.begin(), pending.begin() + static_cast<long>(frames * samplesPerSbcFrame));
    return out;
}

struct SbcDecoder::State {
    sbc_t sbc;
};

SbcDecoder::SbcDecoder() : state(std::make_unique<State>()) { reset(); }
SbcDecoder::~SbcDecoder() = default;

void SbcDecoder::reset() {
    sbc_reset(&state->sbc);
    pending.clear();
}

std::vector<int16_t> SbcDecoder::decode(const uint8_t* data, size_t size) {
    pending.insert(pending.end(), data, data + size);
    std::vector<int16_t> pcm;
    size_t pos = 0;
    while (pending.size() - pos >= SBC_PROBE_SIZE) {
        if (pending[pos] != 0x9c) {   // Not a frame start: look for the next one
            pos++;
            continue;
        }
        sbc_frame frame{};
        if (sbc_probe(pending.data() + pos, &frame) != 0 || frame.mode != SBC_MODE_MONO) {
            pos++;
            continue;
        }
        size_t frameSize = sbc_get_frame_size(&frame);
        if (pending.size() - pos < frameSize) break;   // The rest comes later
        int16_t out[SBC_MAX_SAMPLES];
        if (sbc_decode(&state->sbc, pending.data() + pos, static_cast<unsigned>(frameSize), &frame,
                       out, 1, nullptr, 0) == 0) {
            pcm.insert(pcm.end(), out, out + frame.nblocks * frame.nsubbands);
            pos += frameSize;
        } else {
            bad++;
            pos++;
        }
    }
    pending.erase(pending.begin(), pending.begin() + static_cast<long>(pos));
    return pcm;
}

// ---- Transmit -------------------------------------------------------------------------

Transmitter::Transmitter(int threshold, int holdMs, int leadMs)
    : threshold(threshold),
      holdSamples(static_cast<size_t>(audioRate) * static_cast<size_t>(holdMs) / 1000),
      leadSamples(static_cast<size_t>(audioRate) * static_cast<size_t>(leadMs) / 1000) {}

Bytes Transmitter::packets(bool flush) {
    Bytes out;
    size_t packetSize = encoder.frameSize() * sbcFramesPerPacket;
    size_t pos = 0;
    while (sbc.size() - pos >= packetSize || (flush && pos < sbc.size())) {
        size_t n = std::min(packetSize, sbc.size() - pos);
        Bytes frame = audioFrame(audioData, sbc.data() + pos, n);
        out.insert(out.end(), frame.begin(), frame.end());
        pos += n;
    }
    sbc.erase(sbc.begin(), sbc.begin() + static_cast<long>(pos));
    return out;
}

Bytes Transmitter::feed(const int16_t* samples, size_t count) {
    Bytes out;
    size_t i = 0;
    while (i < count) {
        if (!keyedNow) {
            // Skip silence until something is heard
            while (i < count && std::abs(samples[i]) <= threshold) i++;
            if (i == count) break;
            keyedNow = true;
            leadSent = false;
            quietSamples = 0;
            bufferedSamples = 0;
            encoder.reset();
            sbc.clear();
            started++;
        }
        // Keyed: take samples up to the end of the hold time
        size_t start = i;
        bool ended = false;
        for (; i < count; i++) {
            if (std::abs(samples[i]) > threshold) {
                quietSamples = 0;
            } else if (++quietSamples >= holdSamples) {
                i++;
                ended = true;
                break;
            }
        }
        Bytes encoded = encoder.encode(samples + start, i - start);
        sbc.insert(sbc.end(), encoded.begin(), encoded.end());
        bufferedSamples += i - start;
        if (bufferedSamples >= leadSamples) leadSent = true;
        if (ended) {
            Bytes rest = finish();
            out.insert(out.end(), rest.begin(), rest.end());
        } else if (leadSent) {
            Bytes ready = packets(false);
            out.insert(out.end(), ready.begin(), ready.end());
        }
    }
    return out;
}

Bytes Transmitter::finish() {
    if (!keyedNow) return {};
    Bytes out = packets(true);
    const Bytes& end = endAudioFrame();
    out.insert(out.end(), end.begin(), end.end());
    keyedNow = false;
    leadSent = false;
    return out;
}

} // namespace bt
