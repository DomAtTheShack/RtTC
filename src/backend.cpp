#include "backend.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <random>
#include <utility>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <mmsystem.h>
    #include <io.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <sys/socket.h>
    #ifdef __APPLE__
        #include <mach-o/dyld.h>
        #include <pulse/pulseaudio.h>
        #include <pulse/simple.h>
    #else
        #include <alsa/asoundlib.h>
    #endif
    #include <cerrno>
    #include <csignal>
    #include <fcntl.h>
    #include <glob.h>
    #include <poll.h>
    #include <signal.h>
    #include <sys/ioctl.h>
    #include <sys/wait.h>
    #include <termios.h>
    #include <unistd.h>
#endif

namespace os {

// ---- Shared ------------------------------------------------------------------

bool portAvailable(int port);   // Per OS, below

int freeTcpPort() {
    // direwolf only takes ports up to 49151
    constexpr int low = 20000, high = 49151;
    static std::mt19937 random{std::random_device{}()};
    std::uniform_int_distribution<int> pick(low, high);
    for (int attempt = 0; attempt < 100; attempt++) {
        int port = pick(random);
        if (portAvailable(port)) return port;
    }
    return 0;
}

Process::~Process() {
    closeStdin();
    closePipe();
    if (running()) kill();
    wait();
}

Process::Process(Process&& other) noexcept { *this = std::move(other); }

int runAndPipe(const std::vector<std::string>& argv,
               const std::function<void(std::string_view)>& onOutput) {
    Process process;
    if (!process.start(argv)) {
        std::cerr << process.error() << std::endl;
        return -1;
    }
    char buffer[256];
    long bytesRead;
    while ((bytesRead = process.read(buffer, sizeof buffer)) > 0) {
        onOutput(std::string_view(buffer, static_cast<size_t>(bytesRead)));
    }
    if (bytesRead == -1) std::cerr << process.error() << std::endl;
    return process.wait();
}

#ifdef _WIN32
// ==== Windows =================================================================

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), len);
    return result;
}

std::string narrow(const wchar_t* s) {
    int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string result(len - 1, '\0');   // len counts the null terminator
    WideCharToMultiByte(CP_UTF8, 0, s, -1, result.data(), len, nullptr, nullptr);
    return result;
}

std::string lastErrorMessage(const char* what) {
    DWORD code = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string message = std::string(what) + ": " + (text ? narrow(text) : std::to_string(code));
    if (text) LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();
    return message;
}

// Quotes an argument by the CommandLineToArgvW rules
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            cmd.append(backslashes * 2, L'\\');   // Escape them so the closing quote survives
            break;
        }
        if (*it == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
        } else {
            cmd.append(backslashes, L'\\');
        }
        cmd += *it;
    }
    cmd += L'"';
}

} // namespace

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        std::swap(processHandle, other.processHandle);
        std::swap(pipeRead, other.pipeRead);
        std::swap(pipeWrite, other.pipeWrite);
        std::swap(exitCode, other.exitCode);
        std::swap(lastError, other.lastError);
    }
    return *this;
}

void Process::closePipe() {
    if (pipeRead) {
        CloseHandle(static_cast<HANDLE>(pipeRead));
        pipeRead = nullptr;
    }
}

bool Process::start(const std::vector<std::string>& argv, bool captureStdout, bool pipeStdin,
                    const std::string& workingDir, bool mergeStderr) {
    if (argv.empty()) {
        lastError = "start: no program given";
        return false;
    }
    std::wstring dir = widen(workingDir);
    std::wstring cmdLine;
    for (const std::string& arg : argv) {
        if (!cmdLine.empty()) cmdLine += L' ';
        appendQuoted(cmdLine, widen(arg));
    }

    STARTUPINFOW si{};
    si.cb = sizeof si;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};   // Inheritable
    HANDLE outRead = nullptr, outWrite = nullptr;
    HANDLE inRead  = nullptr, inWrite  = nullptr;
    if (captureStdout) {
        if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
            lastError = lastErrorMessage("CreatePipe");
            return false;
        }
        // Only the write end goes to the child
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    }
    if (pipeStdin) {
        if (!CreatePipe(&inRead, &inWrite, &sa, 0)) {
            lastError = lastErrorMessage("CreatePipe");
            if (outRead) { CloseHandle(outRead); CloseHandle(outWrite); }
            return false;
        }
        // Only the read end goes to the child
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    }
    if (captureStdout || pipeStdin) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = pipeStdin     ? inRead   : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = captureStdout ? outWrite : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = captureStdout && mergeStderr ? outWrite : GetStdHandle(STD_ERROR_HANDLE);
    }

    PROCESS_INFORMATION pi{};
    // No application name, so Windows searches PATH
    BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0,
                             nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    if (!ok) lastError = lastErrorMessage(argv[0].c_str());
    // Close our copies of the child's ends so EOF works both ways
    if (outWrite) CloseHandle(outWrite);
    if (inRead)   CloseHandle(inRead);
    if (!ok) {
        if (outRead) CloseHandle(outRead);
        if (inWrite) CloseHandle(inWrite);
        return false;
    }
    CloseHandle(pi.hThread);
    processHandle = pi.hProcess;
    pipeRead      = outRead;
    pipeWrite     = inWrite;
    exitCode      = -1;
    return true;
}

