// Headless: connects one radio (direwolf, AX.25 and FX.25) to the main program over
// JSON/TCP. See docs/connector-protocol.md.
//
// One thread runs everything below; the main program's socket, direwolf's output and its
// AGW port each have a reader thread that only puts events on the queue. AGW carries the
// connected-mode sessions (docs/connected-mode.md).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "../backend.h"
#include "../Radio/aioc.h"
#include "../Radio/cat.h"
#include "agw.h"
#include "connector.h"
#include "connector_config.h"
#include "../Radio/modem.h"
#include "packet.h"
#include "sessions.h"
#include "../settings.h"
#ifdef RTC_BLUETOOTH
#include "../Bluetooth/bt_audio.h"
#endif

namespace fs = std::filesystem;
using connector::Json;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using namespace std::chrono_literals;

namespace {

const char* const programVersion = "0.1.2";
constexpr size_t maxLineBytes = 64 * 1024;
constexpr auto reconnectDelay = 5s;
constexpr auto direwolfStartLimit = 15s;
constexpr auto radioRetryDelay = 10s;

std::atomic<bool> quitRequested{false};
bool verbose = false;        // rx/tx frames and JSON lines
bool showDirewolf = false;   // direwolf's own output
bool quitOnDisconnect = false;   // RadioTests: stop (and stop direwolf) when it hangs up
std::string settingsFile;   // connector.ini, or --settings

void onSignal(int) { quitRequested = true; }

void logLine(const char* format, ...) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[16];
    std::strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    std::fprintf(stderr, "%s ", stamp);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// Events

struct Event {
    enum class Type {
        MainConnected, MainLine, MainTooLong, MainClosed, MainFailed,
        DirewolfOutput, DirewolfEnded, AgwFrame, AgwClosed,
    } type;
    int generation = 0;     // Radio events: which direwolf run they came from
    std::string text;
    agw::Frame agwFrame;
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

// The main program

// Dials out, and dials again 5 s after a failure or a drop
class MainLink {
public:
    explicit MainLink(EventQueue& events) : events(events) {}
    ~MainLink() { stop(); }

    void start(const std::string& host, int port) {
        stop();
        stopping = false;
        thread = std::thread([this, host, port] { run(host, port); });
    }

    void stop() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            if (connected) connection.shutdown();
        }
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }

    bool send(const Json& message) {
        std::string line = message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
        std::lock_guard lock(mutex);
        if (!connected) return false;
        if (verbose) logLine("to main: %s", line.substr(0, line.size() - 1).c_str());
        if (!connection.write(line)) {
            connection.shutdown();   // The reader sees the drop and reconnects
            return false;
        }
        return true;
    }

    bool isConnected() {
        std::lock_guard lock(mutex);
        return connected;
    }

private:
    void run(const std::string& host, int port) {
        bool reported = false;
        for (;;) {
            {
                std::lock_guard lock(mutex);
                if (stopping) return;
            }
            if (connection.connect(host, port)) {
                {
                    std::lock_guard lock(mutex);
                    if (stopping) {
                        connection.close();
                        return;
                    }
                    connected = true;
                }
                reported = false;
                events.push({Event::Type::MainConnected});
                readLines();
                {
                    std::lock_guard lock(mutex);
                    connected = false;
                    connection.close();
                }
                events.push({Event::Type::MainClosed});
            } else if (!reported) {
                events.push({Event::Type::MainFailed, 0, connection.error()});
                reported = true;   // Once, not every 5 s
            }
            std::unique_lock lock(mutex);
            if (wake.wait_for(lock, reconnectDelay, [this] { return stopping; })) return;
        }
    }

    void readLines() {
        std::string pending;
        bool discarding = false;   // Inside a line that was too long
        char buffer[4096];
        long n;
        while ((n = connection.read(buffer, sizeof buffer)) > 0) {
            for (long i = 0; i < n; i++) {
                char c = buffer[i];
                if (c == '\n') {
                    if (!discarding) {
                        if (!pending.empty() && pending.back() == '\r') pending.pop_back();
                        if (!pending.empty()) events.push({Event::Type::MainLine, 0, std::move(pending)});
                    }
                    pending.clear();
                    discarding = false;
                } else if (!discarding) {
                    pending += c;
                    if (pending.size() > maxLineBytes) {
                        pending.clear();
                        discarding = true;
                        events.push({Event::Type::MainTooLong});
                    }
                }
            }
        }
    }

