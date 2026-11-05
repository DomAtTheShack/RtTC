// virtual_audio.h - The sound card direwolf (and the tuning tone) use in Bluetooth mode.
//   Linux, macOS  Two PulseAudio (or PipeWire) null sinks the bridge makes:
//                   rtc_bt_rx  the bridge plays the radio's audio in; modems record its monitor
//                   rtc_bt_tx  modems play in; the bridge records its monitor and sends it
//   Windows       A virtual cable (e.g. VB-Cable) the user installs: the bridge plays into
//                 one cable and records another, by WinMM number
#ifndef RTC_VIRTUAL_AUDIO_H
#define RTC_VIRTUAL_AUDIO_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace bt {

// 32 kHz mono 16-bit both ways (the radio's rate)
class VirtualAudio {
public:
    virtual ~VirtualAudio() = default;

    // Radio audio for the modems to hear
    virtual bool play(const int16_t* samples, size_t count) = 0;
    // The modems' audio, in real time. Blocks until count samples; false once closed
    virtual bool record(int16_t* samples, size_t count) = 0;
    // record() then returns false within about 20 ms
    virtual void close() = 0;

    // Sound card names the modems use instead of the saved ones ("" = keep the saved one).
    // Same kinds of names as os::listAudioDevices gives
    virtual std::string modemCaptureDevice() const { return ""; }
    virtual std::string modemPlaybackDevice() const { return ""; }

    // Linux and macOS: PulseAudio. Windows: playDevice and recordDevice are the WinMM numbers
    // of the cable ends the bridge uses. nullptr with error set if it can't
    static std::unique_ptr<VirtualAudio> open(const std::string& playDevice,
                                              const std::string& recordDevice, std::string& error);
};

} // namespace bt

#endif //RTC_VIRTUAL_AUDIO_H