bool Process::write(std::string_view data) {
    if (!pipeWrite) return false;
    while (!data.empty()) {
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(pipeWrite), data.data(), static_cast<DWORD>(data.size()),
                       &written, nullptr)) {
            lastError = lastErrorMessage("WriteFile");
            return false;
        }
        data.remove_prefix(written);
    }
    return true;
}

void Process::closeStdin() {
    if (pipeWrite) {
        CloseHandle(static_cast<HANDLE>(pipeWrite));
        pipeWrite = nullptr;
    }
}

long Process::read(char* buf, size_t size) {
    if (!pipeRead) return 0;
    DWORD bytesRead = 0;
    if (!ReadFile(static_cast<HANDLE>(pipeRead), buf, static_cast<DWORD>(size), &bytesRead, nullptr)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) return 0;   // The child closed its stdout
        lastError = lastErrorMessage("ReadFile");
        return -1;
    }
    return static_cast<long>(bytesRead);
}

int Process::wait() {
    if (!processHandle) return exitCode;
    HANDLE h = static_cast<HANDLE>(processHandle);
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    exitCode = GetExitCodeProcess(h, &code) ? static_cast<int>(code) : -1;
    CloseHandle(h);
    processHandle = nullptr;
    return exitCode;
}

bool Process::running() const {
    return processHandle && WaitForSingleObject(static_cast<HANDLE>(processHandle), 0) == WAIT_TIMEOUT;
}

void Process::kill() {
    if (processHandle) TerminateProcess(static_cast<HANDLE>(processHandle), 1);
}

void Process::interrupt() {
    // No usable Ctrl-C for a child on Windows
    kill();
}

// ---- Network (Winsock) ---------------------------------------------------------

namespace {

bool winsockReady() {
    static const bool ready = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

std::string socketError(const char* what) {
    return std::string(what) + ": Winsock error " + std::to_string(WSAGetLastError());
}

} // namespace

TcpConnection::~TcpConnection() { close(); }

bool TcpConnection::connect(const std::string& host, int port) {
    close();
    if (!winsockReady()) {
        lastError = "Winsock could not start";
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        lastError = socketError("socket");
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR) {
        lastError = socketError(("connect to " + host + ":" + std::to_string(port)).c_str());
        closesocket(s);
        return false;
    }
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);
    sock = static_cast<long long>(s);
    return true;
}

long TcpConnection::read(char* buf, size_t size) {
    if (sock == -1) return 0;
    int n = recv(static_cast<SOCKET>(sock), buf, static_cast<int>(size), 0);
    if (n == SOCKET_ERROR) {
        lastError = socketError("recv");
        return -1;
    }
    return n;
}

