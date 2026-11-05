// RadioTests: every test, with no arguments needed (run by ctest too).
//   unit   The code on its own: settings, direwolf's config, AX.25, FX.25, the connector,
//          the Bluetooth protocol
//   audio  Decodes tests/audio with direwolf's atest (skipped without it)
//   channel AX.25 through simulated NFM and SSB radios with noise, mistuning and fading:
//          how far each mode gets. The audio is saved in channel-audio next to the program
//   cat    Radio control through Hamlib's dummy rig, or a real radio with --cat-port
//   air    RadioConnector and a stand-in station over the sound cards (e.g. VB-Cable):
//          receiving, a missing part, and sending with acks. Skipped without sound cards
// RadioTests --help lists the options. On GitHub Actions, results also go to annotations
// and the job summary.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../backend.h"
#include "../Bluetooth/bt_protocol.h"
#include "../Connector/connector.h"
#include "../Connector/connector_config.h"
#include "../Radio/cat.h"
#include "../Radio/fsk.h"
#include "../Radio/modem.h"
#include "../Connector/packet.h"
#include "../settings.h"
#include "../testcases.h"
#include "channel.h"

using std::string;

namespace {

// GitHub Actions reporting

bool onGitHub() {
    const char* value = std::getenv("GITHUB_ACTIONS");
    return value && std::strcmp(value, "true") == 0;
}

// Annotation messages are one line: GitHub wants %, CR and LF escaped
std::string escapeAnnotation(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '%') out += "%25";
        else if (c == '\r') out += "%0D";
        else if (c == '\n') out += "%0A";
        else out += c;
    }
    return out;
}

// level is "error", "warning" or "notice"; file is relative to the repository
void annotate(const char* level, const std::string& file, int line, const std::string& title,
              const std::string& message) {
    if (!onGitHub()) return;
    std::printf("::%s file=%s", level, file.c_str());
    if (line > 0) std::printf(",line=%d", line);
    std::printf(",title=%s::%s\n", title.c_str(), escapeAnnotation(message).c_str());
}

void appendSummary(const std::string& markdown) {
    const char* path = std::getenv("GITHUB_STEP_SUMMARY");
    if (!path) return;
    if (std::FILE* f = std::fopen(path, "a")) {
        std::fputs(markdown.c_str(), f);
        std::fclose(f);
    }
}

std::string runnerName() {
    const char* os = std::getenv("RUNNER_OS");
    return os ? os : "local";
}

// Keeps a table cell on one line and stops '|' from starting a new column
std::string cell(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '|') out += "\\|";
        else if (c == '\n' || c == '\r') out += ' ';
        else out += c;
    }
    return out;
}

std::string percent(float value) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.0f%%", value * 100);
    return buf;
}

// Unit test checks

int failures = 0;
int checks = 0;
std::vector<std::string> failureLines;   // For the summary

#define CHECK(condition) check((condition), #condition, __LINE__)

void check(bool ok, const char* what, int line) {
    checks++;
    if (ok) return;
    failures++;
    std::printf("FAIL src/tests/radio_tests.cpp:%d: %s\n", line, what);
    annotate("error", "src/tests/radio_tests.cpp", line, "Unit test failed", what);
    failureLines.push_back("line " + std::to_string(line) + ": `" + cell(what) + "`");
}

bool contains(const std::vector<std::string>& args, const std::vector<std::string>& run) {
    for (size_t i = 0; i + run.size() <= args.size(); i++) {
        bool match = true;
        for (size_t j = 0; j < run.size() && match; j++) match = args[i + j] == run[j];
        if (match) return true;
    }
    return false;
}

void testSettingsRoundTrip() {
    Settings s;
    s.operation = Operation::Duplex;
    s.packetBaud = 300;
    s.rxAudio = "plughw:CARD=PCH,DEV=0";
    s.rxAudioDesc = "HDA Intel PCH - Front mic";
    s.txAudio = "default";
    s.catPort = "/dev/ttyACM0";
    s.catBaud = 115200;
    s.audioFile = "C:\\Users\\me\\My Recordings\\packet test.wav";

    Settings back = parseSettings(formatSettings(s));
    CHECK(back.operation == Operation::Duplex);
    CHECK(back.packetBaud == 300);
    CHECK(back.rxAudio == s.rxAudio);
    CHECK(back.rxAudioDesc == s.rxAudioDesc);
    CHECK(back.txAudio == "default");
    CHECK(back.catPort == "/dev/ttyACM0");
    CHECK(back.catBaud == 115200);
    CHECK(back.audioFile == s.audioFile);
}

void testSettingsGarbage() {
    Settings s = parseSettings("packet_baud=abc\noperation=sideways\nnonsense\n=\ncallsign = N0CALL-3 \r\n"
                               "# comment=1\r\nfx25=64\r\n");
    Settings defaults;
    CHECK(s.packetBaud == defaults.packetBaud);
    CHECK(s.operation == defaults.operation);
    CHECK(s.callsign == "N0CALL-3");   // Spaces and Windows line endings are fine
    CHECK(s.fx25 == 64);
}

void testPacketSettings() {
    Settings s;
    s.packetBaud = 9600;
    s.callsign = "N0CALL-7";
    s.rigModel = 3073;
    Settings back = parseSettings(formatSettings(s));
    CHECK(back.packetBaud == 9600);
    CHECK(back.callsign == "N0CALL-7");
    CHECK(back.rigModel == 3073);
    CHECK(formatSettings(s).find("modem=") == std::string::npos);   // minimodem's keys are gone
    // Files from the minimodem days still load: their RTTY lines are ignored
    Settings old = parseSettings("modem=minimodem\nprotocol=rtty\nbaud=45.45\nshift=170\nsquelch=3.5\n"
                                 "quiet=1\npacket_baud=4800\noperation=transmit\ncallsign=N0CALL\n");
    CHECK(old.rigModel == defaultRigModel);
    CHECK(old.packetBaud == 1200);                        // 4800 isn't a direwolf speed we offer
    CHECK(old.operation == Operation::Transmit && old.callsign == "N0CALL");
}

void testPttSettings() {
    Settings s;
    s.catPort = "COM5";
    s.ptt = PttType::Rts;
    s.pttPort = "COM6";
    Settings back = parseSettings(formatSettings(s));
    CHECK(back.ptt == PttType::Rts && back.pttPort == "COM6" && back.keyingPort() == "COM6");
    // Files from before ptt=: a CAT port meant CAT PTT, no port meant VOX
    CHECK(parseSettings("cat_port=COM5\n").ptt == PttType::Cat);
    CHECK(parseSettings("cat_port=\n").ptt == PttType::Vox);
    CHECK(parseSettings("cat_port=COM5\nptt=vox\n").ptt == PttType::Vox);   // Radio control only
    CHECK(parseSettings("ptt=sometimes\n").ptt == PttType::Vox);
    CHECK(Settings{}.ptt == PttType::Vox);

    Settings p;
    CHECK(pttProblem(p).empty());
    p.ptt = PttType::Cat;
    CHECK(!pttProblem(p).empty());   // No CAT port to send the command on
    p.catPort = "COM5";
    CHECK(pttProblem(p).empty());
    p.ptt = PttType::Dtr;
    CHECK(pttProblem(p).empty() && p.keyingPort() == "COM5");   // The CAT port's own line
    p.catPort.clear();
    CHECK(!pttProblem(p).empty());   // No port at all
    p.pttPort = "/dev/ttyUSB1";
    CHECK(pttProblem(p).empty());    // Keying only, no radio control
    p.catPort = "127.0.0.1:4532";    // Their own rigctld for CAT...
    p.rigModel = ownRigctldModel;
    CHECK(pttProblem(p).empty());    // ...and RTS on a real port is fine
    p.pttPort.clear();
    CHECK(!pttProblem(p).empty());   // An address has no RTS line
}

void testFx25() {
    Settings s;
    CHECK(s.fx25 == 1);                                   // Auto unless chosen
    for (int mode : {0, 1, 16, 32, 64}) {
        s.fx25 = mode;
        CHECK(parseSettings(formatSettings(s)).fx25 == mode);
    }
    CHECK(parseSettings("fx25=7\n").fx25 == 1);           // Not a mode direwolf offers
    CHECK(fx25Name(0) == "Off");
    CHECK(fx25Name(1) == "Auto");
    CHECK(fx25Name(32) == "32 check bytes");

    s.fx25 = 0;
    CHECK(direwolfConfig(s, "1", "2", 7001).find("FX25TX") == std::string::npos);
    CHECK(!contains(genPacketsArgs(s, "out.wav"), {"-X"}));
    s.fx25 = 16;
    CHECK(direwolfConfig(s, "1", "2", 7001).find("FX25TX 16\n") != std::string::npos);
    s.fx25 = 1;
    auto gen = genPacketsArgs(s, "out.wav");
    CHECK(contains(gen, {"-X", "1"}));
    CHECK(contains(gen, {"-o", "out.wav", "-"}));        // Output still last

    // The longest packet must fit one FX.25 block even with worst-case bit stuffing
    // (all ones: a stuffed bit every 5), or direwolf sends it as plain AX.25
    CHECK(packet::maxInfoFor(0) == packet::maxInfo);
    const int modes[] = {1, 16, 32, 64};
    const size_t blocks[] = {239, 239, 223, 191};
    for (int i = 0; i < 4; i++) {
        size_t info = packet::maxInfoFor(modes[i]);
        CHECK(info > 100 && info < packet::maxInfo);
        auto frame = packet::encodeUi("N0CALL-15", "CQ", std::string(info, '\xff'));
        size_t bits = (frame.size() + 2) * 8;            // With the FCS
        CHECK((bits + bits / 5 + 7) / 8 + 2 <= blocks[i]);
    }
    CHECK(packet::maxInfoFor(64) < packet::maxInfoFor(32));
    CHECK(packet::maxInfoFor(32) < packet::maxInfoFor(16));

    // Check bytes and the padded block take longer to send
    auto shortFrame = packet::encodeUi("N0CALL", "CQ", "hi");
    CHECK(packet::airtime(shortFrame.size(), 1200, 1) > packet::airtime(shortFrame.size(), 1200));
    CHECK(packet::airtime(shortFrame.size(), 1200, 64) > packet::airtime(shortFrame.size(), 1200, 16));
}

void testCallsigns() {
    CHECK(packet::normalizeCallsign("n0call") == "N0CALL");
    CHECK(packet::normalizeCallsign(" w1aw-7 ") == "W1AW-7");
    CHECK(packet::normalizeCallsign("W1AW-0") == "W1AW");
    CHECK(packet::normalizeCallsign("W1AW-15") == "W1AW-15");
    CHECK(packet::normalizeCallsign("W1AW-16").empty());
    CHECK(packet::normalizeCallsign("TOOLONG1").empty());
    CHECK(packet::normalizeCallsign("W1/AW").empty());
    CHECK(packet::normalizeCallsign("W1AW-").empty());
    CHECK(packet::normalizeCallsign("").empty());
}

