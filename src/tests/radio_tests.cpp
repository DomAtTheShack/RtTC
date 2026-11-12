// RadioTests: every test, with no arguments needed (run by ctest too).
//   unit   The code on its own: settings, direwolf's config, AX.25, FX.25, the connector,
//          the Bluetooth protocol
//   audio  Decodes tests/audio with direwolf's atest (skipped without it)
//   channel AX.25 and FX.25 through simulated VHF/UHF FM radios with noise: how far each
//          mode gets. The audio is saved in channel-audio next to the program
//   cat    Radio control through Hamlib's dummy rig, or a real radio with --cat-port
//   air    RadioConnector and a stand-in station over the sound cards (e.g. VB-Cable):
//          receiving, a missing part, and sending with acks; then connected-mode sessions
//          with paging, two users at once, and a session the connector opens. Skipped
//          without sound cards
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
#include "../Connector/agw.h"
#include "../Connector/connector.h"
#include "../Connector/connector_config.h"
#include "../Radio/aioc.h"
#include "../Radio/cat.h"
#include "../Radio/fsk.h"
#include "../Radio/modem.h"
#include "../Connector/packet.h"
#include "../Connector/sessions.h"
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
    s.txAudioDesc = "C:\\Users\\me\\My Recordings: = test";   // Spaces, colons and = survive

    Settings back = parseSettings(formatSettings(s));
    CHECK(back.operation == Operation::Duplex);
    CHECK(back.packetBaud == 300);
    CHECK(back.rxAudio == s.rxAudio);
    CHECK(back.rxAudioDesc == s.rxAudioDesc);
    CHECK(back.txAudio == "default");
    CHECK(back.catPort == "/dev/ttyACM0");
    CHECK(back.catBaud == 115200);
    CHECK(back.txAudioDesc == s.txAudioDesc);

    // RadioConnectorCli's connected mode: who to connect to and the frequency to tune to
    s.operation = Operation::Connected;
    s.sendTo = "K1ABC-3";
    s.frequency = 145050000;
    back = parseSettings(formatSettings(s));
    CHECK(back.operation == Operation::Connected);
    CHECK(back.sendTo == "K1ABC-3");
    CHECK(back.frequency == 145050000);
    CHECK(parseSettings("frequency=145.05\n").frequency == 0);   // Hz, whole numbers only
    CHECK(parseSettings("frequency=-5\n").frequency == 0);
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
    CHECK(old.operation == Operation::Duplex && old.callsign == "N0CALL");   // Transmit is gone
    // Receive is gone too, and the audio file moved to the Test tools
    Settings older = parseSettings("operation=receive\naudio_file=C:\\x.wav\n");
    CHECK(older.operation == Operation::Duplex);
    CHECK(formatSettings(older).find("audio_file") == std::string::npos);
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

