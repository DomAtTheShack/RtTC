#include "sessions.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace connector {

namespace {

using namespace std::chrono_literals;

constexpr auto askAgain = 1s;            // Between 'Y' questions while frames are out
constexpr auto answerLimit = 5s;         // A 'Y' with no answer by then is asked again
constexpr auto connectLimit = 3min;      // direwolf gives up sooner; this is a backstop
constexpr auto drainLimit = 60s;         // Closing: wait this long for the last text to be acked
constexpr auto disconnectLimit = 45s;    // Then this long for direwolf to say it's closed
constexpr size_t longestLine = 4096;     // A line with no end is cut here

const char* const offlineText = "The repeater's program is offline. Try again later.\r";
const char* const noMoreText = "Nothing more to show.\r";

// Bytes of text from offset, at most max, not ending inside a UTF-8 character
size_t utf8Cut(std::string_view text, size_t offset, size_t max) {
    size_t n = std::min(max, text.size() - offset);
    if (offset + n < text.size())
        while (n > 1 && (static_cast<uint8_t>(text[offset + n]) & 0xC0) == 0x80) n--;
    return n;
}

std::string upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

std::string trim(const std::string& text) {
    size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = text.find_last_not_of(" \t");
    return text.substr(start, end - start + 1);
}

} // namespace

const char* const morePrompt = "[M]ore, [N]ew, [B]ye\r";

std::vector<std::string> splitPages(std::string_view text, size_t pageBytes) {
    if (pageBytes == 0 || text.size() <= pageBytes) return {std::string(text)};
    std::vector<std::string> pages;
    size_t at = 0;
    while (at < text.size()) {
        size_t n = utf8Cut(text, at, pageBytes);
        if (at + n < text.size()) {
            std::string_view piece = text.substr(at, n);
            size_t lineEnd = piece.rfind('\r');
            size_t space = piece.rfind(' ');
            if (lineEnd != std::string_view::npos && lineEnd >= n / 2) n = lineEnd + 1;
            else if (space != std::string_view::npos && space >= n / 2) n = space + 1;
        }
        pages.emplace_back(text.substr(at, n));
        at += n;
    }
    return pages;
}

std::string toCrLines(std::string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;   // \r\n -> \r
        out += text[i] == '\n' ? '\r' : text[i];
    }
    if (out.empty() || out.back() != '\r') out += '\r';
    return out;
}

Sessions::Sessions(SessionHooks hooks) : hooks(std::move(hooks)) {}

void Sessions::setOptions(const SessionOptions& options, TimePoint) {
    bool newCall = options.callsign != opts.callsign;
    if (registered && (newCall || !options.enabled)) {
        toDirewolf(agw::unregisterCall(opts.callsign));
        registered = false;
    }
    if (opts.enabled && !options.enabled) closeAll("disabled", true);
    else if (newCall) closeAll("restart", true);
    opts = options;
    if (linkUp && opts.enabled && !registered && !opts.callsign.empty()) {
        toDirewolf(agw::registerCall(opts.callsign));
        registered = true;
    }
    sweep();
}

void Sessions::setLinkUp(bool up, TimePoint) {
    if (up == linkUp) return;
    if (!up) {
        closeAll("radio_down", false);
        linkUp = false;
        registered = false;
        sweep();
        return;
    }
    linkUp = true;
    if (opts.enabled && !opts.callsign.empty()) {
        toDirewolf(agw::registerCall(opts.callsign));
        registered = true;
    }
}

void Sessions::setMainConnected(bool connected, TimePoint) {
    mainConnected = connected;
    if (!connected) return;
    for (const Session& s : sessions)
        if (!s.done && !s.hidden && s.stage == Stage::Open) emitOpened(s);
}

