// What the Radio Connector and the main program share (docs/connector-protocol.md): the
// JSON envelope, and the text and size rules for connected-mode sessions.
#ifndef RTC_CONNECTOR_H
#define RTC_CONNECTOR_H

#include <chrono>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace connector {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

constexpr int protocolVersion = 1;

// Text bytes per connected-mode I frame: the most that fits one FX.25 block with this
// FX.25 mode and digipeater path
size_t connectedChunkBytes(int fx25, size_t pathHops);
bool validUtf8(std::string_view text);
// As "2026-10-28T15:02:11Z"
std::string isoUtc(std::chrono::system_clock::time_point when);
std::string utcNow();

// {"v":1,"type":...,"id":...,"re":...,"body":...}. id and re may be null
Json envelope(const std::string& type, Json body, Json id = nullptr, Json re = nullptr);
Json errorEnvelope(const Json& re, const std::string& code, const std::string& message,
                   const std::string& field = "");

} // namespace connector

#endif //RTC_CONNECTOR_H