bool TcpConnection::write(std::string_view data) {
    if (sock == -1) return false;
    while (!data.empty()) {
        int n = send(static_cast<SOCKET>(sock), data.data(), static_cast<int>(data.size()), 0);
        if (n == SOCKET_ERROR) {
            lastError = socketError("send");
            return false;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

void TcpConnection::shutdown() {
    if (sock != -1) ::shutdown(static_cast<SOCKET>(sock), SD_BOTH);
}

void TcpConnection::close() {
    if (sock != -1) {
        closesocket(static_cast<SOCKET>(sock));
        sock = -1;
    }
}

TcpListener::~TcpListener() { close(); }

bool TcpListener::listen(int port) {
    close();
    if (!winsockReady()) {
        lastError = "Winsock could not start";
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        lastError = socketError("socket");
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR ||
        ::listen(s, 1) == SOCKET_ERROR) {
        lastError = socketError(("listen on port " + std::to_string(port)).c_str());
        closesocket(s);
        return false;
    }
    sock = static_cast<long long>(s);
    return true;
}

bool TcpListener::accept(TcpConnection& connection, int timeoutMs) {
    if (sock == -1) return false;
    fd_set waiting;
    FD_ZERO(&waiting);
    FD_SET(static_cast<SOCKET>(sock), &waiting);
    timeval wait{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int ready = select(0, &waiting, nullptr, nullptr, &wait);
    if (ready <= 0) {
        lastError = ready == 0 ? "nothing connected in time" : socketError("select");
        return false;
    }
    SOCKET s = ::accept(static_cast<SOCKET>(sock), nullptr, nullptr);
    if (s == INVALID_SOCKET) {
        lastError = socketError("accept");
        return false;
    }
    connection.close();
    connection.sock = static_cast<long long>(s);
    return true;
}

void TcpListener::close() {
    if (sock != -1) {
        closesocket(static_cast<SOCKET>(sock));
        sock = -1;
    }
}

bool portAvailable(int port) {
    if (!winsockReady()) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    // Without this Windows lets bind succeed on a port already in use
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // How direwolf listens
    addr.sin_port = htons(static_cast<u_short>(port));
    bool ok = bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    closesocket(s);
    return ok;
}

std::vector<AudioDevice> listAudioDevices(AudioDirection direction) {
    std::vector<AudioDevice> devices;
    if (direction == AudioDirection::Capture) {
        UINT count = waveInGetNumDevs();
        for (UINT i = 0; i < count; i++) {
            WAVEINCAPSW caps{};
            if (waveInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR)
                devices.push_back({std::to_string(i), narrow(caps.szPname)});
        }
    } else {
        UINT count = waveOutGetNumDevs();
        for (UINT i = 0; i < count; i++) {
            WAVEOUTCAPSW caps{};
            if (waveOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR)
                devices.push_back({std::to_string(i), narrow(caps.szPname)});
        }
    }
    return devices;
}

std::string playTone(const std::string& device, const std::vector<int16_t>& second, int rate,
                     const std::function<bool()>& keepGoing) {
    if (device.empty() || device.find_first_not_of("0123456789") != std::string::npos)
        return "\"" + device + "\" isn't a Windows sound card number";
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = static_cast<DWORD>(rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = static_cast<DWORD>(rate) * 2;
    HWAVEOUT out = nullptr;
    if (waveOutOpen(&out, static_cast<UINT>(std::stoul(device)), &format, 0, 0, CALLBACK_NULL) !=
        MMSYSERR_NOERROR)
        return "can't open sound card " + device;
    // One buffer, looped by the driver until reset
    std::vector<int16_t> samples = second;
    WAVEHDR header{};
    header.lpData = reinterpret_cast<LPSTR>(samples.data());
    header.dwBufferLength = static_cast<DWORD>(samples.size() * sizeof(int16_t));
    header.dwFlags = WHDR_BEGINLOOP | WHDR_ENDLOOP;
    header.dwLoops = 0xFFFFFFFF;
    waveOutPrepareHeader(out, &header, sizeof header);
    std::string error;
    if (waveOutWrite(out, &header, sizeof header) != MMSYSERR_NOERROR) error = "can't play on sound card " + device;
    while (error.empty() && keepGoing()) Sleep(50);
    waveOutReset(out);
    waveOutUnprepareHeader(out, &header, sizeof header);
    waveOutClose(out);
    return error;
}

bool fileExists(const std::string& path) {
    return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool fileReadable(const std::string& path) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool isTerminal(FILE* stream) {
    return _isatty(_fileno(stream)) != 0;
}

void setRawMode(bool enable) {
    static DWORD oldMode;
    static bool active = false;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (!isTerminal(stdin)) return;
    if (enable && !active) {
        GetConsoleMode(in, &oldMode);
        DWORD newMode = oldMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        // Arrow keys as escape sequences, like Linux
        newMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        SetConsoleMode(in, newMode);
        active = true;
    } else if (!enable && active) {
        SetConsoleMode(in, oldMode);
        active = false;
    }
}

void setInterruptKey(bool enable) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (!isTerminal(stdin) || !GetConsoleMode(in, &mode)) return;
    SetConsoleMode(in, enable ? mode | ENABLE_PROCESSED_INPUT : mode & ~ENABLE_PROCESSED_INPUT);
}

int readInputByte() {
    unsigned char c;
    DWORD bytesRead = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), &c, 1, &bytesRead, nullptr) || bytesRead == 0)
        return -1;
    return c;
}

bool waitForInput(int timeoutMs) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (!isTerminal(stdin)) return WaitForSingleObject(in, timeoutMs) == WAIT_OBJECT_0;
    // Skip console events that aren't key presses (releases, focus, mouse)
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        INPUT_RECORD record;
        DWORD count = 0;
        while (PeekConsoleInputW(in, &record, 1, &count) && count == 1) {
            if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown &&
                record.Event.KeyEvent.uChar.UnicodeChar != 0) return true;
            ReadConsoleInputW(in, &record, 1, &count);
        }
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        WaitForSingleObject(in, static_cast<DWORD>(deadline - now));
    }
}

