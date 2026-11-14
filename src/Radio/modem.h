// Direwolf's command lines and config.
#ifndef RTC_MODEM_H
#define RTC_MODEM_H

#include <optional>
#include <string>
#include <vector>

#include "../settings.h"

const char* packetModemName(int baud);

// Sound card name for ADEVICE. On macOS, the CoreAudio name of the AIOC cable's device if
// aiocCable, else of the system default input (or output, if playback) device
std::string direwolfDevice(const std::string& name, bool playback, bool aiocCable = false);

// The default AGW and KISS ports are off so another direwolf doesn't clash. direwolf keys
// the radio itself, the way settings.ptt says. kissPort > 0 opens KISS there (raw UI
// packets). agwPort > 0 opens AGW there, for connected mode, with paclen as the largest
// I frame (0 = direwolf's default)
std::string direwolfConfig(const Settings& settings, const std::string& rxDevice,
                           const std::string& txDevice, int kissPort, int agwPort = 0, int paclen = 0);
std::vector<std::string> direwolfArgs(const std::string& configPath);
std::vector<std::string> atestArgs(const Settings& settings, const std::string& wavPath);
std::vector<std::string> genPacketsArgs(const Settings& settings, const std::string& wavPath);

// direwolf's "heard" line for each frame it decodes, e.g.
// "N8GM-1 audio level = 48(14/12)   [NONE]   |||||||__". level is 0-100ish (about 50 is
// best); mark and space are the two AFSK tones' levels, -1 when the line doesn't give them
struct AudioLevel {
    std::string station;   // Who was heard: the sender, or the digipeater that repeated it
    int level = 0;
    int mark = -1;
    int space = -1;
};
std::optional<AudioLevel> parseAudioLevel(const std::string& line);

// How a level decodes: Good (30-70), Fair (20-29, 71-90) or Bad (outside that)
enum class LevelRating { Good, Fair, Bad };
LevelRating rateLevel(int level);

// "1200 baud AX.25 (AFSK, VHF/FM) FX.25"
std::string describeModem(const Settings& settings);
// One line, e.g. "Duplex | 1200 baud AX.25 (AFSK, VHF/FM) FX.25 | N0CALL-7"
std::string describe(const Settings& settings);

#endif //RTC_MODEM_H
