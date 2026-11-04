// virtual_audio_windows.cpp - Windows has no virtual sound card of its own, so this uses a
// cable the user installs (e.g. VB-Cable, or VB-Cable A+B for both ways). The bridge plays
// the radio's audio into one cable's input, where the modems hear it on its output, and
// records the modems from a second cable's output. Untested on a radio.
#include "virtual_audio.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <mmsystem.h>

#include "bt_protocol.h"

namespace bt {

namespace {

constexpr int bufferMs = 20;
constexpr size_t bufferSamples = static_cast<size_t>(audioRate) * bufferMs / 1000;
constexpr int recordBuffers = 10;
constexpr size_t maxQueuedPlay = 50;   // Buffers queued for playing (about a second)

WAVEFORMATEX radioFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = audioRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = audioRate * 2;
    return format;
}

bool parseDevice(const std::string& text, UINT& device) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) return false;
    device = static_cast<UINT>(std::stoul(text));
    return true;
}

struct Buffer {
    WAVEHDR header{};
    std::vector<int16_t> samples;
};

class WinmmVirtualAudio : public VirtualAudio {
public:
    ~WinmmVirtualAudio() override {
        if (in) {
            waveInReset(in);
            for (Buffer& b : recordBufs) waveInUnprepareHeader(in, &b.header, sizeof b.header);
            waveInClose(in);
        }
        if (out) {
            waveOutReset(out);
            for (Buffer& b : playing) waveOutUnprepareHeader(out, &b.header, sizeof b.header);
            waveOutClose(out);
        }
        if (recordEvent) CloseHandle(recordEvent);
    }

    bool start(const std::string& playDevice, const std::string& recordDevice, std::string& error) {
        UINT playId, recordId;
        if (!parseDevice(playDevice, playId) || !parseDevice(recordDevice, recordId)) {
            error = "Bluetooth audio on Windows needs a virtual cable (e.g. VB-Cable): pick the "
                    "cables in Settings > Bluetooth";
            return false;
        }
        WAVEFORMATEX format = radioFormat();
        if (waveOutOpen(&out, playId, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            out = nullptr;
            error = "can't play into sound card " + playDevice;
            return false;
        }
        recordEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (waveInOpen(&in, recordId, &format, reinterpret_cast<DWORD_PTR>(recordEvent), 0, CALLBACK_EVENT) !=
            MMSYSERR_NOERROR) {
            in = nullptr;
            error = "can't record sound card " + recordDevice;
            return false;
        }
        recordBufs.resize(recordBuffers);
        for (Buffer& b : recordBufs) {
            b.samples.resize(bufferSamples);
            b.header.lpData = reinterpret_cast<LPSTR>(b.samples.data());
            b.header.dwBufferLength = static_cast<DWORD>(bufferSamples * sizeof(int16_t));
            waveInPrepareHeader(in, &b.header, sizeof b.header);
            waveInAddBuffer(in, &b.header, sizeof b.header);
        }
        waveInStart(in);
        return true;
    }

    bool play(const int16_t* samples, size_t count) override {
        // Free what has finished, then queue this
        while (!playing.empty() && (playing.front().header.dwFlags & WHDR_DONE)) {
            waveOutUnprepareHeader(out, &playing.front().header, sizeof playing.front().header);
            playing.pop_front();
        }
        if (playing.size() >= maxQueuedPlay) return true;   // Behind: drop rather than lag
        playing.emplace_back();
        Buffer& b = playing.back();
        b.samples.assign(samples, samples + count);
        b.header.lpData = reinterpret_cast<LPSTR>(b.samples.data());
        b.header.dwBufferLength = static_cast<DWORD>(count * sizeof(int16_t));
        waveOutPrepareHeader(out, &b.header, sizeof b.header);
        return waveOutWrite(out, &b.header, sizeof b.header) == MMSYSERR_NOERROR;
    }

    bool record(int16_t* samples, size_t count) override {
        size_t filled = 0;
        while (filled < count) {
            if (closed) return false;
            if (spare.empty()) {
                Buffer& b = recordBufs[next];
                if (!(b.header.dwFlags & WHDR_DONE)) {
                    WaitForSingleObject(recordEvent, bufferMs);
                    continue;
                }
                spare.insert(spare.end(), b.samples.begin(),
                             b.samples.begin() + static_cast<long>(b.header.dwBytesRecorded / sizeof(int16_t)));
                b.header.dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
                waveInAddBuffer(in, &b.header, sizeof b.header);
                next = (next + 1) % recordBufs.size();
            }
            size_t n = std::min(count - filled, spare.size());
            std::copy(spare.begin(), spare.begin() + static_cast<long>(n), samples + filled);
            spare.erase(spare.begin(), spare.begin() + static_cast<long>(n));
            filled += n;
        }
        return true;
    }

    void close() override {
        closed = true;
        if (recordEvent) SetEvent(recordEvent);
    }

private:
    HWAVEOUT out = nullptr;
    HWAVEIN in = nullptr;
    HANDLE recordEvent = nullptr;
    std::deque<Buffer> playing;
    std::vector<Buffer> recordBufs;
    size_t next = 0;
    std::deque<int16_t> spare;
    std::atomic<bool> closed{false};
};

} // namespace

std::unique_ptr<VirtualAudio> VirtualAudio::open(const std::string& playDevice, const std::string& recordDevice,
                                                 std::string& error) {
    auto audio = std::make_unique<WinmmVirtualAudio>();
    if (!audio->start(playDevice, recordDevice, error)) return nullptr;
    return audio;
}

} // namespace bt
