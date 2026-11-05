// What goes over Bluetooth to radios with a Benshi audio channel (BTech
// UV-Pro and UV-50Pro, Radioddity GA-5WB, Vero VR-N75/N76/N7500/N7600): the audio
// channel's framing, its SBC audio, and the GAIA control frames. No Bluetooth calls here.
//
// Worked out from HTCommander (github.com/Ylianst/HTCommander) and UVProTermBT
// (github.com/chengmania/UVProTermBT, docs/GAIA_AUDIO_SSTV.md). docs/bluetooth-audio.md
// has the details.
#ifndef RTC_BT_PROTOCOL_H
#define RTC_BT_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bt {

using Bytes = std::vector<uint8_t>;

// Services

// RFCOMM services, found with SDP. Control: Serial Port, carrying GAIA. Audio: the vendor
// "BS AOC" service, carrying SBC audio
constexpr const char* controlUuid = "00001101-0000-1000-8000-00805f9b34fb";
constexpr const char* audioUuid   = "39144315-32fa-40db-85ed-fbfeba2d86e6";

// 16 bytes, most significant first. false if text isn't a UUID
bool parseUuid(std::string_view text, uint8_t out[16]);
// "AA:BB:CC:DD:EE:FF" (any case, ':' or '-') into 6 bytes, most significant first
bool parseAddress(std::string_view text, uint8_t out[6]);
// Upper case with ':'; "" if text isn't an address
std::string normalizeAddress(std::string_view text);

// Audio channel framing

// 0x7e <command> <payload> 0x7e, with 0x7d and 0x7e in between sent as 0x7d, byte ^ 0x20
constexpr uint8_t audioFlag = 0x7e;
constexpr uint8_t audioEscape = 0x7d;

enum AudioCommand : uint8_t {
    audioData    = 0x00,   // SBC frames, both ways
    audioEnd     = 0x01,   // The radio's received audio stopped
    audioAck     = 0x02,
    audioDataAlt = 0x03,   // SBC frames from the radio, too
    audioTxEcho  = 0x09,   // Our own transmit audio, sent back
};

Bytes audioFrame(uint8_t command, const uint8_t* payload, size_t size);
// Ends a transmission: the radio unkeys
const Bytes& endAudioFrame();

struct AudioFrame {
    uint8_t command;
    Bytes payload;
};

// Bytes arrive in any size of piece. A frame starts after the first 0x7e
class AudioDeframer {
public:
    std::vector<AudioFrame> feed(const uint8_t* data, size_t size);

private:
    bool started = false;
    bool escaped = false;
    Bytes current;
};

// GAIA (control channel)

// FF 01 <flags> <data length> <group: 2 bytes> <command: 2 bytes> <data>, big endian
constexpr uint16_t gaiaBasic = 2;
enum GaiaCommand : uint16_t {
    gaiaGetDevInfo = 4,
    gaiaRegisterNotification = 6,
    gaiaEventNotification = 9,
    gaiaGetHtStatus = 20,
};
// Notifications to register for
constexpr uint8_t notifyHtStatus = 1;
constexpr uint8_t notifyRadioStatus = 8;
constexpr uint8_t notifyFreqMode = 14;

Bytes gaiaFrame(uint16_t group, uint16_t command, const Bytes& data = {});

// What HTCommander sends when it connects. Until the radio has had it, it ignores
// transmit audio
std::vector<Bytes> gaiaHandshake();

struct GaiaFrame {
    uint16_t group;
    uint16_t command;     // Without the reply bit
    bool reply;           // Radios set 0x8000 in the command of their replies
    Bytes data;
};

class GaiaDeframer {
public:
    // Skips anything that isn't a frame
    std::vector<GaiaFrame> feed(const uint8_t* data, size_t size);

private:
    Bytes buffer;
};

// From an HT status notification: whether the radio is transmitting. -1 if frame isn't one
int transmitting(const GaiaFrame& frame);

// SBC

// The radio's audio: 32 kHz mono 16-bit, SBC with 16 blocks, 8 subbands and loudness
// allocation. Each SBC frame is 128 samples (4 ms)
constexpr int audioRate = 32000;
constexpr int samplesPerSbcFrame = 128;
// HTCommander sends 18 (the radio's own rate); UVProTermBT sends 40. Both work on the
// air. 40 keeps more of a data signal and is still only 22 kB/s
constexpr int txBitpool = 40;
// SBC frames per audio frame. The radio ignores much bigger audio frames
constexpr int sbcFramesPerPacket = 4;

class SbcEncoder {
public:
    SbcEncoder();
    ~SbcEncoder();
    // Whole SBC frames for the samples so far; the rest wait for the next call
    Bytes encode(const int16_t* samples, size_t count);
    size_t frameSize() const;
    void reset();

private:
    struct State;
    std::unique_ptr<State> state;
    std::vector<int16_t> pending;
};

class SbcDecoder {
public:
    SbcDecoder();
    ~SbcDecoder();
    // Every whole SBC frame in data, plus any left from last time. Bad frames are skipped
    std::vector<int16_t> decode(const uint8_t* data, size_t size);
    void reset();
    int badFrames() const { return bad; }

private:
    struct State;
    std::unique_ptr<State> state;
    Bytes pending;
    int bad = 0;
};

// Transmit

// The radio transmits while audio frames arrive and stops at endAudioFrame(). The modems
// play into a virtual sound card that gives silence (zeros) between transmissions, so
// sound keys the radio and a gap unkeys it. Real time in, audio frames out.
class Transmitter {
public:
    // threshold: a sample bigger than this (out of 32767) is sound. holdMs: silence that
    // ends a transmission. leadMs: audio buffered before the first frame goes out, so a
    // Bluetooth hiccup doesn't starve the radio part way through
    explicit Transmitter(int threshold = 64, int holdMs = 300, int leadMs = 250);

    // Bytes to send to the audio channel (maybe none)
    Bytes feed(const int16_t* samples, size_t count);
    // Ends a transmission now (e.g. on shutdown). Bytes to send
    Bytes finish();
    bool keyed() const { return keyedNow; }
    int transmissions() const { return started; }

private:
    Bytes packets(bool flush);

    int threshold;
    size_t holdSamples, leadSamples;
    bool keyedNow = false;
    bool leadSent = false;
    size_t quietSamples = 0;
    size_t bufferedSamples = 0;
    int started = 0;
    SbcEncoder encoder;
    Bytes sbc;   // Encoded, not yet sent
};

} // namespace bt

#endif //RTC_BT_PROTOCOL_H