    EventQueue& events;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    bool connected = false;
    os::TcpConnection connection;
    std::thread thread;
};

// The radio (direwolf)

class Radio {
public:
    enum class State { Unconfigured, Starting, Ready, Restarting, Error };

    explicit Radio(EventQueue& events) : events(events) {}
    ~Radio() { stop(); }

    // Starts direwolf in the background; tick() connects to it
    void start(const Settings& newSettings, bool restarting) {
        stop();
        settings = newSettings;
        generation++;
        log.clear();
        retryAt.reset();
        if (std::string missing = connector::missingConfig(settings); !missing.empty()) {
            setState(State::Unconfigured, "missing " + missing);
            return;
        }
        // An AIOC cable's port and sound card can change each time it's plugged in
        if (settings.ptt == PttType::Aioc) {
            if (std::string problem = aioc::useCable(settings, aioc::find()); !problem.empty())
                return fail(problem);
            logLine("AIOC cable: PTT on %s, sound card %s", settings.pttPort.c_str(),
                    settings.rxAudio.c_str());
        }
        // Bluetooth audio first: direwolf then uses its sound card
        std::string rxDevice = settings.rxAudio, txDevice = settings.txAudio;
        if (settings.usesBluetooth()) {
#if defined(RTC_BLUETOOTH) && !defined(__APPLE__)
            logLine("connecting to the radio over Bluetooth (%s)", settings.bluetooth.c_str());
            bluetooth = std::make_unique<bt::BluetoothAudio>();
            if (!bluetooth->start(settings.bluetooth, settings.btPlayAudio, settings.btRecordAudio)) {
                std::string problem = "Bluetooth: " + bluetooth->error();
                bluetooth.reset();
                return fail(problem);
            }
            if (!bluetooth->modemCaptureDevice().empty()) rxDevice = bluetooth->modemCaptureDevice();
            if (!bluetooth->modemPlaybackDevice().empty()) txDevice = bluetooth->modemPlaybackDevice();
            logLine("Bluetooth audio connected");
#elif defined(__APPLE__)
            // Homebrew's direwolf uses the Mac's default sound card, which the bridge can't be
            return fail("Bluetooth audio needs Linux or Windows: direwolf on macOS only uses the "
                        "default sound card");
#else
            return fail("this build has no Bluetooth audio (bluetooth= is set)");
#endif
        }
        agwPort = os::freeTcpPort();
        if (agwPort == 0) return fail("no free TCP port for direwolf's AGW port");
        // direwolf truncates -c paths at 99 characters, so it runs from the config's folder
        configName = "radio-connector-" + std::to_string(agwPort) + ".conf";
        configPath = fs::temp_directory_path() / configName;
        {
            std::ofstream out(configPath, std::ios::trunc);
            bool cable = settings.ptt == PttType::Aioc;
            out << direwolfConfig(settings, direwolfDevice(rxDevice, false, cable),
                                  direwolfDevice(txDevice, true, cable), 0,
                                  agwPort, connector::direwolfPaclen(settings));
            if (!out) return fail("couldn't write " + configPath.string());
        }
        process = std::make_unique<os::Process>();
        if (!process->start(direwolfArgs(configName), true, false, configPath.parent_path().string())) {
            std::string why = process->error();
            process.reset();
            return fail("couldn't start direwolf (" + why + "). Looked for it at " +
                        std::string(os::direwolfPath()));
        }
        direwolfRunning = true;
        partialLine.clear();
        int gen = generation;
        outputReader = std::thread([this, gen] {
            char buffer[512];
            long n;
            while ((n = process->read(buffer, sizeof buffer)) > 0)
                events.push({Event::Type::DirewolfOutput, gen, std::string(buffer, static_cast<size_t>(n))});
            events.push({Event::Type::DirewolfEnded, gen});
        });
        startDeadline = Clock::now() + direwolfStartLimit;
        nextConnectTry = Clock::now() + 250ms;
        setState(restarting ? State::Restarting : State::Starting, "");
        logLine("direwolf starting: %s, %d baud, FX.25 %s, AGW port %d", settings.callsign.c_str(),
                settings.packetBaud, fx25Name(settings.fx25).c_str(), agwPort);
    }

