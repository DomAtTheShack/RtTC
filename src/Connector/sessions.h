// Connected-mode sessions (docs/connected-mode.md): AX.25 connections through direwolf's
// AGW port, with paging and turn-taking between users. No I/O here, like Engine: the
// program passes in AGW frames, requests from the main program and the time, and gets
// AGW frames and JSON back through the hooks.
#ifndef RTC_SESSIONS_H
#define RTC_SESSIONS_H

#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "agw.h"
#include "connector.h"

namespace connector {

struct SessionOptions {
    bool enabled = false;
    std::string callsign;            // Normalized
    std::vector<std::string> path;   // Digipeaters for session.open
    int maxSessions = 4;
    int idleMinutes = 10;            // 0 = never
    size_t pageBytes = 1024;         // 0 = no paging
    std::string busyText = "Busy, try again in a few minutes";
    size_t chunkBytes = 180;         // Text per I frame (connectedChunkBytes)
    int window = 4;                  // Frames per session waiting in direwolf
};

struct SessionHooks {
    std::function<bool(const agw::Frame& frame)> toDirewolf;
    std::function<void(const Json& envelope)> toMain;
    std::function<void(const std::string& line)> log;   // May be empty
};

// Splits text into pages of at most pageBytes, at a line end or space when there's one
// in the second half of the page, never inside a UTF-8 character. 0 = one page
std::vector<std::string> splitPages(std::string_view text, size_t pageBytes);
// "\n" and "\r\n" -> "\r", ending with one
std::string toCrLines(std::string_view text);

extern const char* const morePrompt;   // After every page but the last

class Sessions {
public:
    explicit Sessions(SessionHooks hooks);

    // Turning enabled off, or a new callsign, closes every session
    void setOptions(const SessionOptions& options, TimePoint now);
    const SessionOptions& options() const { return opts; }
    // direwolf's AGW port is up (registers the callsign when enabled) or gone
    void setLinkUp(bool up, TimePoint now);
    // On connect, every open session is announced again
    void setMainConnected(bool connected, TimePoint now);

    void onAgw(const agw::Frame& frame, TimePoint now);

    // Requests from the main program; each replies or errors through toMain
    void open(const Json& body, const Json& requestId, TimePoint now);    // session.open
    void close(const Json& body, const Json& requestId, TimePoint now);   // session.close
    void send(const Json& body, const Json& requestId, TimePoint now);    // message.send with session
    void list(const Json& requestId) const;                               // session.list

    void tick(TimePoint now);

    int count() const;   // Sessions the main program knows about (not busy refusals)

private:
    enum class MessageState { Queued, Sending, Paused, Received, Skipped, Failed };
    enum class Stage { Connecting, Open, Closing };

    struct Message {
        long number = 0;                 // 0 = ours (busy, idle, offline): no status
        Json ref;
        std::vector<std::string> pages;
        size_t page = 0;
        size_t offset = 0;               // Bytes of this page given to direwolf
        size_t pagesDone = 0;            // Sent and acked
        MessageState state = MessageState::Queued;
    };

    struct Session {
        std::string id;                  // "s-3"
        std::string peer;
        bool incoming = true;
        bool hidden = false;             // A busy or offline refusal: the main program never sees it
        Stage stage = Stage::Open;
        std::string reason;              // Closing: what session.closed will say
        TimePoint since{}, lastActivity{};
        std::string inbox;               // Text after the last line end
        std::deque<Message> out;
        int outstanding = 0;             // Frames direwolf still holds, from the last 'Y'
        bool askedY = false;
        TimePoint askedAt{};
        TimePoint closingAt{};
        bool disconnectSent = false;
        TimePoint disconnectAt{};
        long nextIn = 1, nextOut = 1;
        bool done = false;               // Removed at the end of the call
    };

    Session* find(const std::string& peer);
    Session* byId(const Json& id);
    Session& add(const std::string& peer, bool incoming, bool hidden, TimePoint now);
    void refuse(const std::string& peer, const std::string& text, const std::string& reason, TimePoint now);
    void onLine(Session& s, const std::string& line, TimePoint now);
    void queueOwn(Session& s, const std::string& text);
    // keepQueued: the main program's queued messages still go out first (session.close)
    void startClosing(Session& s, const std::string& reason, TimePoint now, bool keepQueued = false);
    void dropQueued(Session& s, bool pausedOnly);
    void finish(Session& s, const std::string& reason);
    void closeAll(const std::string& reason, bool disconnect);
    void pump(Session& s, TimePoint now);
    void pageDone(Session& s, TimePoint now);
    void sweep();
    void setState(Session& s, Message& m, MessageState state, const std::string& reason = "");
    void emitStatus(const Session& s, const Message& m, const std::string& reason = "");
    void emitOpened(const Session& s);
    void error(const Json& re, const std::string& code, const std::string& message,
               const std::string& field = "");
    void say(const std::string& line);
    bool toDirewolf(const agw::Frame& frame);
    static bool final(MessageState state);
    static const char* stateName(MessageState state);
    static const char* stageName(Stage stage);

    SessionHooks hooks;
    SessionOptions opts;
    bool linkUp = false;
    bool registered = false;
    bool mainConnected = false;
    long nextSession = 1;
    std::deque<Session> sessions;   // A deque, so adding one keeps references to the rest
};

} // namespace connector

#endif //RTC_SESSIONS_H
