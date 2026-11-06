// Keys, program output and received packets all arrive on one event queue.
// Packet sessions keep one direwolf running.
#include "session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "ansi.hpp"
#include "backend.h"
#include "Connector/packet.h"

namespace {

const ansi::Color statusColor   = ansi::Color::BrightYellow;
const ansi::Color sentColor     = ansi::Color::BrightCyan;
const ansi::Color receivedColor = ansi::Color::BrightGreen;
const ansi::Color errorColor    = ansi::Color::BrightRed;

enum class Key { Char, Enter, Backspace, Tab, Esc };

struct Event {
    enum class Type { Key, Output, OutputEnded, Packet, LinkClosed } type;
    Key key = Key::Char;
    char ch = 0;          // For Key::Char
    std::string text;     // For Output; the information field for Packet
    std::string from;     // For Packet: who sent it
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
// channel wait and TXDELAY; tail covers TXTAIL and sound card buffering
constexpr double keyUpSeconds = 1.0;
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
                    // Show who it's for unless it's to everyone with no path, e.g. an ack to
                    // this station. Then the digipeaters as direwolf shows them: a "*" on
                    // ones that have repeated it, e.g. N0CALL-7>APRS,WIDE1-1*,WIDE2-1
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

    void add(char ch) {
        line += ch;
        ansi::print(sentColor, "%c", ch);
        ansi::flush();
    }

    void backspace() {
        if (line.empty()) return;
        line.pop_back();
        std::fputs("\b \b", stdout);
        ansi::flush();
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
    void clear() {
        if (typing) ansi::clearLine();
    }

    void redraw() {
        if (typing) ansi::print(sentColor, "> %s", line.c_str());
        ansi::flush();
    }

    bool typing;
    std::string line;
};

} // namespace

bool runPacketSession(const PacketSession& p, bool& transmitted) {
    transmitted = false;
    EventQueue events;
    KeyReader keys(events);

    ansi::print(statusColor, "\nStarting direwolf...\n");
    ansi::flush();
    Modem direwolf(events);
    if (!direwolf.start(p.direwolfArgs, false, p.workingDir)) return false;

    // direwolf's output, shown only if it fails
    std::string log;
    bool direwolfEnded = false;
    auto handleOutput = [&](const Event& event) {
        if (event.type == Event::Type::Output) {
            log += event.text;
            if (log.size() > 6000) log.erase(0, log.size() - 6000);
        } else if (event.type == Event::Type::OutputEnded) {
            direwolfEnded = true;
        }
    };
    // SIGINT first so direwolf unkeys the radio, then kill
    auto stopDirewolf = [&] {
        if (!direwolfEnded) direwolf.process.interrupt();
        auto deadline = Clock::now() + 3s;
        while (!direwolfEnded) {
            if (auto event = events.popFor(100ms)) handleOutput(*event);
            if (!direwolfEnded && Clock::now() > deadline) {
                direwolf.process.kill();
                deadline = Clock::time_point::max();
            }
        }
        return direwolf.wait();
    };
    auto showFailure = [&](const std::string& why) {
        ansi::print(errorColor, "\n%s\n", why.c_str());
        if (!log.empty()) {
            ansi::print(errorColor, "direwolf's last output:\n");
            ansi::print(ansi::Color::Gray, "%s\n", log.c_str());
        }
    };

    KissLink link(events);
    auto giveUp = Clock::now() + 15s;
    while (!link.connect(p.kissPort)) {
        if (auto event = events.popFor(250ms)) {
            handleOutput(*event);
            if (direwolfEnded) {
                int status = direwolf.wait();
                showFailure("direwolf stopped while starting (exit " + std::to_string(status) + ")");
                return false;
            }
            if (event->type == Event::Type::Key && event->key == Key::Esc) {
                stopDirewolf();
                return true;
            }
        }
        if (Clock::now() > giveUp) {
            stopDirewolf();
            showFailure("direwolf didn't start within 15 s. With CAT PTT, check the radio is on "
                        "and Settings > PTT is right.");
            return false;
        }
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
                Clock::time_point begins = busyUntil > now ? busyUntil : now + seconds(keyUpSeconds);
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
        handleOutput(*event);
        if (direwolfEnded) {
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
                // Only Transmit finishes on Tab
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
    int status = stopDirewolf();
    if (!ok) {
        showFailure(failure + " (exit " + std::to_string(status) + ")");
        return false;
    }
    return true;
}
