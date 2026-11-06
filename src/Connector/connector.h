// The Radio Connector's message protocol (docs/connector-protocol.md).
// No I/O here: the program passes in frames heard, requests from the main program and
// the time, and the engine answers through two callbacks. That keeps it testable.
#ifndef RTC_CONNECTOR_H
#define RTC_CONNECTOR_H

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "packet.h"

namespace connector {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

constexpr int protocolVersion = 1;
constexpr int maxParts = 40;
inline const std::string airTag = "RC1";

// Air format

struct AirFrame {
    enum class Kind { Data, Ack, Result } kind = Kind::Data;
    unsigned id = 0;               // 0..0xFFF
    int part = 0, total = 0;       // Data: 1-based part of total
    std::string text;              // Data
    std::vector<int> parts;        // Ack: parts received, ascending
    std::string result;            // Result: OK, EXP or ERR
};

std::string hexId(unsigned id);   // 0x3F -> "03F"
std::string formatData(unsigned id, int part, int total, std::string_view text);
std::string formatAck(unsigned id, const std::vector<int>& parts);
std::string formatResult(unsigned id, const std::string& result);
// nullopt if it isn't one of our frames, or is malformed
std::optional<AirFrame> parseAir(std::string_view info);

// {1,2,3,5} -> "1-3,5"
std::string formatRanges(const std::vector<int>& parts);
std::optional<std::vector<int>> parseRanges(std::string_view text);

// Text bytes per part: the packet limit for this FX.25 mode and path, minus our header
size_t partBytes(int fx25, size_t pathHops);
// Text bytes per connected-mode I frame: the same limit, with no RC1 header
size_t connectedChunkBytes(int fx25, size_t pathHops);
// Fills each part to maxBytes, never cutting a UTF-8 character
std::vector<std::string> splitParts(std::string_view text, size_t maxBytes);
bool validUtf8(std::string_view text);
// Now, as "2026-10-28T15:02:11Z"
std::string utcNow();

// Envelope

// {"v":1,"type":...,"id":...,"re":...,"body":...}. id and re may be null
Json envelope(const std::string& type, Json body, Json id = nullptr, Json re = nullptr);
Json errorEnvelope(const Json& re, const std::string& code, const std::string& message,
                   const std::string& field = "");

// Engine

struct Options {
    std::string callsign;            // Normalized
    std::vector<std::string> path;   // Digipeaters
    int fx25 = 1;
    int baud = 1200;                 // For airtime estimates
    int tries = 4;
    int extraWaitSeconds = 10;
    int holdForMainSeconds = 180;
};

struct Hooks {
    // Sends one frame. destination is the next station (today always the addressee)
    std::function<bool(const std::string& destination, const std::string& info)> transmit;
    // A message for the main program. Dropped by the caller if it isn't connected
    std::function<void(const Json& envelope)> toMain;
};

class Engine {
public:
    Engine(Hooks hooks, unsigned firstId);

    void setOptions(const Options& options);
    const Options& options() const { return opts; }

    // Timers for sending stop while the radio is down, so retries aren't used up
    void setRadioReady(bool ready, TimePoint now);
    // On connect, held messages and the state of every message are sent again
    void setMainConnected(bool connected, TimePoint now);

    // message.send. Replies (message.accepted or error) through toMain, before any status
    void send(const Json& body, const Json& requestId, TimePoint now);
    // ack from the main program; re is the id of a message.received
    void mainAck(const Json& re, TimePoint now);
    void onFrame(const packet::Frame& frame, TimePoint now);
    void tick(TimePoint now);

    // For radio.status
    int txQueue() const;
    int inFlight() const;
    int heldForMain() const;
    std::optional<std::chrono::system_clock::time_point> lastHeard() const { return heard; }

private:
    enum class State { Queued, Sending, Waiting, Retrying, Received, Delivered, Expired, Failed };

    struct Outgoing {
        unsigned id = 0;
        std::string from;               // Our callsign when it was sent
        std::string to;                 // Final destination (today also the next station)
        Json ref;
        std::vector<std::string> parts;
        std::vector<bool> acked;
        State state = State::Queued;
        int attempt = 0;
        bool ackThisRound = false;
        TimePoint sendDone{};           // Estimated end of the latest transmission
        TimePoint deadline{};
        std::string reason;
    };

    struct Incoming {
        enum class Stage { Collecting, Holding, Done } stage = Stage::Collecting;
        std::string from;
        unsigned id = 0;
        std::vector<std::string> parts;
        std::vector<bool> have;
        std::vector<std::string> path;
        std::string text;
        std::string firstHeardUtc, completedUtc;
        TimePoint lastHeard{};
        std::optional<TimePoint> ackAt;
        TimePoint holdUntil{}, forgetAt{};
        std::string result;             // Done: OK, EXP or ERR
        std::string envelopeId;         // Holding: id of our message.received
    };

    void pump(TimePoint now);
    void prune();
    void transmitParts(Outgoing& msg, const std::vector<int>& indices, TimePoint now);
    TimePoint transmitFrame(const std::string& destination, const std::string& info, TimePoint now);
    void setState(Outgoing& msg, State state, const std::string& reason = "");
    void emitStatus(const Outgoing& msg);
    void finishIncoming(Incoming& in, const std::string& result, TimePoint now);
    void deliver(Incoming& in);
    void sendAck(const Incoming& in, TimePoint now);
    Clock::duration ackWait(const Incoming& in, int part, size_t pathHops) const;
    Outgoing* active();
    unsigned allocateId();
    static bool final(State state);
    static const char* stateName(State state);

    Hooks hooks;
    Options opts;
    unsigned nextId;
    long long nextEnvelope = 1;
    bool radioReady = false;
    TimePoint pausedSince{};
    bool mainConnected = false;
    TimePoint busyUntil{};
    std::vector<Outgoing> outgoing;              // In arrival order
    std::map<std::string, Incoming> incoming;    // "FROM/03F"
    std::optional<std::chrono::system_clock::time_point> heard;
};

} // namespace connector

#endif //RTC_CONNECTOR_H
