// Keys, program output and received packets all arrive on one event queue.
// Packet sessions keep one direwolf running; connected sessions use its AGW port.
#include "session.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "ansi.hpp"
#include "backend.h"
#include "Connector/agw.h"
#include "Connector/link_watch.h"
#include "Connector/packet.h"
#include "Connector/sessions.h"

namespace {

const ansi::Color statusColor   = ansi::Color::BrightYellow;
const ansi::Color sentColor     = ansi::Color::BrightCyan;
const ansi::Color receivedColor = ansi::Color::BrightGreen;
const ansi::Color errorColor    = ansi::Color::BrightRed;

enum class Key { Char, Enter, Backspace, Tab, Esc };

struct Event {
    enum class Type { Key, Output, OutputEnded, Packet, LinkClosed, Agw } type;
    Key key = Key::Char;
    char ch = 0;          // For Key::Char
    std::string text;     // For Output; the information field for Packet
    std::string from;     // For Packet: who sent it
    agw::Frame agw{};     // For Agw
};

class EventQueue {
public:
    void push(Event event) {
        {
            std::lock_guard lock(mutex);
            events.push_back(std::move(event));
        }
        ready.notify_one();
    }

    Event pop() {
        std::unique_lock lock(mutex);
        ready.wait(lock, [this] { return !events.empty(); });
        Event event = std::move(events.front());
        events.pop_front();
        return event;
    }

    // nullopt if nothing arrived in time
    std::optional<Event> popFor(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, timeout, [this] { return !events.empty(); })) return std::nullopt;
        Event event = std::move(events.front());
        events.pop_front();
        return event;
    }

private:
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Event> events;
};

// Reads the keyboard on a thread while it exists. Escape sequences (arrows etc.) are
// skipped; a lone ESC is the Esc key
class KeyReader {
public:
    explicit KeyReader(EventQueue& events) : events(events) {
        ansi::setRawMode(true);
        // Ctrl-C mustn't kill us with the radio keyed
        os::setInterruptKey(false);
        thread = std::thread([this] { run(); });
    }

    ~KeyReader() {
        stopping = true;
        thread.join();
        ansi::setRawMode(false);   // Also turns the interrupt key back on
    }

    KeyReader(const KeyReader&) = delete;
    KeyReader& operator=(const KeyReader&) = delete;

private:
    void push(Key key, char ch = 0) { events.push({Event::Type::Key, key, ch, {}, {}}); }

    void run() {
        while (!stopping) {
            if (!os::waitForInput(100)) continue;
            int c = os::readInputByte();
            switch (c) {
                case -1:             // stdin closed
                    push(Key::Esc);
                    return;
                case 3:              // Ctrl-C
                    push(Key::Esc);
                    break;
                case 4:              // Ctrl-D
                case '\t':
                    push(Key::Tab);
                    break;
                case '\r':
                case '\n':
                    push(Key::Enter);
                    break;
                case 8:
                case 127:
                    push(Key::Backspace);
                    break;
                case 27:
                    if (!os::waitForInput(50)) {
                        push(Key::Esc);
                        break;
                    }
                    c = os::readInputByte();
                    if (c == '[' || c == 'O') {
                        do c = os::readInputByte(); while (c != -1 && (c < 0x40 || c > 0x7e));
                    }
                    break;
                default:
                    if (c >= 32 && c < 127) push(Key::Char, static_cast<char>(c));
                    break;
            }
        }
    }

    EventQueue& events;
    std::atomic<bool> stopping{false};
    std::thread thread;
};

void printColored(ansi::Color color, const std::string& text) {
    ansi::setColor(color);
    std::fwrite(text.data(), 1, text.size(), stdout);
    ansi::reset();
    ansi::flush();
}

// One modem process. Its output goes to the queue, then OutputEnded when it exits
class Modem {
public:
    explicit Modem(EventQueue& events) : events(events) {}

    ~Modem() {
        if (reader.joinable()) {
            process.kill();
            reader.join();
        }
    }

    Modem(const Modem&) = delete;
    Modem& operator=(const Modem&) = delete;

    bool start(const std::vector<std::string>& args, bool pipeStdin, const std::string& workingDir = "") {
        if (!process.start(args, true, pipeStdin, workingDir)) {
            ansi::print(errorColor, "%s\n", process.error().c_str());
            return false;
        }
        reader = std::thread([this] {
            char buffer[256];
            long n;
            while ((n = process.read(buffer, sizeof buffer)) > 0)
                events.push({Event::Type::Output, Key::Char, 0, std::string(buffer, n), {}});
            events.push({Event::Type::OutputEnded, Key::Char, 0, {}, {}});
        });
        return true;
    }

    // Call after OutputEnded. Returns the exit code
    int wait() {
        reader.join();
        return process.wait();
    }

    os::Process process;

private:
    EventQueue& events;
    std::thread reader;
};

enum class TurnEnd { Next, Quit, Failed };

