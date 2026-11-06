#include "agw.h"

#include <algorithm>

namespace agw {

namespace {

constexpr size_t callBytes = 10;

void putCall(std::string& out, const std::string& call) {
    std::string field = call.substr(0, callBytes - 1);
    field.resize(callBytes, '\0');
    out += field;
}

std::string getCall(std::string_view header, size_t at) {
    std::string_view field = header.substr(at, callBytes);
    size_t end = field.find('\0');
    return std::string(field.substr(0, end));
}

void putLe32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; i++) out += static_cast<char>((value >> (8 * i)) & 0xFF);
}

uint32_t getLe32(std::string_view bytes, size_t at) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) value |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + i])) << (8 * i);
    return value;
}

Frame make(char kind, const std::string& from, const std::string& to) {
    Frame frame;
    frame.kind = kind;
    frame.from = from;
    frame.to = to;
    return frame;
}

} // namespace

std::string encode(const Frame& frame) {
    std::string out;
    out += static_cast<char>(frame.port);
    out.append(3, '\0');
    out += frame.kind;
    out += '\0';
    out += static_cast<char>(frame.pid);
    out += '\0';
    putCall(out, frame.from);
    putCall(out, frame.to);
    putLe32(out, static_cast<uint32_t>(frame.data.size()));
    putLe32(out, 0);
    return out + frame.data;
}

std::vector<Frame> Decoder::feed(std::string_view bytes) {
    std::vector<Frame> frames;
    if (lost) return frames;
    buffer.append(bytes);
    size_t at = 0;
    while (buffer.size() - at >= headerBytes) {
        std::string_view header(buffer.data() + at, headerBytes);
        uint32_t length = getLe32(header, 28);
        if (length > maxDataBytes) {
            lost = true;
            buffer.clear();
            return frames;
        }
        if (buffer.size() - at < headerBytes + length) break;
        Frame frame;
        frame.port = static_cast<uint8_t>(header[0]);
        frame.kind = header[4];
        frame.pid = static_cast<uint8_t>(header[6]);
        frame.from = getCall(header, 8);
        frame.to = getCall(header, 18);
        frame.data.assign(buffer, at + headerBytes, length);
        frames.push_back(std::move(frame));
        at += headerBytes + length;
    }
    buffer.erase(0, at);
    return frames;
}

Frame registerCall(const std::string& call) { return make('X', call, ""); }

Frame unregisterCall(const std::string& call) { return make('x', call, ""); }

Frame connect(const std::string& own, const std::string& peer, const std::vector<std::string>& via) {
    if (via.empty()) return make('C', own, peer);
    Frame frame = make('v', own, peer);
    size_t hops = std::min<size_t>(via.size(), 7);
    frame.data += static_cast<char>(hops);
    for (size_t i = 0; i < hops; i++) putCall(frame.data, via[i]);
    return frame;
}

Frame data(const std::string& own, const std::string& peer, std::string_view text) {
    Frame frame = make('D', own, peer);
    frame.pid = pidText;
    frame.data = std::string(text);
    return frame;
}

Frame disconnect(const std::string& own, const std::string& peer) { return make('d', own, peer); }

Frame outstanding(const std::string& own, const std::string& peer) { return make('Y', own, peer); }

long outstandingCount(const Frame& frame) {
    if (frame.kind != 'Y' || frame.data.size() < 4) return -1;
    return static_cast<long>(getLe32(frame.data, 0));
}

} // namespace agw
