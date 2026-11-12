// Direwolf's command lines and config.
#ifndef RTC_MODEM_H
#define RTC_MODEM_H

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

// "1200 baud AX.25 (AFSK, VHF/FM) FX.25"
std::string describeModem(const Settings& settings);
// One line, e.g. "Duplex | 1200 baud AX.25 (AFSK, VHF/FM) FX.25 | N0CALL-7"
std::string describe(const Settings& settings);

#endif //RTC_MODEM_H