    // Stops direwolf (SIGINT first so it unkeys the radio) and makes sure PTT is off
    void stop() {
        if (agwReader.joinable()) {
            agw.shutdown();
            agwReader.join();
        }
        agw.close();
        if (process) {
            if (process->running()) {
                process->interrupt();
                auto deadline = Clock::now() + 3s;
                while (process->running() && Clock::now() < deadline) std::this_thread::sleep_for(50ms);
                if (process->running()) process->kill();
            }
            if (outputReader.joinable()) outputReader.join();
            process->wait();
            process.reset();
        }
        direwolfRunning = false;
#ifdef RTC_BLUETOOTH
        // After direwolf, so its last audio goes out; stopping unkeys the radio
        bluetooth.reset();
#endif
        if (!configPath.empty()) {
            std::error_code ec;
            fs::remove(configPath, ec);
            configPath.clear();
        }
        if (transmitted && settings.keysRadio()) {
            try {
                if (auto radio = openPtt(settings)) radio->setPtt(false);
            } catch (const std::exception& e) {
                logLine("WARNING: couldn't unkey the radio (%s). Check it isn't transmitting!", e.what());
            }
        }
        transmitted = false;
        if (state == State::Ready || state == State::Starting || state == State::Restarting)
            state = State::Error;   // Until start() is called again
    }

    void tick(TimePoint now) {
#ifdef RTC_BLUETOOTH
        if (bluetooth && !bluetooth->connected() && state != State::Error) {
            stop();
            return fail("lost the Bluetooth connection to the radio");
        }
#endif
        if ((state == State::Starting || state == State::Restarting) && now >= nextConnectTry) {
            nextConnectTry = now + 250ms;
            int gen = generation;
            if (agw.connect("127.0.0.1", agwPort)) {
                agwReader = std::thread([this, gen] { readAgw(gen); });
                setState(State::Ready, "");
                logLine("radio ready");
            } else if (now > startDeadline) {
                stop();
                fail("direwolf didn't start within 15 s. With CAT PTT, check the radio is on and "
                     "the PTT settings are right");
            }
        }
        if (state == State::Error && retryAt && now >= *retryAt) {
            retryAt.reset();
            logLine("starting direwolf again");
            start(settings, true);
        }
    }

    void onOutput(const Event& event) {
        if (event.generation != generation) return;
        log += event.text;
        if (log.size() > 4000) log.erase(0, log.size() - 4000);
        if (!showDirewolf) return;
        // Whole lines with a prefix, so they don't run into our own log lines. direwolf
        // buffers its output, so these can show up a few seconds late
        partialLine += event.text;
        size_t newline;
        while ((newline = partialLine.find('\n')) != std::string::npos) {
            std::string line = partialLine.substr(0, newline);
            partialLine.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) logLine("direwolf: %s", line.c_str());
        }
    }

    // direwolf exited or closed its AGW port when it shouldn't have
    void onStopped(const Event& event) {
        if (event.generation != generation || state == State::Unconfigured || state == State::Error) return;
        stop();
        fail(event.type == Event::Type::DirewolfEnded ? "direwolf stopped unexpectedly"
                                                      : "direwolf closed the AGW connection");
    }

    bool agwSend(const agw::Frame& frame) {
        if (state != State::Ready) return false;
        if (verbose) logLine("agw out %c %s>%s: %s", frame.kind, frame.from.c_str(), frame.to.c_str(),
                             packet::printable(frame.data).c_str());
        if (!agw.write(agw::encode(frame))) {
            logLine("couldn't send to direwolf's AGW port: %s", agw.error().c_str());
            return false;
        }
        if (frame.kind == 'D' || frame.kind == 'C' || frame.kind == 'v') transmitted = true;
        return true;
    }

    int currentGeneration() const { return generation; }

    State current() const { return state; }
    bool ready() const { return state == State::Ready; }
    const std::string& problem() const { return why; }
    const char* direwolfState() const {
        if (!direwolfRunning) return "stopped";
        return state == State::Ready ? "running" : "starting";
    }

    static const char* name(State state) {
        switch (state) {
            case State::Unconfigured: return "unconfigured";
            case State::Starting:     return "starting";
            case State::Ready:        return "ready";
            case State::Restarting:   return "restarting";
            case State::Error:        return "error";
        }
        return "?";
    }