std::vector<std::string> listSerialPorts() {
    std::vector<std::string> ports;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ,
                      &key) != ERROR_SUCCESS) return ports;
    for (DWORD i = 0;; i++) {
        wchar_t name[256], data[64];
        DWORD nameLen = std::size(name), dataSize = sizeof data, type;
        LONG status = RegEnumValueW(key, i, name, &nameLen, nullptr, &type,
                                    reinterpret_cast<BYTE*>(data), &dataSize);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || type != REG_SZ) continue;
        data[std::size(data) - 1] = L'\0';
        ports.push_back(narrow(data));
    }
    RegCloseKey(key);
    // COM2 before COM10
    std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return ports;
}

void enableAnsiEscapes() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode))
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
}

namespace {

bool isFile(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

namespace {

// Our folder first (the build puts a Hamlib-enabled direwolf there), then PATH, then
// the usual install folders
std::string findDirewolfTool(const std::string& name) {
    std::wstring file = widen(name + ".exe");
    std::string exeDir = executableDir();
    if (!exeDir.empty() && isFile(widen(exeDir) + L"\\" + file)) return exeDir + "\\" + name + ".exe";
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, file.c_str(), nullptr, MAX_PATH, found, nullptr) > 0) return narrow(found);
    for (const wchar_t* dir : {L"C:\\direwolf", L"C:\\Program Files\\direwolf",
                               L"C:\\Program Files (x86)\\direwolf"}) {
        std::wstring candidate = std::wstring(dir) + L"\\" + file;
        if (isFile(candidate)) return narrow(candidate.c_str());
    }
    return name + ".exe";
}

} // namespace

const char* direwolfPath() {
    static const std::string path = findDirewolfTool("direwolf");
    return path.c_str();
}

const char* atestPath() {
    static const std::string path = findDirewolfTool("atest");
    return path.c_str();
}

const char* genPacketsPath() {
    static const std::string path = findDirewolfTool("gen_packets");
    return path.c_str();
}

TerminalSize terminalSize() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return {24, 80};
    return {info.srWindow.Bottom - info.srWindow.Top + 1, info.srWindow.Right - info.srWindow.Left + 1};
}

std::string executableDir() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return "";
    std::string full = narrow(path);
    size_t slash = full.find_last_of("\\/");
    return slash == std::string::npos ? "" : full.substr(0, slash);
}

#else
// ==== Linux and macOS =========================================================

namespace {

std::string errnoMessage(const std::string& what, int err) {
    return what + ": " + std::strerror(err);
}

#ifndef __APPLE__

// Joins a multi-line ALSA description with " - "
std::string formatDescription(const char* desc) {
    std::string result;
    std::string line;
    auto addLine = [&] {
        size_t start = line.find_first_not_of(" \t");
        size_t end   = line.find_last_not_of(" \t");
        if (start != std::string::npos) {
            if (!result.empty()) result += " - ";
            result += line.substr(start, end - start + 1);
        }
        line.clear();
    };
    for (const char* c = desc; *c != '\0'; ++c) {
        if (*c == '\n') addLine();
        else line += *c;
    }
    addLine();
    return result;
}

// Many hinted devices don't really open (e.g. under WSL), so try each one
bool canOpen(const char* name, snd_pcm_stream_t stream) {
    // Mute stderr: failed opens print errors
    fflush(stderr);
    int savedStderr = dup(STDERR_FILENO);
    int devNull = open("/dev/null", O_WRONLY);
    dup2(devNull, STDERR_FILENO);
    close(devNull);

    snd_pcm_t* pcm = nullptr;
    bool ok = snd_pcm_open(&pcm, name, stream, SND_PCM_NONBLOCK) == 0;
    if (ok) snd_pcm_close(pcm);

    fflush(stderr);
    dup2(savedStderr, STDERR_FILENO);
    close(savedStderr);
    return ok;
}
#endif

} // namespace

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        std::swap(pid, other.pid);
        std::swap(pipeFd, other.pipeFd);
        std::swap(stdinFd, other.stdinFd);
        std::swap(exitCode, other.exitCode);
        std::swap(lastError, other.lastError);
    }
    return *this;
}