void Sessions::onAgw(const agw::Frame& frame, TimePoint now) {
    switch (frame.kind) {
        case 'X':
            if (frame.data.empty() || frame.data[0] != 1)
                say("direwolf wouldn't register " + frame.from + " for connected mode");
            break;

        case 'C': {   // A session is open: ours (session.open) or someone else's connect
            std::string peer = packet::normalizeCallsign(frame.from);
            if (peer.empty()) break;
            Session* s = find(peer);
            if (s && !s->incoming && s->stage == Stage::Connecting) {
                s->stage = Stage::Open;
                s->lastActivity = now;
                say("session " + s->id + " to " + peer + " open");
                emitOpened(*s);
                pump(*s, now);
                break;
            }
            if (s) finish(*s, "replaced");   // The same station connected again
            if (!opts.enabled) {
                toDirewolf(agw::disconnect(opts.callsign, peer));
            } else if (!mainConnected) {
                refuse(peer, offlineText, "offline", now);
            } else if (count() >= opts.maxSessions) {
                refuse(peer, toCrLines(opts.busyText), "busy", now);
            } else {
                Session& added = add(peer, true, false, now);
                say("session " + added.id + " from " + peer + " open");
                emitOpened(added);
            }
            break;
        }

        case 'D': {
            Session* s = find(packet::normalizeCallsign(frame.from));
            if (!s || s->hidden) break;
            s->lastActivity = now;
            for (char c : frame.data) {
                if (c == '\r' || c == '\n') {
                    if (!s->inbox.empty()) onLine(*s, std::exchange(s->inbox, std::string()), now);
                } else {
                    s->inbox += c;
                    if (s->inbox.size() >= longestLine) onLine(*s, std::exchange(s->inbox, std::string()), now);
                }
                if (s->done) break;
            }
            break;
        }

        case 'd': {   // Closed: by us, by them, or the link failed
            Session* s = find(packet::normalizeCallsign(frame.from));
            if (!s) break;
            std::string reason = s->stage == Stage::Closing    ? s->reason
                               : s->stage == Stage::Connecting ? "no_answer"
                               : frame.data.find("RETRYOUT") != std::string::npos ? "link_lost"
                                                                                   : "remote";
            finish(*s, reason);
            break;
        }

        case 'Y': {   // Frames still out: from = us, to = them
            Session* s = find(packet::normalizeCallsign(frame.to));
            long n = agw::outstandingCount(frame);
            if (!s || n < 0) break;
            s->outstanding = static_cast<int>(n);
            s->askedY = false;
            if (n == 0 && !s->out.empty()) {
                Message& m = s->out.front();
                if (m.state != MessageState::Paused && m.offset >= m.pages[m.page].size()) pageDone(*s, now);
            }
            pump(*s, now);
            break;
        }

        default:
            break;
    }
    sweep();
}

void Sessions::open(const Json& body, const Json& requestId, TimePoint now) {
    if (!opts.enabled) return error(requestId, "connected_off", "connected mode is off (connected.enabled)");
    if (!linkUp) return error(requestId, "radio_not_ready", "direwolf isn't running");
    if (!body.contains("to") || !body["to"].is_string())
        return error(requestId, "bad_field", "must be a callsign", "to");
    std::string to = packet::normalizeCallsign(body["to"].get<std::string>());
    if (to.empty()) return error(requestId, "bad_field", "must be a callsign, e.g. K1ABC-3", "to");
    if (to == opts.callsign) return error(requestId, "bad_field", "is this station's own callsign", "to");
    if (Session* s = find(to))
        return error(requestId, "bad_field", s->hidden ? "is being disconnected" : "already has session " + s->id, "to");
    if (count() >= opts.maxSessions)
        return error(requestId, "too_many_sessions",
                     "already " + std::to_string(opts.maxSessions) + " sessions (connected.maxSessions)");
    std::vector<std::string> via = opts.path;
    if (body.contains("path")) {
        const Json& path = body["path"];
        if (!path.is_array() || path.size() > 7)
            return error(requestId, "bad_field", "must be a list of up to 7 digipeaters", "path");
        via.clear();
        for (const Json& hop : path) {
            std::string call = hop.is_string() ? packet::normalizeCallsign(hop.get<std::string>()) : "";
            if (call.empty()) return error(requestId, "bad_field", "must be a list of callsigns", "path");
            via.push_back(call);
        }
    }
    if (via.size() > 7) via.resize(7);

    Session& s = add(to, false, false, now);
    s.stage = Stage::Connecting;
    toDirewolf(agw::connect(opts.callsign, to, via));
    say("session " + s.id + ": connecting to " + to);
    hooks.toMain(envelope("session.opening", {{"session", s.id}, {"peer", to}}, nullptr, requestId));
}