void testAx25Frames() {
    // Same bytes direwolf's gen_packets makes for N0CALL>CQ, apart from the C bits
    packet::Bytes frame = packet::encodeUi("N0CALL", "CQ", "Hi");
    packet::Bytes expected = {0x86, 0xa2, 0x40, 0x40, 0x40, 0x40, 0xe0,
                              0x9c, 0x60, 0x86, 0x82, 0x98, 0x98, 0x61, 0x03, 0xf0, 'H', 'i'};
    CHECK(frame == expected);

    auto back = packet::decode(packet::encodeUi("W1AW-12", "CQ", "{\"msg\":\"73\"}"));
    CHECK(back.has_value());
    CHECK(back && back->source == "W1AW-12");
    CHECK(back && back->destination == "CQ");
    CHECK(back && back->path.empty());
    CHECK(back && back->info == "{\"msg\":\"73\"}");

    // Non-UI frames without information, and truncated ones, give nothing
    packet::Bytes sabm = frame;
    sabm.resize(15);
    sabm[14] = 0x2F;
    CHECK(!packet::decode(sabm));
    CHECK(!packet::decode(packet::Bytes(frame.begin(), frame.begin() + 10)));
    CHECK(!packet::decode({}));

    CHECK(packet::printable("hello\r\n") == "hello");
    CHECK(packet::printable("a\tb") == "a<0x09>b");
    CHECK(packet::printable("caf\xc3\xa9") == "caf\xc3\xa9");   // UTF-8 left alone
}

void testSplitInfo() {
    std::string longLine(600, 'x');
    auto pieces = packet::splitInfo(longLine);
    CHECK(pieces.size() == 3);
    CHECK(pieces.size() == 3 && pieces[0].size() == 256 && pieces[2].size() == 88);
    // Never inside a UTF-8 character: 255 x's then a 2-byte character
    std::string utf8 = std::string(255, 'x') + "\xc3\xa9";
    auto split = packet::splitInfo(utf8);
    CHECK(split.size() == 2 && split[0].size() == 255 && split[1] == "\xc3\xa9");
    CHECK(packet::splitInfo("").empty());
}

void testKiss() {
    packet::Bytes frame = {0x01, 0xC0, 0x02, 0xDB, 0x03};
    std::string wire = packet::kissEncode(frame);
    std::string expected = {'\xC0', '\x00', '\x01', '\xDB', '\xDC', '\x02', '\xDB', '\xDD', '\x03', '\xC0'};
    CHECK(wire == expected);

    // Split across reads, with a non-data command frame and empty frames around it
    packet::KissDecoder decoder;
    std::string stream = std::string("\xC0\xC0\x06\x10\xC0", 5) + wire;
    auto first = decoder.feed(stream.substr(0, 9));
    auto second = decoder.feed(stream.substr(9));
    CHECK(first.empty());
    CHECK(second.size() == 1 && second[0] == frame);
}

void testAtestDump() {
    // Shortened from real `atest -h` output, colour codes and all
    std::string output =
        "\x1b[38;2;0;192;0m\nDECODED[1] 0:00.486 N0CALL audio level = 50(14/14)\n"
        "\x1b[38;2;0;192;0m[0] N0CALL>CQ:Hi <0x0a>\n------\n"
        "U frame UI: p/f=0, No layer 3 protocol implemented., length = 18\n"
        " dest    CQ      0 c/r=1 res=3 last=0\n"
        " source  N0CALL  0 c/r=1 res=3 last=1\n"
        "  000:  86 a2 40 40 40 40 e0 9c 60 86 82 98 98 e1 03 f0  ..@@@@..`.......\n"
        "  010:  48 69                                            Hi\n"
        "------\n"
        "1 packets decoded in 0.016 seconds.\n";
    auto frames = packet::parseAtestDump(output);
    CHECK(frames.size() == 1);
    auto frame = frames.empty() ? std::nullopt : packet::decode(frames[0]);
    CHECK(frame && frame->source == "N0CALL" && frame->info == "Hi");
}

void testMonitorLines() {
    auto lines = packet::monitorLines("N0CALL-1", "CQ", "a<b\tc");
    CHECK(lines.size() == 1 && lines[0] == "N0CALL-1>CQ:a<0x3c>b<0x09>c");
    // Escapes make this too long for one gen_packets line, so it's split
    auto escaped = packet::monitorLines("N0CALL", "CQ", std::string(200, '<'));
    CHECK(escaped.size() == 4);
    for (const auto& line : escaped) CHECK(line.size() <= 390);
}

void testDirewolfConfig() {
    Settings s;
    s.packetBaud = 300;
    s.callsign = "N0CALL-7";
    std::string vox = direwolfConfig(s, "plughw:1,0", "default", 7001);
    CHECK(vox.find("ADEVICE plughw:1,0 default\n") != std::string::npos);
    CHECK(vox.find("MODEM 300\n") != std::string::npos);
    CHECK(vox.find("MYCALL N0CALL-7\n") != std::string::npos);
    CHECK(vox.find("KISSPORT 0\nKISSPORT 7001\n") != std::string::npos);
    CHECK(vox.find("AGWPORT 0\n") != std::string::npos);
    CHECK(vox.find("PTT") == std::string::npos);           // VOX: direwolf mustn't key anything

    s.catPort = "COM5";
    CHECK(direwolfConfig(s, "1", "2", 7001).find("PTT") == std::string::npos);   // A CAT port alone keys nothing
    s.ptt = PttType::Cat;
    s.catBaud = 38400;
    s.rigModel = 1037;   // FTDX-3000
    CHECK(direwolfConfig(s, "1", "2", 7001).find("PTT RIG 1037 COM5 38400\n") != std::string::npos);
    s.ptt = PttType::Rts;   // The FTDX3000's Standard port, next to CAT on its Enhanced port
    s.pttPort = "COM6";
    CHECK(direwolfConfig(s, "1", "2", 7001).find("PTT COM6 RTS\n") != std::string::npos);
    s.ptt = PttType::Dtr;   // The CAT port's own line
    s.pttPort.clear();
    CHECK(direwolfConfig(s, "1", "2", 7001).find("PTT COM5 DTR\n") != std::string::npos);
    s.ptt = PttType::Cat;
    s.rigModel = ownRigctldModel;   // rigctld: host:port, no serial speed
    s.catPort = "localhost:4532";
    CHECK(direwolfConfig(s, "1", "2", 7001).find("PTT RIG 2 localhost:4532\n") != std::string::npos);

    auto atest = atestArgs(s, "in.wav");
    CHECK(contains(atest, {"-B", "300", "-h", "in.wav"}));
    auto gen = genPacketsArgs(s, "out.wav");
    CHECK(contains(gen, {"-B", "300"}));
    CHECK(contains(gen, {"-o", "out.wav", "-"}));
}

void testAx25Audio() {
    // The FCS of "123456789" is CRC-16/X.25's check value
    std::vector<uint8_t> digits = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(fsk::ax25Fcs(digits) == 0x906E);
    fsk::Audio audio = fsk::ax25({packet::encodeUi("N0CALL", "CQ", "x")}, 1200, 48000);
    CHECK(audio.rate == 48000);
    CHECK(!audio.samples.empty());
}

// Tone frequency from zero crossings, over the middle half (past the filters' settling)
double toneHz(const fsk::Audio& audio) {
    size_t from = audio.samples.size() / 4, to = audio.samples.size() * 3 / 4;
    int crossings = 0;
    for (size_t i = from + 1; i < to; i++)
        if ((audio.samples[i - 1] < 0) != (audio.samples[i] < 0)) crossings++;
    return crossings / 2.0 / (static_cast<double>(to - from) / audio.rate);
}

// The simulated radios themselves, so the channel group's numbers mean what they say
void testChannelModels() {
    fsk::Audio tone = fsk::tone(1000, 1, 48000);
    sim::Nfm micPath, dataPorts;
    dataPorts.micPath = false;
    CHECK(std::fabs(toneHz(sim::nfm(tone, micPath)) - 1000) < 2);
    CHECK(std::fabs(toneHz(sim::nfm(tone, dataPorts)) - 1000) < 2);
    sim::Ssb ssb;
    CHECK(std::fabs(toneHz(sim::ssb(tone, ssb)) - 1000) < 2);
    ssb.mistuneHz = 50;
    CHECK(std::fabs(toneHz(sim::ssb(tone, ssb)) - 1050) < 2);
    ssb.mistuneHz = -25;
    CHECK(std::fabs(toneHz(sim::ssb(tone, ssb)) - 975) < 2);

    // At 10 dB SNR, the clean signal is 10/11 of the noisy one's power
    sim::Ssb clean, noisy;
    noisy.snrDb = 10;
    fsk::Audio a = sim::ssb(tone, clean), b = sim::ssb(tone, noisy);
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = a.samples.size() / 4; i < a.samples.size() * 3 / 4; i++) {
        ab += a.samples[i] * b.samples[i];
        aa += a.samples[i] * a.samples[i];
        bb += b.samples[i] * b.samples[i];
    }
    double signalShare = ab * ab / (aa * bb);
    CHECK(std::fabs(signalShare - 10.0 / 11) < 0.02);

    // The same seed gives the same audio
    noisy.dopplerHz = 1;
    noisy.delayMs = 2;
    CHECK(sim::ssb(tone, noisy).samples == sim::ssb(tone, noisy).samples);
    sim::Nfm weak;
    weak.cnrDb = 8;
    CHECK(sim::nfm(tone, weak).samples == sim::nfm(tone, weak).samples);
}

void testAccuracy() {
    CHECK(textAccuracy("THE FOX", "THE FOX") == 1.0f);
    CHECK(textAccuracy("THE FOX", "\r\nTHE   FOX\n") == 1.0f);   // Spacing doesn't count
    CHECK(textAccuracy("ABCDEFGHIJ", "ABCDEFGHI") == 0.9f);       // One character lost
    CHECK(textAccuracy("ABC", "") == 0.0f);
    CHECK(textAccuracy("", "") == 1.0f);
    CHECK(normalizeText("  A \r\n B  ") == "A B");
}

// Radio Connector

void testAx25Path() {
    packet::Bytes frame = packet::encodeUi("N0CALL-7", "K1ABC-3", "x", {"WIDE1-1", "WIDE2-2"});
    CHECK(frame.size() == 4 * 7 + 2 + 1);
    CHECK((frame[13] & 1) == 0);    // Source isn't the last address any more
    CHECK((frame[27] & 1) == 1);
    auto back = packet::decode(frame);
    CHECK(back && back->path == std::vector<std::string>({"WIDE1-1", "WIDE2-2"}));
    frame[13 + 7] |= 0x80;          // A digipeater has repeated it
    back = packet::decode(frame);
    CHECK(back && back->path[0] == "WIDE1-1*");
    CHECK(back && back->info == "x");
}