void Process::closePipe() {
    if (pipeFd != -1) {
        close(pipeFd);
        pipeFd = -1;
    }
}

bool Process::start(const std::vector<std::string>& argv, bool captureStdout, bool pipeStdin,
                    const std::string& workingDir, bool mergeStderr) {
    if (argv.empty()) {
        lastError = "start: no program given";
        return false;
    }
    std::vector<std::string> argStrings = argv;
    std::vector<char*> args;
    for (std::string& s : argStrings) args.push_back(s.data());
    args.push_back(nullptr);

    int outPipe[2] = {-1, -1};
    int inPipe[2]  = {-1, -1};
    auto closePipes = [&] {
        for (int fd : {outPipe[0], outPipe[1], inPipe[0], inPipe[1]})
            if (fd != -1) close(fd);
    };
    if (captureStdout && pipe(outPipe) == -1) {
        lastError = errnoMessage("pipe", errno);
        return false;
    }
    if (pipeStdin) {
        if (pipe(inPipe) == -1) {
            lastError = errnoMessage("pipe", errno);
            closePipes();
            return false;
        }
        fcntl(inPipe[1], F_SETFD, FD_CLOEXEC);
        signal(SIGPIPE, SIG_IGN);   // write() fails with EPIPE instead
    }
    // The child writes errno here if exec fails; it closes on a successful exec
    int errPipe[2];
    if (pipe(errPipe) == -1) {
        lastError = errnoMessage("pipe", errno);
        closePipes();
        return false;
    }
    fcntl(errPipe[1], F_SETFD, FD_CLOEXEC);

    pid_t child = fork();
    if (child == -1) {
        lastError = errnoMessage("fork", errno);
        closePipes();
        close(errPipe[0]);
        close(errPipe[1]);
        return false;
    }

    if (child == 0) {
        close(errPipe[0]);
        if (captureStdout) {
            close(outPipe[0]);
            dup2(outPipe[1], STDOUT_FILENO);
            if (mergeStderr) dup2(outPipe[1], STDERR_FILENO);
            close(outPipe[1]);
        }
        if (pipeStdin) {
            dup2(inPipe[0], STDIN_FILENO);
            close(inPipe[0]);
        }
        if (workingDir.empty() || chdir(workingDir.c_str()) == 0) execv(args[0], args.data());
        int err = errno;
        ssize_t ignored = ::write(errPipe[1], &err, sizeof err);
        (void)ignored;
        _exit(127);
    }

    close(errPipe[1]);
    if (captureStdout) close(outPipe[1]);
    if (pipeStdin)     close(inPipe[0]);

    int childErr = 0;
    ssize_t n;
    do { n = ::read(errPipe[0], &childErr, sizeof childErr); } while (n == -1 && errno == EINTR);
    close(errPipe[0]);
    if (n == sizeof childErr) {
        lastError = errnoMessage(argv[0], childErr);
        if (captureStdout) close(outPipe[0]);
        if (pipeStdin)     close(inPipe[1]);
        waitpid(child, nullptr, 0);
        return false;
    }

    pid      = child;
    pipeFd   = captureStdout ? outPipe[0] : -1;
    stdinFd  = pipeStdin ? inPipe[1] : -1;
    exitCode = -1;
    return true;
}