void Sessions::close(const Json& body, const Json& requestId, TimePoint now) {
    Session* s = byId(body.contains("session") ? body["session"] : Json(nullptr));
    if (!s) return error(requestId, "bad_field", "no such session", "session");
    if (body.contains("text") && !body["text"].is_string())
        return error(requestId, "bad_field", "must be a string", "text");
    std::string text = body.value("text", "");
    if (!text.empty() && !validUtf8(text)) return error(requestId, "bad_field", "must be UTF-8", "text");
    hooks.toMain(envelope("session.closing", {{"session", s->id}, {"peer", s->peer}}, nullptr, requestId));
    if (s->stage == Stage::Closing) return;
    if (!text.empty() && s->stage == Stage::Open) queueOwn(*s, toCrLines(text));
    startClosing(*s, "closed", now, true);
    sweep();
}

void Sessions::send(const Json& body, const Json& requestId, TimePoint now) {
    Session* s = byId(body.contains("session") ? body["session"] : Json(nullptr));
    if (!s) return error(requestId, "bad_field", "no such session", "session");
    if (s->stage == Stage::Closing) return error(requestId, "bad_field", "is closing", "session");
    if (!body.contains("text") || !body["text"].is_string())
        return error(requestId, "bad_field", "must be a string", "text");
    const std::string& text = body["text"].get_ref<const std::string&>();
    if (text.empty()) return error(requestId, "bad_field", "must not be empty", "text");
    if (!validUtf8(text)) return error(requestId, "bad_field", "must be UTF-8", "text");

    Message m;
    m.number = s->nextOut++;
    m.ref = body.contains("ref") ? body["ref"] : Json(nullptr);
    m.pages = splitPages(toCrLines(text), opts.pageBytes);
    for (size_t i = 0; i + 1 < m.pages.size(); i++) {
        if (m.pages[i].back() != '\r') m.pages[i] += '\r';
        m.pages[i] += morePrompt;
    }
    hooks.toMain(envelope("message.accepted",
                          {{"msgId", s->id + "/t" + std::to_string(m.number)},
                           {"session", s->id},
                           {"pages", m.pages.size()}},
                          nullptr, requestId));
    s->out.push_back(std::move(m));
    emitStatus(*s, s->out.back());
    pump(*s, now);
}

void Sessions::list(const Json& requestId) const {
    Json all = Json::array();
    for (const Session& s : sessions) {
        if (s.done || s.hidden) continue;
        int queued = 0;
        for (const Message& m : s.out)
            if (m.number) queued++;
        all.push_back({{"session", s.id},
                       {"peer", s.peer},
                       {"direction", s.incoming ? "in" : "out"},
                       {"state", stageName(s.stage)},
                       {"queued", queued}});
    }
    hooks.toMain(envelope("sessions", {{"sessions", all}}, nullptr, requestId));
}

void Sessions::tick(TimePoint now) {
    for (Session& s : sessions) {
        if (s.done) continue;
        if (s.stage == Stage::Connecting && now - s.since >= connectLimit) {
            toDirewolf(agw::disconnect(opts.callsign, s.peer));
            finish(s, "no_answer");
            continue;
        }
        if (s.stage == Stage::Open && !s.hidden && opts.idleMinutes > 0 &&
            now - s.lastActivity >= std::chrono::minutes(opts.idleMinutes)) {
            dropQueued(s, false);
            queueOwn(s, "Disconnected after " + std::to_string(opts.idleMinutes) + " minutes idle.\r");
            startClosing(s, "idle", now);
            continue;
        }
        if (s.stage == Stage::Closing) {
            if (!s.disconnectSent && now - s.closingAt >= drainLimit) {
                toDirewolf(agw::disconnect(opts.callsign, s.peer));
                s.disconnectSent = true;
                s.disconnectAt = now;
            } else if (s.disconnectSent && now - s.disconnectAt >= disconnectLimit) {
                finish(s, s.reason);
                continue;
            }
        }
        pump(s, now);
    }
    sweep();
}

int Sessions::count() const {
    return static_cast<int>(std::count_if(sessions.begin(), sessions.end(),
                                          [](const Session& s) { return !s.done && !s.hidden; }));
}

Sessions::Session* Sessions::find(const std::string& peer) {
    for (Session& s : sessions)
        if (!s.done && s.peer == peer) return &s;
    return nullptr;
}

Sessions::Session* Sessions::byId(const Json& id) {
    if (!id.is_string()) return nullptr;
    for (Session& s : sessions)
        if (!s.done && !s.hidden && s.id == id.get<std::string>()) return &s;
    return nullptr;
}