TurnEnd transmitTurn(const std::vector<std::string>& args, EventQueue& events, const char* help,
                     const TransmitFormat& format) {
    ansi::print(statusColor, ansi::Bold, "\n== TX ==");
    ansi::print(statusColor, "  %s\n", help);
    ansi::flush();

    Modem modem(events);
    if (!modem.start(args, true)) return TurnEnd::Failed;

    auto toProgram = [&](const std::string& typed) {
        return format.format ? format.format(typed) : typed;
    };
    std::string line;
    std::string output;       // With hideOutput: shown only on failure
    bool finishing = false;   // Tab pressed: sending what's left, then exits
    bool quitting  = false;
    auto sendLine = [&] {
        modem.process.write(toProgram(line + "\n"));
        line.clear();
        std::fputs("\n", stdout);
    };

    for (;;) {
        Event event = events.pop();
        if (event.type == Event::Type::OutputEnded) break;
        if (event.type == Event::Type::Output) {
            // Nothing is printed while it works, so output means a problem
            if (format.hideOutput) output += event.text;
            else printColored(errorColor, event.text);
            continue;
        }
        if (finishing || quitting) {
            if (event.key == Key::Esc && !quitting) {
                modem.process.kill();
                quitting = true;
            }
            continue;
        }
        switch (event.key) {
            case Key::Char:
                line += event.ch;
                ansi::print(sentColor, "%c", event.ch);
                break;
            case Key::Backspace:
                if (!line.empty()) {
                    line.pop_back();
                    std::fputs("\b \b", stdout);
                }
                break;
            case Key::Enter:
                sendLine();
                break;
            case Key::Tab:
                if (!line.empty()) sendLine();
                modem.process.closeStdin();   // It writes what's queued, then exits
                finishing = true;
                ansi::print(statusColor, "\n(finishing...)\n");
                break;
            case Key::Esc:
                modem.process.kill();
                quitting = true;
                break;
        }
        ansi::flush();
    }
    int status = modem.wait();
    if (quitting) return TurnEnd::Quit;
    if (!finishing || status != 0) {
        if (!output.empty()) printColored(errorColor, output);
        ansi::print(errorColor, "%s stopped unexpectedly (exit %d)\n", format.name, status);
        return TurnEnd::Failed;
    }
    return TurnEnd::Next;
}

} // namespace

bool runFileTransmit(const std::vector<std::string>& args, const TransmitFormat& format) {
    EventQueue events;
    KeyReader keys(events);
    return transmitTurn(args, events, "Enter: send | Tab: finish | Esc: stop", format) != TurnEnd::Failed;
}

// Packets (direwolf)

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// direwolf doesn't say when a packet has gone out, so estimate it. Key-up covers the
// channel wait (DWAIT and TXDELAY are added per session); tail covers TXTAIL and sound card buffering
constexpr double keyUpSeconds = 0.7;
constexpr double tailSeconds  = 1.0;

Clock::duration seconds(double s) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s));
}

// direwolf's KISS port. Packets heard go to the queue, then LinkClosed
class KissLink {
public:
    explicit KissLink(EventQueue& events) : events(events) {}
    ~KissLink() { stop(); }

    KissLink(const KissLink&) = delete;
    KissLink& operator=(const KissLink&) = delete;

    bool connect(int port) { return connection.connect("127.0.0.1", port); }
    void start() { reader = std::thread([this] { run(); }); }
    bool send(const packet::Bytes& frame) { return connection.write(packet::kissEncode(frame)); }
    void stop() {
        if (reader.joinable()) {
            connection.shutdown();   // Wakes the reader
            reader.join();
        }
        connection.close();
    }

private:
    void run() {
        packet::KissDecoder decoder;
        char buffer[1024];
        long n;
        while ((n = connection.read(buffer, sizeof buffer)) > 0) {
            for (const packet::Bytes& bytes : decoder.feed(std::string_view(buffer, static_cast<size_t>(n)))) {
                if (auto frame = packet::decode(bytes)) {
                    // Show who it's for unless it's to CQ with no path. Then the
                    // digipeaters as direwolf shows them: a "*" on ones that have repeated
                    // it, e.g. N0CALL-7>APRS,WIDE1-1*,WIDE2-1
                    std::string from = frame->source;
                    if (frame->destination != packet::groupAddress || !frame->path.empty())
                        from += ">" + frame->destination;
                    for (const std::string& hop : frame->path) from += "," + hop;
                    events.push({Event::Type::Packet, Key::Char, 0, frame->info, from});
                }
            }
        }
        events.push({Event::Type::LinkClosed, Key::Char, 0, {}, {}});
    }

    EventQueue& events;
    os::TcpConnection connection;
    std::thread reader;
};

// The typed line stays at the bottom; packets and status lines print above it
class ChatScreen {
public:
    explicit ChatScreen(bool typing) : typing(typing) { redraw(); }

    void showPacket(const std::string& from, const std::string& info, ansi::Color color) {
        clear();
        ansi::print(color, ansi::Bold, "%s:", from.c_str());
        ansi::print(color, " %s\n", packet::printable(info).c_str());
        redraw();
    }