bool Process::write(std::string_view data) {
    if (stdinFd == -1) return false;
    while (!data.empty()) {
        ssize_t n = ::write(stdinFd, data.data(), data.size());
        if (n == -1) {
            if (errno == EINTR) continue;
            lastError = errnoMessage("write", errno);
            return false;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

void Process::closeStdin() {
    if (stdinFd != -1) {
        close(stdinFd);
        stdinFd = -1;
    }
}

long Process::read(char* buf, size_t size) {
    if (pipeFd == -1) return 0;
    ssize_t n;
    do { n = ::read(pipeFd, buf, size); } while (n == -1 && errno == EINTR);
    if (n == -1) lastError = errnoMessage("read", errno);
    return static_cast<long>(n);
}

int Process::wait() {
    if (pid == -1) return exitCode;
    int status = 0;
    pid_t r;
    do { r = waitpid(pid, &status, 0); } while (r == -1 && errno == EINTR);
    if (r == -1) {
        lastError = errnoMessage("waitpid", errno);
        exitCode = -1;
    } else if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    } else {
        exitCode = 128 + WTERMSIG(status);
    }
    pid = -1;
    return exitCode;
}

bool Process::running() const {
    if (pid == -1) return false;
    siginfo_t info{};
    // WNOWAIT leaves the exit status for wait()
    if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == -1)
        return false;
    return info.si_pid == 0;
}

void Process::kill() {
    if (pid != -1) ::kill(pid, SIGTERM);
}

void Process::interrupt() {
    if (pid != -1) ::kill(pid, SIGINT);
}

// ---- Network -------------------------------------------------------------------

TcpConnection::~TcpConnection() { close(); }

bool TcpConnection::connect(const std::string& host, int port) {
    close();
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1) {
        lastError = errnoMessage("socket", errno);
        return false;
    }
    fcntl(s, F_SETFD, FD_CLOEXEC);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    int r;
    do { r = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr); } while (r == -1 && errno == EINTR);
    if (r == -1) {
        lastError = errnoMessage("connect to " + host + ":" + std::to_string(port), errno);
        ::close(s);
        return false;
    }
    int noDelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof noDelay);
#ifdef SO_NOSIGPIPE
    int noSigpipe = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof noSigpipe);
#endif
    sock = s;
    return true;
}

long TcpConnection::read(char* buf, size_t size) {
    if (sock == -1) return 0;
    ssize_t n;
    do { n = recv(static_cast<int>(sock), buf, size, 0); } while (n == -1 && errno == EINTR);
    if (n == -1) lastError = errnoMessage("recv", errno);
    return static_cast<long>(n);
}

bool TcpConnection::write(std::string_view data) {
    if (sock == -1) return false;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    while (!data.empty()) {
        ssize_t n = send(static_cast<int>(sock), data.data(), data.size(), flags);
        if (n == -1) {
            if (errno == EINTR) continue;
            lastError = errnoMessage("send", errno);
            return false;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

void TcpConnection::shutdown() {
    if (sock != -1) ::shutdown(static_cast<int>(sock), SHUT_RDWR);
}

void TcpConnection::close() {
    if (sock != -1) {
        ::close(static_cast<int>(sock));
        sock = -1;
    }
}

TcpListener::~TcpListener() { close(); }

bool TcpListener::listen(int port) {
    close();
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1) {
        lastError = errnoMessage("socket", errno);
        return false;
    }
    fcntl(s, F_SETFD, FD_CLOEXEC);
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == -1 || ::listen(s, 1) == -1) {
        lastError = errnoMessage("listen on port " + std::to_string(port), errno);
        ::close(s);
        return false;
    }
    sock = s;
    return true;
}

bool TcpListener::accept(TcpConnection& connection, int timeoutMs) {
    if (sock == -1) return false;
    pollfd waiting{static_cast<int>(sock), POLLIN, 0};
    int ready;
    do { ready = poll(&waiting, 1, timeoutMs); } while (ready == -1 && errno == EINTR);
    if (ready <= 0) {
        lastError = ready == 0 ? "nothing connected in time" : errnoMessage("poll", errno);
        return false;
    }
    int s = ::accept(static_cast<int>(sock), nullptr, nullptr);
    if (s == -1) {
        lastError = errnoMessage("accept", errno);
        return false;
    }
    fcntl(s, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int noSigpipe = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof noSigpipe);
#endif
    connection.close();
    connection.sock = s;
    return true;
}

void TcpListener::close() {
    if (sock != -1) {
        ::close(static_cast<int>(sock));
        sock = -1;
    }
}

bool portAvailable(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // How direwolf listens
    addr.sin_port = htons(static_cast<uint16_t>(port));
    bool ok = bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    ::close(s);
    return ok;
}

#ifdef __APPLE__
namespace {

void addSource(pa_context*, const pa_source_info* info, int eol, void* userdata) {
    // Skip monitors of outputs
    if (eol || info->monitor_of_sink != PA_INVALID_INDEX) return;
    static_cast<std::vector<AudioDevice>*>(userdata)->push_back(
        {info->name, info->description ? info->description : info->name});
}

void addSink(pa_context*, const pa_sink_info* info, int eol, void* userdata) {
    if (eol) return;
    static_cast<std::vector<AudioDevice>*>(userdata)->push_back(
        {info->name, info->description ? info->description : info->name});
}

} // namespace

// PulseAudio devices; names are what PULSE_SOURCE/PULSE_SINK take
std::vector<AudioDevice> listAudioDevices(AudioDirection direction) {
    std::vector<AudioDevice> devices;
    pa_mainloop* loop = pa_mainloop_new();
    pa_context* context = pa_context_new(pa_mainloop_get_api(loop), "Radio to Text Connector");

    bool ready = context && pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) >= 0;
    while (ready && pa_context_get_state(context) != PA_CONTEXT_READY) {
        if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(context)) ||
            pa_mainloop_iterate(loop, 1, nullptr) < 0) ready = false;
    }

    if (ready) {
        pa_operation* op = direction == AudioDirection::Capture
                               ? pa_context_get_source_info_list(context, addSource, &devices)
                               : pa_context_get_sink_info_list(context, addSink, &devices);
        while (op && pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            if (pa_mainloop_iterate(loop, 1, nullptr) < 0) break;
        }
        if (op) pa_operation_unref(op);
    } else {
        std::cerr << "Error: Cannot reach PulseAudio. Start it with: brew services start pulseaudio"
                  << std::endl;
    }

    if (context) {
        pa_context_disconnect(context);
        pa_context_unref(context);
    }
    pa_mainloop_free(loop);
    return devices;
}

