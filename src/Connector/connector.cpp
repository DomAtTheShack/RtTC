#include "connector.h"

#include <algorithm>
#include <cstdint>
#include <ctime>

#include "packet.h"

namespace connector {

size_t connectedChunkBytes(int fx25, size_t pathHops) {
    constexpr size_t addressBytes = 7;
    size_t info = packet::maxInfoFor(fx25);
    // maxInfoFor assumes no path; with FX.25 each digipeater takes room in the block
    if (fx25 > 0) info -= std::min(info / 2, pathHops * addressBytes);
    return info;
}

bool validUtf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        auto byte = static_cast<uint8_t>(text[i]);
        size_t length = byte < 0x80 ? 1 : (byte >> 5) == 0x6 ? 2 : (byte >> 4) == 0xE ? 3
                      : (byte >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > text.size()) return false;
        uint32_t code = length == 1 ? byte : byte & (0x7F >> length);
        for (size_t k = 1; k < length; k++) {
            auto next = static_cast<uint8_t>(text[i + k]);
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        // Overlong forms, surrogates and values past Unicode
        static const uint32_t smallest[] = {0, 0, 0x80, 0x800, 0x10000};
        if (code < smallest[length] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return false;
        i += length;
    }
    return true;
}

std::string isoUtc(std::chrono::system_clock::time_point when) {
    std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string utcNow() { return isoUtc(std::chrono::system_clock::now()); }

Json envelope(const std::string& type, Json body, Json id, Json re) {
    Json out = {{"v", protocolVersion}, {"type", type}};
    if (!id.is_null()) out["id"] = std::move(id);
    out["re"] = std::move(re);
    out["body"] = std::move(body);
    return out;
}

Json errorEnvelope(const Json& re, const std::string& code, const std::string& message,
                   const std::string& field) {
    Json body = {{"code", code}, {"message", message}};
    if (!field.empty()) body["field"] = field;
    return envelope("error", std::move(body), nullptr, re);
}

} // namespace connector