    void status(const char* text) {
        clear();
        ansi::print(statusColor, "%s\n", text);
        redraw();
    }

    // A whole line of text in one color
    void note(ansi::Color color, const std::string& text) {
        clear();
        ansi::print(color, "%s\n", text.c_str());
        redraw();
    }

    // What's shown before the typed line, e.g. "K1ABC-3> "
    void setPrompt(const std::string& text) {
        if (text == prompt) return;
        clear();
        prompt = text;
        redraw();
    }

    void add(char ch) {
        line += ch;
        if (typing && !isContinuationByte(ch)) shownChars++;
        ansi::print(sentColor, "%c", ch);
        ansi::flush();
    }

    void backspace() {
        if (line.empty()) return;
        // A whole UTF-8 character. Redrawn, as "\b" can't go back up onto a wrapped row
        clear();
        while (!line.empty() && isContinuationByte(line.back())) line.pop_back();
        if (!line.empty()) line.pop_back();
        redraw();
    }

    // Takes the typed line, leaving the prompt empty
    std::string take() {
        std::string taken;
        taken.swap(line);
        clear();
        redraw();
        return taken;
    }

    void stopTyping() {
        clear();
        typing = false;
        ansi::flush();
    }

    bool empty() const { return line.empty(); }

private:
    static bool isContinuationByte(char ch) { return (static_cast<unsigned char>(ch) & 0xC0) == 0x80; }

    static int countChars(const std::string& text) {
        int n = 0;
        for (char ch : text)
            if (!isContinuationByte(ch)) n++;
        return n;
    }

    // Erases the prompt and typed line, every row they wrap onto, leaving the cursor at
    // the start of the first. A full last row leaves the cursor on it (terminals wrap
    // only when the next character comes)
    void clear() {
        if (!typing) return;
        int columns = os::terminalColumns();
        int rowsAbove = shownChars > 0 ? (shownChars - 1) / columns : 0;
        if (rowsAbove > 0) {
            std::string up = "\033[" + std::to_string(rowsAbove) + "A";
            ansi::esc(up.c_str());
        }
        ansi::esc("\r\033[J");   // Start of the row, then clear to the end of the screen
        shownChars = 0;
    }

    void redraw() {
        if (typing) {
            ansi::print(sentColor, "%s%s", prompt.c_str(), line.c_str());
            shownChars = countChars(prompt) + countChars(line);
        }
        ansi::flush();
    }

    bool typing;
    std::string prompt = "> ";
    std::string line;
    int shownChars = 0;   // Characters of prompt and line on screen, for clear()
};

// direwolf for one session: its output is kept, and shown only if it fails
class Direwolf {
public:
    explicit Direwolf(EventQueue& events) : events(events), modem(events) {}

    bool start(const std::vector<std::string>& args, const std::string& workingDir) {
        ansi::print(statusColor, "\nStarting direwolf...\n");
        ansi::flush();
        return modem.start(args, false, workingDir);
    }

    // Takes the event if it's direwolf's output
    void handle(const Event& event) {
        if (event.type == Event::Type::Output) {
            log += event.text;
            if (log.size() > 6000) log.erase(0, log.size() - 6000);
        } else if (event.type == Event::Type::OutputEnded) {
            ended = true;
        }
    }

    bool hasEnded() const { return ended; }

    enum class Wait { Up, Esc, Failed };

    // Calls connect (to one of its ports) until it works, for up to 15 s
    template <typename Connect>
    Wait waitForPort(Connect connect) {
        auto giveUp = Clock::now() + 15s;
        while (!connect()) {
            if (auto event = events.popFor(250ms)) {
                handle(*event);
                if (ended) {
                    int status = modem.wait();
                    showFailure("direwolf stopped while starting (exit " + std::to_string(status) + ")");
                    return Wait::Failed;
                }
                if (event->type == Event::Type::Key && event->key == Key::Esc) {
                    stop();
                    return Wait::Esc;
                }
            }
            if (Clock::now() > giveUp) {
                stop();
                showFailure("direwolf didn't start within 15 s. With CAT PTT, check the radio is on "
                            "and Settings > PTT is right.");
                return Wait::Failed;
            }
        }
        return Wait::Up;
    }

    // SIGINT first so direwolf unkeys the radio, then kill. Returns the exit code
    int stop() {
        if (!ended) modem.process.interrupt();
        auto deadline = Clock::now() + 3s;
        while (!ended) {
            if (auto event = events.popFor(100ms)) handle(*event);
            if (!ended && Clock::now() > deadline) {
                modem.process.kill();
                deadline = Clock::time_point::max();
            }
        }
        return modem.wait();
    }

    void showFailure(const std::string& why) const {
        ansi::print(errorColor, "\n%s\n", why.c_str());
        if (!log.empty()) {
            ansi::print(errorColor, "direwolf's last output:\n");
            ansi::print(ansi::Color::Gray, "%s\n", log.c_str());
        }
    }

private:
    EventQueue& events;
    Modem modem;
    std::string log;
    bool ended = false;
};

} // namespace