std::string playTone(const std::string& device, const std::vector<int16_t>& second, int rate,
                     const std::function<bool()>& keepGoing) {
    pa_sample_spec spec{PA_SAMPLE_S16LE, static_cast<uint32_t>(rate), 1};
    int err = 0;
    pa_simple* out = pa_simple_new(nullptr, "Radio to Text Connector", PA_STREAM_PLAYBACK,
                                   device.empty() ? nullptr : device.c_str(), "Tuning tone", &spec,
                                   nullptr, nullptr, &err);
    if (!out) return std::string("can't play on ") + (device.empty() ? "the default sound card" : device) +
                     ": " + pa_strerror(err);
    // A tenth of a second at a time, so stopping is quick
    size_t chunk = static_cast<size_t>(rate) / 10, pos = 0;
    std::string error;
    while (keepGoing()) {
        size_t n = std::min(chunk, second.size() - pos);
        if (pa_simple_write(out, second.data() + pos, n * sizeof(int16_t), &err) < 0) {
            error = pa_strerror(err);
            break;
        }
        pos = (pos + n) % second.size();
    }
    pa_simple_flush(out, &err);
    pa_simple_free(out);
    return error;
}
#else
std::vector<AudioDevice> listAudioDevices(AudioDirection direction) {
    std::vector<AudioDevice> devices;
    snd_pcm_stream_t stream = direction == AudioDirection::Capture
                                  ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
    void** hints;

    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        std::cerr << "Error: Failed to fetch ALSA device hints." << std::endl;
        return devices;
    }

    for (void** n = hints; *n != nullptr; ++n) {
        char* name = snd_device_name_get_hint(*n, "NAME");
        char* desc = snd_device_name_get_hint(*n, "DESC");
        char* ioid = snd_device_name_get_hint(*n, "IOID");

        const char* wrongWay = stream == SND_PCM_STREAM_CAPTURE ? "Output" : "Input";
        bool rightDirection = (ioid == nullptr || strcmp(ioid, wrongWay) != 0);
        if (name != nullptr && rightDirection && strcmp(name, "null") != 0 && canOpen(name, stream)) {
            std::string description = desc ? formatDescription(desc) : "";
            devices.push_back({name, description.empty() ? name : description});
        }

        if (name) free(name);
        if (desc) free(desc);
        if (ioid) free(ioid);
    }

    snd_device_name_free_hint(hints);
    return devices;
}

std::string playTone(const std::string& device, const std::vector<int16_t>& second, int rate,
                     const std::function<bool()>& keepGoing) {
    // A bare card name goes through plug, which converts the rate
    std::string name = device.empty() ? "default" : device;
    if (name.find(':') == std::string::npos && name != "default") name = "plug:" + name;
    snd_pcm_t* pcm = nullptr;
    if (int err = snd_pcm_open(&pcm, name.c_str(), SND_PCM_STREAM_PLAYBACK, 0); err < 0)
        return "can't open " + name + ": " + snd_strerror(err);
    std::string error;
    if (int err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1,
                                     static_cast<unsigned>(rate), 1, 100000); err < 0)
        error = std::string("can't set up ") + name + ": " + snd_strerror(err);
    // A tenth of a second at a time, so stopping is quick
    size_t chunk = static_cast<size_t>(rate) / 10, pos = 0;
    while (error.empty() && keepGoing()) {
        size_t n = std::min(chunk, second.size() - pos);
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, second.data() + pos, n);
        if (written < 0) written = snd_pcm_recover(pcm, static_cast<int>(written), 1);
        if (written < 0) {
            error = snd_strerror(static_cast<int>(written));
            break;
        }
        pos = (pos + static_cast<size_t>(written)) % second.size();
    }
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    return error;
}
#endif

