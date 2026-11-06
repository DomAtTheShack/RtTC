#include "connector.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace connector {

namespace {

using namespace std::chrono_literals;

// direwolf doesn't say when a frame has gone out, so estimate it (as session.cpp does).
// Key-up covers the channel wait and TXDELAY; tail covers TXTAIL and sound card buffering
constexpr auto keyUp = 1s;
constexpr auto tail = 1s;
constexpr auto ackDelay = 2s;          // After the last part we expect
constexpr auto collectTimeout = 2min;  // With no new part heard
constexpr auto remember = 10min;       // Finished message IDs
constexpr auto resultGrace = 30s;      // On top of the other side's hold time

// "RC1|D|FFF|40/40|": the longest data header
constexpr size_t dataHeaderBytes = 16;
constexpr size_t addressBytes = 7;
constexpr size_t frameHeaderBytes = 2 * addressBytes + 2;   // Addresses, control, PID

Clock::duration toDuration(double seconds) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

std::string utcNow() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
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

std::optional<int> parseNumber(std::string_view text, int low, int high) {
    if (text.empty() || text.size() > 4) return std::nullopt;
    int value = 0;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        value = value * 10 + (c - '0');
    }
    if (value < low || value > high) return std::nullopt;
    return value;
}

std::optional<unsigned> parseId(std::string_view text) {
    if (text.size() != 3) return std::nullopt;
    unsigned value = 0;
    for (char c : text) {
        int digit = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (digit < 0) return std::nullopt;
        value = value * 16 + static_cast<unsigned>(digit);
    }
    return value;
}

// Next field up to '|', removed from text with its '|'. nullopt if there's no '|'
std::optional<std::string_view> takeField(std::string_view& text) {
    size_t bar = text.find('|');
    if (bar == std::string_view::npos) return std::nullopt;
    std::string_view field = text.substr(0, bar);
    text.remove_prefix(bar + 1);
    return field;
}

std::vector<int> partNumbers(const std::vector<bool>& have) {
    std::vector<int> parts;
    for (size_t i = 0; i < have.size(); i++)
        if (have[i]) parts.push_back(static_cast<int>(i) + 1);
    return parts;
}

bool allTrue(const std::vector<bool>& flags) {
    return std::all_of(flags.begin(), flags.end(), [](bool b) { return b; });
}

} // namespace

// Air format

std::string hexId(unsigned id) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%03X", id & 0xFFF);
    return buf;
}

std::string formatData(unsigned id, int part, int total, std::string_view text) {
    return airTag + "|D|" + hexId(id) + "|" + std::to_string(part) + "/" + std::to_string(total) +
           "|" + std::string(text);
}

std::string formatAck(unsigned id, const std::vector<int>& parts) {
    return airTag + "|A|" + hexId(id) + "|" + formatRanges(parts);
}

std::string formatResult(unsigned id, const std::string& result) {
    return airTag + "|R|" + hexId(id) + "|" + result;
}

std::optional<AirFrame> parseAir(std::string_view info) {
    // Some stations add CR/LF at the end; never part of our fields except data text
    auto tag = takeField(info);
    if (!tag || *tag != airTag) return std::nullopt;
    auto kind = takeField(info);
    auto idText = takeField(info);
    if (!kind || !idText) return std::nullopt;
    auto id = parseId(*idText);
    if (!id) return std::nullopt;

    AirFrame frame;
    frame.id = *id;
    if (*kind == "D") {
        frame.kind = AirFrame::Kind::Data;
        auto spec = takeField(info);   // The rest is the text, which may contain '|'
        if (!spec) return std::nullopt;
        size_t slash = spec->find('/');
        if (slash == std::string_view::npos) return std::nullopt;
        auto total = parseNumber(spec->substr(slash + 1), 1, maxParts);
        if (!total) return std::nullopt;
        auto part = parseNumber(spec->substr(0, slash), 1, *total);
        if (!part) return std::nullopt;
        frame.part = *part;
        frame.total = *total;
        frame.text = std::string(info);
        return frame;
    }
    while (!info.empty() && (info.back() == '\r' || info.back() == '\n')) info.remove_suffix(1);
    if (*kind == "A") {
        frame.kind = AirFrame::Kind::Ack;
        auto parts = parseRanges(info);
        if (!parts || parts->empty()) return std::nullopt;
        frame.parts = *parts;
        return frame;
    }
    if (*kind == "R") {
        if (info != "OK" && info != "EXP" && info != "ERR") return std::nullopt;
        frame.kind = AirFrame::Kind::Result;
        frame.result = std::string(info);
        return frame;
    }
    return std::nullopt;
}