bool runPacketSession(const PacketSession& p, bool& transmitted) {
    transmitted = false;
    EventQueue events;
    KeyReader keys(events);

    Direwolf direwolf(events);
    if (!direwolf.start(p.direwolfArgs, p.workingDir)) return false;

    KissLink link(events);
    switch (direwolf.waitForPort([&] { return link.connect(p.kissPort); })) {
        case Direwolf::Wait::Up:     break;
        case Direwolf::Wait::Esc:    return true;
        case Direwolf::Wait::Failed: return false;
    }
    link.start();

    const char* help = !p.canType        ? "Esc: stop"
                     : p.finishAfterText ? "Esc: stop"
                     : p.showReceived    ? "Enter: send | Esc: stop"
                                         : "Enter: send | Tab: finish | Esc: stop";
    ansi::print(statusColor, ansi::Bold, "== PACKET ==");
    ansi::print(statusColor, "  %s\n", help);
    ChatScreen screen(p.canType && !p.finishAfterText);

    Clock::time_point busyUntil = Clock::now();
    bool ok = true;
    std::string failure;
    // One packet per line (long lines split)
    auto send = [&](const std::string& text) {
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            for (const std::string& piece : packet::splitInfo(line, packet::maxInfoFor(p.fx25))) {
                packet::Bytes frame = packet::encodeUi(p.callsign, p.destination, piece);
                if (!link.send(frame)) return false;
                auto now = Clock::now();
                Clock::time_point begins = busyUntil > now ? busyUntil : now + seconds(keyUpSeconds + (p.turnaroundWaitMs + p.txDelayMs) / 1000.0);
                busyUntil = begins + seconds(packet::airtime(frame.size(), p.baud, p.fx25));
                transmitted = true;
            }
            screen.showPacket(p.destination == packet::groupAddress ? p.callsign
                                                                    : p.callsign + ">" + p.destination,
                              line, sentColor);
        }
        return true;
    };
    auto onAirDone = [&] { return !transmitted || Clock::now() >= busyUntil + seconds(tailSeconds); };

    bool finishing = false;   // Waiting for sent packets to go out
    if (!p.text.empty() && !send(p.text)) {
        ok = false;
        failure = "Couldn't send to direwolf";
    }
    if (p.finishAfterText) finishing = true;

    while (ok && !(finishing && onAirDone())) {
        auto event = events.popFor(100ms);
        if (!event) continue;
        direwolf.handle(*event);
        if (direwolf.hasEnded()) {
            ok = false;
            failure = "direwolf stopped unexpectedly";
            break;
        }
        if (event->type == Event::Type::LinkClosed) {
            ok = false;
            failure = "direwolf closed the KISS connection";
            break;
        }
        if (event->type == Event::Type::Packet) {
            if (p.showReceived) screen.showPacket(event->from, event->text, receivedColor);
            continue;
        }
        if (event->type != Event::Type::Key) continue;

        if (finishing) {
            if (event->key == Key::Esc) {
                screen.status("(stopped)");
                break;
            }
            continue;
        }
        bool canType = p.canType;
        switch (event->key) {
            case Key::Char:
                if (canType) screen.add(event->ch);
                break;
            case Key::Backspace:
                if (canType) screen.backspace();
                break;
            case Key::Enter:
                if (canType && !screen.empty() && !send(screen.take())) {
                    ok = false;
                    failure = "Couldn't send to direwolf";
                }
                break;
            case Key::Tab:
                // Only a send-only session finishes on Tab
                if (!canType || p.showReceived) break;
                if (!screen.empty() && !send(screen.take())) {
                    ok = false;
                    failure = "Couldn't send to direwolf";
                    break;
                }
                screen.stopTyping();
                if (!onAirDone()) screen.status("(finishing...)");
                finishing = true;
                break;
            case Key::Esc:
                finishing = true;
                if (onAirDone()) break;
                screen.stopTyping();
                screen.status("(finishing... Esc again to stop now)");
                break;
        }
    }
    screen.stopTyping();

    link.stop();
    int status = direwolf.stop();
    if (!ok) {
        direwolf.showFailure(failure + " (exit " + std::to_string(status) + ")");
        return false;
    }
    return true;
}

// Connected mode

namespace {

using connector::Json;

const ansi::Color noteColor = ansi::Color::Gray;
const ansi::Color goodColor = ansi::Color::BrightGreen;

constexpr size_t maxVia = 2;               // Digipeaters for /c; frames are sized to fit them
constexpr auto echoLimit = 120s;           // Range test: a line with no echo by then is lost
const char* const rangeTag = "RANGE ";
const char* const echoTag = "ECHO ";

// direwolf's AGW port. Frames heard go to the queue, then LinkClosed
class AgwLink {
public:
    explicit AgwLink(EventQueue& events) : events(events) {}
    ~AgwLink() { stop(); }

