#include "testcases.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "backend.h"
#include "Radio/modem.h"
#include "Connector/packet.h"

namespace fs = std::filesystem;

namespace {

// The test audio is plain AX.25 (fsk::ax25 makes no FX.25)
Settings ax25(int baud) {
    Settings s;
    s.packetBaud = baud;
    s.fx25 = 0;
    s.callsign = testCallsign;
    return s;
}

const char* const packetText =
    "Hello from Radio to Text Connector\n"
    "The quick brown fox jumps over the lazy dog 0123456789\n"
    "{\"type\":\"text\",\"msg\":\"73 <de> N0CALL\"}";

std::string percent(float value) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.0f%%", value * 100);
    return buf;
}

std::string oneLine(const std::string& text) {
    std::string out;
    for (char c : text) out += c == '\n' ? std::string(" / ") : std::string(1, c);
    return out;
}

} // namespace

const char* const testMessage = "Hello from Radio to Text Connector! The quick brown fox 0123456789";

std::vector<packet::Bytes> packetFrames(const std::string& text, const std::string& callsign) {
    std::vector<packet::Bytes> frames;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        for (const std::string& piece : packet::splitInfo(std::string_view(text).substr(start, end - start)))
            frames.push_back(packet::encodeUi(callsign, packet::groupAddress, piece));
        start = end + 1;
    }
    return frames;
}

const std::vector<AudioCase>& standardCases() {
    static const std::vector<AudioCase> cases = {
        {"packet_1200.wav", "direwolf: 1200 baud AX.25 (VHF/FM), three packets",
         ax25(1200), 24000, packetText, false, 0, 1.0f},
        {"packet_1200_noisy.wav", "direwolf: 1200 baud AX.25 with noise (10 dB SNR over the whole band)",
         ax25(1200), 24000, packetText, true, 10, 1.0f},
        {"packet_300.wav", "direwolf: 300 baud AX.25 (HF), three packets",
         ax25(300), 11025, packetText, false, 0, 1.0f},
        {"packet_9600.wav", "direwolf: 9600 baud G3RUH AX.25, three packets",
         ax25(9600), 48000, packetText, false, 0, 1.0f},
        {"packet_noise_only.wav", "direwolf: noise only, no packets may come out",
         ax25(1200), 24000, "", false, 0, 1.0f},
    };
    return cases;
}

const std::vector<ToneFile>& toneFiles() {
    static const std::vector<ToneFile> tones = {
        {"tone_1000hz.wav", "1000 Hz reference, for setting audio levels", 1000, 5},
        {"tone_1200hz_mark.wav", "1200 Hz, 1200 baud AFSK mark", 1200, 5},
        {"tone_2200hz_space.wav", "2200 Hz, 1200 baud AFSK space", 2200, 5},
        {"tone_1600hz_mark_hf.wav", "1600 Hz, 300 baud (HF) AFSK mark", 1600, 5},
        {"tone_1800hz_space_hf.wav", "1800 Hz, 300 baud (HF) AFSK space", 1800, 5},
    };
    return tones;
}

fsk::Audio generate(const AudioCase& c) {
    fsk::Audio audio;
    if (c.text.empty()) {
        audio = fsk::noise(6, c.rate, 3);
    } else {
        audio = fsk::ax25(packetFrames(c.text, c.settings.callsign), c.settings.packetBaud, c.rate);
    }
    if (c.noisy) fsk::addNoise(audio, c.snrDb, 7);
    return audio;
}

std::string normalizeText(const std::string& text) {
    std::string out;
    bool space = false;
    for (char c : text) {
        bool blank = c == ' ' || c == '\r' || c == '\n' || c == '\t';
        if (blank) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

float textAccuracy(const std::string& expected, const std::string& decoded) {
    std::string a = normalizeText(expected), b = normalizeText(decoded);
    if (a.empty()) return b.empty() ? 1.0f : 0.0f;
    // Levenshtein distance
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) row[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); j++) {
            size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] != b[j - 1])});
            diagonal = above;
        }
    }
    float accuracy = 1.0f - static_cast<float>(row[b.size()]) / static_cast<float>(a.size());
    return accuracy < 0 ? 0 : accuracy;
}

namespace {

std::string runForOutput(const std::vector<std::string>& args, int& status, std::string& error) {
    std::string output;
    os::Process process;
    if (!process.start(args)) {
        error = process.error();
        status = -1;
        return output;
    }
    char buffer[4096];
    long n;
    while ((n = process.read(buffer, sizeof buffer)) > 0) output.append(buffer, static_cast<size_t>(n));
    status = process.wait();
    return output;
}

} // namespace

