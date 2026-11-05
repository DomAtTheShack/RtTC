// AX.25 UI frames, KISS framing, and atest/gen_packets formats.
#ifndef RTC_PACKET_H
#define RTC_PACKET_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace packet {

using Bytes = std::vector<uint8_t>;

inline const std::string groupAddress = "CQ";
constexpr size_t maxInfo = 256;   // AX.25 default; longer text is split

// FX.25 (Reed-Solomon error correction). 0 = off, 1 = auto, 16/32/64 = check bytes.
// One FX.25 block holds at most 239 bytes, so FX.25 needs shorter packets: a bigger
// frame goes out as plain AX.25. Assumes no digipeater path
size_t maxInfoFor(int fx25);

// "n0call-7" -> "N0CALL-7", "N0CALL-0" -> "N0CALL". Empty if invalid
std::string normalizeCallsign(const std::string& text);

struct Frame {
    std::string source;
    std::string destination;
    std::vector<std::string> path;   // Digipeaters; "*" marks one that has repeated it
    std::string info;
};

// Without the FCS. Callsigns must be normalized. path: digipeaters, at most 8
Bytes encodeUi(const std::string& source, const std::string& destination, std::string_view info,
               const std::vector<std::string>& path = {});
// UI and I frames only
std::optional<Frame> decode(const Bytes& frame);

// Never splits a UTF-8 character
std::vector<std::string> splitInfo(std::string_view text, size_t maxBytes = maxInfo);

// One line; control bytes shown as <0x0d>
std::string printable(std::string_view info);

// KISS

std::string kissEncode(const Bytes& frame);   // Port 0 data frame

class KissDecoder {
public:
    std::vector<Bytes> feed(std::string_view bytes);

private:
    Bytes current;
    bool escaped = false;
    bool inFrame = false;
};

// direwolf's file tools

// Frames from "atest -h" hex dumps
std::vector<Bytes> parseAtestDump(std::string_view output);

// gen_packets input lines: "N0CALL>CQ:text"
std::vector<std::string> monitorLines(const std::string& source, const std::string& destination,
                                      std::string_view text, size_t maxInfoBytes = maxInfo);

// Seconds on air, not counting key-up. fx25 as for maxInfoFor
double airtime(size_t frameBytes, int baud, int fx25 = 0);

} // namespace packet

#endif //RTC_PACKET_H