    AgwLink(const AgwLink&) = delete;
    AgwLink& operator=(const AgwLink&) = delete;

    bool connect(int port) { return connection.connect("127.0.0.1", port); }
    void start() { reader = std::thread([this] { run(); }); }
    bool send(const agw::Frame& frame) { return connection.write(agw::encode(frame)); }
    void stop() {
        if (reader.joinable()) {
            connection.shutdown();   // Wakes the reader
            reader.join();
        }
        connection.close();
    }

private:
    void run() {
        agw::Decoder decoder;
        char buffer[1024];
        long n;
        while ((n = connection.read(buffer, sizeof buffer)) > 0) {
            for (agw::Frame& frame : decoder.feed(std::string_view(buffer, static_cast<size_t>(n)))) {
                Event event{Event::Type::Agw};
                event.agw = std::move(frame);
                events.push(std::move(event));
            }
            if (decoder.broken()) break;
        }
        events.push({Event::Type::LinkClosed});
    }

    EventQueue& events;
    os::TcpConnection connection;
    std::thread reader;
};

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string clockTime() {
    std::time_t t = std::time(nullptr);
    char buffer[16];
    std::strftime(buffer, sizeof buffer, "%H:%M:%S", std::localtime(&t));
    return buffer;
}

std::string secondsText(double s) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f s", s);
    return buffer;
}

// Why a session closed, from session.closed's reason
std::string closeReason(const std::string& reason) {
    if (reason == "no_answer")  return "no answer";
    if (reason == "link_lost")  return "link lost: too many retries";
    if (reason == "remote")     return "they disconnected";
    if (reason == "bye")        return "they typed B";
    if (reason == "closed")     return "you disconnected";
    if (reason == "replaced")   return "they connected again";
    if (reason == "radio_down") return "direwolf stopped";
    return reason;
}

// Plays the main program's part for the session code RadioConnector uses: what it would
// get as JSON is shown as plain text, and typed lines become its requests
class ConnectedChat {
public:
    ConnectedChat(const ConnectedSession& p, AgwLink& link, ChatScreen& screen)
        : p(p), link(link), screen(screen),
          sessions({[this](const agw::Frame& frame) { return toDirewolf(frame); },
                    [this](const Json& envelope) { fromSessions(envelope); },
                    {}}) {}

    void start(connector::TimePoint now) {
        connector::SessionOptions o;
        o.enabled = true;
        o.callsign = packet::normalizeCallsign(p.callsign);
        o.maxSessions = p.maxSessions;
        o.idleMinutes = 0;   // A test screen: sessions stay open until closed
        o.pageBytes = 0;     // Typed lines are short; paging is the Radio Connector's
        o.chunkBytes = connector::connectedChunkBytes(p.fx25, maxVia);
        own = o.callsign;
        watch = connector::LinkWatch(own);
        toDirewolf(agw::rawFrames());   // Every frame heard, for the link watch
        sessions.setOptions(o, now);
        sessions.setLinkUp(true, now);
        // Calls are accepted while this screen is open, as with a main program connected
        sessions.setMainConnected(true, now);
        screen.note(noteColor, "Other stations can connect to " + own + ". /h lists the commands.");
        if (!p.connectTo.empty()) connect(p.connectTo, {}, now);
        updatePrompt();
    }

    void typed(const std::string& line, connector::TimePoint now) {
        if (line.empty()) return;
        if (line[0] == '/') {
            command(line.substr(1), now);
        } else if (active.empty()) {
            screen.note(statusColor, "Not connected. /c CALL connects, e.g. /c K1ABC-3");
        } else {
            screen.showPacket(own + ">" + peers[active].call, line, sentColor);
            sessions.send({{"session", active}, {"text", line}}, nullptr, now);
        }
        updatePrompt();
    }

    void onAgw(const agw::Frame& frame, connector::TimePoint now) {
        if (frame.kind == 'K' && !frame.data.empty()) {
            packet::Bytes bytes(frame.data.begin() + 1, frame.data.end());   // After the port byte
            for (const std::string& problem : watch.heard(bytes)) report(problem);
        } else if (frame.kind == 'Y') {
            int count = static_cast<int>(agw::outstandingCount(frame));
            for (const std::string& problem : watch.outstanding(packet::normalizeCallsign(frame.to), count, now))
                report(problem);
        }
        sessions.onAgw(frame, now);
        updatePrompt();
    }

    void tick(connector::TimePoint now) {
        sessions.tick(now);
        tickRange(now);
        updatePrompt();
    }

    // Disconnects every session. true if there were none
    bool closeAll(connector::TimePoint now) {
        if (range.running) finishRange("stopped");
        std::vector<std::string> ids;
        for (const auto& entry : peers) ids.push_back(entry.first);
        for (const std::string& id : ids) sessions.close({{"session", id}}, nullptr, now);
        return peers.empty();
    }