CaseResult decodeFile(const Settings& settings, const std::string& path,
                      const std::string& expected, float minAccuracy) {
    CaseResult result;
    if (!os::fileReadable(path)) {
        result.error = path + ": not found";
        return result;
    }
    int status;
    std::string output = runForOutput(atestArgs(settings, path), status, result.error);
    if (!result.error.empty()) return result;
    if (status != 0) {
        result.error = "atest exited with " + std::to_string(status);
        return result;
    }
    std::string text;
    for (const packet::Bytes& bytes : packet::parseAtestDump(output)) {
        auto frame = packet::decode(bytes);
        if (!frame) continue;
        result.packets.push_back(frame->source + ": " + packet::printable(frame->info));
        if (!text.empty()) text += "\n";
        text += packet::printable(frame->info);
        if (!settings.callsign.empty() && frame->source != settings.callsign)
            result.error = "packet from " + frame->source + ", expected " + settings.callsign;
    }
    result.decoded = normalizeText(text);
    result.accuracy = textAccuracy(expected, text);
    result.passed = result.error.empty() && result.accuracy >= minAccuracy;
    return result;
}

std::string decodePacketsFile(const Settings& settings, const std::string& path, std::string& error) {
    Settings anySender = settings;
    anySender.callsign.clear();          // Any sender
    CaseResult result = decodeFile(anySender, path, "", 0);
    error = result.error;
    std::string text;
    for (const std::string& line : result.packets) text += line + "\n";
    return text;
}

CaseResult runCase(const AudioCase& c, const std::string& dir) {
    return decodeFile(c.settings, (fs::path(dir) / c.file).string(), c.text, c.minAccuracy);
}

namespace {

// These tools have no --version, so just check they start
bool starts(const std::vector<std::string>& args) {
    os::Process process;
    if (!process.start(args)) return false;
    char buffer[256];
    while (process.read(buffer, sizeof buffer) > 0) {}
    process.wait();
    return true;
}

} // namespace

bool direwolfAvailable() { return starts({os::direwolfPath(), "-u"}); }
bool atestAvailable()    { return starts({os::atestPath()}); }
bool genPacketsAvailable() { return starts({os::genPacketsPath()}); }

bool writeTestAudio(const std::string& dir, std::string& error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const AudioCase& c : standardCases()) {
        std::string path = (fs::path(dir) / c.file).string();
        if (!fsk::writeWav(path, generate(c))) {
            error = "cannot write " + path;
            return false;
        }
    }
    for (const ToneFile& t : toneFiles()) {
        std::string path = (fs::path(dir) / t.file).string();
        if (!fsk::writeWav(path, fsk::tone(t.hz, t.seconds, 8000))) {
            error = "cannot write " + path;
            return false;
        }
    }

    std::ofstream readme(fs::path(dir) / "README.md");
    readme << "# Test audio\n\n"
              "Generated by `RadioTests --generate src/tests/audio` from `testcases.cpp`. Don't edit.\n"
              "Packet files are from " << testCallsign << " to " << packet::groupAddress << ".\n\n"
              "| File | What it is | Settings | Must match | Text sent |\n"
              "|---|---|---|---|---|\n";
    for (const AudioCase& c : standardCases()) {
        readme << "| `" << c.file << "` | " << c.description << " | " << describeModem(c.settings)
               << ", " << c.rate << " Hz | " << percent(c.minAccuracy) << " | "
               << (c.text.empty() ? "*(nothing)*" : "`" + oneLine(c.text) + "`") << " |\n";
    }
    readme << "\n## Tones\n\n8 kHz, 16-bit.\n\n"
              "| File | Tone |\n|---|---|\n";
    for (const ToneFile& t : toneFiles()) readme << "| `" << t.file << "` | " << t.description << " |\n";
    if (!readme) {
        error = "cannot write README.md";
        return false;
    }
    return true;
}

std::string findTestAudioDir() {
    std::vector<fs::path> candidates;
    std::string exeDir = os::executableDir();
    if (!exeDir.empty()) candidates.push_back(fs::path(exeDir) / "test-audio");
#ifdef RTC_SOURCE_DIR
    candidates.push_back(fs::path(RTC_SOURCE_DIR) / "tests" / "audio");
#endif
    candidates.push_back(fs::path("tests") / "audio");
    for (const fs::path& dir : candidates) {
        std::error_code ec;
        if (fs::exists(dir / standardCases().front().file, ec)) return dir.string();
    }
    return "";
}