// An AIOC cable: its sound card picked from a list, its port and sound card put into the
// settings, and direwolf's PTT line
void testAioc() {
    using os::AudioDevice;
    // Linux: ALSA lists several names per card; plughw converts the rate
    std::vector<AudioDevice> alsa = {
        {"default", "Default ALSA Output"},
        {"hw:CARD=AllInOneCable,DEV=0", "All-In-One-Cable, USB Audio - Direct hardware device"},
        {"plughw:CARD=AllInOneCable,DEV=0", "All-In-One-Cable, USB Audio - Hardware device with all software conversions"},
        {"plughw:CARD=PCH,DEV=0", "HDA Intel PCH, ALC892 Analog"},
    };
    auto picked = aioc::pickCableAudio(alsa);
    CHECK(picked && picked->name == "plughw:CARD=AllInOneCable,DEV=0");
    // Windows: a WinMM number, named after the cable (cut to 31 characters)
    picked = aioc::pickCableAudio({{"0", "Microphone (Realtek Audio)"}, {"1", "Microphone (2- All-In-One-Cable"}});
    CHECK(picked && picked->name == "1");
    CHECK(!aioc::pickCableAudio({{"0", "Speakers (USB Audio CODEC)"}}));

    Settings s;
    s.ptt = PttType::Aioc;
    CHECK(pttProblem(s).empty());          // The port is found when it starts
    CHECK(s.keyingPort().empty());         // Never the CAT port
    s.catPort = "COM5";
    CHECK(s.keyingPort().empty());

    aioc::Cable cable;
    CHECK(!aioc::useCable(s, cable).empty());   // Nothing plugged in
    cable.port = "/dev/ttyACM0";
    CHECK(!aioc::useCable(s, cable).empty());   // No sound card
    CHECK(s.pttPort.empty());                   // Unchanged on an error
    cable.capture = AudioDevice{"plughw:CARD=AllInOneCable,DEV=0", "All-In-One-Cable"};
    cable.playback = cable.capture;
    CHECK(aioc::useCable(s, cable).empty());
    CHECK(s.pttPort == "/dev/ttyACM0" && s.keyingPort() == "/dev/ttyACM0");
    CHECK(s.rxAudio == "plughw:CARD=AllInOneCable,DEV=0" && s.txAudio == s.rxAudio);

    // Keyed while DTR is on and RTS off; RTS is held on the rest of the time
    CHECK(direwolfConfig(s, "1", "2", 7001).find("PTT /dev/ttyACM0 DTR -RTS\n") != std::string::npos);

    Settings back = parseSettings(formatSettings(s));
    CHECK(back.ptt == PttType::Aioc && back.pttPort == "/dev/ttyACM0");

    // Bluetooth audio keys the radio itself, so it can't also use the cable
    s.bluetooth = "38:D2:00:01:C0:AB";
    CHECK(!pttProblem(s).empty());
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
    CHECK(vox.find("MODEM 300 AB+\nFIX_BITS 1 AX25\n") != std::string::npos);
    CHECK(vox.find("MYCALL N0CALL-7\n") != std::string::npos);
    CHECK(vox.find("KISSPORT 0\nKISSPORT 7001\n") != std::string::npos);
    CHECK(vox.find("AGWPORT 0\n") != std::string::npos);
    CHECK(vox.find("PACLEN") == std::string::npos);
    std::string agw = direwolfConfig(s, "1", "2", 7001, 7002, 180);   // The connector, for connected mode
    CHECK(agw.find("AGWPORT 7002\nPACLEN 180\n") != std::string::npos);
    CHECK(agw.find("KISSPORT 7001\n") != std::string::npos);
    std::string agwOnly = direwolfConfig(s, "1", "2", 0, 7002, 180);   // RadioConnector: no KISS
    CHECK(agwOnly.find("KISSPORT 0\n") != std::string::npos && agwOnly.find("KISSPORT 7") == std::string::npos);
    CHECK(vox.find("PTT") == std::string::npos);           // VOX: direwolf mustn't key anything
    Settings vhf = s;
    vhf.packetBaud = 1200;
    CHECK(direwolfConfig(vhf, "1", "2", 7001).find("MODEM 1200 E+\nFIX_BITS 1 AX25\n") != std::string::npos);
    CHECK(contains(atestArgs(vhf, "in.wav"), {"-B", "1200", "-P", "E+", "-F", "1"}));
    vhf.packetBaud = 9600;
    CHECK(direwolfConfig(vhf, "1", "2", 7001).find("MODEM 9600 +\nFIX_BITS 1 AX25\n") != std::string::npos);

    s.catPort = "COM5";
    CHECK(direwolfConfig(s, "1", "2", 7001).find("PTT") == std::string::npos);   // A CAT port alone keys nothing
    s.ptt = PttType::Cat;
    s.catBaud = 38400;
    s.rigModel = 1037;   // FTDX-3000
    CHECK(direwolfConfig(s, "1", "2", 7001).find("PTT RIG 1037 COM5 38400\n") != std::string::npos);
    s.ptt = PttType::Rts;   // A radio's second USB serial port, next to CAT on its first
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
    CHECK(contains(atest, {"-B", "300", "-P", "AB+", "-F", "1", "-h", "in.wav"}));
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

    // The same seed gives the same audio
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

void testConnectorText() {
    using namespace connector;
    // Text per I frame: one FX.25 block, less 7 bytes for each digipeater
    CHECK(connectedChunkBytes(1, 0) == 179);
    CHECK(connectedChunkBytes(0, 0) == 256);
    CHECK(connectedChunkBytes(1, 2) == 165);
    CHECK(validUtf8("caf\xc3\xa9 \xf0\x9f\x93\xbb"));
    CHECK(!validUtf8("\xc3"));
    CHECK(!validUtf8("\xc0\xaf"));       // Overlong
    CHECK(!validUtf8("\xed\xa0\x80"));   // Surrogate
    CHECK(isoUtc(std::chrono::system_clock::time_point{}) == "1970-01-01T00:00:00Z");
}

void testConnectorConfig() {
    using namespace connector;
    Settings s;
    Json config = configToJson(s);
    CHECK(config["fx25"] == "auto");   // On by default
    CHECK(config["ptt"]["type"] == "vox");
    CHECK(!config.contains("retry") && !config.contains("holdForMainSeconds"));   // RC1's, gone
    CHECK(missingConfig(s) == "callsign");

    ConfigChange change = applyConfig(s, {{"callsign", "n0call-7"}, {"fx25", 32}, {"path", {"wide1-1"}}});
    CHECK(change.ok && change.restartRequired);
    CHECK(s.callsign == "N0CALL-7" && s.fx25 == 32 && s.path == std::vector<std::string>({"WIDE1-1"}));

    CHECK(applyConfig(s, {{"retry", {{"tries", 6}}}}).field == "retry");
    CHECK(applyConfig(s, {{"path", std::vector<std::string>(8, "WIDE1-1")}}).field == "path");   // AGW takes 7

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
    // AIOC: no port to give, and no sound cards needed up front: both are found at start
    CHECK(applyConfig(s, {{"ptt", {{"type", "aioc"}, {"port", "COM9"}}}}).field == "ptt.port");
    change = applyConfig(s, {{"ptt", {{"type", "aioc"}}}});
    CHECK(change.ok && change.restartRequired && s.ptt == PttType::Aioc);
    CHECK(configToJson(s)["ptt"]["type"] == "aioc");
    {
        Settings noAudio = s;
        noAudio.callsign = "N0CALL-7";
        noAudio.rxAudio.clear();
        noAudio.txAudio.clear();
        CHECK(connector::missingConfig(noAudio).empty());
    }
    CHECK(applyConfig(s, {{"ptt", {{"type", "vox"}}}}).ok && s.pttPort.empty());

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
    CHECK(back.path == s.path && back.mainAddress == "localhost:9000");
    CHECK(parseSettings("max_sessions=0\n").maxSessions == 4);   // Out of range keeps the default

    // Connected mode
    Settings c;
    Json connected = configToJson(c)["connected"];
    CHECK(connected["enabled"] == true);   // On by default
    CHECK(connected["maxSessions"] == 4 && connected["idleMinutes"] == 10 && connected["pageBytes"] == 1024);
    change = applyConfig(c, {{"connected", {{"enabled", false}, {"maxSessions", 2}, {"pageBytes", 0}}}});
    CHECK(change.ok && !change.restartRequired);
    CHECK(!c.connectedEnabled && c.maxSessions == 2 && c.pageBytes == 0 && c.idleMinutes == 10);
    CHECK(!parseSettings(formatSettings(c)).connectedEnabled);   // Off stays off in the file
    CHECK(parseSettings("").connectedEnabled);                   // Files from before connected= get it on
    change = applyConfig(c, {{"connected", {{"enabled", true}}}});
    CHECK(change.ok && c.connectedEnabled);
    CHECK(applyConfig(c, {{"connected", {{"maxSessions", 0}}}}).field == "connected.maxSessions");
    CHECK(applyConfig(c, {{"connected", {{"pageBytes", 100}}}}).field == "connected.pageBytes");
    CHECK(applyConfig(c, {{"connected", {{"enabled", "yes"}}}}).field == "connected.enabled");
    CHECK(applyConfig(c, {{"connected", {{"busyText", "two\nlines"}}}}).field == "connected.busyText");
    CHECK(applyConfig(c, {{"connected", {{"colour", 1}}}}).field == "connected.colour");
    CHECK(applyConfig(c, {{"connected", {{"busyText", "Full up | try later"}}}}).ok);
    Settings cBack = parseSettings(formatSettings(c));
    CHECK(cBack.connectedEnabled && cBack.maxSessions == 2 && cBack.pageBytes == 0);
    CHECK(cBack.busyText == "Full up | try later");
    CHECK(parseSettings("page_bytes=50\n").pageBytes == 1024);   // Too small keeps the default
    SessionOptions o = sessionOptions(c);
    CHECK(o.enabled && o.maxSessions == 2 && o.chunkBytes == connectedChunkBytes(c.fx25, 0));
    CHECK(static_cast<size_t>(direwolfPaclen(c)) >= o.chunkBytes);
}

// Connected mode

void testAgwFrames() {
    std::string bytes = agw::encode(agw::data("N0CALL-7", "K1ABC-3", "hi\r"));
    CHECK(bytes.size() == agw::headerBytes + 3);
    CHECK(bytes[4] == 'D' && static_cast<uint8_t>(bytes[6]) == agw::pidText);
    CHECK(bytes.substr(8, 9) == std::string("N0CALL-7\0", 9));
    CHECK(bytes[28] == 3 && bytes[29] == 0);   // Length, little endian

    agw::Decoder decoder;
    std::vector<agw::Frame> frames;
    for (char c : bytes + agw::encode(agw::registerCall("N0CALL-7")))   // One byte at a time
        for (agw::Frame& f : decoder.feed(std::string_view(&c, 1))) frames.push_back(f);
    CHECK(frames.size() == 2);
    CHECK(!frames.empty() && frames[0].kind == 'D' && frames[0].from == "N0CALL-7" &&
          frames[0].to == "K1ABC-3" && frames[0].data == "hi\r");
    CHECK(frames.size() > 1 && frames[1].kind == 'X' && frames[1].to.empty());

    agw::Frame via = agw::connect("N0CALL-7", "K1ABC-3", {"WIDE1-1", "WIDE2-1"});
    CHECK(via.kind == 'v' && via.data.size() == 21 && via.data[0] == 2);
    CHECK(via.data.substr(11, 7) == "WIDE2-1");
    CHECK(agw::connect("N0CALL-7", "K1ABC-3").kind == 'C');

    agw::Frame y;
    y.kind = 'Y';
    y.data = std::string("\x05\x00\x00\x00", 4);
    CHECK(agw::outstandingCount(y) == 5);
    y.data.clear();
    CHECK(agw::outstandingCount(y) == -1);

    std::string broken = agw::encode(agw::registerCall("X"));
    broken[30] = 0x7F;   // A length of 2 GB
    agw::Decoder bad;
    CHECK(bad.feed(broken).empty() && bad.broken());
}

void testPages() {
    using connector::splitPages;
    CHECK(splitPages("short", 1024).size() == 1);
    CHECK(splitPages(std::string(5000, 'x'), 0).size() == 1);   // 0 = no paging
    std::string text;
    for (int i = 0; text.size() < 2500; i++) text += "word" + std::to_string(i) + " ";
    auto pages = splitPages(text, 1024);
    CHECK(pages.size() == 3);
    std::string joined;
    for (size_t i = 0; i < pages.size(); i++) {
        CHECK(pages[i].size() <= 1024);
        if (i + 1 < pages.size()) CHECK(pages[i].back() == ' ');   // Ends at a word
        joined += pages[i];
    }
    CHECK(joined == text);
    std::string accents;
    while (accents.size() < 900) accents += "\xc3\xa9";   // No spaces: cut by bytes, never mid-character
    for (const std::string& page : splitPages(accents, 301)) CHECK(connector::validUtf8(page));
    CHECK(connector::toCrLines("a\nb\r\nc") == "a\rb\rc\r");
    CHECK(connector::toCrLines("done\r") == "done\r");
}

// One Sessions with a stand-in direwolf: the AGW frames it sends are kept, and the test
// answers for direwolf
struct Link {
    std::string call = "N0CALL-7";
    std::unique_ptr<connector::Sessions> sessions;
    std::vector<agw::Frame> sent;
    std::vector<connector::Json> toMain;
    connector::TimePoint now{};
    size_t yAnswered = 0;   // sent[] up to here has had its 'Y' answered

    explicit Link(connector::SessionOptions o = {}) {
        sessions = std::make_unique<connector::Sessions>(connector::SessionHooks{
            [this](const agw::Frame& f) {
                sent.push_back(f);
                return true;
            },
            [this](const connector::Json& m) { toMain.push_back(m); },
            {}});
        o.enabled = true;
        o.callsign = call;
        o.chunkBytes = 100;
        sessions->setOptions(o, now);
        sessions->setLinkUp(true, now);
        sessions->setMainConnected(true, now);
    }

    void in(char kind, const std::string& peer, const std::string& data = "") {
        agw::Frame f;
        f.kind = kind;
        f.from = peer;
        f.to = call;
        f.data = data;
        sessions->onAgw(f, now);
    }
    void connect(const std::string& peer) { in('C', peer, "*** CONNECTED To Station " + peer + "\r"); }
    void type(const std::string& peer, const std::string& line) { in('D', peer, line + "\r"); }

    // direwolf has sent and had acked everything given to it so far
    void settle() {
        for (int round = 0; round < 1000; round++) {
            bool answered = false;
            while (yAnswered < sent.size()) {
                const agw::Frame f = sent[yAnswered++];
                if (f.kind != 'Y') continue;
                agw::Frame reply;
                reply.kind = 'Y';
                reply.from = call;
                reply.to = f.to;
                reply.data = std::string(4, '\0');
                sessions->onAgw(reply, now);
                answered = true;
            }
            if (!answered) return;
        }
    }

    void run(double seconds) {
        auto end = now + std::chrono::duration_cast<connector::Clock::duration>(std::chrono::duration<double>(seconds));
        while (now < end) {
            now += std::chrono::seconds(1);
            sessions->tick(now);
        }
    }

    void sendText(const std::string& session, const std::string& text, const std::string& id = "m-1") {
        sessions->send({{"session", session}, {"text", text}}, id, now);
    }

    std::vector<connector::Json> ofType(const std::string& type) const {
        std::vector<connector::Json> out;
        for (const connector::Json& m : toMain)
            if (m["type"] == type) out.push_back(m);
        return out;
    }
    std::vector<std::string> states() const {
        std::vector<std::string> out;
        for (const connector::Json& m : ofType("message.status")) out.push_back(m["body"]["state"]);
        return out;
    }
    std::string textTo(const std::string& peer) const {
        std::string text;
        for (const agw::Frame& f : sent)
            if (f.kind == 'D' && f.to == peer) text += f.data;
        return text;
    }
    int framesTo(const std::string& peer, char kind, size_t from = 0) const {
        int n = 0;
        for (size_t i = from; i < sent.size(); i++)
            if (sent[i].kind == kind && sent[i].to == peer) n++;
        return n;
    }
};

std::string withoutPrompts(std::string text) {
    std::string prompt = connector::morePrompt;
    for (size_t at; (at = text.find(prompt)) != std::string::npos;) text.erase(at, prompt.size());
    return text;
}

void testSessionIncoming() {
    Link link;
    CHECK(!link.sent.empty() && link.sent[0].kind == 'X' && link.sent[0].from == "N0CALL-7");   // Registered
    link.connect("K1ABC-3");
    auto opened = link.ofType("session.opened");
    CHECK(opened.size() == 1);
    CHECK(!opened.empty() && opened[0]["body"]["session"] == "s-1" && opened[0]["body"]["direction"] == "in");
    CHECK(link.sessions->count() == 1);

    link.in('D', "K1ABC-3", "Hello th");   // A line can come in pieces
    link.in('D', "K1ABC-3", "ere\r\n");
    auto received = link.ofType("message.received");
    CHECK(received.size() == 1);
    CHECK(!received.empty() && received[0]["body"]["text"] == "Hello there");
    CHECK(!received.empty() && received[0]["body"]["session"] == "s-1" && received[0]["body"]["from"] == "K1ABC-3");
    CHECK(!received.empty() && !received[0].contains("id"));   // No ack needed

    std::string text;
    for (int i = 0; text.size() < 2500; i++) text += "Line " + std::to_string(i) + " of a long answer.\n";
    link.sendText("s-1", text);
    auto accepted = link.ofType("message.accepted");
    CHECK(accepted.size() == 1 && accepted[0]["body"]["pages"] == 3 && accepted[0]["body"]["msgId"] == "s-1/t1");
    // One window, then it waits for direwolf
    CHECK(link.framesTo("K1ABC-3", 'D') == 4);
    for (const agw::Frame& f : link.sent) CHECK(f.kind != 'D' || f.data.size() <= 100);
    link.run(3);
    CHECK(link.framesTo("K1ABC-3", 'D') == 4);
    link.settle();
    CHECK(link.states().back() == "paused");   // Page 1 is out; waiting for M
    std::string firstPage = link.textTo("K1ABC-3");
    CHECK(firstPage.find(connector::morePrompt) != std::string::npos);
    link.type("K1ABC-3", "m");
    link.settle();
    CHECK(link.states().back() == "paused");
    link.type("K1ABC-3", "MORE");
    link.settle();
    CHECK(link.states() == std::vector<std::string>({"queued", "sending", "paused", "sending", "paused",
                                                     "sending", "received"}));
    CHECK(withoutPrompts(link.textTo("K1ABC-3")) == connector::toCrLines(text));
    CHECK(link.ofType("message.status").back()["body"]["pagesSent"] == 3);
    CHECK(link.ofType("message.received").size() == 1);   // M isn't a prompt

    link.type("K1ABC-3", "M");   // Nothing left
    link.settle();
    CHECK(link.textTo("K1ABC-3").find("Nothing more") != std::string::npos);
}

void testSessionTurns() {
    Link link;
    link.connect("K1ABC-3");
    link.connect("W2XYZ-1");
    std::string text(3000, 'a');
    link.sendText("s-1", text, "m-1");
    link.sendText("s-2", text, "m-2");
    // Neither gets more than one window in direwolf ahead of the other
    CHECK(link.framesTo("K1ABC-3", 'D') == 4);
    CHECK(link.framesTo("W2XYZ-1", 'D') == 4);
    link.settle();
    CHECK(link.framesTo("K1ABC-3", 'D') > 4 && link.framesTo("W2XYZ-1", 'D') > 4);
}

void testSessionLimits() {
    connector::SessionOptions o;
    o.maxSessions = 2;
    o.busyText = "Full, sorry";
    Link link(o);
    link.connect("K1ABC-3");
    link.connect("W2XYZ-1");
    link.connect("KD9QQQ");
    CHECK(link.ofType("session.opened").size() == 2);   // The third is never seen
    CHECK(link.sessions->count() == 2);
    CHECK(link.textTo("KD9QQQ") == "Full, sorry\r");
    CHECK(link.framesTo("KD9QQQ", 'd') == 0);   // Not until it's acked: direwolf drops unsent text
    link.settle();
    CHECK(link.framesTo("KD9QQQ", 'd') == 1);
    link.type("KD9QQQ", "hello?");
    link.in('d', "KD9QQQ", "*** DISCONNECTED From Station KD9QQQ\r");
    CHECK(link.ofType("message.received").empty() && link.ofType("session.closed").empty());

    connector::Json open = {{"to", "N1NEW"}};
    link.sessions->open(open, "m-9", link.now);
    CHECK(link.toMain.back()["type"] == "error" && link.toMain.back()["body"]["code"] == "too_many_sessions");

    // The same station connecting again replaces its session
    link.connect("W2XYZ-1");
    auto closed = link.ofType("session.closed");
    CHECK(closed.size() == 1 && closed[0]["body"]["session"] == "s-2" && closed[0]["body"]["reason"] == "replaced");
    CHECK(link.ofType("session.opened").back()["body"]["session"] == "s-4");

    // Idle: told, then disconnected once that's acked
    link.run(9 * 60);
    CHECK(link.framesTo("K1ABC-3", 'd') == 0);
    link.run(61);
    CHECK(link.textTo("K1ABC-3").find("idle") != std::string::npos);
    link.settle();
    CHECK(link.framesTo("K1ABC-3", 'd') == 1);
    link.in('d', "K1ABC-3", "*** DISCONNECTED From Station K1ABC-3\r");
    CHECK(link.ofType("session.closed").back()["body"]["reason"] == "idle");
}

void testSessionCommands() {
    Link link;
    link.connect("K1ABC-3");
    link.sendText("s-1", std::string(2500, 'z'));
    link.settle();
    CHECK(link.states().back() == "paused");
    link.type("K1ABC-3", "What about tomorrow?");   // A new prompt instead of M
    CHECK(link.states().back() == "skipped");
    CHECK(link.ofType("message.received").back()["body"]["text"] == "What about tomorrow?");

    link.sendText("s-1", std::string(2500, 'y'), "m-2");
    link.settle();
    link.type("K1ABC-3", "n");
    CHECK(link.states().back() == "skipped");
    auto fresh = link.ofType("session.new");
    CHECK(fresh.size() == 1 && fresh[0]["body"]["session"] == "s-1");

    link.type("K1ABC-3", "bye");
    CHECK(link.framesTo("K1ABC-3", 'd') == 1);
    link.in('d', "K1ABC-3", "*** DISCONNECTED From Station K1ABC-3\r");
    CHECK(link.ofType("session.closed").back()["body"]["reason"] == "bye");
    CHECK(link.sessions->count() == 0);

    // With the main program away: a prompt gets a notice, and a new connect is turned away
    link.connect("W2XYZ-1");
    link.sessions->setMainConnected(false, link.now);
    link.type("W2XYZ-1", "anyone there?");
    CHECK(link.textTo("W2XYZ-1").find("offline") != std::string::npos);
    link.connect("KD9QQQ");
    CHECK(link.textTo("KD9QQQ").find("offline") != std::string::npos);
    link.sessions->setMainConnected(true, link.now);   // Open sessions are announced again
    CHECK(link.ofType("session.opened").back()["body"]["peer"] == "W2XYZ-1");

    // Remote hang-up and a lost link
    link.in('d', "W2XYZ-1", "*** DISCONNECTED RETRYOUT With W2XYZ-1\r");
    CHECK(link.ofType("session.closed").back()["body"]["reason"] == "link_lost");
}

void testSessionOutgoing() {
    connector::SessionOptions o;
    o.path = {"WIDE1-1"};
    Link link(o);
    link.sessions->open({{"to", "n0call-7"}}, "m-1", link.now);
    CHECK(link.toMain.back()["body"]["field"] == "to");   // Ourselves
    link.sessions->open({{"to", "k1abc-3"}}, "m-2", link.now);
    CHECK(link.toMain.back()["type"] == "session.opening" && link.toMain.back()["re"] == "m-2");
    CHECK(link.toMain.back()["body"]["session"] == "s-1" && link.toMain.back()["body"]["peer"] == "K1ABC-3");
    CHECK(!link.sent.empty() && link.sent.back().kind == 'v' && link.sent.back().to == "K1ABC-3");   // Through WIDE1-1
    link.sendText("s-1", "Queued until it's open");
    CHECK(link.framesTo("K1ABC-3", 'D') == 0);
    link.in('C', "K1ABC-3", "*** CONNECTED With Station K1ABC-3\r");
    auto opened = link.ofType("session.opened");
    CHECK(opened.size() == 1 && opened[0]["body"]["direction"] == "out");
    CHECK(link.textTo("K1ABC-3") == "Queued until it's open\r");

    link.sessions->close({{"session", "s-1"}, {"text", "73!"}}, "m-3", link.now);
    CHECK(link.toMain.back()["type"] == "session.closing");
    CHECK(link.framesTo("K1ABC-3", 'd') == 0);   // Not before the 73 is out and acked
    link.settle();
    CHECK(link.textTo("K1ABC-3") == "Queued until it's open\r73!\r");
    CHECK(link.framesTo("K1ABC-3", 'd') == 1);
    link.in('d', "K1ABC-3", "*** DISCONNECTED From Station K1ABC-3\r");
    CHECK(link.ofType("session.closed").back()["body"]["reason"] == "closed");

    // No answer: direwolf gives up and says so
    link.sessions->open({{"to", "W2XYZ-1"}, {"path", connector::Json::array()}}, "m-4", link.now);
    CHECK(link.sent.back().kind == 'C');   // An empty path: direct
    link.in('d', "W2XYZ-1", "*** DISCONNECTED RETRYOUT With W2XYZ-1\r");
    CHECK(link.ofType("session.closed").back()["body"]["reason"] == "no_answer");

    link.sessions->send({{"session", "s-9"}, {"text", "x"}}, "m-5", link.now);
    CHECK(link.toMain.back()["body"]["field"] == "session");

    // Turned off: no new sessions, and open ones close
    link.connect("KD9QQQ");
    connector::SessionOptions off = link.sessions->options();
    off.enabled = false;
    link.sessions->setOptions(off, link.now);
    CHECK(link.ofType("session.closed").back()["body"]["reason"] == "disabled");
    CHECK(link.sent.back().kind == 'd' || link.sent.back().kind == 'x');
    link.sessions->open({{"to", "KD9QQQ"}}, "m-6", link.now);
    CHECK(link.toMain.back()["body"]["code"] == "connected_off");
}

void testSessionRadioDown() {
    Link link;
    link.connect("K1ABC-3");
    link.sendText("s-1", std::string(500, 'q'));
    link.sessions->setLinkUp(false, link.now);
    CHECK(link.ofType("session.closed").back()["body"]["reason"] == "radio_down");
    CHECK(link.states().back() == "failed");
    CHECK(link.sessions->count() == 0);
    size_t before = link.sent.size();
    link.sessions->setLinkUp(true, link.now);
    CHECK(link.sent.size() == before + 1 && link.sent.back().kind == 'X');   // Registered again
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
        {"AIOC cable", testAioc},
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
        {"Connector text and frame sizes", testConnectorText},
        {"Connector config", testConnectorConfig},
        {"AGW frames", testAgwFrames},
        {"Connected mode pages", testPages},
        {"Connected session in, with paging", testSessionIncoming},
        {"Connected sessions take turns", testSessionTurns},
        {"Connected session limits, busy and idle", testSessionLimits},
        {"Connected session commands and offline", testSessionCommands},
        {"Connected session out, close and no answer", testSessionOutgoing},
        {"Connected sessions when the radio stops", testSessionRadioDown},
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

// What the test hears: JSON lines from the connector, and AGW frames from the stand-in
// station's connected-mode sessions
struct Heard {
    bool isAgw = false;
    string line;
    agw::Frame agwFrame;
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
        peerAgw.shutdown();
        if (peerAgwReader.joinable()) peerAgwReader.join();
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
        peerCall2 = base + "-13";   // A second connected-mode user on the stand-in station

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
        station.path.clear();

        // The stand-in station: its own direwolf, driven over AGW by this program
        Settings peerSettings = station;
        peerSettings.callsign = peerCall;
        int peerAgwPort = os::freeTcpPort();
        {
            std::ofstream out(workDir / "peer.conf");
            out << direwolfConfig(peerSettings, direwolfDevice(rx, false), direwolfDevice(tx, true), 0,
                                  peerAgwPort, connector::direwolfPaclen(peerSettings));
        }
        if (!peer.start(direwolfArgs("peer.conf"), workDir.string()))
            return failSetup("couldn't start direwolf: " + peer.error());
        auto deadline = Clock::now() + std::chrono::seconds(15);
        while (!peerAgw.connect("127.0.0.1", peerAgwPort)) {
            if (Clock::now() > deadline) return failSetup("direwolf didn't open its AGW port:\n" + peer.log());
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        peerAgwReader = std::thread([this] {
            agw::Decoder decoder;
            char buffer[2048];
            long n;
            while ((n = peerAgw.read(buffer, sizeof buffer)) > 0)
                for (agw::Frame& frame : decoder.feed(std::string_view(buffer, static_cast<size_t>(n))))
                    inbox.push({true, {}, std::move(frame)});
        });
        peerSend(agw::registerCall(peerCall));
        peerSend(agw::registerCall(peerCall2));

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

    // Connected mode on, then the stand-in station connects, types a line, and reads a
    // two-page reply, asking for the second page with M
    bool sessionPaging() {
        mark();
        sendToConnector({{"v", 1}, {"type", "config.set"}, {"id", "t-c1"},
                         {"body", {{"config", {{"connected", {{"enabled", true}, {"pageBytes", 300}}}}}}}});
        bool on = waitFor(seconds(), [&] {
            return findLine("config", [](const connector::Json& body) {
                return body["config"]["connected"].value("enabled", false);
            }) != nullptr;
        });
        if (!on) return fail("config.set didn't turn connected mode on");
        peerSend(agw::connect(peerCall, connectorCall));
        const connector::Json* opened = nullptr;
        bool ok = waitFor(seconds(), [&] {
            opened = findLine("session.opened", [&](const connector::Json& body) {
                return body.value("peer", "") == peerCall;
            });
            return opened != nullptr;
        });
        if (!ok) return fail("no session.opened after the stand-in station connected");
        firstSession = (*opened)["body"].value("session", "");
        // direwolf drops text for a link it's still opening, so wait as a person would
        if (!waitFor(seconds(), [&] { return peerConnected(peerCall); }))
            return fail("the stand-in station's direwolf never said it was connected");

        peerSend(agw::data(peerCall, connectorCall, "Hello repeater\r"));
        if (!waitFor(seconds(), [&] { return heardLine(firstSession, "Hello repeater"); }))
            return fail("the line typed never reached the main program as message.received");

        string text;
        for (int i = 1; text.size() < 500; i++) text += "Answer line " + std::to_string(i) + ", from the main program.\n";
        sendToConnector({{"v", 1}, {"type", "message.send"}, {"id", "t-c2"},
                         {"body", {{"session", firstSession}, {"text", text}}}});
        if (!waitFor(seconds() * 2, [&] { return sessionText(peerCall).find(connector::morePrompt) != string::npos; }))
            return fail("the first page never arrived with the [M]ore prompt (got " +
                        std::to_string(sessionText(peerCall).size()) + " bytes)");
        if (!waitFor(seconds(), [&] { return sessionStatus(firstSession, "paused"); }))
            return fail("the reply didn't pause after the first page");
        peerSend(agw::data(peerCall, connectorCall, "M\r"));
        if (!waitFor(seconds() * 2, [&] { return sessionStatus(firstSession, "received"); }))
            return fail("the reply never got to received after M");
        if (withoutPrompts(sessionText(peerCall)) != connector::toCrLines(text))
            return fail("the pages didn't join back into the reply sent");
        return true;
    }

    // A second user connects while the first is still on; both ask and both get their
    // own answer, then both say bye
    bool twoUsers() {
        mark();
        if (firstSession.empty()) return fail("needs the session from the step before");
        peerSend(agw::connect(peerCall2, connectorCall));
        const connector::Json* opened = nullptr;
        bool ok = waitFor(seconds(), [&] {
            opened = findLine("session.opened", [&](const connector::Json& body) {
                return body.value("peer", "") == peerCall2;
            });
            return opened != nullptr;
        });
        if (!ok) return fail("no session.opened for the second user");
        string second = (*opened)["body"].value("session", "");
        if (!waitFor(seconds(), [&] { return peerConnected(peerCall2); }))
            return fail("the stand-in station's direwolf never said the second user was connected");

        peerSend(agw::data(peerCall, connectorCall, "Question one\r"));
        peerSend(agw::data(peerCall2, connectorCall, "Question two\r"));
        if (!waitFor(seconds(), [&] { return heardLine(firstSession, "Question one") && heardLine(second, "Question two"); }))
            return fail("both questions didn't reach the main program");
        string one, two;
        for (int i = 1; one.size() < 250; i++) one += "For user one, line " + std::to_string(i) + ". ";
        for (int i = 1; two.size() < 250; i++) two += "For user two, line " + std::to_string(i) + ". ";
        sendToConnector({{"v", 1}, {"type", "message.send"}, {"id", "t-c3"},
                         {"body", {{"session", firstSession}, {"text", one}}}});
        sendToConnector({{"v", 1}, {"type", "message.send"}, {"id", "t-c4"},
                         {"body", {{"session", second}, {"text", two}}}});
        if (!waitFor(seconds() * 3, [&] { return sessionStatus(firstSession, "received") && sessionStatus(second, "received"); }))
            return fail("both answers didn't get to received (user one " + std::to_string(sessionText(peerCall).size()) +
                        " bytes, user two " + std::to_string(sessionText(peerCall2).size()) + " bytes)");
        if (sessionText(peerCall) != connector::toCrLines(one) || sessionText(peerCall2) != connector::toCrLines(two))
            return fail("an answer went to the wrong user, or came out changed");

        peerSend(agw::data(peerCall, connectorCall, "B\r"));
        peerSend(agw::data(peerCall2, connectorCall, "bye\r"));
        if (!waitFor(seconds() * 2, [&] { return sessionClosed(firstSession, "bye") && sessionClosed(second, "bye"); }))
            return fail("both sessions didn't close with reason bye");
        return true;
    }

    // The main program opens a session to the stand-in station, then closes it with a 73
    bool connectsOut() {
        mark();
        sendToConnector({{"v", 1}, {"type", "session.open"}, {"id", "t-c5"}, {"body", {{"to", peerCall}}}});
        const connector::Json* opened = nullptr;
        bool ok = waitFor(seconds(), [&] {
            opened = findLine("session.opened", [&](const connector::Json& body) {
                return body.value("peer", "") == peerCall && body.value("direction", "") == "out";
            });
            return opened != nullptr;
        });
        if (!ok) {
            if (auto* error = findLine("error", [](const connector::Json&) { return true; }))
                return fail("session.open failed: " + (*error)["body"].value("message", ""));
            return fail("no session.opened (direction out) after session.open");
        }
        string session = (*opened)["body"].value("session", "");
        if (!peerConnected(peerCall) && !waitFor(seconds(), [&] { return peerConnected(peerCall); }))
            return fail("the stand-in station never heard the connect");
        sendToConnector({{"v", 1}, {"type", "session.close"}, {"id", "t-c6"},
                         {"body", {{"session", session}, {"text", "73 from the repeater"}}}});
        if (!waitFor(seconds() * 2, [&] { return sessionClosed(session, "closed"); }))
            return fail("no session.closed after session.close");
        if (sessionText(peerCall) != "73 from the repeater\r")
            return fail("the stand-in station got \"" + sessionText(peerCall) + "\", not the 73");
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
        agwStart = agwFrames.size();
    }

    void peerSend(const agw::Frame& frame) { peerAgw.write(agw::encode(frame)); }

    // Session text the stand-in station got from the connector, for one of its callsigns
    string sessionText(const string& to) const {
        string text;
        for (size_t i = agwStart; i < agwFrames.size(); i++) {
            const agw::Frame& f = agwFrames[i];
            if (f.kind == 'D' && packet::normalizeCallsign(f.from) == connectorCall &&
                packet::normalizeCallsign(f.to) == to)
                text += f.data;
        }
        return text;
    }

    // The stand-in station's direwolf says a session with the connector is open
    bool peerConnected(const string& call) const {
        for (size_t i = agwStart; i < agwFrames.size(); i++) {
            const agw::Frame& f = agwFrames[i];
            if (f.kind == 'C' && packet::normalizeCallsign(f.from) == connectorCall &&
                packet::normalizeCallsign(f.to) == call)
                return true;
        }
        return false;
    }

    bool heardLine(const string& session, const string& text) {
        return findLine("message.received", [&](const connector::Json& body) {
            return body.value("session", "") == session && body.value("text", "") == text;
        }) != nullptr;
    }

    bool sessionStatus(const string& session, const string& state) {
        return findLine("message.status", [&](const connector::Json& body) {
            return body.value("session", "") == session && body.value("state", "") == state;
        }) != nullptr;
    }

    bool sessionClosed(const string& session, const string& reason) {
        return findLine("session.closed", [&](const connector::Json& body) {
            return body.value("session", "") == session && body.value("reason", "") == reason;
        }) != nullptr;
    }

    bool waitFor(Clock::duration limit, const std::function<bool()>& done) {
        auto deadline = Clock::now() + limit;
        while (!done()) {
            auto now = Clock::now();
            if (now >= deadline) return false;
            auto heard = inbox.pop(std::min<Clock::duration>(deadline - now, std::chrono::milliseconds(200)));
            if (!heard) continue;
            if (heard->isAgw) {
                agwFrames.push_back(std::move(heard->agwFrame));
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

    void sendToConnector(const connector::Json& message) { mainLink.write(message.dump() + "\n"); }

    const Options& options;
    string program, connectorCall, peerCall, peerCall2, problem;
    string firstSession;   // From sessionPaging, for twoUsers
    std::filesystem::path workDir;
    bool brokenSetup = false;
    Child peer, connector;
    os::TcpConnection mainLink, peerAgw;
    std::thread mainReader, peerAgwReader;
    Inbox inbox;
    std::vector<connector::Json> lines;
    std::vector<agw::Frame> agwFrames;
    size_t lineStart = 0, agwStart = 0;
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
        {"Connected: a session in, with a two-page reply", &AirTest::sessionPaging},
        {"Connected: two users at once, then both say bye", &AirTest::twoUsers},
        {"Connected: opens a session out and closes it with a 73", &AirTest::connectsOut},
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
    int fx25 = 0;                // >0: sent as FX.25 (made by gen_packets), as Settings::fx25
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

std::vector<ChannelRow> nfm1200Sweep(const string& prefix) {
    return noiseSweep(prefix, "CNR", {20, 15, 12, 10, 9, 8, 7, 6, 5},
                      [](float cnr) { return nfmRadio(true, 12500, cnr); }, 0.95f);
}

std::vector<ChannelRow> nfm9600Sweep(const string& prefix) {
    return noiseSweep(prefix, "CNR", {25, 20, 16, 13, 11, 10, 9, 8, 7},
                      [](float cnr) { return nfmRadio(false, 15000, cnr); }, 0.95f);
}

std::vector<ChannelScenario> channelScenarios() {
    std::vector<ChannelScenario> scenarios;
    scenarios.push_back({"AX.25 1200 on NFM, microphone in and speaker out",
        "The usual VHF/UHF setup: 3 kHz deviation, 12.5 kHz receiver IF, pre- and de-emphasis, "
        "300-3000 Hz audio. CNR is carrier to noise in the IF; FM's threshold is near 10 dB",
        1200, 20, nfm1200Sweep("nfm1200")});
    ChannelScenario fx1200{"FX.25 1200 on NFM, microphone in and speaker out",
        "As AX.25 1200, sent with FX.25 (Auto), as RadioConnector sends by default",
        1200, 20, nfm1200Sweep("fx25_nfm1200")};
    fx1200.fx25 = 1;
    scenarios.push_back(fx1200);
    scenarios.push_back({"AX.25 9600 on NFM, data ports",
        "Flat audio in and out (the radio's 9600 data jack): 3 kHz deviation, 15 kHz IF",
        9600, 20, nfm9600Sweep("nfm9600")});
    ChannelScenario fx9600{"FX.25 9600 on NFM, data ports",
        "As AX.25 9600, sent with FX.25 (Auto)", 9600, 20, nfm9600Sweep("fx25_nfm9600")};
    fx9600.fx25 = 1;
    scenarios.push_back(fx9600);
    scenarios.push_back({"AX.25 9600 on NFM, microphone and speaker",
        "Not expected to decode: emphasis and the 300-3000 Hz audio filters ruin 9600, which "
        "is why it needs the data ports",
        9600, 20,
        noiseSweep("nfm9600mic", "CNR", {20},
                   [](float cnr) { return nfmRadio(true, 15000, cnr); }, -1, -1)});
    return scenarios;
}

// channel-audio next to the program
string channelDir() {
    string exeDir = os::executableDir();
    return (std::filesystem::path(exeDir.empty() ? "." : exeDir) / "channel-audio").string();
}

// FX.25 frames: direwolf's gen_packets makes them, as RadioConnector's direwolf would send
bool genPacketsAudio(const Settings& settings, const std::vector<string>& texts, const string& path,
                     fsk::Audio& audio, string& error) {
    os::Process process;
    if (!process.start(genPacketsArgs(settings, path), true, true, "", true)) {
        error = "gen_packets: " + process.error();
        return false;
    }
    string lines;
    for (const string& text : texts)
        for (const string& line : packet::monitorLines(testCallsign, packet::groupAddress, text))
            lines += line + "\n";
    process.write(lines);
    process.closeStdin();
    char buffer[4096];
    while (process.read(buffer, sizeof buffer) > 0) {}   // It echoes every frame
    int status = process.wait();
    if (status != 0) {
        error = "gen_packets exited with " + std::to_string(status);
        return false;
    }
    bool ok = fsk::readWav(path, audio);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (!ok) error = "can't read gen_packets' " + path;
    return ok;
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
        fsk::Audio clean;
        if (scenario.fx25 > 0) {
            settings.fx25 = scenario.fx25;
            string error;
            string cleanPath = (std::filesystem::path(dir) / "fx25_clean.wav").string();
            if (!genPacketsAvailable() || !genPacketsAudio(settings, texts, cleanPath, clean, error)) {
                string why = error.empty() ? "gen_packets (from direwolf) not found" : error;
                std::printf("%s\n  SKIP  %s\n", scenario.title.c_str(), why.c_str());
                summary += "\n#### " + scenario.title + "\n\n⏭️ Skipped: " + why + "\n";
                continue;
            }
        } else {
            clean = fsk::ax25(frames, scenario.baud, 48000);
        }

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
        "channel (AX.25 and FX.25 through simulated VHF/UHF FM radios; needs atest)\n"
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