    bool idle() const { return peers.empty(); }
    bool failed() const { return linkFailed; }

private:
    struct Peer {
        std::string call;
        bool incoming = false;
        bool open = false;
    };

    // Range test: numbered lines, each echoed by the other RadioConnectorCli
    struct Range {
        bool running = false;
        std::string session;
        int total = 0, sent = 0, lost = 0;
        int gapSeconds = 5;
        int waiting = 0;   // The line whose echo is due, 0 = none
        connector::TimePoint sentAt{}, nextAt{};
        std::vector<double> trips;
    };

    // Something that shouldn't happen on a healthy link: shown, and kept for /log
    void report(const std::string& text) {
        std::string time = clockTime();
        remember(time, "! " + text);
        screen.note(noteColor, "  ! " + time + " " + text);
    }

    // For /log only (what's shown already says it)
    void remember(const std::string& time, const std::string& text) {
        log.push_back(time + "  " + text);
        if (log.size() > maxLog) log.pop_front();
    }
    void remember(const std::string& text) { remember(clockTime(), text); }

    bool toDirewolf(const agw::Frame& frame) {
        if (!link.send(frame)) linkFailed = true;
        return !linkFailed;
    }

    void connect(const std::string& call, const std::vector<std::string>& via, connector::TimePoint now) {
        Json body = {{"to", call}};
        if (!via.empty()) body["path"] = via;
        sessions.open(body, nullptr, now);
    }

    void fromSessions(const Json& envelope) {
        std::string type = envelope.value("type", "");
        const Json body = envelope.contains("body") ? envelope["body"] : Json::object();
        std::string id = body.value("session", "");
        std::string call = body.value("peer", "");

        if (type == "session.opening") {
            peers[id] = {call, false, false};
            if (active.empty()) active = id;
            watch.setPeer(call, false);
            remember("Connecting to " + call);
            screen.note(noteColor, "Connecting to " + call + "...");
        } else if (type == "session.opened") {
            Peer& peer = peers[id];
            peer.call = call;
            peer.incoming = body.value("direction", "") == "in";
            peer.open = true;
            if (active.empty()) active = id;
            watch.setPeer(call, true);
            std::string text = peer.incoming ? call + " connected" : "Connected to " + call;
            remember(text);
            if (active != id) text += ". /s " + call + " to talk to them";
            screen.note(goodColor, text);
        } else if (type == "session.closed") {
            std::string reason = body.value("reason", "");
            std::string text = "Disconnected from " + call + " (" + closeReason(reason) + ")";
            // Not asked for by either side: the link failed
            bool failed = reason == "no_answer" || reason == "link_lost" || reason == "radio_down";
            remember((failed ? "! " : "") + text);
            screen.note(statusColor, text);
            watch.removePeer(call);
            if (range.running && range.session == id) finishRange("ended, the link closed");
            peers.erase(id);
            if (active == id) {
                active = peers.empty() ? "" : peers.begin()->first;
                if (!active.empty()) screen.note(noteColor, "Now talking to " + peers[active].call);
            }
        } else if (type == "session.new") {
            screen.note(noteColor, call + " typed N (new conversation)");
        } else if (type == "message.received") {
            std::string text = body.value("text", "");
            screen.showPacket(body.value("from", call), text, receivedColor);
            onReceived(id, text);
        } else if (type == "message.status") {
            if (body.value("state", "") == "failed") {
                std::string text = "Not delivered to " + body.value("to", "");
                if (body.contains("reason") && body["reason"].is_string())
                    text += " (" + body["reason"].get<std::string>() + ")";
                remember("! " + text);
                screen.note(errorColor, text);
            }
        } else if (type == "error") {
            std::string field = body.value("field", "");
            screen.note(errorColor, "Can't: " + (field.empty() ? "" : field + " ") + body.value("message", ""));
        }
        // message.accepted and session.closing need nothing shown
    }

    void onReceived(const std::string& id, const std::string& text) {
        auto now = connector::Clock::now();
        if (text.rfind(rangeTag, 0) == 0) {   // The other end's range test: echo it
            sessions.send({{"session", id}, {"text", echoTag + text}}, nullptr, now);
            screen.note(noteColor, "(echoed)");
            return;
        }
        std::string echo = std::string(echoTag) + rangeTag;
        if (!range.running || id != range.session || text.rfind(echo, 0) != 0) return;
        int n = std::atoi(text.c_str() + echo.size());
        if (n == 0 || n != range.waiting) return;
        double trip = std::chrono::duration<double>(now - range.sentAt).count();
        range.trips.push_back(trip);
        range.waiting = 0;
        range.nextAt = now + std::chrono::seconds(range.gapSeconds);
        screen.note(goodColor, "Range " + std::to_string(n) + "/" + std::to_string(range.total) +
                    ": echoed after " + secondsText(trip));
    }

    // A session by id ("s-3" or "3") or callsign. "" if none
    std::string resolve(const std::string& target) const {
        if (peers.count(target)) return target;
        if (peers.count("s-" + target)) return "s-" + target;
        std::string call = packet::normalizeCallsign(target);
        for (const auto& entry : peers)
            if (entry.second.call == call) return entry.first;
        return "";
    }