void testAirFormat() {
    using namespace connector;
    CHECK(hexId(0x3F) == "03F");
    CHECK(hexId(0x1FFF) == "FFF");
    CHECK(formatData(0x3F, 2, 5, "a|b") == "RC1|D|03F|2/5|a|b");
    auto data = parseAir("RC1|D|03F|2/5|a|b");
    CHECK(data && data->kind == AirFrame::Kind::Data && data->id == 0x3F);
    CHECK(data && data->part == 2 && data->total == 5 && data->text == "a|b");
    CHECK(parseAir("RC1|D|03F|1/1|") && parseAir("RC1|D|03F|1/1|")->text.empty());
    CHECK(!parseAir("RC1|D|03F|6/5|x"));      // Part past the end
    CHECK(!parseAir("RC1|D|03F|1/41|x"));     // More than 40 parts
    CHECK(!parseAir("RC1|D|3F|1/1|x"));       // ID is 3 digits
    CHECK(!parseAir("RC2|D|03F|1/1|x"));      // Not our version
    CHECK(!parseAir("hello"));

    CHECK(formatRanges({1, 2, 3, 5}) == "1-3,5");
    CHECK(formatRanges({7}) == "7");
    CHECK(formatAck(0xABC, {1, 2, 3, 5}) == "RC1|A|ABC|1-3,5");
    auto ack = parseAir("RC1|A|abc|1-3,5\r\n");
    CHECK(ack && ack->kind == AirFrame::Kind::Ack && ack->id == 0xABC);
    CHECK(ack && ack->parts == std::vector<int>({1, 2, 3, 5}));
    CHECK(!parseRanges("3-1"));
    CHECK(!parseRanges("0"));
    CHECK(!parseRanges("1,,2"));
    CHECK(!parseAir("RC1|A|001|"));

    CHECK(formatResult(1, "OK") == "RC1|R|001|OK");
    auto result = parseAir("RC1|R|001|EXP");
    CHECK(result && result->kind == AirFrame::Kind::Result && result->result == "EXP");
    CHECK(!parseAir("RC1|R|001|MAYBE"));

    // Part sizes: the packet limit minus the header
    CHECK(partBytes(1, 0) == 163);
    CHECK(partBytes(0, 0) == 240);
    CHECK(partBytes(1, 2) == 149);   // Each digipeater takes 7 bytes of the FX.25 block
    CHECK(validUtf8("caf\xc3\xa9 \xf0\x9f\x93\xbb"));
    CHECK(!validUtf8("\xc3"));
    CHECK(!validUtf8("\xc0\xaf"));       // Overlong
    CHECK(!validUtf8("\xed\xa0\x80"));   // Surrogate
}

// Two connectors on a simulated channel. drop() decides which frames get lost
struct Air {
    struct Station {
        std::string call;
        std::unique_ptr<connector::Engine> engine;
        std::vector<connector::Json> toMain;
        int sent = 0;
    };
    Station a, b;
    connector::TimePoint now{};
    std::vector<packet::Frame> onAir;
    std::function<bool(const packet::Frame&)> drop = [](const packet::Frame&) { return false; };

    explicit Air(int holdSeconds = 180) {
        for (Station* s : {&a, &b}) {
            s->call = s == &a ? "N0CALL-7" : "K1ABC-3";
            s->engine = std::make_unique<connector::Engine>(connector::Hooks{
                [this, s](const std::string& to, const std::string& info) {
                    s->sent++;
                    onAir.push_back({s->call, to, {}, info});
                    return true;
                },
                [s](const connector::Json& message) { s->toMain.push_back(message); }}, s == &a ? 0x10 : 0x20);
            connector::Options o;
            o.callsign = s->call;
            o.holdForMainSeconds = holdSeconds;
            s->engine->setOptions(o);
            s->engine->setRadioReady(true, now);
            s->engine->setMainConnected(true, now);
        }
    }

    void run(double seconds) {
        auto end = now + std::chrono::duration_cast<connector::Clock::duration>(std::chrono::duration<double>(seconds));
        while (now < end) {
            now += std::chrono::milliseconds(100);
            std::vector<packet::Frame> frames;
            frames.swap(onAir);
            for (const packet::Frame& frame : frames) {
                if (drop(frame)) continue;
                a.engine->onFrame(frame, now);
                b.engine->onFrame(frame, now);
            }
            a.engine->tick(now);
            b.engine->tick(now);
        }
    }

    void send(const std::string& text, const std::string& id = "m-1") {
        a.engine->send({{"to", "k1abc-3"}, {"text", text}, {"ref", "order-881"}}, id, now);
    }

    static std::vector<connector::Json> ofType(const Station& s, const std::string& type) {
        std::vector<connector::Json> out;
        for (const connector::Json& m : s.toMain)
            if (m["type"] == type) out.push_back(m);
        return out;
    }

    static std::vector<std::string> states(const Station& s) {
        std::vector<std::string> out;
        for (const connector::Json& m : ofType(s, "message.status")) out.push_back(m["body"]["state"]);
        return out;
    }

    // The main program at b takes every message it's given
    void bAcksAll() {
        for (const connector::Json& m : ofType(b, "message.received")) b.engine->mainAck(m["id"], now);
    }
};

std::string longText(size_t bytes) {
    std::string text;
    for (size_t i = 0; text.size() < bytes; i++) text += "line " + std::to_string(i) + " | caf\xc3\xa9; ";
    text.resize(bytes);
    // Don't end mid-character
    while (!text.empty() && (static_cast<uint8_t>(text.back()) & 0x80)) text.pop_back();
    return text;
}

void testConnectorDelivery() {
    Air air;
    std::string text = longText(500);   // 4 parts at 163 bytes
    air.send(text);
    CHECK(!air.a.toMain.empty() && air.a.toMain[0]["type"] == "message.accepted");   // Before any status
    CHECK(!air.a.toMain.empty() && air.a.toMain[0]["re"] == "m-1");
    CHECK(!air.a.toMain.empty() && air.a.toMain[0]["body"]["msgId"] == "N0CALL-7/010");
    CHECK(!air.a.toMain.empty() && air.a.toMain[0]["body"]["parts"] == 4);
    air.run(20);

    auto received = Air::ofType(air.b, "message.received");
    CHECK(received.size() == 1);
    CHECK(!received.empty() && received[0]["body"]["text"] == text);   // Exact, UTF-8 intact
    CHECK(!received.empty() && received[0]["body"]["from"] == "N0CALL-7");
    CHECK(!received.empty() && received[0]["body"]["msgId"] == "N0CALL-7/010");
    CHECK(Air::states(air.a).back() == "received");   // b's main program hasn't said yet
    CHECK(air.b.engine->heldForMain() == 1);

    air.bAcksAll();
    air.run(5);
    auto states = Air::states(air.a);
    // Frames arrive instantly here, so the ack can beat the airtime estimate and skip "waiting"
    states.erase(std::remove(states.begin(), states.end(), "waiting"), states.end());
    CHECK(states == std::vector<std::string>({"queued", "sending", "received", "delivered"}));
    auto last = Air::ofType(air.a, "message.status").back()["body"];
    CHECK(last["ref"] == "order-881");
    CHECK(last["attempt"] == 1);
    CHECK(last["partsAcked"] == connector::Json({1, 2, 3, 4}));
    CHECK(air.a.engine->inFlight() == 0);
    CHECK(air.b.engine->heldForMain() == 0);
}

void testConnectorLostPart() {
    Air air;
    int dropped = 0;
    air.drop = [&](const packet::Frame& f) {
        if (f.info.rfind("RC1|D|010|2/", 0) == 0 && dropped == 0) return ++dropped > 0;
        return false;
    };
    std::string text = longText(500);
    air.send(text);
    air.run(40);
    air.bAcksAll();
    air.run(5);
    auto received = Air::ofType(air.b, "message.received");
    CHECK(received.size() == 1 && received[0]["body"]["text"] == text);
    auto states = Air::states(air.a);
    CHECK(std::find(states.begin(), states.end(), "retrying") != states.end());
    CHECK(states.back() == "delivered");
    CHECK(Air::ofType(air.a, "message.status").back()["body"]["attempt"] == 2);
    // The retry sent part 2 only: 4 parts, then 1
    CHECK(air.a.sent == 5);
}

void testConnectorNoAnswer() {
    Air air;
    air.drop = [](const packet::Frame&) { return true; };   // Nobody hears anything
    air.send("hello");
    air.run(200);
    auto last = Air::ofType(air.a, "message.status").back()["body"];
    CHECK(last["state"] == "failed");
    CHECK(last["attempt"] == 4);
    CHECK(last["reason"].is_string());
    CHECK(air.a.sent == 4);   // One part, sent once per try
    CHECK(air.a.engine->inFlight() == 0);
}

void testConnectorExpired() {
    Air air(5);   // b holds messages 5 s for its main program
    air.b.engine->setMainConnected(false, air.now);
    air.send("nobody home");
    air.run(30);
    CHECK(Air::ofType(air.b, "message.received").empty());
    CHECK(Air::states(air.a).back() == "expired");

    // b's main program comes back after the hold: the message is gone, and a repeat
    // part gets the ack and EXP again, not a second delivery
    air.b.engine->setMainConnected(true, air.now);
    air.onAir.push_back({"N0CALL-7", "K1ABC-3", {}, connector::formatData(0x10, 1, 1, "nobody home")});
    air.run(5);
    CHECK(Air::ofType(air.b, "message.received").empty());
}

void testConnectorReconnect() {
    Air air;
    air.b.engine->setMainConnected(false, air.now);
    air.send("held for you");
    air.run(20);
    CHECK(Air::ofType(air.b, "message.received").empty());
    CHECK(air.b.engine->heldForMain() == 1);
    air.b.engine->setMainConnected(true, air.now);   // Sent on reconnect
    auto received = Air::ofType(air.b, "message.received");
    CHECK(received.size() == 1 && received[0]["body"]["text"] == "held for you");
    air.bAcksAll();
    air.run(5);
    CHECK(Air::states(air.a).back() == "delivered");

    // A repeat after delivery is acked but not delivered twice
    air.onAir.push_back({"N0CALL-7", "K1ABC-3", {}, connector::formatData(0x10, 1, 1, "held for you")});
    air.run(5);
    CHECK(Air::ofType(air.b, "message.received").size() == 1);
}

void testConnectorLostResult() {
    Air air;
    bool droppedResult = false;
    air.drop = [&](const packet::Frame& f) {
        if (!droppedResult && f.info == "RC1|R|010|OK") return droppedResult = true;
        return false;
    };
    air.send("result goes missing");
    air.run(10);
    air.bAcksAll();
    air.run(250);   // Past b's hold time + 30 s: a pokes with the last part
    CHECK(droppedResult);
    CHECK(Air::states(air.a).back() == "delivered");
    CHECK(Air::ofType(air.b, "message.received").size() == 1);
}

void testConnectorOneAtATime() {
    Air air;
    air.drop = [](const packet::Frame& f) { return f.source == "K1ABC-3"; };   // No acks
    air.send("first", "m-1");
    air.send("second", "m-2");
    CHECK(air.a.engine->txQueue() == 1);
    CHECK(air.a.engine->inFlight() == 1);
    air.run(1);
    CHECK(air.a.sent == 1);   // Only the first message is on the air
    air.drop = [](const packet::Frame&) { return false; };
    air.run(60);
    air.bAcksAll();
    air.run(10);
    CHECK(Air::ofType(air.b, "message.received").size() == 2);
    CHECK(air.a.engine->txQueue() == 0);
}