private:
    void readAgw(int gen) {
        agw::Decoder decoder;
        char buffer[2048];
        long n;
        while ((n = agw.read(buffer, sizeof buffer)) > 0) {
            for (agw::Frame& frame : decoder.feed(std::string_view(buffer, static_cast<size_t>(n)))) {
                Event event{Event::Type::AgwFrame, gen};
                event.agwFrame = std::move(frame);
                events.push(std::move(event));
            }
            if (decoder.broken()) break;
        }
        events.push({Event::Type::AgwClosed, gen});
    }

    void fail(const std::string& problem) {
        setState(State::Error, problem);
        logLine("radio error: %s", problem.c_str());
        if (!log.empty()) logLine("direwolf's last output:\n%s", log.c_str());
        retryAt = Clock::now() + radioRetryDelay;
    }

    void setState(State newState, const std::string& problem) {
        state = newState;
        why = problem;
        if (newState == State::Unconfigured) logLine("radio not configured: %s", problem.c_str());
    }

    EventQueue& events;
    Settings settings;
    State state = State::Unconfigured;
    std::string why;
    int generation = 0;
    int agwPort = 0;
    std::string configName;
    fs::path configPath;
    std::unique_ptr<os::Process> process;
    std::thread outputReader;
    bool direwolfRunning = false;
    os::TcpConnection agw;
    std::thread agwReader;
    TimePoint startDeadline{}, nextConnectTry{};
    std::optional<TimePoint> retryAt;
    std::string log;
    std::string partialLine;   // direwolf output after the last newline, for --direwolf
    bool transmitted = false;
#ifdef RTC_BLUETOOTH
    std::unique_ptr<bt::BluetoothAudio> bluetooth;
#endif
};

// The program

class Connector {
public:
    explicit Connector(Settings initial)
        : settings(std::move(initial)), main(events), radio(events),
          sessions({[this](const agw::Frame& frame) { return radio.agwSend(frame); },
                    [this](const Json& message) { main.send(message); },
                    [](const std::string& line) { logLine("%s", line.c_str()); }}) {
        sessions.setOptions(connector::sessionOptions(settings), Clock::now());
    }

    int run() {
        std::string host;
        int port = 0;
        connector::parseHostPort(settings.mainAddress, host, port);
        logLine("Radio Connector %s, main program at %s:%d", programVersion, host.c_str(), port);
        logLine("settings: %s (callsign %s)", settingsFile.c_str(),
                settings.callsign.empty() ? "not set" : settings.callsign.c_str());
        radio.start(settings, false);
        main.start(host, port);

        while (!quitRequested) {
            auto event = events.popFor(100ms);
            TimePoint now = Clock::now();
            if (event) handle(*event, now);
            radio.tick(now);
            sessions.setLinkUp(radio.ready(), now);
            sessions.tick(now);
            Json status = radioStatus();
            if (status != lastStatus && main.isConnected()) {
                main.send(connector::envelope("radio.status", status));
                lastStatus = status;
            }
        }
        logLine("stopping");
        main.stop();
        radio.stop();
        return 0;
    }

private:
    void handle(const Event& event, TimePoint now) {
        switch (event.type) {
            case Event::Type::MainConnected:
                logLine("connected to the main program");
                sendHello();
                sessions.setMainConnected(true, now);
                lastStatus = nullptr;   // Full status next
                break;
            case Event::Type::MainClosed:
                sessions.setMainConnected(false, now);
                if (quitOnDisconnect) {
                    logLine("main program disconnected; quitting");
                    quitRequested = true;
                    break;
                }
                logLine("main program disconnected; trying again every 5 s");
                break;
            case Event::Type::MainFailed:
                logLine("can't reach the main program (%s); trying every 5 s", event.text.c_str());
                break;
            case Event::Type::MainTooLong:
                main.send(connector::errorEnvelope(nullptr, "too_long", "lines must be at most 64 KiB"));
                break;
            case Event::Type::MainLine:
                handleRequest(event.text, now);
                break;
            case Event::Type::DirewolfOutput:
                radio.onOutput(event);
                break;
            case Event::Type::DirewolfEnded:
            case Event::Type::AgwClosed:
                radio.onStopped(event);
                break;
            case Event::Type::AgwFrame: {
                if (event.generation != radio.currentGeneration()) break;   // From a direwolf since stopped
                const agw::Frame& f = event.agwFrame;
                if (verbose && f.kind != 'Y') {
                    logLine("agw in %c %s>%s: %s", f.kind, f.from.c_str(), f.to.c_str(),
                            packet::printable(f.data).c_str());
                }
                // A connect or text means another station was heard
                if (f.kind == 'C' || f.kind == 'D') lastHeard = std::chrono::system_clock::now();
                sessions.onAgw(f, now);
                break;
            }
        }
    }

