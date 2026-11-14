// direwolf's AGW port: the frames the connector uses for connected mode
// (docs/connected-mode.md). Encoding and decoding only, no sockets.
#ifndef RTC_AGW_H
#define RTC_AGW_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agw {

constexpr size_t headerBytes = 36;
constexpr uint8_t pidText = 0xF0;   // Plain text in I frames
// direwolf limits data to this; anything bigger means we've lost our place in the stream
constexpr size_t maxDataBytes = 64 * 1024;

struct Frame {
    char kind = 0;          // 'X', 'C', 'v', 'D', 'd', 'Y', ...
    uint8_t port = 0;       // direwolf channel
    uint8_t pid = 0;
    std::string from, to;   // Callsigns, at most 9 characters each
    std::string data;
};

std::string encode(const Frame& frame);

// Splits the byte stream from direwolf into frames
class Decoder {
public:
    std::vector<Frame> feed(std::string_view bytes);
    bool broken() const { return lost; }   // A length that made no sense

private:
    std::string buffer;
    bool lost = false;
};

Frame registerCall(const std::string& call);
Frame unregisterCall(const std::string& call);
// 'C', or 'v' with digipeaters (at most 7)
Frame connect(const std::string& own, const std::string& peer, const std::vector<std::string>& via = {});
Frame data(const std::string& own, const std::string& peer, std::string_view text);
Frame disconnect(const std::string& own, const std::string& peer);
Frame outstanding(const std::string& own, const std::string& peer);
// 'k': direwolf then sends every frame it hears as 'K', a port byte then the frame (no FCS)
Frame rawFrames();

// The count in a 'Y' reply, or -1
long outstandingCount(const Frame& frame);

} // namespace agw

#endif //RTC_AGW_H