// At 300 baud one full part takes ~7 s, so the receiver must not ack between parts
void testConnectorSlowAck() {
    using namespace std::chrono;
    struct Rx {
        std::vector<std::pair<double, std::string>> acks;   // When, which parts
        connector::TimePoint start{}, now{};
        connector::Engine engine;
        Rx() : engine({[this](const std::string&, const std::string& info) {
                           auto air = connector::parseAir(info);
                           if (air && air->kind == connector::AirFrame::Kind::Ack)
                               acks.push_back({duration<double>(now - start).count(), connector::formatRanges(air->parts)});
                           return true;
                       },
                       [](const connector::Json&) {}}, 0x20) {
            connector::Options o;
            o.callsign = "K1ABC-3";
            o.baud = 300;
            engine.setOptions(o);
            engine.setRadioReady(true, now);
        }
        void runTo(double seconds) {
            while (duration<double>(now - start).count() < seconds) {
                now += milliseconds(100);
                engine.tick(now);
            }
        }
        void hear(int part) {
            engine.onFrame({"N0CALL-7", "K1ABC-3", {}, connector::formatData(0x10, part, 4, "x")}, now);
        }
    };
    const double gap = 6.5;

    Rx all;   // Every part heard: one ack, once the last is in
    for (int part = 1; part <= 4; part++) {
        all.runTo(gap * (part - 1));
        all.hear(part);
    }
    all.runTo(gap * 3 + 10);
    CHECK(all.acks.size() == 1);
    CHECK(!all.acks.empty() && all.acks[0].first >= gap * 3 && all.acks[0].second == "1-4");

    Rx middle;   // Part 2 lost: the gap from 1 to 3 is two parts long
    for (int part : {1, 3, 4}) {
        middle.runTo(gap * (part - 1));
        middle.hear(part);
    }
    middle.runTo(gap * 3 + 10);
    CHECK(middle.acks.size() == 1);
    CHECK(!middle.acks.empty() && middle.acks[0].first > gap * 3 && middle.acks[0].second == "1,3-4");

    Rx tail;   // Parts 2-4 lost: ack part 1 once they'd have been heard
    tail.hear(1);
    tail.runTo(60);
    CHECK(tail.acks.size() == 1);
    CHECK(!tail.acks.empty() && tail.acks[0].first > gap * 3 && tail.acks[0].first < 30);
    CHECK(!tail.acks.empty() && tail.acks[0].second == "1");
}

void testConnectorRequests() {
    Air air;
    auto lastError = [&] { return air.a.toMain.back()["body"]; };
    air.a.engine->send({{"to", "not a call"}, {"text", "x"}}, "m-1", air.now);
    CHECK(air.a.toMain.back()["type"] == "error" && lastError()["field"] == "to");
    CHECK(air.a.toMain.back()["re"] == "m-1");
    air.a.engine->send({{"to", "N0CALL-7"}, {"text", "x"}}, "m-2", air.now);
    CHECK(lastError()["field"] == "to");   // Ourselves
    air.a.engine->send({{"to", "K1ABC-3"}, {"text", ""}}, "m-3", air.now);
    CHECK(lastError()["field"] == "text");
    air.a.engine->send({{"to", "K1ABC-3"}, {"text", std::string(40 * 163 + 1, 'x')}}, "m-4", air.now);
    CHECK(lastError()["code"] == "too_long");
    air.a.engine->send({{"to", "K1ABC-3"}, {"text", std::string(40 * 163, 'x')}}, "m-5", air.now);
    CHECK(air.a.toMain.back()["type"] == "message.status");   // Exactly 40 parts is fine
}

void testConnectorConfig() {
    using namespace connector;
    Settings s;
    Json config = configToJson(s);
    CHECK(config["fx25"] == "auto");   // On by default
    CHECK(config["ptt"]["type"] == "vox");
    CHECK(config["retry"]["tries"] == 4);
    CHECK(missingConfig(s) == "callsign");

    ConfigChange change = applyConfig(s, {{"callsign", "n0call-7"}, {"fx25", 32}, {"path", {"wide1-1"}}});
    CHECK(change.ok && change.restartRequired);
    CHECK(s.callsign == "N0CALL-7" && s.fx25 == 32 && s.path == std::vector<std::string>({"WIDE1-1"}));

    change = applyConfig(s, {{"retry", {{"tries", 6}}}, {"holdForMainSeconds", 60}});
    CHECK(change.ok && !change.restartRequired);
    CHECK(s.retryTries == 6 && s.retryExtraWait == 10 && s.holdForMain == 60);

    change = applyConfig(s, {{"callsign", "N1NEW"}, {"fx25", "sometimes"}});
    CHECK(!change.ok && change.field == "fx25");
    CHECK(s.callsign == "N0CALL-7");   // Nothing changes on an error
    CHECK(!applyConfig(s, {{"colour", "red"}}).ok);
    CHECK(config["radio"]["type"] == "none");
    CHECK(applyConfig(s, {{"radio", {{"type", "cat"}}}}).field == "radio.port");   // Needs a port
    CHECK(applyConfig(s, {{"ptt", {{"type", "cat"}}}}).field == "ptt");   // Needs radio control

    change = applyConfig(s, {{"radio", {{"type", "cat"}, {"port", "COM5"}, {"rigModel", 3073}}}});
    CHECK(change.ok && change.restartRequired);
    CHECK(s.catPort == "COM5" && s.rigModel == 3073 && s.catBaud == 19200);
    CHECK(s.ptt == PttType::Vox);   // Radio control alone doesn't change the keying
    CHECK(configToJson(s)["radio"]["type"] == "cat");
    change = applyConfig(s, {{"ptt", {{"type", "rts"}, {"port", "COM6"}}}});
    CHECK(change.ok && change.restartRequired && s.ptt == PttType::Rts && s.pttPort == "COM6");
    CHECK(configToJson(s)["ptt"]["port"] == "COM6");
    CHECK(applyConfig(s, {{"ptt", {{"type", "cat"}, {"port", "COM6"}}}}).field == "ptt.port");   // rts/dtr only
    change = applyConfig(s, {{"ptt", {{"type", "cat"}}}});
    CHECK(change.ok && s.ptt == PttType::Cat && s.pttPort.empty());
    CHECK(!configToJson(s)["ptt"].contains("port"));
    // CAT keying needs radio control, so taking the radio away alone is refused
    CHECK(applyConfig(s, {{"radio", {{"type", "none"}}}}).field == "ptt");
    CHECK(s.catPort == "COM5");
    CHECK(applyConfig(s, {{"radio", {{"type", "none"}}}, {"ptt", {{"type", "dtr"}, {"port", "COM7"}}}}).ok);
    CHECK(!s.hasCat() && s.ptt == PttType::Dtr && s.keyingPort() == "COM7");
    // Their own rigctld for CAT, with DTR still on its own port
    change = applyConfig(s, {{"radio", {{"type", "rigctld"}, {"port", "127.0.0.1:4532"}}}});
    CHECK(change.ok && s.rigModel == ownRigctldModel && configToJson(s)["radio"]["type"] == "rigctld");
    CHECK(applyConfig(s, {{"ptt", {{"type", "dtr"}, {"port", ""}}}}).field == "ptt");   // No line on an address
    CHECK(applyConfig(s, {{"ptt", {{"type", "cat"}}}}).ok && s.ptt == PttType::Cat);
    CHECK(applyConfig(s, {{"ptt", {{"type", "vox"}}}, {"radio", {{"type", "none"}}}}).ok && !s.hasCat());
    CHECK(applyConfig(s, {{"ptt", {{"type", "push"}}}}).field == "ptt.type");

    change = applyConfig(s, {{"main", "localhost:9000"}});
    CHECK(change.ok && change.mainChanged && !change.restartRequired);
    CHECK(!applyConfig(s, {{"main", "example.com:9000"}}).ok);   // IP addresses only
    std::string host;
    int port = 0;
    CHECK(parseHostPort("localhost:9000", host, port) && host == "127.0.0.1" && port == 9000);
    CHECK(!parseHostPort("127.0.0.1", host, port));
    CHECK(!parseHostPort("127.0.0.1:70000", host, port));

    // The connector's keys survive settings.ini
    Settings back = parseSettings(formatSettings(s));
    CHECK(back.path == s.path && back.retryTries == 6 && back.holdForMain == 60);
    CHECK(back.mainAddress == "localhost:9000");
    CHECK(parseSettings("retry_tries=0\n").retryTries == 4);   // Out of range keeps the default
}

// Bluetooth audio (protocol only; no radio needed)

void testBtAddresses() {
    uint8_t uuid[16];
    CHECK(bt::parseUuid(bt::audioUuid, uuid) && uuid[0] == 0x39 && uuid[15] == 0xe6);
    CHECK(!bt::parseUuid("39144315-32fa-40db-85ed", uuid));
    CHECK(bt::normalizeAddress("38:d2:00:01:c0:ab") == "38:D2:00:01:C0:AB");
    CHECK(bt::normalizeAddress("38-D2-00-01-C0-AB") == "38:D2:00:01:C0:AB");
    CHECK(bt::normalizeAddress("38:D2:00:01:C0").empty());
    CHECK(bt::normalizeAddress("38:D2:00:01:C0:AG").empty());
}

void testBtAudioFraming() {
    // 0x7d and 0x7e inside a frame are escaped
    const uint8_t payload[] = {0x01, 0x7e, 0x02, 0x7d, 0x03};
    bt::Bytes frame = bt::audioFrame(bt::audioData, payload, sizeof payload);
    CHECK(frame == bt::Bytes({0x7e, 0x00, 0x01, 0x7d, 0x5e, 0x02, 0x7d, 0x5d, 0x03, 0x7e}));
    // UVProTermBT's stop frame
    CHECK(bt::endAudioFrame() == bt::Bytes({0x7e, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e}));

    // Junk before the first flag, empty frames, and a byte at a time
    bt::Bytes stream = {0x55, 0x66};
    stream.insert(stream.end(), frame.begin(), frame.end());
    stream.insert(stream.end(), {0x7e, 0x7e});
    stream.insert(stream.end(), bt::endAudioFrame().begin(), bt::endAudioFrame().end());
    bt::AudioDeframer deframer;
    std::vector<bt::AudioFrame> frames;
    for (uint8_t b : stream)
        for (bt::AudioFrame& f : deframer.feed(&b, 1)) frames.push_back(f);
    CHECK(frames.size() == 2);
    CHECK(frames.size() == 2 && frames[0].command == bt::audioData &&
          frames[0].payload == bt::Bytes(payload, payload + sizeof payload));
    CHECK(frames.size() == 2 && frames[1].command == bt::audioEnd);
}

void testBtGaia() {
    // The handshake: device info, then notifications for HT status, frequency mode and radio status
    std::vector<bt::Bytes> hello = bt::gaiaHandshake();
    CHECK(hello.size() == 4);
    CHECK(hello[0] == bt::Bytes({0xff, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x04, 0x03}));
    CHECK(hello[1] == bt::Bytes({0xff, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00, 0x06, 0x01, 0x0e}));
    CHECK(hello[3] == bt::Bytes({0xff, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x14}));

    // Replies have 0x8000 in the command. Junk (e.g. KISS) in between is skipped
    bt::Bytes stream = {0xc0, 0x00, 0xc0, 0xff};
    bt::Bytes reply = {0xff, 0x01, 0x00, 0x02, 0x00, 0x02, 0x80, 0x09, 0x01, 0xc0};   // HT status, transmitting
    bt::Bytes idle = {0xff, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00, 0x09, 0x01, 0x80};
    stream.insert(stream.end(), reply.begin(), reply.end());
    stream.insert(stream.end(), idle.begin(), idle.end());
    bt::GaiaDeframer deframer;
    std::vector<bt::GaiaFrame> frames;
    for (size_t i = 0; i < stream.size(); i += 3)
        for (bt::GaiaFrame& f : deframer.feed(stream.data() + i, std::min<size_t>(3, stream.size() - i)))
            frames.push_back(f);
    CHECK(frames.size() == 2);
    CHECK(frames.size() == 2 && frames[0].reply && frames[0].command == bt::gaiaEventNotification);
    CHECK(frames.size() == 2 && bt::transmitting(frames[0]) == 1 && bt::transmitting(frames[1]) == 0);
    CHECK(bt::transmitting({bt::gaiaBasic, bt::gaiaGetDevInfo, true, {1, 0x40}}) == -1);
}

