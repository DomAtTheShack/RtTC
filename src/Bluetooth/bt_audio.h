// Bluetooth audio to and from a radio that supports it (docs/bluetooth-audio.md), for
// direwolf. It connects the radio's control and audio channels and moves
// audio between them and a virtual sound card (virtual_audio.h), which the modems use like
// any other. The radio transmits while the modems make sound, so PTT is VOX-like.
#ifndef RTC_BT_AUDIO_H
#define RTC_BT_AUDIO_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "rfcomm.h"
#include "virtual_audio.h"

namespace bt {

class BluetoothAudio {
public:
    BluetoothAudio() = default;
    ~BluetoothAudio();
    BluetoothAudio(const BluetoothAudio&) = delete;
    BluetoothAudio& operator=(const BluetoothAudio&) = delete;

    // address: the radio's. playDevice, recordDevice: Windows only, the virtual cable ends
    // (see virtual_audio.h). log gets a line now and then, from any thread. Takes a few
    // seconds. false with error() set if it can't
    bool start(const std::string& address, const std::string& playDevice, const std::string& recordDevice,
               std::function<void(const std::string&)> log = {});
    void stop();

    // What the modems use, or "" for the saved sound card (Windows)
    std::string modemCaptureDevice() const { return audio ? audio->modemCaptureDevice() : ""; }
    std::string modemPlaybackDevice() const { return audio ? audio->modemPlaybackDevice() : ""; }

    bool connected() const { return running && !lost; }
    const std::string& error() const { return lastError; }

private:
    void controlLoop();
    void receiveLoop();
    void transmitLoop();
    void say(const std::string& line);

    std::unique_ptr<VirtualAudio> audio;
    Rfcomm control, radioAudio;
    std::thread controlThread, receiveThread, transmitThread;
    std::mutex writeLock;
    std::atomic<bool> running{false};
    std::atomic<bool> lost{false};
    std::atomic<bool> stopping{false};
    std::function<void(const std::string&)> log;
    std::string lastError;
};

} // namespace bt

#endif //RTC_BT_AUDIO_H