    void command(const std::string& text, connector::TimePoint now) {
        std::istringstream words(text);
        std::string name, arg;
        words >> name;
        name = lower(name);
        std::vector<std::string> args;
        while (words >> arg) args.push_back(arg);

        if (name == "c" || name == "connect") {
            const char* usage = "/c CALL [via DIGI1,DIGI2]";
            if (args.empty()) return screen.note(statusColor, usage);
            std::vector<std::string> via;
            if (args.size() == 3 && lower(args[1]) == "via") {
                std::istringstream hops(args[2]);
                std::string hop;
                while (std::getline(hops, hop, ','))
                    if (!hop.empty()) via.push_back(hop);
                if (via.size() > maxVia)
                    return screen.note(statusColor, "At most " + std::to_string(maxVia) + " digipeaters");
            } else if (args.size() > 1) {
                return screen.note(statusColor, usage);
            }
            connect(args[0], via, now);
        } else if (name == "d" || name == "disconnect") {
            std::string id = args.empty() ? active : resolve(args[0]);
            if (id.empty()) return screen.note(statusColor, "No such session");
            screen.note(noteColor, "Disconnecting from " + peers[id].call + "...");
            sessions.close({{"session", id}}, nullptr, now);
        } else if (name == "s" || name == "sessions") {
            if (!args.empty()) {
                std::string id = resolve(args[0]);
                if (id.empty()) return screen.note(statusColor, "No such session");
                active = id;
                return screen.note(noteColor, "Now talking to " + peers[id].call);
            }
            if (peers.empty()) return screen.note(noteColor, "No sessions");
            for (const auto& [id, peer] : peers) {
                screen.note(noteColor, std::string(id == active ? "* " : "  ") + id + "  " + peer.call +
                            (peer.incoming ? "  in" : "  out") + (peer.open ? "" : ", connecting"));
            }
        } else if (name == "r" || name == "range") {
            if (!args.empty() && lower(args[0]) == "stop") {
                if (range.running) finishRange("stopped");
                return;
            }
            int count = args.size() > 0 ? std::atoi(args[0].c_str()) : 10;
            int gap = args.size() > 1 ? std::atoi(args[1].c_str()) : 5;
            if (count < 1 || count > 1000 || gap < 0 || gap > 3600)
                return screen.note(statusColor, "/r [lines 1-1000] [seconds 0-3600], or /r stop");
            startRange(count, gap, now);
        } else if (name == "l" || name == "log") {
            if (log.empty()) return screen.note(noteColor, "Nothing logged yet");
            int problems = 0;
            for (const std::string& line : log)
                if (line.find("  ! ") != std::string::npos) problems++;
            screen.note(noteColor, "Link log, " + std::to_string(problems) +
                                       (problems == 1 ? " problem" : " problems") + " (marked !):");
            for (const std::string& line : log) screen.note(noteColor, "  " + line);
        } else if (name == "h" || name == "help" || name == "?") {
            const char* help[] = {
                "Typed lines go to the session shown before the >.",
                "/c CALL [via D1,D2]  connect, e.g. /c K1ABC-3 or /c K1ABC-3 via WIDE1-1",
                "/d [CALL]            disconnect (the current session if no CALL)",
                "/s [CALL]            list the sessions, or talk to CALL's",
                "/r [N] [SECS]        range test: N numbered lines (10), each SECS (5) after the last",
                "                     one's echo. The far end must be RadioConnectorCli in Connected mode",
                "/r stop              stop the range test",
                "/l                   the link log: connects, disconnects and anything that went",
                "                     wrong (resends, no answers, missed frames), marked !",
                "Esc                  disconnect everything and stop. Esc again stops now",
                "To a Radio Connector: after a long reply type M (more), N (new chat) or B (bye)",
            };
            for (const char* line : help) screen.note(noteColor, line);
        } else {
            screen.note(statusColor, "Unknown command /" + name + ". /h lists them");
        }
    }

    void startRange(int count, int gap, connector::TimePoint now) {
        if (active.empty()) return screen.note(statusColor, "Connect first: /c CALL");
        if (range.running) return screen.note(statusColor, "A range test is running. /r stop stops it");
        range = Range{};
        range.running = true;
        range.session = active;
        range.total = count;
        range.gapSeconds = gap;
        range.nextAt = now;
        screen.note(noteColor, "Range test with " + peers[active].call + ": " + std::to_string(count) +
                    " lines, each " + std::to_string(gap) + " s after the last one's echo");
    }