std::vector<int16_t> btTone(double hz, double seconds, double amplitude = 12000) {
    std::vector<int16_t> pcm(static_cast<size_t>(seconds * bt::audioRate));
    for (size_t i = 0; i < pcm.size(); i++)
        pcm[i] = static_cast<int16_t>(amplitude * std::sin(2 * 3.14159265358979323846 * hz * static_cast<double>(i) / bt::audioRate));
    return pcm;
}

void testBtSbc() {
    // The radio's format: 32 kHz mono, 16 blocks, 8 subbands, loudness. 88-byte frames
    bt::SbcEncoder encoder;
    CHECK(encoder.frameSize() == 88);
    std::vector<int16_t> tone = btTone(1200, 1.0);
    bt::Bytes sbc = encoder.encode(tone.data(), tone.size());
    CHECK(sbc.size() == 250 * 88);
    CHECK(sbc.size() > 3 && sbc[0] == 0x9c && sbc[1] == 0x71 && sbc[2] == bt::txBitpool);

    // Decoding in odd pieces gives back the tone (after the filter delay)
    bt::SbcDecoder decoder;
    std::vector<int16_t> out;
    for (size_t i = 0; i < sbc.size(); i += 37) {
        std::vector<int16_t> pcm = decoder.decode(sbc.data() + i, std::min<size_t>(37, sbc.size() - i));
        out.insert(out.end(), pcm.begin(), pcm.end());
    }
    CHECK(out.size() == tone.size() && decoder.badFrames() == 0);
    double best = 0;
    for (size_t lag = 0; lag < 200 && out.size() == tone.size(); lag++) {
        double signal = 0, error = 0;
        for (size_t i = 1000; i < 30000; i++) {
            double a = tone[i], b = out[i + lag];
            signal += a * a;
            error += (a - b) * (a - b);
        }
        best = std::max(best, 10 * std::log10(signal / std::max(error, 1.0)));
    }
    CHECK(best > 40);   // dB; about 60 in practice

    // Junk between frames is skipped
    bt::Bytes junk = {0x12, 0x34};
    junk.insert(junk.end(), sbc.begin(), sbc.begin() + 88);
    bt::SbcDecoder fresh;
    CHECK(fresh.decode(junk.data(), junk.size()).size() == 128);
}

// Audio bytes the radio would get, as SBC seconds and end frames
struct BtSent {
    double seconds = 0;
    int ends = 0;
    size_t biggestFrame = 0;
};

BtSent btDecodeSent(const bt::Bytes& bytes) {
    BtSent sent;
    bt::AudioDeframer deframer;
    for (const bt::AudioFrame& f : deframer.feed(bytes.data(), bytes.size())) {
        if (f.command == bt::audioData) {
            sent.seconds += static_cast<double>(f.payload.size()) / 88 * 128 / bt::audioRate;
            sent.biggestFrame = std::max(sent.biggestFrame, f.payload.size());
        } else if (f.command == bt::audioEnd) {
            sent.ends++;
        }
    }
    return sent;
}

void testBtTransmitter() {
    bt::Transmitter tx;   // Keys over 64, unkeys after 300 ms quiet, 250 ms lead
    std::vector<int16_t> quiet(640, 0), noise(640, 40);
    bt::Bytes sent;
    for (int i = 0; i < 50; i++) {
        bt::Bytes b = tx.feed(quiet.data(), quiet.size());
        sent.insert(sent.end(), b.begin(), b.end());
        b = tx.feed(noise.data(), noise.size());   // Below the threshold
        sent.insert(sent.end(), b.begin(), b.end());
    }
    CHECK(sent.empty() && !tx.keyed());

    // A second of tone: nothing until the lead is buffered, then frames of 4 SBC frames,
    // then the end frame 300 ms after the tone stops
    std::vector<int16_t> tone = btTone(1000, 1.0);
    bt::Bytes first = tx.feed(tone.data(), 640 * 5);   // 100 ms
    CHECK(first.empty() && tx.keyed());
    sent = first;
    for (size_t i = 640 * 5; i < tone.size(); i += 640) {
        bt::Bytes b = tx.feed(tone.data() + i, 640);
        sent.insert(sent.end(), b.begin(), b.end());
    }
    CHECK(tx.keyed() && btDecodeSent(sent).ends == 0);
    for (int i = 0; i < 25; i++) {   // 500 ms quiet
        bt::Bytes b = tx.feed(quiet.data(), quiet.size());
        sent.insert(sent.end(), b.begin(), b.end());
    }
    BtSent result = btDecodeSent(sent);
    CHECK(!tx.keyed() && tx.transmissions() == 1);
    CHECK(result.ends == 1);
    CHECK(std::fabs(result.seconds - 1.3) < 0.02);   // The tone and 300 ms of quiet
    CHECK(result.biggestFrame == 4 * 88);

    // finish() unkeys part way through; with nothing keyed it sends nothing
    tx.feed(tone.data(), 6400);
    bt::Bytes rest = tx.finish();
    CHECK(btDecodeSent(rest).ends == 1 && !tx.keyed());
    CHECK(tx.finish().empty());
}

void testBtSettings() {
    Settings s;
    s.bluetooth = "38:D2:00:01:C0:AB";
    s.bluetoothName = "UV-PRO";
    s.btPlayAudio = "3";
    s.btRecordAudio = "4";
    Settings back = parseSettings(formatSettings(s));
    CHECK(back.bluetooth == s.bluetooth && back.bluetoothName == s.bluetoothName);
    CHECK(back.btPlayAudio == "3" && back.btRecordAudio == "4");
    CHECK(s.usesBluetooth());
    s.audioFile = "x.wav";
    CHECK(!s.usesBluetooth());   // A file wins

    // Sound keys the radio, so VOX only
    s.catPort = "/dev/ttyUSB0";
    s.ptt = PttType::Cat;
    CHECK(!pttProblem(s).empty());
    s.ptt = PttType::Vox;
    CHECK(pttProblem(s).empty());

    using namespace connector;
    Settings c;
    c.callsign = "N0CALL-7";
    c.catPort = "/dev/ttyUSB0";
    c.ptt = PttType::Cat;
    ConfigChange change = applyConfig(c, {{"bluetooth", {{"address", "38-d2-00-01-c0-ab"}}}});
    CHECK(change.ok && change.restartRequired);
    CHECK(c.bluetooth == "38:D2:00:01:C0:AB" && c.ptt == PttType::Vox);
    CHECK(configToJson(c)["bluetooth"]["address"] == "38:D2:00:01:C0:AB");
    CHECK(applyConfig(c, {{"bluetooth", {{"address", "radio"}}}}).field == "bluetooth.address");
    CHECK(applyConfig(c, {{"bluetooth", {{"colour", "blue"}}}}).field == "bluetooth.colour");
    CHECK(applyConfig(c, {{"ptt", {{"type", "cat"}}}}).field == "ptt");   // Not with Bluetooth
    if (bluetoothUsesSavedAudio()) {
        CHECK(missingConfig(c) == "rxAudio");
    } else {
        CHECK(missingConfig(c).empty());   // It brings its own sound card
    }
    CHECK(applyConfig(c, {{"bluetooth", {{"address", ""}}}}).ok && c.bluetooth.empty());
    CHECK(missingConfig(c) == "rxAudio");
}

int runUnit() {
    struct Test {
        const char* name;
        void (*run)();
    };
    const Test tests[] = {
        {"Settings save and load", testSettingsRoundTrip},
        {"Broken settings file", testSettingsGarbage},
        {"Accuracy scoring", testAccuracy},
        {"Simulated radios", testChannelModels},
        {"Packet settings", testPacketSettings},
        {"PTT settings", testPttSettings},
        {"Callsigns", testCallsigns},
        {"AX.25 frames", testAx25Frames},
        {"Splitting long text", testSplitInfo},
        {"KISS framing", testKiss},
        {"Reading atest's hex dump", testAtestDump},
        {"gen_packets lines", testMonitorLines},
        {"direwolf config and command lines", testDirewolfConfig},
        {"FX.25", testFx25},
        {"AX.25 test audio", testAx25Audio},
        {"AX.25 digipeater path", testAx25Path},
        {"Connector air format", testAirFormat},
        {"Connector delivery", testConnectorDelivery},
        {"Connector lost part", testConnectorLostPart},
        {"Connector no answer", testConnectorNoAnswer},
        {"Connector expired", testConnectorExpired},
        {"Connector main reconnect", testConnectorReconnect},
        {"Connector lost result", testConnectorLostResult},
        {"Connector one message at a time", testConnectorOneAtATime},
        {"Connector waits at 300 baud before acking", testConnectorSlowAck},
        {"Connector request errors", testConnectorRequests},
        {"Connector config", testConnectorConfig},
        {"Bluetooth addresses and UUIDs", testBtAddresses},
        {"Bluetooth audio framing", testBtAudioFraming},
        {"Bluetooth GAIA control frames", testBtGaia},
        {"Bluetooth SBC audio", testBtSbc},
        {"Bluetooth transmit keying", testBtTransmitter},
        {"Bluetooth settings", testBtSettings},
    };
    string summary = "### Unit tests (" + runnerName() + ")\n\n| Result | Test | Checks |\n|---|---|---|\n";
    for (const Test& test : tests) {
        int failuresBefore = failures, checksBefore = checks;
        test.run();
        int ran = checks - checksBefore, failed = failures - failuresBefore;
        std::printf("%s  %s (%d checks)\n", failed ? "FAIL" : "PASS", test.name, ran);
        summary += string("| ") + (failed ? "❌ FAIL" : "✅ PASS") + " | " + test.name + " | " +
                   std::to_string(ran - failed) + "/" + std::to_string(ran) + " |\n";
    }
    std::printf("%d of %d checks passed\n", checks - failures, checks);
    summary += "\n**" + std::to_string(checks - failures) + " of " + std::to_string(checks) +
               " checks passed**\n";
    if (!failureLines.empty()) {
        summary += "\nFailed checks in `src/tests/radio_tests.cpp`:\n";
        for (const string& line : failureLines) summary += "- " + line + "\n";
    }
    appendSummary(summary + "\n");
    return failures == 0 ? 0 : 1;
}

int runAudio(const std::string& dir) {
    if (!atestAvailable()) {
        std::printf("atest not found; skipping the audio tests\n");
        annotate("notice", "src/tests/audio/README.md", 0, "Audio tests skipped",
                 "direwolf (atest) isn't installed on this runner, so tests/audio wasn't decoded here");
        appendSummary("### Audio tests (" + runnerName() + ")\n\n⏭️ Skipped: direwolf (atest) "
                      "isn't installed on this runner.\n\n");
        return 77;
    }
    int failed = 0;
    string summary = "### Audio tests (" + runnerName() + ")\n\nEach file in `tests/audio` decoded "
                     "by direwolf's atest with the program's own settings.\n\n"
                     "| Result | Right | Needed | File | What it tests |\n|---|---|---|---|---|\n";
    string failureDetails;
    for (const AudioCase& c : standardCases()) {
        CaseResult result = runCase(c, dir);
        std::printf("%s %3.0f%% (need %.0f%%)  %s\n", result.passed ? "PASS" : "FAIL",
                    result.accuracy * 100, c.minAccuracy * 100, c.file.c_str());
        summary += string("| ") + (result.passed ? "✅ PASS" : "❌ FAIL") + " | " +
                   percent(result.accuracy) + " | " + percent(c.minAccuracy) + " | `" + c.file +
                   "` | " + cell(c.description) + " |\n";
        if (result.passed) continue;

        failed++;
        string expected = c.text.empty() ? "(nothing)" : c.text;
        string detail = result.error.empty()
            ? "expected: " + expected + "\ndecoded:  " + result.decoded
            : result.error;
        string indented = detail;
        for (size_t at = indented.find('\n'); at != string::npos; at = indented.find('\n', at + 6))
            indented.replace(at, 1, "\n     ");
        std::printf("     %s\n", indented.c_str());
        annotate("error", "src/tests/audio/" + c.file, 0, "Audio test failed: " + c.file,
                 percent(result.accuracy) + " right, needs " + percent(c.minAccuracy) + "\n" + detail);
        failureDetails += "\n**`" + c.file + "`** (" + cell(describe(c.settings)) + ")\n```\n" +
                    detail + "\n```\n";
    }
    int total = static_cast<int>(standardCases().size());
    std::printf("%d of %d passed\n", total - failed, total);
    summary += "\n**" + std::to_string(total - failed) + " of " + std::to_string(total) +
               " passed**\n" + failureDetails;
    appendSummary(summary + "\n");
    return failed == 0 ? 0 : 1;
}