    void handleRequest(const std::string& line, TimePoint now) {
        if (verbose) logLine("from main: %s", line.c_str());
        Json request = Json::parse(line, nullptr, false);
        if (request.is_discarded() || !request.is_object())
            return reply(connector::errorEnvelope(nullptr, "bad_json", "each line must be one JSON object"));
        Json id = request.contains("id") ? request["id"] : Json(nullptr);
        if (!request.contains("v") || request["v"] != connector::protocolVersion)
            return reply(connector::errorEnvelope(id, "bad_version", "this connector speaks version 1"));
        if (!request.contains("type") || !request["type"].is_string())
            return reply(connector::errorEnvelope(id, "bad_envelope", "type must be a string"));
        Json body = request.contains("body") ? request["body"] : Json::object();
        if (!body.is_object())
            return reply(connector::errorEnvelope(id, "bad_envelope", "body must be an object", "body"));
        std::string type = request["type"];

        if (type == "message.send") {
            if (!radio.ready()) {
                std::string why = radio.problem().empty() ? "" : ": " + radio.problem();
                return reply(connector::errorEnvelope(id, "radio_not_ready",
                                                      std::string("radio is ") + Radio::name(radio.current()) + why));
            }
            sessions.send(body, id, now);
        } else if (type == "session.open") {
            sessions.open(body, id, now);
        } else if (type == "session.close") {
            sessions.close(body, id, now);
        } else if (type == "session.list") {
            sessions.list(id);
        } else if (type == "config.get") {
            reply(connector::envelope("config", {{"config", connector::configToJson(settings)},
                                                 {"restartRequired", false}}, nullptr, id));
        } else if (type == "config.set") {
            if (!body.contains("config"))
                return reply(connector::errorEnvelope(id, "bad_field", "body needs config", "config"));
            Settings changed = settings;
            connector::ConfigChange change = connector::applyConfig(changed, body["config"]);
            if (!change.ok) return reply(connector::errorEnvelope(id, "bad_field", change.message, change.field));
            settings = changed;
            if (!saveSettingsTo(settings, settingsFile)) logLine("WARNING: couldn't save %s", settingsFile.c_str());
            sessions.setOptions(connector::sessionOptions(settings), now);
            reply(connector::envelope("config", {{"config", connector::configToJson(settings)},
                                                 {"restartRequired", change.restartRequired}}, nullptr, id));
            if (change.restartRequired) {
                logLine("config changed; restarting direwolf");
                radio.start(settings, true);   // Open sessions close (radio_down)
            }
            if (change.mainChanged) {
                std::string host;
                int port = 0;
                connector::parseHostPort(settings.mainAddress, host, port);
                logLine("main program moved to %s:%d", host.c_str(), port);
                main.start(host, port);
            }
        } else if (type == "audio.list") {
            reply(connector::envelope("audio.devices", {
                {"capture", audioDevices(os::AudioDirection::Capture)},
                {"playback", audioDevices(os::AudioDirection::Playback)},
            }, nullptr, id));
        } else if (type == "status.get") {
            reply(connector::envelope("radio.status", radioStatus(), nullptr, id));
        } else if (type == "radio.restart") {
            logLine("restart requested");
            radio.start(settings, true);
            reply(connector::envelope("radio.status", radioStatus(), nullptr, id));
        } else {
            reply(connector::errorEnvelope(id, "unknown_type", "no such request: " + type, "type"));
        }
    }

    void reply(const Json& message) { main.send(message); }

