#include "bt_audio.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "bt_protocol.h"

namespace bt {

namespace {

// Scales samples by percent, clipping at full scale. 100 leaves them alone
void applyGain(int16_t* samples, size_t count, int percent) {
    if (percent == 100) return;
    for (size_t i = 0; i < count; i++) {
        long scaled = static_cast<long>(samples[i]) * percent / 100;
        samples[i] = static_cast<int16_t>(std::clamp(scaled, -32768L, 32767L));
    }
}

} // namespace

BluetoothAudio::~BluetoothAudio() { stop(); }

void BluetoothAudio::say(const std::string& line) {
    if (log) log(line);
}

bool BluetoothAudio::start(const std::string& address, const std::string& playDevice,
                           const std::string& recordDevice, std::function<void(const std::string&)> logger) {
    stop();
    log = std::move(logger);
    lost = false;
    stopping = false;
    std::string normalized = normalizeAddress(address);
    if (normalized.empty()) {
        lastError = "\"" + address + "\" isn't a Bluetooth address";
        return false;
    }

    // The sound card first: it's the likelier thing to be missing, and quick to check
    audio = VirtualAudio::open(playDevice, recordDevice, lastError);
    if (!audio) return false;

    // The radio ignores transmit audio until a controller has said hello on the control
    // channel, and keeps listening to it while that channel stays open
    say("Connecting to the radio's control channel...");
    if (!control.connect(normalized, controlUuid)) {
        lastError = "control channel: " + control.error();
        audio.reset();
        return false;
    }
    for (const Bytes& frame : gaiaHandshake()) {
        control.write(frame.data(), frame.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    say("Connecting to the radio's audio channel...");
    if (!radioAudio.connect(normalized, audioUuid)) {
        lastError = "audio channel: " + radioAudio.error();
        control.close();
        audio.reset();
        return false;
    }

    running = true;
    controlThread = std::thread([this] { controlLoop(); });
    receiveThread = std::thread([this] { receiveLoop(); });
    transmitThread = std::thread([this] { transmitLoop(); });
    say("Bluetooth audio connected to " + normalized);
    return true;
}

void BluetoothAudio::stop() {
    if (!running.exchange(false)) return;
    stopping = true;
    if (audio) audio->close();                        // transmitLoop ends and unkeys
    if (transmitThread.joinable()) transmitThread.join();
    radioAudio.close();
    control.close();
    if (receiveThread.joinable()) receiveThread.join();
    if (controlThread.joinable()) controlThread.join();
    audio.reset();
}

void BluetoothAudio::controlLoop() {
    // Nothing needs answering; reading keeps the channel flowing and shows when it drops
    GaiaDeframer deframer;
    uint8_t buf[512];
    while (true) {
        long n = control.read(buf, sizeof buf);
        if (n <= 0) break;
        deframer.feed(buf, static_cast<size_t>(n));
    }
    if (!stopping) {
        lost = true;
        say("The radio's control channel closed");
    }
}

void BluetoothAudio::receiveLoop() {
    AudioDeframer deframer;
    SbcDecoder decoder;
    uint8_t buf[2048];
    while (true) {
        long n = radioAudio.read(buf, sizeof buf);
        if (n <= 0) break;
        for (const AudioFrame& frame : deframer.feed(buf, static_cast<size_t>(n))) {
            if (frame.command == audioData || frame.command == audioDataAlt) {
                std::vector<int16_t> pcm = decoder.decode(frame.payload.data(), frame.payload.size());
                applyGain(pcm.data(), pcm.size(), rxGain);
                if (!pcm.empty()) audio->play(pcm.data(), pcm.size());
            } else if (frame.command == audioEnd) {
                decoder.reset();   // The next transmission starts afresh
            }
            // audioTxEcho is our own transmission: the modems don't need to hear it
        }
    }
    if (!stopping) {
        lost = true;
        say("The radio's audio channel closed");
    }
}

void BluetoothAudio::transmitLoop() {
    Transmitter transmitter;
    std::vector<int16_t> chunk(static_cast<size_t>(audioRate) / 50);   // 20 ms
    auto send = [&](const Bytes& bytes) {
        if (bytes.empty()) return;
        std::lock_guard<std::mutex> lock(writeLock);
        if (!radioAudio.write(bytes.data(), bytes.size()) && !stopping) {
            lost = true;
            say("Can't send audio to the radio: " + radioAudio.error());
        }
    };
    while (audio->record(chunk.data(), chunk.size())) {
        // Before keying decides: at the 10% least, direwolf's tones stay far above its threshold
        applyGain(chunk.data(), chunk.size(), txGain);
        send(transmitter.feed(chunk.data(), chunk.size()));
    }
    send(transmitter.finish());   // Never leave the radio keyed
}

} // namespace bt
