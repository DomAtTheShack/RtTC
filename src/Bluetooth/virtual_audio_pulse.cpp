// virtual_audio_pulse.cpp - Linux and macOS: two PulseAudio null sinks. PipeWire's
// PulseAudio server takes the same requests.
#include "virtual_audio.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

#include "bt_protocol.h"

namespace bt {

namespace {

constexpr const char* rxSink = "rtc_bt_rx";   // Radio -> modems
constexpr const char* txSink = "rtc_bt_tx";   // Modems -> radio

// One request at a time to the PulseAudio server
class PulseControl {
public:
    bool connect(std::string& error) {
        loop = pa_mainloop_new();
        context = pa_context_new(pa_mainloop_get_api(loop), "Radio to Text Connector");
        if (!context || pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            error = "can't reach PulseAudio";
            return false;
        }
        while (pa_context_get_state(context) != PA_CONTEXT_READY) {
            if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(context)) || pa_mainloop_iterate(loop, 1, nullptr) < 0) {
                error = "can't reach PulseAudio (is it, or PipeWire, running?)";
                return false;
            }
        }
        return true;
    }

    ~PulseControl() {
        if (context) {
            pa_context_disconnect(context);
            pa_context_unref(context);
        }
        if (loop) pa_mainloop_free(loop);
    }

    bool sinkExists(const char* name) {
        bool found = false;
        wait(pa_context_get_sink_info_by_name(
            context, name,
            [](pa_context*, const pa_sink_info* info, int eol, void* result) {
                if (!eol && info) *static_cast<bool*>(result) = true;
            },
            &found));
        return found;
    }

    // Module index, or PA_INVALID_INDEX
    uint32_t loadModule(const std::string& name, const std::string& args) {
        uint32_t index = PA_INVALID_INDEX;
        wait(pa_context_load_module(
            context, name.c_str(), args.c_str(),
            [](pa_context*, uint32_t idx, void* out) { *static_cast<uint32_t*>(out) = idx; }, &index));
        return index;
    }

    void unloadModule(uint32_t index) {
        wait(pa_context_unload_module(context, index, [](pa_context*, int, void*) {}, nullptr));
    }

private:
    void wait(pa_operation* op) {
        while (op && pa_operation_get_state(op) == PA_OPERATION_RUNNING)
            if (pa_mainloop_iterate(loop, 1, nullptr) < 0) break;
        if (op) pa_operation_unref(op);
    }

    pa_mainloop* loop = nullptr;
    pa_context* context = nullptr;
};

class PulseVirtualAudio : public VirtualAudio {
public:
    ~PulseVirtualAudio() override {
        if (playStream) pa_simple_free(playStream);
        if (recordStream) pa_simple_free(recordStream);
#ifndef __APPLE__
        // Later sessions on ordinary sound cards mustn't look for these sinks
        if (pointedModems) {
            unsetenv("PULSE_SOURCE");
            unsetenv("PULSE_SINK");
        }
#endif
        if (loaded.empty()) return;
        // Only the sinks we made; ones left from before are someone else's to keep
        PulseControl control;
        std::string error;
        if (control.connect(error))
            for (uint32_t index : loaded) control.unloadModule(index);
    }

    bool start(std::string& error) {
        PulseControl control;
        if (!control.connect(error)) return false;
        for (const char* sink : {rxSink, txSink}) {
            if (control.sinkExists(sink)) continue;   // e.g. left by a run that crashed
            std::string description = sink == rxSink ? "Bluetooth-radio-receive" : "Bluetooth-radio-transmit";
            uint32_t index = control.loadModule(
                "module-null-sink", std::string("sink_name=") + sink + " rate=" + std::to_string(audioRate) +
                                        " channels=1 format=s16le sink_properties=device.description=" +
                                        description);
            if (index == PA_INVALID_INDEX) {
                error = std::string("PulseAudio wouldn't make the ") + sink + " sound card";
                return false;
            }
            loaded.push_back(index);
        }

        pa_sample_spec spec{PA_SAMPLE_S16LE, static_cast<uint32_t>(audioRate), 1};
        auto bytes = [](int ms) { return static_cast<uint32_t>(audioRate * 2 * ms / 1000); };
        // Short buffers: the radio's audio should reach the modems quickly, and record()
        // returns every 20 ms so close() takes effect
        pa_buffer_attr playAttr{bytes(200), bytes(60), static_cast<uint32_t>(-1), static_cast<uint32_t>(-1),
                                static_cast<uint32_t>(-1)};
        pa_buffer_attr recordAttr{bytes(500), static_cast<uint32_t>(-1), static_cast<uint32_t>(-1),
                                  static_cast<uint32_t>(-1), bytes(20)};
        int err = 0;
        playStream = pa_simple_new(nullptr, "Radio to Text Connector", PA_STREAM_PLAYBACK, rxSink,
                                   "Bluetooth radio audio", &spec, nullptr, &playAttr, &err);
        if (!playStream) {
            error = std::string("can't play into rtc_bt_rx: ") + pa_strerror(err);
            return false;
        }
        std::string monitor = std::string(txSink) + ".monitor";
        recordStream = pa_simple_new(nullptr, "Radio to Text Connector", PA_STREAM_RECORD, monitor.c_str(),
                                     "Audio to transmit", &spec, nullptr, &recordAttr, &err);
        if (!recordStream) {
            error = "can't record " + monitor + ": " + pa_strerror(err);
            return false;
        }
        return true;
    }

    bool play(const int16_t* samples, size_t count) override {
        int err = 0;
        return pa_simple_write(playStream, samples, count * sizeof(int16_t), &err) == 0;
    }

    bool record(int16_t* samples, size_t count) override {
        if (closed) return false;
        int err = 0;
        return pa_simple_read(recordStream, samples, count * sizeof(int16_t), &err) == 0 && !closed;
    }

    void close() override { closed = true; }

#ifdef __APPLE__
    // PulseAudio names, for programs that take them (PULSE_SOURCE and PULSE_SINK)
    std::string modemCaptureDevice() const override { return std::string(rxSink) + ".monitor"; }
    std::string modemPlaybackDevice() const override { return txSink; }
#else
    // direwolf uses ALSA; its PulseAudio plugin ("pulse") goes to these
    std::string modemCaptureDevice() const override { return "pulse"; }
    std::string modemPlaybackDevice() const override { return "pulse"; }
#endif

private:
public:
    bool pointedModems = false;   // PULSE_SOURCE and PULSE_SINK are ours

private:
    std::vector<uint32_t> loaded;
    pa_simple* playStream = nullptr;
    pa_simple* recordStream = nullptr;
    std::atomic<bool> closed{false};
};

} // namespace

std::unique_ptr<VirtualAudio> VirtualAudio::open(const std::string&, const std::string&, std::string& error) {
    auto audio = std::make_unique<PulseVirtualAudio>();
    if (!audio->start(error)) return nullptr;
#ifndef __APPLE__
    // Where ALSA's "pulse" device goes, for the modems we start
    setenv("PULSE_SOURCE", (std::string(rxSink) + ".monitor").c_str(), 1);
    setenv("PULSE_SINK", txSink, 1);
    audio->pointedModems = true;
#endif
    return audio;
}

} // namespace bt