Sessions::Session& Sessions::add(const std::string& peer, bool incoming, bool hidden, TimePoint now) {
    Session s;
    s.id = "s-" + std::to_string(nextSession++);
    s.peer = peer;
    s.incoming = incoming;
    s.hidden = hidden;
    s.since = s.lastActivity = now;
    sessions.push_back(std::move(s));
    return sessions.back();
}

void Sessions::refuse(const std::string& peer, const std::string& text, const std::string& reason, TimePoint now) {
    say("refused " + peer + " (" + reason + ")");
    Session& s = add(peer, true, true, now);
    queueOwn(s, text);
    startClosing(s, reason, now);
}

void Sessions::onLine(Session& s, const std::string& raw, TimePoint now) {
    std::string line = trim(raw);
    if (line.empty() || s.stage != Stage::Open) return;
    std::string command = upper(line);
    bool paused = !s.out.empty() && s.out.front().state == MessageState::Paused;

    if (command == "B" || command == "BYE") {
        startClosing(s, "bye", now);
        return;
    }
    if (command == "M" || command == "MORE") {
        if (paused) {
            setState(s, s.out.front(), MessageState::Sending);
            pump(s, now);
        } else {
            queueOwn(s, noMoreText);
            pump(s, now);
        }
        return;
    }
    if (command == "N" || command == "NEW") {
        dropQueued(s, false);
        hooks.toMain(envelope("session.new", {{"session", s.id}, {"peer", s.peer}}));
        return;
    }
    // A new prompt: the rest of the reply they were reading isn't wanted
    if (paused) {
        setState(s, s.out.front(), MessageState::Skipped);
        s.out.pop_front();
    }
    if (!mainConnected) {
        queueOwn(s, offlineText);
        pump(s, now);
        return;
    }
    std::string heard = utcNow();
    hooks.toMain(envelope("message.received", {
        {"msgId", s.id + "/r" + std::to_string(s.nextIn++)},
        {"session", s.id},
        {"from", s.peer},
        {"to", opts.callsign},
        {"text", line},
        {"firstHeard", heard},
        {"completed", heard},
    }));
}

void Sessions::queueOwn(Session& s, const std::string& text) {
    Message m;
    m.pages = {text};
    s.out.push_back(std::move(m));
}

void Sessions::dropQueued(Session& s, bool pausedOnly) {
    for (auto it = s.out.begin(); it != s.out.end();) {
        bool paused = it->state == MessageState::Paused;
        bool started = it->offset > 0 || it->pagesDone > 0;
        // A message part-way through a page can't be pulled back from direwolf, so it stays
        if (!it->number || (pausedOnly && !paused) || (started && !paused)) {
            ++it;
            continue;
        }
        setState(s, *it, MessageState::Skipped);
        it = s.out.erase(it);
    }
}

void Sessions::startClosing(Session& s, const std::string& reason, TimePoint now, bool keepQueued) {
    if (s.done || s.stage == Stage::Closing) return;
    s.reason = reason;
    s.closingAt = now;
    if (s.stage == Stage::Connecting) {
        s.stage = Stage::Closing;
        toDirewolf(agw::disconnect(opts.callsign, s.peer));
        s.disconnectSent = true;
        s.disconnectAt = now;
        return;
    }
    s.stage = Stage::Closing;
    dropQueued(s, keepQueued);
    pump(s, now);
}

void Sessions::finish(Session& s, const std::string& reason) {
    if (s.done) return;
    s.done = true;
    for (Message& m : s.out) {
        if (!m.number || final(m.state)) continue;
        if (m.state == MessageState::Paused) setState(s, m, MessageState::Skipped);
        else setState(s, m, MessageState::Failed, "the session closed (" + reason + ")");
    }
    if (s.hidden) return;
    say("session " + s.id + " with " + s.peer + " closed (" + reason + ")");
    hooks.toMain(envelope("session.closed", {{"session", s.id}, {"peer", s.peer}, {"reason", reason}}));
}

void Sessions::closeAll(const std::string& reason, bool disconnect) {
    for (Session& s : sessions) {
        if (s.done) continue;
        if (disconnect && !s.disconnectSent) toDirewolf(agw::disconnect(opts.callsign, s.peer));
        finish(s, reason);
    }
}