// Options

const std::vector<string> allGroups = {"unit", "audio", "channel", "cat", "air"};

struct Options {
    std::set<string> groups{allGroups.begin(), allGroups.end()};
    string audioDir;                 // Empty = test-audio next to the program, else tests/audio
    string catPort;                  // Empty = Hamlib's dummy rig
    int catModel = 0;                // 0 = dummy rig, or the radio from settings with --cat-port
    int catBaud = 0;                 // 0 = from settings
    string rxAudio, txAudio;         // Empty = from settings.ini
    string callsign;                 // Empty = from settings.ini, else N0CALL
    int airSeconds = 60;             // Longest wait for any one step over the air
};

const int skippedCode = 77;

// CAT

// Hamlib's dummy rig by default; with --cat-port, a real radio. A real radio is keyed for
// a moment, then put back on its frequency and mode
int runCat(const Options& options) {
    Settings settings = loadSettings();
    bool real = !options.catPort.empty();
    rig_model_t model = options.catModel ? options.catModel : real ? settings.rigModel : RIG_MODEL_DUMMY;
    int baud = options.catBaud ? options.catBaud : settings.catBaud;
    string port = real ? options.catPort : "dummy";
    string name = radioName(model);
    std::printf("Radio: %s (Hamlib model %d)%s\n", name.empty() ? "?" : name.c_str(), static_cast<int>(model),
                real ? (" on " + port).c_str() : "");
    int failuresBefore = failures, checksBefore = checks;
    try {
        Cat radio(model, port, baud);
        double startHz = radio.frequency();
        rmode_t startMode = radio.mode();
        const double testHz = 14080000;
        radio.setFrequency(testHz);
        CHECK(std::fabs(radio.frequency() - testHz) < 1);
        radio.setMode(RIG_MODE_PKTUSB);
        CHECK(radio.mode() == RIG_MODE_PKTUSB);
        {
            PttGuard keyed(radio);
            CHECK(radio.transmitting());
            std::this_thread::sleep_for(std::chrono::milliseconds(real ? 500 : 50));
        }
        CHECK(!radio.transmitting());
        radio.setFrequency(startHz);
        radio.setMode(startMode);
    } catch (const std::exception& e) {
        std::printf("FAIL %s\n", e.what());
        annotate("error", "src/Radio/cat.cpp", 0, "CAT test failed", e.what());
        failures++;
        checks++;
    }
    int ran = checks - checksBefore, failed = failures - failuresBefore;
    std::printf("%s  CAT: frequency, mode and PTT (%d checks)\n", failed ? "FAIL" : "PASS", ran);
    appendSummary("### CAT test (" + runnerName() + ")\n\n" + (failed ? "❌ FAIL" : "✅ PASS") + ": " +
                  cell(name.empty() ? "Hamlib model " + std::to_string(model) : name) +
                  ", frequency, mode and PTT\n\n");
    return failed ? 1 : 0;
}

// Over the air

using Clock = std::chrono::steady_clock;

// What the test hears: JSON lines from the connector, and frames from the stand-in station
struct Heard {
    bool frame = false;
    string line;
    packet::Frame packet;
};

class Inbox {
public:
    void push(Heard heard) {
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(heard));
        }
        ready.notify_one();
    }
    std::optional<Heard> pop(Clock::duration wait) {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, wait, [this] { return !queue.empty(); })) return std::nullopt;
        Heard heard = std::move(queue.front());
        queue.pop_front();
        return heard;
    }

private:
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Heard> queue;
};

// A child process whose output is kept, to show if something fails
class Child {
public:
    ~Child() { stop(); }
    bool start(const std::vector<string>& argv, const string& dir = "") {
        if (!process.start(argv, true, false, dir, true)) return false;
        reader = std::thread([this] {
            char buffer[1024];
            long n;
            while ((n = process.read(buffer, sizeof buffer)) > 0) {
                std::lock_guard lock(mutex);
                output.append(buffer, static_cast<size_t>(n));
                if (output.size() > 20000) output.erase(0, output.size() - 20000);
            }
        });
        return true;
    }
    // Waits up to `grace` for it to exit on its own, then kills it
    void stop(std::chrono::seconds grace = std::chrono::seconds(0)) {
        auto deadline = Clock::now() + grace;
        while (process.running() && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (process.running()) process.kill();
        if (reader.joinable()) reader.join();
        process.wait();
    }
    string log() {
        std::lock_guard lock(mutex);
        return output;
    }
    const string& error() const { return process.error(); }

private:
    os::Process process;
    std::thread reader;
    std::mutex mutex;
    string output;
};

string connectorPath() {
#ifdef _WIN32
    const char* name = "RadioConnector.exe";
#else
    const char* name = "RadioConnector";
#endif
    std::filesystem::path path = std::filesystem::path(os::executableDir()) / name;
    std::error_code ec;
    return std::filesystem::exists(path, ec) ? path.string() : "";
}

string baseCallsign(const string& callsign) {
    string call = packet::normalizeCallsign(callsign);
    if (call.empty()) return "N0CALL";
    return call.substr(0, call.find('-'));
}

string indent(const string& text) {
    string out = "     | ";
    for (char c : text) {
        if (c == '\r') continue;
        out += c;
        if (c == '\n') out += "     | ";
    }
    return out;
}

class AirTest {
public:
    explicit AirTest(const Options& options) : options(options) {}

    ~AirTest() {
        mainLink.shutdown();   // The connector quits when its main program hangs up
        if (mainReader.joinable()) mainReader.join();
        connector.stop(std::chrono::seconds(10));
        peerLink.shutdown();
        if (peerReader.joinable()) peerReader.join();
        peer.stop();
        std::error_code ec;
        if (!workDir.empty()) std::filesystem::remove_all(workDir, ec);
    }

