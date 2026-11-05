// The tests/audio test cases, used by ctest and the Test tools menu.
#ifndef RTC_TESTCASES_H
#define RTC_TESTCASES_H

#include <string>
#include <vector>

#include "Radio/fsk.h"
#include "Connector/packet.h"
#include "settings.h"

inline const std::string testCallsign = "N0CALL-1";

struct AudioCase {
    std::string file;
    std::string description;
    Settings settings;
    int rate;
    std::string text;          // Empty = noise only
    bool noisy;
    float snrDb;
    float minAccuracy;         // 0-1
};

struct ToneFile {
    std::string file;
    std::string description;
    float hz;
    float seconds;
};

const std::vector<AudioCase>& standardCases();
std::string testMessage(const Settings& settings);
// One UI frame per line
std::vector<packet::Bytes> packetFrames(const std::string& text, const std::string& callsign);
const std::vector<ToneFile>& toneFiles();

fsk::Audio generate(const AudioCase& testCase);

// Collapses whitespace and trims
std::string normalizeText(const std::string& text);
// 0-1, from edit distance
float textAccuracy(const std::string& expected, const std::string& decoded);

struct CaseResult {
    bool passed = false;
    float accuracy = 0;
    std::string decoded;       // Normalized
    std::string error;
    std::vector<std::string> packets;   // "SENDER: text"
};

CaseResult runCase(const AudioCase& testCase, const std::string& dir);
// Each packet must come from settings.callsign when set
CaseResult decodeFile(const Settings& settings, const std::string& path,
                      const std::string& expected, float minAccuracy);
// One "SENDER: text" line per packet
std::string decodePacketsFile(const Settings& settings, const std::string& path, std::string& error);

bool direwolfAvailable();
bool atestAvailable();
bool genPacketsAvailable();

// Writes every case, tone file and a README
bool writeTestAudio(const std::string& dir, std::string& error);

// test-audio next to the program, else tests/audio. "" if neither
std::string findTestAudioDir();

#endif //RTC_TESTCASES_H