// Tops the session up to one window of frames in direwolf, and asks how many are left
void Sessions::pump(Session& s, TimePoint now) {
    if (!linkUp || s.done || s.stage == Stage::Connecting || s.disconnectSent) return;
    if (s.askedY) {
        if (now - s.askedAt < answerLimit) return;
        s.askedY = false;   // The answer got lost; ask again
    }
    int handed = 0;
    Message* m = s.out.empty() ? nullptr : &s.out.front();
    if (m && m->state != MessageState::Paused) {
        const std::string& page = m->pages[m->page];
        while (m->offset < page.size() && s.outstanding + handed < opts.window) {
            size_t n = utf8Cut(page, m->offset, std::max<size_t>(opts.chunkBytes, 4));
            if (!toDirewolf(agw::data(opts.callsign, s.peer, std::string_view(page).substr(m->offset, n)))) break;
            m->offset += n;
            handed++;
        }
        if (handed && m->state == MessageState::Queued) setState(s, *m, MessageState::Sending);
    }
    if (handed) s.lastActivity = now;
    bool pageOut = m && m->state != MessageState::Paused && m->offset >= m->pages[m->page].size();
    if (handed > 0 || s.outstanding > 0 || pageOut) {
        if (handed > 0 || now - s.askedAt >= askAgain) {
            s.outstanding += handed;
            if (toDirewolf(agw::outstanding(opts.callsign, s.peer))) {
                s.askedY = true;
                s.askedAt = now;
            }
        }
        return;
    }
    // Nothing left to send or ack
    if (s.stage == Stage::Closing && s.out.empty()) {
        toDirewolf(agw::disconnect(opts.callsign, s.peer));
        s.disconnectSent = true;
        s.disconnectAt = now;
    }
}

// The current page is out and acked
void Sessions::pageDone(Session& s, TimePoint now) {
    Message& m = s.out.front();
    m.pagesDone++;
    if (m.page + 1 < m.pages.size()) {
        m.page++;
        m.offset = 0;
        setState(s, m, MessageState::Paused);   // Until they type M
        return;
    }
    setState(s, m, MessageState::Received);
    s.out.pop_front();
    s.lastActivity = now;
}

void Sessions::sweep() {
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [](const Session& s) { return s.done; }),
                   sessions.end());
}

void Sessions::setState(Session& s, Message& m, MessageState state, const std::string& reason) {
    if (m.state == state) return;
    m.state = state;
    emitStatus(s, m, reason);
}

void Sessions::emitStatus(const Session& s, const Message& m, const std::string& reason) {
    if (!m.number) return;
    hooks.toMain(envelope("message.status", {
        {"msgId", s.id + "/t" + std::to_string(m.number)},
        {"session", s.id},
        {"ref", m.ref},
        {"to", s.peer},
        {"state", stateName(m.state)},
        {"pages", m.pages.size()},
        {"pagesSent", m.pagesDone},
        {"reason", reason.empty() ? Json(nullptr) : Json(reason)},
    }));
}

void Sessions::emitOpened(const Session& s) {
    hooks.toMain(envelope("session.opened",
                          {{"session", s.id}, {"peer", s.peer}, {"direction", s.incoming ? "in" : "out"}}));
}

void Sessions::error(const Json& re, const std::string& code, const std::string& message, const std::string& field) {
    hooks.toMain(errorEnvelope(re, code, message, field));
}

void Sessions::say(const std::string& line) {
    if (hooks.log) hooks.log(line);
}

bool Sessions::toDirewolf(const agw::Frame& frame) {
    if (!linkUp) return false;
    return hooks.toDirewolf(frame);
}

bool Sessions::final(MessageState state) {
    return state == MessageState::Received || state == MessageState::Skipped || state == MessageState::Failed;
}

const char* Sessions::stateName(MessageState state) {
    switch (state) {
        case MessageState::Queued:   return "queued";
        case MessageState::Sending:  return "sending";
        case MessageState::Paused:   return "paused";
        case MessageState::Received: return "received";
        case MessageState::Skipped:  return "skipped";
        case MessageState::Failed:   return "failed";
    }
    return "?";
}

const char* Sessions::stageName(Stage stage) {
    switch (stage) {
        case Stage::Connecting: return "connecting";
        case Stage::Open:       return "open";
        case Stage::Closing:    return "closing";
    }
    return "?";
}

} // namespace connector
