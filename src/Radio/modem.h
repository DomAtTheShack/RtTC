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

// AGW and the default KISS port are off so another direwolf doesn't clash. direwolf keys
// the radio itself, the way settings.ptt says
std::string direwolfConfig(const Settings& settings, const std::string& rxDevice,
                           const std::string& txDevice, int kissPort);
std::vector<std::string> direwolfArgs(const std::string& configPath);
std::vector<std::string> atestArgs(const Settings& settings, const std::string& wavPath);
std::vector<std::string> genPacketsArgs(const Settings& settings, const std::string& wavPath);

// One-line summary, e.g. "Receive | 1200 baud AX.25 (AFSK, VHF/FM) | N0CALL-7"
std::string describe(const Settings& settings);

#endif //RTC_MODEM_H