std::string formatRanges(const std::vector<int>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size();) {
        size_t j = i;
        while (j + 1 < parts.size() && parts[j + 1] == parts[j] + 1) j++;
        if (!out.empty()) out += ",";
        out += std::to_string(parts[i]);
        if (j > i) out += "-" + std::to_string(parts[j]);
        i = j + 1;
    }
    return out;
}

std::optional<std::vector<int>> parseRanges(std::string_view text) {
    std::vector<bool> seen(maxParts + 1, false);
    while (!text.empty()) {
        size_t comma = text.find(',');
        std::string_view item = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        size_t dash = item.find('-');
        auto first = parseNumber(item.substr(0, dash), 1, maxParts);
        auto last = dash == std::string_view::npos ? first
                                                   : parseNumber(item.substr(dash + 1), 1, maxParts);
        if (!first || !last || *last < *first) return std::nullopt;
        for (int n = *first; n <= *last; n++) seen[static_cast<size_t>(n)] = true;
    }
    std::vector<int> parts;
    for (int n = 1; n <= maxParts; n++)
        if (seen[static_cast<size_t>(n)]) parts.push_back(n);
    return parts;
}

size_t partBytes(int fx25, size_t pathHops) {
    size_t info = packet::maxInfoFor(fx25);
    // maxInfoFor assumes no path; with FX.25 each digipeater takes room in the block
    if (fx25 > 0) info -= std::min(info / 2, pathHops * addressBytes);
    return info - dataHeaderBytes;
}