    void tickRange(connector::TimePoint now) {
        if (!range.running) return;
        if (range.waiting && now - range.sentAt >= echoLimit) {
            std::string text = "Range " + std::to_string(range.waiting) + ": no echo after " +
                               secondsText(std::chrono::duration<double>(echoLimit).count());
            remember("! " + text);
            screen.note(statusColor, text);
            range.lost++;
            range.waiting = 0;
            range.nextAt = now;
        }
        if (range.waiting || now < range.nextAt) return;
        if (range.sent >= range.total) return finishRange("");
        // Wait for the session to open before sending
        if (!peers.count(range.session) || !peers[range.session].open) return;
        range.sent++;
        range.waiting = range.sent;
        range.sentAt = now;
        std::string text = rangeTag + std::to_string(range.sent) + "/" + std::to_string(range.total);
        screen.showPacket(own + ">" + peers[range.session].call, text, sentColor);
        sessions.send({{"session", range.session}, {"text", text}}, nullptr, now);
    }

    void finishRange(const std::string& why) {
        if (range.waiting) range.lost++;
        range.waiting = 0;
        range.running = false;
        int echoed = static_cast<int>(range.trips.size());
        std::string text = "Range test " + (why.empty() ? std::string("done") : why) + ": " +
                           std::to_string(range.sent) + " sent, " + std::to_string(echoed) + " echoed, " +
                           std::to_string(range.lost) + " lost";
        if (echoed > 0) {
            double low = range.trips[0], high = range.trips[0], sum = 0;
            for (double t : range.trips) {
                low = std::min(low, t);
                high = std::max(high, t);
                sum += t;
            }
            text += ". Round trip " + secondsText(low) + " to " + secondsText(high) +
                    ", average " + secondsText(sum / echoed);
        }
        screen.note(echoed == range.sent ? goodColor : statusColor, text);
    }

    void updatePrompt() {
        auto it = peers.find(active);
        if (it == peers.end()) return screen.setPrompt("> ");
        screen.setPrompt(it->second.call + (it->second.open ? "> " : " (connecting)> "));
    }

    const ConnectedSession& p;
    AgwLink& link;
    ChatScreen& screen;
    std::string own;
    std::map<std::string, Peer> peers;   // By session id
    std::string active;                  // Where typed lines go
    Range range;
    bool linkFailed = false;
    static constexpr size_t maxLog = 500;
    std::deque<std::string> log;         // For /log: "HH:MM:SS  text", problems as "! text"
    connector::LinkWatch watch{""};
    connector::Sessions sessions;        // Last: its hooks use the members above
};

} // namespace

bool runConnectedSession(const ConnectedSession& p, bool& transmitted) {
    transmitted = false;
    EventQueue events;
    KeyReader keys(events);

    Direwolf direwolf(events);
    if (!direwolf.start(p.direwolfArgs, p.workingDir)) return false;
    AgwLink link(events);
    switch (direwolf.waitForPort([&] { return link.connect(p.agwPort); })) {
        case Direwolf::Wait::Up:     break;
        case Direwolf::Wait::Esc:    return true;
        case Direwolf::Wait::Failed: return false;
    }
    link.start();
    transmitted = true;   // From here direwolf may answer a connect

    ansi::print(statusColor, ansi::Bold, "== CONNECTED ==");
    ansi::print(statusColor, "  Enter: send | /h: commands | Esc: disconnect and stop\n");
    ChatScreen screen(true);
    ConnectedChat chat(p, link, screen);
    chat.start(Clock::now());

    bool ok = true;
    bool closing = false;   // Esc: waiting for the sessions to close
    bool stopNow = false;
    std::string failure;
    while (ok && !stopNow) {
        auto event = events.popFor(100ms);
        auto now = Clock::now();
        if (event) {
            direwolf.handle(*event);
            if (direwolf.hasEnded()) {
                ok = false;
                failure = "direwolf stopped unexpectedly";
                break;
            }
            if (event->type == Event::Type::LinkClosed) {
                ok = false;
                failure = "direwolf closed the AGW connection";
                break;
            }
            if (event->type == Event::Type::Agw) chat.onAgw(event->agw, now);
            if (event->type == Event::Type::Key) {
                switch (event->key) {
                    case Key::Char:
                        if (!closing) screen.add(event->ch);
                        break;
                    case Key::Backspace:
                        if (!closing) screen.backspace();
                        break;
                    case Key::Enter:
                        if (!closing && !screen.empty()) chat.typed(screen.take(), now);
                        break;
                    case Key::Tab:
                        break;
                    case Key::Esc:
                        if (closing) {
                            screen.status("(stopped)");
                            stopNow = true;
                            break;
                        }
                        closing = true;
                        screen.stopTyping();
                        if (chat.closeAll(now)) {
                            stopNow = true;
                            break;
                        }
                        screen.status("(disconnecting... Esc again to stop now)");
                        break;
                }
            }
        }
        if (stopNow) break;
        chat.tick(now);
        if (chat.failed()) {
            ok = false;
            failure = "Couldn't send to direwolf";
            break;
        }
        if (closing && chat.idle()) break;
    }
    screen.stopTyping();

    link.stop();
    int status = direwolf.stop();
    if (!ok) {
        direwolf.showFailure(failure + " (exit " + std::to_string(status) + ")");
        return false;
    }
    return true;
}