    // name is what rxAudio/txAudio take (a WinMM index on Windows); desc is for people
    static Json audioDevices(os::AudioDirection direction) {
        Json list = Json::array();
        for (const os::AudioDevice& device : os::listAudioDevices(direction))
            list.push_back({{"name", device.name}, {"desc", device.desc}});
        return list;
    }

    void sendHello() {
        main.send(connector::envelope("hello", {
            {"program", "radio-connector"},
            {"version", programVersion},
            {"instance", settings.callsign},
            {"capabilities", {"ax25", "fx25", "connected"}},
            {"config", connector::configToJson(settings)},
            {"radio", Radio::name(radio.current())},
        }, "c-hello"));
    }

    Json radioStatus() const {
        return {
            {"state", Radio::name(radio.current())},
            {"direwolf", radio.direwolfState()},
            {"ptt", pttKey(settings.ptt)},
            {"sessions", sessions.count()},
            {"lastHeard", lastHeard ? Json(connector::isoUtc(*lastHeard)) : Json(nullptr)},
            {"error", radio.problem().empty() ? Json(nullptr) : Json(radio.problem())},
        };
    }

    Settings settings;
    EventQueue events;
    MainLink main;
    Radio radio;
    connector::Sessions sessions;
    Json lastStatus;
    std::optional<std::chrono::system_clock::time_point> lastHeard;
};

void usage(const char* program) {
    std::fprintf(stderr,
                 "Usage: %s [--settings file] [--main host:port] [--verbose] [--direwolf]\n"
                 "  --settings file   settings to use instead of %s\n"
                 "  --main host:port  where the main program listens (saved for next time)\n"
                 "  --verbose         log every session frame and every JSON line\n"
                 "  --direwolf        also show direwolf's own output\n"
                 "  --quit-on-disconnect  exit when the main program hangs up (for test programs)\n",
                 program, connectorSettingsPath().c_str());
}

// The first time, the connector's file starts as a copy of RadioConnectorCli's
// settings (audio, PTT, baud), with its own callsign (the next SSID), so it's a
// different station
Settings loadConnectorSettings() {
    bool found = false;
    Settings settings = loadSettingsFrom(settingsFile, &found);
    if (found) return settings;
    settings = loadSettings();
    std::string shared = settings.callsign;
    settings.callsign = packet::otherSsid(shared);
    if (!saveSettingsTo(settings, settingsFile)) {
        std::fprintf(stderr, "Couldn't create %s\n", settingsFile.c_str());
        return settings;
    }
    std::fprintf(stderr, "Made %s from %s", settingsFile.c_str(), settingsPath().c_str());
    if (!shared.empty())
        std::fprintf(stderr, ", with callsign %s (RadioConnectorCli is %s)",
                     settings.callsign.c_str(), packet::normalizeCallsign(shared).c_str());
    std::fprintf(stderr, ". Edit it to change the connector's settings.\n");
    return settings;
}

} // namespace

int main(int argc, char** argv) {
    settingsFile = connectorSettingsPath();
    std::string mainAddress;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--main" && i + 1 < argc) {
            mainAddress = argv[++i];
            std::string host;
            int port = 0;
            if (!connector::parseHostPort(mainAddress, host, port)) {
                std::fprintf(stderr, "--main needs an IP address and port, e.g. 127.0.0.1:9000\n");
                return 2;
            }
        } else if (arg == "--settings" && i + 1 < argc) {
            settingsFile = argv[++i];
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--direwolf") {
            showDirewolf = true;
        } else if (arg == "--quit-on-disconnect") {
            quitOnDisconnect = true;
        } else if (arg == "--version") {
            std::printf("radio-connector %s\n", programVersion);
            return 0;
        } else {
            usage(argv[0]);
            return arg == "--help" || arg == "-h" ? 0 : 2;
        }
    }
    Settings settings = loadConnectorSettings();
    if (!mainAddress.empty() && settings.mainAddress != mainAddress) {
        settings.mainAddress = mainAddress;
        if (!saveSettingsTo(settings, settingsFile))
            std::fprintf(stderr, "Couldn't save %s\n", settingsFile.c_str());
    }
    std::string host;
    int port = 0;
    if (!connector::parseHostPort(settings.mainAddress, host, port)) {
        std::fprintf(stderr, "No main program address yet: run with --main host:port\n");
        usage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, onSignal);
#endif
    return Connector(settings).run();
}