std::vector<std::string> splitParts(std::string_view text, size_t maxBytes) {
    return packet::splitInfo(text, maxBytes);
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

// Envelope

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

// Engine

Engine::Engine(Hooks hooks, unsigned firstId) : hooks(std::move(hooks)), nextId(firstId & 0xFFF) {}

void Engine::setOptions(const Options& options) { opts = options; }

void Engine::setRadioReady(bool ready, TimePoint now) {
    if (ready == radioReady) return;
    radioReady = ready;
    if (!ready) {
        pausedSince = now;
        return;
    }
    // Waits for an ack start over from where they stopped
    auto paused = now - pausedSince;
    for (Outgoing& msg : outgoing) {
        if (final(msg.state) || msg.state == State::Queued) continue;
        msg.sendDone += paused;
        msg.deadline += paused;
    }
    pump(now);
}

void Engine::setMainConnected(bool connected, TimePoint) {
    mainConnected = connected;
    if (!connected) return;
    for (const Outgoing& msg : outgoing) emitStatus(msg);
    for (auto& [key, in] : incoming)
        if (in.stage == Incoming::Stage::Holding) deliver(in);
}

void Engine::send(const Json& body, const Json& requestId, TimePoint now) {
    auto fail = [&](const std::string& code, const std::string& message, const std::string& field = "") {
        hooks.toMain(errorEnvelope(requestId, code, message, field));
    };
    if (!body.is_object()) return fail("bad_field", "body must be an object", "body");
    if (!body.contains("to") || !body["to"].is_string())
        return fail("bad_field", "must be a callsign", "to");
    std::string to = packet::normalizeCallsign(body["to"].get<std::string>());
    if (to.empty()) return fail("bad_field", "must be a callsign, e.g. K1ABC-3", "to");
    if (to == opts.callsign) return fail("bad_field", "is this station's own callsign", "to");
    if (!body.contains("text") || !body["text"].is_string())
        return fail("bad_field", "must be a string", "text");
    const std::string& text = body["text"].get_ref<const std::string&>();
    if (text.empty()) return fail("bad_field", "must not be empty", "text");
    if (!validUtf8(text)) return fail("bad_field", "must be UTF-8", "text");

    size_t perPart = partBytes(opts.fx25, opts.path.size());
    std::vector<std::string> parts = splitParts(text, perPart);
    if (parts.size() > static_cast<size_t>(maxParts)) {
        return fail("too_long", "at most " + std::to_string(maxParts * perPart) +
                                " bytes with these settings; this is " + std::to_string(text.size()));
    }

    Outgoing msg;
    msg.id = allocateId();
    msg.from = opts.callsign;
    msg.to = to;
    msg.ref = body.contains("ref") ? body["ref"] : Json(nullptr);
    msg.parts = std::move(parts);
    msg.acked.assign(msg.parts.size(), false);
    outgoing.push_back(std::move(msg));
    const Outgoing& added = outgoing.back();
    hooks.toMain(envelope("message.accepted",
                          {{"msgId", added.from + "/" + hexId(added.id)},
                           {"parts", added.parts.size()}},
                          nullptr, requestId));
    emitStatus(added);
    pump(now);
}

void Engine::mainAck(const Json& re, TimePoint now) {
    if (!re.is_string()) return;
    for (auto& [key, in] : incoming) {
        if (in.stage == Incoming::Stage::Holding && in.envelopeId == re.get<std::string>()) {
            finishIncoming(in, "OK", now);
            return;
        }
    }
}

void Engine::onFrame(const packet::Frame& frame, TimePoint now) {
    if (frame.destination != opts.callsign) return;
    auto air = parseAir(frame.info);
    if (!air) return;
    heard = std::chrono::system_clock::now();

    if (air->kind == AirFrame::Kind::Data) {
        std::string key = frame.source + "/" + hexId(air->id);
        auto [it, added] = incoming.try_emplace(key);
        Incoming& in = it->second;
        if (added) {
            in.from = frame.source;
            in.id = air->id;
            in.parts.resize(static_cast<size_t>(air->total));
            in.have.assign(static_cast<size_t>(air->total), false);
            in.firstHeardUtc = utcNow();
        }
        in.lastHeard = now;
        if (in.stage != Incoming::Stage::Collecting) {
            in.ackAt = now + ackDelay;   // A repeat: ack (and result) again, but no new delivery
            return;
        }
        if (static_cast<size_t>(air->total) != in.parts.size()) {
            finishIncoming(in, "ERR", now);   // Parts disagree on the count
            return;
        }
        size_t index = static_cast<size_t>(air->part - 1);
        in.parts[index] = air->text;
        in.have[index] = true;
        in.path = frame.path;
        if (!allTrue(in.have)) {
            in.ackAt = now + ackWait(in, air->part, frame.path.size());
            return;
        }
        for (const std::string& part : in.parts) in.text += part;
        in.parts.clear();
        in.completedUtc = utcNow();
        if (!validUtf8(in.text)) {
            finishIncoming(in, "ERR", now);
            return;
        }
        in.stage = Incoming::Stage::Holding;
        in.holdUntil = now + std::chrono::seconds(opts.holdForMainSeconds);
        in.envelopeId = "c-" + std::to_string(nextEnvelope++);
        in.ackAt.reset();
        sendAck(in, now);
        if (mainConnected) deliver(in);
        return;
    }

    // Ack or result for one of ours
    Outgoing* msg = nullptr;
    for (Outgoing& candidate : outgoing) {
        if (candidate.id == air->id && candidate.to == frame.source && !final(candidate.state) &&
            candidate.state != State::Queued) {
            msg = &candidate;
            break;
        }
    }
    if (!msg) return;

    if (air->kind == AirFrame::Kind::Result) {
        msg->acked.assign(msg->acked.size(), true);
        if (air->result == "OK") setState(*msg, State::Delivered);
        else if (air->result == "EXP")
            setState(*msg, State::Expired, msg->to + "'s main program didn't take it in time");
        else setState(*msg, State::Failed, msg->to + " rejected the message as bad");
        prune();
        pump(now);
        return;
    }

    // Ack
    bool grew = false;
    for (int part : air->parts) {
        auto index = static_cast<size_t>(part - 1);
        if (index < msg->acked.size() && !msg->acked[index]) {
            msg->acked[index] = true;
            grew = true;
        }
    }
    msg->ackThisRound = true;
    if (msg->state == State::Received) return;   // A repeat
    if (allTrue(msg->acked)) {
        msg->deadline = now + std::chrono::seconds(opts.holdForMainSeconds) + resultGrace;
        setState(*msg, State::Received);
        pump(now);
        return;
    }
    if (msg->state == State::Waiting) {
        // Everything has gone out and the other side has said what's missing
        if (msg->attempt >= opts.tries) {
            setState(*msg, State::Failed, msg->to + " is still missing parts after " +
                                          std::to_string(msg->attempt) + " tries");
            prune();
            pump(now);
            return;
        }
        std::vector<int> missing;
        for (size_t i = 0; i < msg->acked.size(); i++)
            if (!msg->acked[i]) missing.push_back(static_cast<int>(i));
        msg->attempt++;
        msg->ackThisRound = false;
        setState(*msg, State::Retrying);
        transmitParts(*msg, missing, now);
        return;
    }
    if (grew) emitStatus(*msg);
}

void Engine::tick(TimePoint now) {
    if (radioReady) {
        for (Outgoing& msg : outgoing) {
            switch (msg.state) {
                case State::Sending:
                case State::Retrying:
                    if (now >= msg.sendDone) setState(msg, State::Waiting);
                    break;
                case State::Waiting: {
                    if (now < msg.deadline) break;
                    if (msg.attempt >= opts.tries) {
                        bool any = std::any_of(msg.acked.begin(), msg.acked.end(), [](bool b) { return b; });
                        setState(msg, State::Failed,
                                 (any ? msg.to + " is still missing parts" : "no ack from " + msg.to) +
                                 " after " + std::to_string(msg.attempt) + " tries");
                        break;
                    }
                    std::vector<int> missing;
                    for (size_t i = 0; i < msg.acked.size(); i++)
                        if (!msg.acked[i]) missing.push_back(static_cast<int>(i));
                    // With no ack at all, the last part makes the other side ack again
                    if (!msg.ackThisRound) missing = {missing.back()};
                    msg.attempt++;
                    msg.ackThisRound = false;
                    setState(msg, State::Retrying);
                    transmitParts(msg, missing, now);
                    break;
                }
                case State::Received:
                    if (now < msg.deadline) break;
                    if (msg.attempt >= opts.tries) {
                        setState(msg, State::Failed, msg.to + " has every part, but its result never arrived");
                        break;
                    }
                    // A repeat part makes the other side send its ack and result again
                    msg.attempt++;
                    transmitParts(msg, {static_cast<int>(msg.parts.size()) - 1}, now);
                    emitStatus(msg);
                    break;
                default:
                    break;
            }
        }
        prune();
        pump(now);
    }

    for (auto it = incoming.begin(); it != incoming.end();) {
        Incoming& in = it->second;
        if (in.ackAt && now >= *in.ackAt && radioReady) {
            in.ackAt.reset();
            sendAck(in, now);
        }
        bool forget = (in.stage == Incoming::Stage::Collecting && now - in.lastHeard >= collectTimeout) ||
                      (in.stage == Incoming::Stage::Done && now >= in.forgetAt);
        if (in.stage == Incoming::Stage::Holding && now >= in.holdUntil) finishIncoming(in, "EXP", now);
        it = forget ? incoming.erase(it) : std::next(it);
    }
}

int Engine::txQueue() const {
    return static_cast<int>(std::count_if(outgoing.begin(), outgoing.end(),
                                          [](const Outgoing& m) { return m.state == State::Queued; }));
}

int Engine::inFlight() const {
    return static_cast<int>(std::count_if(outgoing.begin(), outgoing.end(), [](const Outgoing& m) {
        return m.state != State::Queued && !final(m.state);
    }));
}

int Engine::heldForMain() const {
    return static_cast<int>(std::count_if(incoming.begin(), incoming.end(), [](const auto& entry) {
        return entry.second.stage == Incoming::Stage::Holding;
    }));
}

// One message on the air at a time: the next starts when this one is acked or has failed
void Engine::pump(TimePoint now) {
    if (!radioReady || active()) return;
    for (Outgoing& msg : outgoing) {
        if (msg.state != State::Queued) continue;
        msg.attempt = 1;
        msg.ackThisRound = false;
        setState(msg, State::Sending);
        std::vector<int> all;
        for (size_t i = 0; i < msg.parts.size(); i++) all.push_back(static_cast<int>(i));
        transmitParts(msg, all, now);
        return;
    }
}

void Engine::prune() {
    outgoing.erase(std::remove_if(outgoing.begin(), outgoing.end(),
                                  [](const Outgoing& m) { return final(m.state); }),
                   outgoing.end());
}

void Engine::transmitParts(Outgoing& msg, const std::vector<int>& indices, TimePoint now) {
    auto total = static_cast<int>(msg.parts.size());
    TimePoint done = now;
    for (int index : indices)
        done = transmitFrame(msg.to, formatData(msg.id, index + 1, total, msg.parts[static_cast<size_t>(index)]), now);
    msg.sendDone = done;
    msg.deadline = done + std::chrono::seconds(opts.extraWaitSeconds);
}

TimePoint Engine::transmitFrame(const std::string& destination, const std::string& info, TimePoint now) {
    if (hooks.transmit) hooks.transmit(destination, info);
    size_t bytes = frameHeaderBytes + opts.path.size() * addressBytes + info.size();
    // Back to back with what's already queued, or after a key-up
    TimePoint start = busyUntil > now ? busyUntil : now + keyUp;
    busyUntil = start + toDuration(packet::airtime(bytes, opts.baud, opts.fx25));
    return busyUntil + tail;
}

// Parts go out back to back in order, so the ones still missing after this part may
// be on the air right now. Wait for them, or at 300 baud we'd ack after every part
Clock::duration Engine::ackWait(const Incoming& in, int part, size_t pathHops) const {
    auto still = std::count(in.have.begin() + part, in.have.end(), false);
    size_t bytes = frameHeaderBytes + pathHops * addressBytes + dataHeaderBytes + partBytes(opts.fx25, pathHops);
    return ackDelay + still * toDuration(packet::airtime(bytes, opts.baud, opts.fx25));
}

void Engine::setState(Outgoing& msg, State state, const std::string& reason) {
    msg.state = state;
    msg.reason = reason;
    emitStatus(msg);
}

void Engine::emitStatus(const Outgoing& msg) {
    if (!mainConnected) return;
    hooks.toMain(envelope("message.status", {
        {"msgId", msg.from + "/" + hexId(msg.id)},
        {"ref", msg.ref},
        {"to", msg.to},
        {"state", stateName(msg.state)},
        {"parts", msg.parts.size()},
        {"partsAcked", partNumbers(msg.acked)},
        {"attempt", msg.attempt},
        {"reason", msg.reason.empty() ? Json(nullptr) : Json(msg.reason)},
    }));
}

void Engine::finishIncoming(Incoming& in, const std::string& result, TimePoint now) {
    in.stage = Incoming::Stage::Done;
    in.result = result;
    in.forgetAt = now + remember;
    in.ackAt.reset();
    if (radioReady) transmitFrame(in.from, formatResult(in.id, result), now);
}

void Engine::deliver(Incoming& in) {
    if (!mainConnected) return;
    hooks.toMain(envelope("message.received", {
        {"msgId", in.from + "/" + hexId(in.id)},
        {"from", in.from},
        {"to", opts.callsign},
        {"path", in.path},
        {"text", in.text},
        {"parts", in.have.size()},
        {"firstHeard", in.firstHeardUtc},
        {"completed", in.completedUtc},
    }, in.envelopeId));
}

void Engine::sendAck(const Incoming& in, TimePoint now) {
    if (in.stage == Incoming::Stage::Done && in.result == "ERR") {
        transmitFrame(in.from, formatResult(in.id, in.result), now);
        return;
    }
    transmitFrame(in.from, formatAck(in.id, partNumbers(in.have)), now);
    if (in.stage == Incoming::Stage::Done) transmitFrame(in.from, formatResult(in.id, in.result), now);
}

Engine::Outgoing* Engine::active() {
    for (Outgoing& msg : outgoing)
        if (msg.state == State::Sending || msg.state == State::Waiting || msg.state == State::Retrying)
            return &msg;
    return nullptr;
}

unsigned Engine::allocateId() {
    for (int tries = 0; tries <= 0xFFF; tries++) {
        unsigned id = nextId;
        nextId = (nextId + 1) & 0xFFF;
        bool used = std::any_of(outgoing.begin(), outgoing.end(), [id](const Outgoing& m) { return m.id == id; });
        if (!used) return id;
    }
    return nextId;
}

bool Engine::final(State state) {
    return state == State::Delivered || state == State::Expired || state == State::Failed;
}

const char* Engine::stateName(State state) {
    switch (state) {
        case State::Queued:    return "queued";
        case State::Sending:   return "sending";
        case State::Waiting:   return "waiting";
        case State::Retrying:  return "retrying";
        case State::Received:  return "received";
        case State::Delivered: return "delivered";
        case State::Expired:   return "expired";
        case State::Failed:    return "failed";
    }
    return "?";
}

} // namespace connector