    // "" when ready to run, else why it's skipped
    string prepare() {
        Settings settings = loadSettings();
        string rx = options.rxAudio.empty() ? settings.rxAudio : options.rxAudio;
        string tx = options.txAudio.empty() ? settings.txAudio : options.txAudio;
        bool devicesGiven = !options.rxAudio.empty() || !options.txAudio.empty();
        if (!direwolfAvailable()) return string("direwolf not found at ") + os::direwolfPath();
        program = connectorPath();
        if (program.empty()) return "RadioConnector isn't next to RadioTests";
        if (rx.empty() || tx.empty())
            return "no sound cards set. Pick them in the menu program, or give --rx-audio and --tx-audio";
        if (settings.keysRadio() && !devicesGiven)
            return "PTT is set to key a radio, so this would transmit for real. Give --rx-audio "
                   "and --tx-audio to run it anyway";

        string base = baseCallsign(options.callsign.empty() ? settings.callsign : options.callsign);
        connectorCall = base + "-14";
        peerCall = base + "-15";

        std::random_device random;
        workDir = std::filesystem::temp_directory_path() / ("radio-tests-" + std::to_string(random() % 1000000));
        std::error_code ec;
        std::filesystem::create_directories(workDir, ec);
        if (ec) return "couldn't make " + workDir.string();

        Settings station = settings;
        station.operation = Operation::Duplex;
        station.rxAudio = rx;
        station.txAudio = tx;
        station.catPort.clear();
        station.ptt = PttType::Vox;
        station.pttPort.clear();
        station.audioFile.clear();
        station.path.clear();

        // The stand-in station: its own direwolf, driven over KISS by this program
        Settings peerSettings = station;
        peerSettings.callsign = peerCall;
        int peerPort = os::freeTcpPort();
        {
            std::ofstream out(workDir / "peer.conf");
            out << direwolfConfig(peerSettings, direwolfDevice(rx, false), direwolfDevice(tx, true), peerPort);
        }
        if (!peer.start(direwolfArgs("peer.conf"), workDir.string()))
            return failSetup("couldn't start direwolf: " + peer.error());
        auto deadline = Clock::now() + std::chrono::seconds(15);
        while (!peerLink.connect("127.0.0.1", peerPort)) {
            if (Clock::now() > deadline) return failSetup("direwolf didn't open its KISS port:\n" + peer.log());
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        peerReader = std::thread([this] {
            packet::KissDecoder decoder;
            char buffer[1024];
            long n;
            while ((n = peerLink.read(buffer, sizeof buffer)) > 0)
                for (const packet::Bytes& bytes : decoder.feed(std::string_view(buffer, static_cast<size_t>(n))))
                    if (auto frame = packet::decode(bytes)) inbox.push({true, {}, *frame});
        });

        // The connector, with this program as its main program
        int mainPort = os::freeTcpPort();
        os::TcpListener listener;
        if (!listener.listen(mainPort)) return failSetup(listener.error());
        station.callsign = connectorCall;
        station.mainAddress = "127.0.0.1:" + std::to_string(mainPort);
        string ini = (workDir / "connector.ini").string();
        if (!saveSettingsTo(station, ini)) return failSetup("couldn't write " + ini);
        if (!connector.start({program, "--settings", ini, "--verbose", "--quit-on-disconnect"}))
            return failSetup("couldn't start RadioConnector: " + connector.error());
        if (!listener.accept(mainLink, 20000))
            return failSetup("RadioConnector didn't connect: " + listener.error() + "\n" + connector.log());
        mainReader = std::thread([this] {
            string pending;
            char buffer[4096];
            long n;
            while ((n = mainLink.read(buffer, sizeof buffer)) > 0) {
                pending.append(buffer, static_cast<size_t>(n));
                size_t newline;
                while ((newline = pending.find('\n')) != string::npos) {
                    inbox.push({false, pending.substr(0, newline), {}});
                    pending.erase(0, newline + 1);
                }
            }
        });
        bool ready = waitFor(std::chrono::seconds(20), [&] {
            return findLine("radio.status", [](const connector::Json& body) {
                return body.value("state", "") == "ready";
            }) != nullptr;
        });
        if (!ready) return failSetup("RadioConnector's radio never got ready:\n" + connector.log());
        std::printf("Connector %s and stand-in station %s, on sound cards in %s / out %s\n",
                    connectorCall.c_str(), peerCall.c_str(), rx.c_str(), tx.c_str());
        return "";
    }

    bool setupFailed() const { return brokenSetup; }

    // Two parts in, an ack for both, the text to the main program, then OK once it acks
    bool receives() {
        mark();
        string one = "Over the air test, part one. ";
        string two = "Part two.";
        sendData("A01", 1, 2, one);
        sendData("A01", 2, 2, two);
        const connector::Json* received = nullptr;
        bool ok = waitFor(seconds(), [&] {
            received = findLine("message.received", [&](const connector::Json& body) {
                return body.value("msgId", "") == peerCall + "/A01";
            });
            return received && heardFromConnector("RC1|A|A01|1-2");
        });
        if (!ok) return fail("no ack for both parts, or the text never reached the main program");
        if ((*received)["body"].value("text", "") != one + two)
            return fail("text came out as \"" + (*received)["body"].value("text", "") + "\"");
        sendToConnector({{"v", 1}, {"type", "ack"}, {"re", (*received)["id"]}, {"body", connector::Json::object()}});
        if (!waitFor(seconds(), [&] { return heardFromConnector("RC1|R|A01|OK"); }))
            return fail("no RC1|R|A01|OK after the main program acked");
        return true;
    }

    // Parts 1 and 3: the ack says which it has. Then part 2 completes it
    bool missingPart() {
        mark();
        sendData("A02", 1, 3, "one ");
        sendData("A02", 3, 3, "three");
        if (!waitFor(seconds(), [&] { return heardFromConnector("RC1|A|A02|1,3"); }))
            return fail("no RC1|A|A02|1,3 for the two parts sent");
        sendData("A02", 2, 3, "two ");
        if (!waitFor(seconds(), [&] { return heardFromConnector("RC1|A|A02|1-3"); }))
            return fail("no RC1|A|A02|1-3 once part 2 came");
        if (auto* received = findLine("message.received", [&](const connector::Json& body) {
                return body.value("msgId", "") == peerCall + "/A02";
            }))
            sendToConnector({{"v", 1}, {"type", "ack"}, {"re", (*received)["id"]}, {"body", connector::Json::object()}});
        return true;
    }

    // The main program sends a long message; the stand-in station acks every part and
    // sends OK, and the main program sees it delivered
    bool sends() {
        mark();
        string text;
        for (int i = 1; text.size() < 400; i++)
            text += "Line " + std::to_string(i) + " of the over the air sending test. ";
        sendToConnector({{"v", 1}, {"type", "message.send"}, {"id", "t-1"}, {"re", nullptr},
                         {"body", {{"to", peerCall}, {"text", text}}}});
        std::map<int, string> parts;
        int total = 0;
        string id;
        bool answered = false;
        bool ok = waitFor(seconds() * 2, [&] {
            for (size_t i = frameStart; i < frames.size(); i++) {
                const packet::Frame& f = frames[i];
                auto air = connector::parseAir(f.info);
                if (f.source != connectorCall || f.destination != peerCall || !air ||
                    air->kind != connector::AirFrame::Kind::Data) continue;
                id = connector::hexId(air->id);
                total = air->total;
                parts[air->part] = air->text;
            }
            if (!answered && total > 0 && static_cast<int>(parts.size()) == total) {
                answered = true;
                sendFrame("RC1|A|" + id + "|1-" + std::to_string(total));
                sendFrame("RC1|R|" + id + "|OK");
            }
            return findLine("message.status", [](const connector::Json& body) {
                return body.value("state", "") == "delivered";
            }) != nullptr;
        });
        if (!ok) {
            if (findLine("message.accepted", [](const connector::Json&) { return true; }) == nullptr)
                return fail("the connector didn't accept message.send");
            return fail(total == 0 ? "the stand-in station heard no parts"
                        : !answered ? "the stand-in station heard " + std::to_string(parts.size()) + " of " +
                                      std::to_string(total) + " parts"
                                    : "the connector never reported delivered after the ack and OK");
        }
        string joined;
        for (auto& [number, part] : parts) joined += part;
        if (joined != text) return fail("the parts didn't join back into the text sent");
        return true;
    }

    const string& lastProblem() const { return problem; }
    string connectorLog() { return connector.log(); }

private:
    std::chrono::seconds seconds() const { return std::chrono::seconds(options.airSeconds); }

    string failSetup(const string& why) {
        brokenSetup = true;
        return why;
    }

    bool fail(const string& why) {
        problem = why;
        return false;
    }

    // Steps look only at what's heard after their start
    void mark() {
        lineStart = lines.size();
        frameStart = frames.size();
    }

    bool waitFor(Clock::duration limit, const std::function<bool()>& done) {
        auto deadline = Clock::now() + limit;
        while (!done()) {
            auto now = Clock::now();
            if (now >= deadline) return false;
            auto heard = inbox.pop(std::min<Clock::duration>(deadline - now, std::chrono::milliseconds(200)));
            if (!heard) continue;
            if (heard->frame) {
                frames.push_back(heard->packet);
            } else {
                connector::Json json = connector::Json::parse(heard->line, nullptr, false);
                if (!json.is_discarded()) lines.push_back(json);
            }
        }
        return true;
    }

    const connector::Json* findLine(const string& type, const std::function<bool(const connector::Json&)>& match) {
        for (size_t i = lineStart; i < lines.size(); i++) {
            const connector::Json& line = lines[i];
            if (line.value("type", "") == type && line.contains("body") && match(line["body"])) return &line;
        }
        return nullptr;
    }

    bool heardFromConnector(const string& info) const {
        for (size_t i = frameStart; i < frames.size(); i++)
            if (frames[i].source == connectorCall && frames[i].destination == peerCall && frames[i].info == info)
                return true;
        return false;
    }

    void sendData(const string& id, int part, int total, const string& text) {
        sendFrame(connector::formatData(std::stoul(id, nullptr, 16), part, total, text));
    }

    void sendFrame(const string& info) {
        peerLink.write(packet::kissEncode(packet::encodeUi(peerCall, connectorCall, info)));
    }

    void sendToConnector(const connector::Json& message) { mainLink.write(message.dump() + "\n"); }

    const Options& options;
    string program, connectorCall, peerCall, problem;
    std::filesystem::path workDir;
    bool brokenSetup = false;
    Child peer, connector;
    os::TcpConnection peerLink, mainLink;
    std::thread peerReader, mainReader;
    Inbox inbox;
    std::vector<connector::Json> lines;
    std::vector<packet::Frame> frames;
    size_t lineStart = 0, frameStart = 0;
};

int runAir(const Options& options) {
    AirTest test(options);
    string why = test.prepare();
    if (!why.empty() && !test.setupFailed()) {
        std::printf("SKIP  over the air: %s\n", why.c_str());
        appendSummary("### Over the air (" + runnerName() + ")\n\n⏭️ Skipped: " + cell(why) + "\n\n");
        return skippedCode;
    }
    if (!why.empty()) {
        std::printf("FAIL  over the air setup:\n%s\n", indent(why).c_str());
        annotate("error", "src/tests/radio_tests.cpp", 0, "Over the air setup failed", why);
        appendSummary("### Over the air (" + runnerName() + ")\n\n❌ Setup failed: " + cell(why) + "\n\n");
        return 1;
    }
    struct Step {
        const char* name;
        bool (AirTest::*run)();
    };
    const Step steps[] = {
        {"Receives a message in two parts, acks it, and sends OK", &AirTest::receives},
        {"Acks the parts it has when one is missing", &AirTest::missingPart},
        {"Sends a long message and reports it delivered", &AirTest::sends},
    };
    int failed = 0;
    string summary = "### Over the air (" + runnerName() + ")\n\n| Result | Step |\n|---|---|\n";
    for (const Step& step : steps) {
        bool live = os::isTerminal(stdout);   // In a terminal, "...." is overwritten by the result
        if (live) std::printf("....  %s", step.name);
        std::fflush(stdout);
        auto start = Clock::now();
        bool ok = (test.*step.run)();
        double took = std::chrono::duration<double>(Clock::now() - start).count();
        std::printf("%s%s  %s (%.0f s)\n", live ? "\r" : "", ok ? "PASS" : "FAIL", step.name, took);
        summary += string("| ") + (ok ? "✅ PASS" : "❌ FAIL") + " | " + step.name + " |\n";
        if (ok) continue;
        failed++;
        std::printf("     %s\n", test.lastProblem().c_str());
        annotate("error", "src/tests/radio_tests.cpp", 0, string("Over the air: ") + step.name, test.lastProblem());
    }
    if (failed) std::printf("     RadioConnector's log:\n%s\n", indent(test.connectorLog()).c_str());
    appendSummary(summary + "\n");
    return failed ? 1 : 0;
}

// Simulated radio channels

// Not pass/fail for every row: how far each mode gets before it breaks. Only the perfect
// and strongest rows have to decode. Every row's audio is saved, to listen to

struct ChannelRow {
    string label;
    string file;                 // In channel-audio
    std::function<fsk::Audio(const fsk::Audio&)> radio;
    float mustDecode = -1;       // Share of frames that must decode, 0-1. -1 = only reported
};

struct ChannelScenario {
    string title;
    string about;
    int baud;
    int frames;
    std::vector<ChannelRow> rows;
};

using Radio = std::function<fsk::Audio(const fsk::Audio&)>;

Radio nfmRadio(bool mic, float ifBandwidth, float cnr) {
    return [=](const fsk::Audio& audio) {
        sim::Nfm radio;
        radio.micPath = mic;
        radio.ifBandwidthHz = ifBandwidth;
        radio.cnrDb = cnr;
        return sim::nfm(audio, radio);
    };
}

Radio ssbRadio(float snr, float mistune = 0, float doppler = 0, float delayMs = 0) {
    return [=](const fsk::Audio& audio) {
        sim::Ssb radio;
        radio.snrDb = snr;
        radio.mistuneHz = mistune;
        radio.dopplerHz = doppler;
        radio.delayMs = delayMs;
        return sim::ssb(audio, radio);
    };
}

// The perfect signal (the radios, no noise), then each noise level. what: "CNR" or "SNR"
std::vector<ChannelRow> noiseSweep(const string& prefix, const string& what,
                                   const std::vector<float>& levels,
                                   const std::function<Radio(float)>& radio,
                                   float strongMust, float perfectMust = 1.0f) {
    std::vector<ChannelRow> rows;
    rows.push_back({"perfect (no noise)", prefix + "_perfect.wav", radio(sim::noNoise), perfectMust});
    for (size_t i = 0; i < levels.size(); i++) {
        char label[32], file[48];
        std::snprintf(label, sizeof label, "%s %g dB", what.c_str(), levels[i]);
        std::snprintf(file, sizeof file, "%s_%s%g.wav", prefix.c_str(), what == "CNR" ? "cnr" : "snr", levels[i]);
        rows.push_back({label, file, radio(levels[i]), i == 0 ? strongMust : -1});
    }
    return rows;
}

std::vector<ChannelScenario> channelScenarios() {
    std::vector<ChannelScenario> scenarios;
    scenarios.push_back({"AX.25 1200 on NFM, microphone in and speaker out",
        "The usual VHF/UHF setup: 3 kHz deviation, 12.5 kHz receiver IF, pre- and de-emphasis, "
        "300-3000 Hz audio. CNR is carrier to noise in the IF; FM's threshold is near 10 dB",
        1200, 20,
        noiseSweep("nfm1200", "CNR", {20, 15, 12, 10, 9, 8, 7, 6, 4},
                   [](float cnr) { return nfmRadio(true, 12500, cnr); }, 0.95f)});
    scenarios.push_back({"AX.25 9600 on NFM, data ports",
        "Flat audio in and out (the radio's 9600 data jack): 3 kHz deviation, 15 kHz IF",
        9600, 20,
        noiseSweep("nfm9600", "CNR", {25, 20, 16, 13, 11, 10, 9, 8},
                   [](float cnr) { return nfmRadio(false, 15000, cnr); }, 0.95f)});
    scenarios.push_back({"AX.25 9600 on NFM, microphone and speaker",
        "Not expected to decode: emphasis and the 300-3000 Hz audio filters ruin 9600, which "
        "is why it needs the data ports",
        9600, 20,
        noiseSweep("nfm9600mic", "CNR", {20},
                   [](float cnr) { return nfmRadio(true, 15000, cnr); }, -1, -1)});
    scenarios.push_back({"AX.25 300 on SSB, noise",
        "HF packet: 1600/1800 Hz tones through a 300-2700 Hz SSB transmitter and receiver. SNR "
        "is the average signal to noise in the 2400 Hz passband",
        300, 10,
        noiseSweep("ssb300", "SNR", {20, 15, 10, 6, 4, 3, 2, 0, -3},
                   [](float snr) { return ssbRadio(snr); }, 0.9f)});

    ChannelScenario mistuned{"AX.25 300 on SSB, mistuned",
        "The receiver off frequency, at 15 dB SNR: every tone moves by that much", 300, 10, {}};
    for (float hz : {10.0f, 25.0f, 50.0f, 100.0f}) {
        char label[32], file[48];
        std::snprintf(label, sizeof label, "%g Hz off", hz);
        std::snprintf(file, sizeof file, "ssb300_mistune%g.wav", hz);
        mistuned.rows.push_back({label, file, ssbRadio(15, hz)});
    }
    scenarios.push_back(mistuned);

    // The ITU-R F.1487 (CCIR 520) HF channels
    ChannelScenario fading{"AX.25 300 on SSB, fading",
        "Watterson model: two equal paths that fade on their own, the second delayed, at 15 dB "
        "average SNR", 300, 10, {}};
    fading.rows.push_back({"good (0.1 Hz, 0.5 ms)", "ssb300_fading_good.wav", ssbRadio(15, 0, 0.1f, 0.5f)});
    fading.rows.push_back({"moderate (0.5 Hz, 1 ms)", "ssb300_fading_moderate.wav", ssbRadio(15, 0, 0.5f, 1)});
    fading.rows.push_back({"poor (1 Hz, 2 ms)", "ssb300_fading_poor.wav", ssbRadio(15, 0, 1, 2)});
    fading.rows.push_back({"flutter (10 Hz, 0.5 ms)", "ssb300_fading_flutter.wav", ssbRadio(15, 0, 10, 0.5f)});
    scenarios.push_back(fading);
    return scenarios;
}

// channel-audio next to the program
string channelDir() {
    string exeDir = os::executableDir();
    return (std::filesystem::path(exeDir.empty() ? "." : exeDir) / "channel-audio").string();
}

int runChannel() {
    if (!atestAvailable()) {
        std::printf("SKIP  atest (from direwolf) not found\n");
        appendSummary("### Simulated radio channels (" + runnerName() + ")\n\n⏭️ Skipped: atest "
                      "(from direwolf) isn't installed on this runner.\n\n");
        return skippedCode;
    }
    string dir = channelDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::printf("Audio saved in %s\n", dir.c_str());

    int failed = 0;
    string summary = "### Simulated radio channels (" + runnerName() + ")\n\nAX.25 frames through "
                     "simulated radios, decoded by direwolf's atest. Only the perfect and strongest "
                     "rows must decode; the rest show where each mode breaks.\n";
    string readme = "Simulated radio audio from RadioTests' channel group, rewritten on every run.\n"
                    "Each file is AX.25 test frames through a simulated radio; play one to hear it.\n";
    for (const ChannelScenario& scenario : channelScenarios()) {
        Settings settings;
        settings.packetBaud = scenario.baud;
        std::vector<string> texts;
        std::vector<packet::Bytes> frames;
        for (int i = 1; i <= scenario.frames; i++) {
            char text[96];
            std::snprintf(text, sizeof text, "Channel test %02d of %d: the quick brown fox jumps over the lazy dog",
                          i, scenario.frames);
            texts.push_back(text);
            frames.push_back(packet::encodeUi(testCallsign, packet::groupAddress, text));
        }
        fsk::Audio clean = fsk::ax25(frames, scenario.baud, 48000);

        std::printf("%s\n", scenario.title.c_str());
        summary += "\n#### " + scenario.title + "\n\n" + scenario.about + "\n\n"
                   "| Result | Signal | Frames decoded | File |\n|---|---|---|---|\n";
        readme += "\n" + scenario.title + "\n  " + scenario.about + "\n";
        for (const ChannelRow& row : scenario.rows) {
            string path = (std::filesystem::path(dir) / row.file).string();
            string error;
            int got = 0;
            if (!fsk::writeWav(path, row.radio(clean))) {
                error = "couldn't write " + path;
            } else {
                std::set<string> decoded;
                std::istringstream lines(decodePacketsFile(settings, path, error));
                string line;
                while (std::getline(lines, line)) {
                    size_t colon = line.find(": ");
                    if (colon != string::npos) decoded.insert(line.substr(colon + 2));
                }
                for (const string& text : texts) got += static_cast<int>(decoded.count(text));
            }
            float share = static_cast<float>(got) / static_cast<float>(scenario.frames);
            bool checked = row.mustDecode >= 0;
            bool ok = error.empty() && (!checked || share >= row.mustDecode);
            if (!ok) failed++;
            const char* result = !ok ? "FAIL" : checked ? "PASS" : "    ";
            std::printf("  %s  %-24s %2d/%-2d %4s  %s\n", result, row.label.c_str(), got, scenario.frames,
                        percent(share).c_str(), row.file.c_str());
            if (!error.empty()) std::printf("        %s\n", error.c_str());
            string fraction = std::to_string(got) + "/" + std::to_string(scenario.frames) + " (" +
                              percent(share) + ")";
            summary += string("| ") + (!ok ? "❌ FAIL" : checked ? "✅ PASS" : "📊") + " | " +
                       cell(row.label) + " | " + fraction +
                       (checked ? ", needs " + percent(row.mustDecode) : "") + " | `" + row.file + "` |\n";
            readme += "  " + row.file + "  " + row.label + ": " + fraction + " decoded\n";
            if (!ok) {
                string why = error.empty() ? fraction + " decoded, needs " + percent(row.mustDecode) : error;
                annotate("error", "src/tests/channel.cpp", 0, "Channel: " + scenario.title + ", " + row.label, why);
            }
        }
    }
    if (std::FILE* f = std::fopen((std::filesystem::path(dir) / "README.txt").string().c_str(), "w")) {
        std::fputs(readme.c_str(), f);
        std::fclose(f);
    }
    appendSummary(summary + "\n");
    return failed == 0 ? 0 : 1;
}

// Command line

void usage(const char* program) {
    std::printf(
        "Usage: %s [options]\n"
        "With no options it runs every test group. A group that can't run here (no decoder,\n"
        "no sound cards) is skipped, not failed.\n"
        "\n"
        "Groups: unit, audio, channel, cat, air\n"
        "  --only GROUPS        run just these, e.g. --only unit,cat\n"
        "  --skip GROUPS        run all but these, e.g. --skip air\n"
        "\n"
        "audio\n"
        "  --audio-dir DIR      test audio to decode (default: test-audio next to the program,\n"
        "                       else the source's tests/audio)\n"
        "  --generate DIR       write the test audio files to DIR, then exit\n"
        "channel (AX.25 through simulated NFM and SSB radios; needs atest)\n"
        "  Every row's audio is saved in channel-audio next to the program\n"
        "cat\n"
        "  --cat-port PORT      test a real radio on PORT (e.g. COM5) instead of Hamlib's\n"
        "                       dummy rig. It is keyed for half a second\n"
        "  --cat-model N        Hamlib model (default: the one in settings.ini)\n"
        "  --cat-baud N         serial speed (default: the one in settings.ini)\n"
        "air (RadioConnector and a stand-in station over the sound cards, e.g. VB-Cable)\n"
        "  --rx-audio DEVICE    sound card to listen on (default: settings.ini's)\n"
        "  --tx-audio DEVICE    sound card to play into (default: settings.ini's)\n"
        "  --callsign CALL      callsign for the two stations, as CALL-14 and CALL-15\n"
        "                       (default: settings.ini's, else N0CALL)\n"
        "  --air-timeout SECS   longest wait for one step (default 60)\n",
        program);
}

std::set<string> parseGroups(const string& list, bool& ok) {
    std::set<string> groups;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        string group = list.substr(start, comma == string::npos ? string::npos : comma - start);
        if (std::find(allGroups.begin(), allGroups.end(), group) == allGroups.end()) ok = false;
        groups.insert(group);
        if (comma == string::npos) break;
        start = comma + 1;
    }
    return groups;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        bool hasValue = i + 1 < argc;
        string value = hasValue ? argv[i + 1] : "";
        auto number = [&](int& out) {
            out = std::atoi(value.c_str());
            i++;
            return out > 0;
        };
        bool ok = true;
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else if (!hasValue) {
            ok = false;
        } else if (arg == "--only") {
            options.groups = parseGroups(value, ok);
            i++;
        } else if (arg == "--skip") {
            for (const string& group : parseGroups(value, ok)) options.groups.erase(group);
            i++;
        } else if (arg == "--audio-dir") {
            options.audioDir = value;
            i++;
        } else if (arg == "--generate") {
            string error;
            if (!writeTestAudio(value, error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            std::printf("Wrote the test files to %s\n", value.c_str());
            return 0;
        } else if (arg == "--cat-port") {
            options.catPort = value;
            i++;
        } else if (arg == "--cat-model") {
            ok = number(options.catModel);
        } else if (arg == "--cat-baud") {
            ok = number(options.catBaud);
        } else if (arg == "--rx-audio") {
            options.rxAudio = value;
            i++;
        } else if (arg == "--tx-audio") {
            options.txAudio = value;
            i++;
        } else if (arg == "--callsign") {
            options.callsign = value;
            i++;
        } else if (arg == "--air-timeout") {
            ok = number(options.airSeconds);
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(stderr, "Didn't understand %s%s%s\n\n", arg.c_str(), hasValue ? " " : "", value.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    struct Group {
        const char* key;
        const char* title;
        std::function<int()> run;
    };
    const Group groups[] = {
        {"unit", "Unit tests", [] { return runUnit(); }},
        {"audio", "Audio files", [&] {
            string dir = options.audioDir.empty() ? findTestAudioDir() : options.audioDir;
            if (dir.empty()) {
                std::printf("SKIP  no test audio found; give --audio-dir\n");
                return skippedCode;
            }
            std::printf("From %s\n", dir.c_str());
            return runAudio(dir);
        }},
        {"channel", "Simulated radio channels", [] { return runChannel(); }},
        {"cat", "CAT radio control", [&] { return runCat(options); }},
        {"air", "Over the air", [&] { return runAir(options); }},
    };
    std::vector<std::pair<string, int>> results;
    for (const Group& group : groups) {
        if (!options.groups.count(group.key)) continue;
        std::printf("\n== %s ==\n", group.title);
        std::fflush(stdout);
        results.emplace_back(group.title, group.run());
    }

    std::printf("\n== Summary ==\n");
    bool anyFailed = false;
    for (const auto& [title, code] : results) {
        const char* result = code == 0 ? "PASS" : code == skippedCode ? "SKIP" : "FAIL";
        anyFailed = anyFailed || (code != 0 && code != skippedCode);
        std::printf("%s  %s\n", result, title.c_str());
    }
    return anyFailed ? 1 : 0;
}
