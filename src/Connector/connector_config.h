// The Radio Connector's config as JSON, kept in connector.ini.
#ifndef RTC_CONNECTOR_CONFIG_H
#define RTC_CONNECTOR_CONFIG_H

#include <string>

#include <nlohmann/json.hpp>

#include "connector.h"
#include "sessions.h"
#include "../settings.h"

namespace connector {

Json configToJson(const Settings& settings);

struct ConfigChange {
    bool ok = true;
    std::string field;     // On an error: e.g. "fx25" or "ptt.port"
    std::string message;
    bool restartRequired = false;   // direwolf needs restarting
    bool mainChanged = false;
};

// Applies the fields given. On an error settings is left as it was
ConfigChange applyConfig(Settings& settings, const Json& patch);

// What's missing before direwolf can start, e.g. "callsign". "" if nothing
std::string missingConfig(const Settings& settings);

// "127.0.0.1:9000" -> host and port. "localhost" becomes 127.0.0.1. false if invalid
bool parseHostPort(const std::string& text, std::string& host, int& port);

Options engineOptions(const Settings& settings);
SessionOptions sessionOptions(const Settings& settings);
// direwolf's PACLEN for connected mode: the largest I frame that fits one FX.25 block
int direwolfPaclen(const Settings& settings);

} // namespace connector

#endif //RTC_CONNECTOR_CONFIG_H