bool fileExists(const std::string& path)   { return access(path.c_str(), F_OK) == 0; }
bool fileReadable(const std::string& path) { return access(path.c_str(), R_OK) == 0; }

bool isTerminal(FILE* stream) {
    return isatty(fileno(stream)) != 0;
}

void setRawMode(bool enable) {
    static termios oldt;
    static bool active = false;
    if (!isatty(STDIN_FILENO)) return;
    if (enable && !active) {
        tcgetattr(STDIN_FILENO, &oldt);
        termios newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        active = true;
    } else if (!enable && active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        active = false;
    }
}

void setInterruptKey(bool enable) {
    termios t;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &t) == -1) return;
    if (enable) t.c_lflag |= ISIG;   // ISIG is what turns Ctrl-C into SIGINT
    else        t.c_lflag &= ~ISIG;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

int readInputByte() {
    unsigned char c;
    ssize_t n;
    do { n = ::read(STDIN_FILENO, &c, 1); } while (n == -1 && errno == EINTR);
    return n == 1 ? c : -1;
}

bool waitForInput(int timeoutMs) {
    pollfd fd{STDIN_FILENO, POLLIN, 0};
    int n;
    do { n = poll(&fd, 1, timeoutMs); } while (n == -1 && errno == EINTR);
    return n > 0;
}

std::vector<std::string> listSerialPorts() {
    std::vector<std::string> ports;
#ifdef __APPLE__
    // cu.* rather than tty.*, which wait for carrier detect
    const char* patterns[] = {"/dev/cu.usbserial*", "/dev/cu.usbmodem*",
                              "/dev/cu.SLAB_USBtoUART*", "/dev/cu.wchusbserial*"};
#else
    // USB only; ttyS* always exist
    const char* patterns[] = {"/dev/ttyUSB*", "/dev/ttyACM*"};
#endif
    for (const char* pattern : patterns) {
        glob_t matches;
        if (glob(pattern, 0, nullptr, &matches) == 0) {
            for (size_t i = 0; i < matches.gl_pathc; i++) ports.emplace_back(matches.gl_pathv[i]);
        }
        globfree(&matches);
    }
    return ports;
}

void enableAnsiEscapes() {}

namespace {

// PATH, /usr/local/bin, /opt/homebrew/bin, then our folder. "" if not found
std::string findProgram(const std::string& name) {
    std::vector<std::string> dirs;
    if (const char* path = getenv("PATH")) {
        std::string list = path;
        size_t start = 0, end;
        while ((end = list.find(':', start)) != std::string::npos) {
            dirs.push_back(list.substr(start, end - start));
            start = end + 1;
        }
        dirs.push_back(list.substr(start));
    }
    dirs.insert(dirs.end(), {"/usr/local/bin", "/opt/homebrew/bin", executableDir()});
    for (const std::string& dir : dirs) {
        if (dir.empty()) continue;
        std::string candidate = dir + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return "";
}

std::string programPath(const std::string& name) {
    std::string found = findProgram(name);
    return found.empty() ? "/usr/local/bin/" + name : found;
}

} // namespace

const char* direwolfPath() {
    static const std::string path = programPath("direwolf");
    return path.c_str();
}

const char* atestPath() {
    static const std::string path = programPath("atest");
    return path.c_str();
}

const char* genPacketsPath() {
    static const std::string path = programPath("gen_packets");
    return path.c_str();
}

TerminalSize terminalSize() {
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1 || size.ws_row == 0) return {24, 80};
    return {size.ws_row, size.ws_col};
}

std::string executableDir() {
    char path[4096];
#ifdef __APPLE__
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0) return "";
    char resolved[4096];
    std::string full = realpath(path, resolved) ? resolved : path;
#else
    ssize_t len = readlink("/proc/self/exe", path, sizeof path - 1);
    if (len <= 0) return "";
    std::string full(path, static_cast<size_t>(len));
#endif
    size_t slash = full.rfind('/');
    return slash == std::string::npos ? "" : full.substr(0, slash);
}

#endif

} // namespace os
