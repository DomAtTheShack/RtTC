// Everything that differs between Windows, Linux and macOS.
#ifndef RTC_BACKEND_H
#define RTC_BACKEND_H

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace os {

// Processes

class Process {
public:
    Process() = default;
    ~Process();
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // captureStdout: read() gets its stdout (and its stderr too, with mergeStderr).
    // pipeStdin: write() feeds its stdin
    bool start(const std::vector<std::string>& argv, bool captureStdout = true,
               bool pipeStdin = false, const std::string& workingDir = "",
               bool mergeStderr = false);
    long read(char* buf, size_t size);   // >0 bytes read, 0 = end of output, -1 = error
    bool write(std::string_view data);   // false if the child has gone or stdin isn't piped
    void closeStdin();
    int  wait();                         // Exit code, or -1
    bool running() const;
    void kill();
    void interrupt();                    // SIGINT; kill() on Windows
    const std::string& error() const { return lastError; }

private:
    void closePipe();

#ifdef _WIN32
    void* processHandle = nullptr;       // HANDLE
    void* pipeRead      = nullptr;       // HANDLE
    void* pipeWrite     = nullptr;       // HANDLE
#else
    int pid     = -1;
    int pipeFd  = -1;
    int stdinFd = -1;
#endif
    int exitCode = -1;
    std::string lastError;
};

// Exit code, or -1 if it couldn't start
int runAndPipe(const std::vector<std::string>& argv,
               const std::function<void(std::string_view)>& onOutput);

// Network

// One thread may read while another writes or shuts it down
class TcpConnection {
public:
    TcpConnection() = default;
    ~TcpConnection();
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    bool connect(const std::string& host, int port);   // host is an IP address
    long read(char* buf, size_t size);   // Blocks. >0 bytes read, 0 = closed, -1 = error
    bool write(std::string_view data);
    void shutdown();                     // A blocked read() then returns 0
    void close();
    const std::string& error() const { return lastError; }

private:
    friend class TcpListener;
    long long sock = -1;                 // SOCKET on Windows
    std::string lastError;
};

// Accepts connections on 127.0.0.1. The test program uses it to stand in for the main program
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    bool listen(int port);
    bool accept(TcpConnection& connection, int timeoutMs);   // false on timeout or error
    void close();
    const std::string& error() const { return lastError; }

private:
    long long sock = -1;                 // SOCKET on Windows
    std::string lastError;
};

// An unused port below 49152 (direwolf's limit), or 0
int freeTcpPort();

// Audio devices

enum class AudioDirection { Capture, Playback };

struct AudioDevice {
    std::string name;   // ALSA name, PulseAudio name (macOS) or WinMM index
    std::string desc;
};

std::vector<AudioDevice> listAudioDevices(AudioDirection direction);

// Plays one second of 16-bit mono audio over and over on a playback device (a name from
// listAudioDevices; macOS and Linux: "" = the default) until keepGoing() is false. Make
// it whole cycles so the joins are seamless. "" when done, else the problem
std::string playTone(const std::string& device, const std::vector<int16_t>& second, int rate,
                     const std::function<bool()>& keepGoing);

// Serial ports

std::vector<std::string> listSerialPorts();
// The serial port of a plugged-in USB device with this vendor and product ID, e.g.
// /dev/ttyACM0 or COM7. "" if there's none
std::string findUsbSerialPort(uint16_t vendorId, uint16_t productId);

// Files

bool fileExists(const std::string& path);
bool fileReadable(const std::string& path);

// Terminal

bool isTerminal(FILE* stream);
// Keys one at a time, no echo
void setRawMode(bool enable);
// false: Ctrl-C arrives as byte 3. setRawMode(false) turns it back on
void setInterruptKey(bool enable);
int readInputByte();                  // -1 when stdin closes
bool waitForInput(int timeoutMs);
void enableAnsiEscapes();
struct TerminalSize { int rows; int cols; };
TerminalSize terminalSize();          // 24x80 if unknown

// Programs

const char* direwolfPath();
const char* atestPath();
const char* genPacketsPath();
std::string executableDir();          // "" if unknown

} // namespace os

#endif //RTC_BACKEND_H
