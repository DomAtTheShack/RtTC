// RFCOMM with Winsock (AF_BTH). Giving connect() the service UUID
// makes Windows look the channel up with SDP. HTCommander's Windows build reaches the same
// services through WinRT; this is the plain Winsock way. Pair the radio in Windows'
// Bluetooth settings first. Untested on a radio.
#include "rfcomm.h"

#include <atomic>
#include <cstdio>
#include <string>

#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>

#include "bt_protocol.h"

namespace bt {

namespace {

bool startWinsock() {
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

std::string winsockError(int code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string out;
    if (text) {
        int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        out.resize(size > 0 ? static_cast<size_t>(size - 1) : 0);
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
        LocalFree(text);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out.empty() ? "Winsock error " + std::to_string(code) : out;
}

GUID toGuid(const uint8_t b[16]) {
    GUID g;
    g.Data1 = static_cast<unsigned long>(b[0]) << 24 | static_cast<unsigned long>(b[1]) << 16 |
              static_cast<unsigned long>(b[2]) << 8 | b[3];
    g.Data2 = static_cast<unsigned short>(b[4] << 8 | b[5]);
    g.Data3 = static_cast<unsigned short>(b[6] << 8 | b[7]);
    for (int i = 0; i < 8; i++) g.Data4[i] = b[8 + i];
    return g;
}

std::string narrow(const wchar_t* text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size > 0 ? static_cast<size_t>(size - 1) : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

} // namespace

struct Rfcomm::Impl {
    std::atomic<SOCKET> sock{INVALID_SOCKET};
};

Rfcomm::Rfcomm() : impl(std::make_unique<Impl>()) {}

Rfcomm::~Rfcomm() {
    SOCKET s = impl->sock.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) closesocket(s);
}

bool Rfcomm::connect(const std::string& address, const char* uuid) {
    uint8_t addr[6], service[16];
    if (!parseAddress(address, addr)) {
        lastError = "\"" + address + "\" isn't a Bluetooth address";
        return false;
    }
    if (!parseUuid(uuid, service)) {
        lastError = std::string("bad UUID ") + uuid;
        return false;
    }
    if (!startWinsock()) {
        lastError = "Winsock didn't start";
        return false;
    }
    SOCKADDR_BTH target{};
    target.addressFamily = AF_BTH;
    for (int i = 0; i < 6; i++) target.btAddr = target.btAddr << 8 | addr[i];
    target.serviceClassId = toGuid(service);
    target.port = 0;   // Look the channel up from serviceClassId

    // The radio sometimes refuses the first try while it wakes up
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (attempt > 1) Sleep(1000);
        SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (s == INVALID_SOCKET) {
            lastError = "no Bluetooth socket (is Bluetooth on?): " + winsockError(WSAGetLastError());
            return false;
        }
        if (::connect(s, reinterpret_cast<sockaddr*>(&target), sizeof target) == 0) {
            impl->sock = s;
            lastError.clear();
            return true;
        }
        lastError = winsockError(WSAGetLastError());
        closesocket(s);
    }
    return false;
}

long Rfcomm::read(uint8_t* buf, size_t size) {
    SOCKET s = impl->sock;
    if (s == INVALID_SOCKET) return 0;
    int n = recv(s, reinterpret_cast<char*>(buf), static_cast<int>(size), 0);
    if (n == SOCKET_ERROR) {
        int code = WSAGetLastError();
        // close() from another thread
        if (code == WSAESHUTDOWN || code == WSAENOTSOCK || code == WSAEINTR) return 0;
        lastError = winsockError(code);
        return -1;
    }
    return n;
}

bool Rfcomm::write(const uint8_t* data, size_t size) {
    SOCKET s = impl->sock;
    while (s != INVALID_SOCKET && size > 0) {
        int n = send(s, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
        if (n == SOCKET_ERROR || n == 0) {
            lastError = winsockError(WSAGetLastError());
            return false;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return s != INVALID_SOCKET;
}

void Rfcomm::close() {
    SOCKET s = impl->sock;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
}

std::vector<Device> pairedDevices() {
    // NS_BTH device lookup without LUP_FLUSHCACHE gives the remembered (paired) devices
    // without an inquiry
    std::vector<Device> devices;
    if (!startWinsock()) return devices;
    WSAQUERYSETW query{};
    query.dwSize = sizeof query;
    query.dwNameSpace = NS_BTH;
    HANDLE lookup = nullptr;
    DWORD flags = LUP_CONTAINERS | LUP_RETURN_NAME | LUP_RETURN_ADDR;
    if (WSALookupServiceBeginW(&query, flags, &lookup) != 0) return devices;
    alignas(WSAQUERYSETW) char buffer[4096];
    while (true) {
        DWORD size = sizeof buffer;
        auto* result = reinterpret_cast<WSAQUERYSETW*>(buffer);
        if (WSALookupServiceNextW(lookup, flags, &size, result) != 0) break;
        if (!result->lpcsaBuffer || !result->lpcsaBuffer->RemoteAddr.lpSockaddr) continue;
        auto* addr = reinterpret_cast<SOCKADDR_BTH*>(result->lpcsaBuffer->RemoteAddr.lpSockaddr);
        char text[18];
        BTH_ADDR a = addr->btAddr;
        std::snprintf(text, sizeof text, "%02X:%02X:%02X:%02X:%02X:%02X",
                      static_cast<unsigned>((a >> 40) & 0xff), static_cast<unsigned>((a >> 32) & 0xff),
                      static_cast<unsigned>((a >> 24) & 0xff), static_cast<unsigned>((a >> 16) & 0xff),
                      static_cast<unsigned>((a >> 8) & 0xff), static_cast<unsigned>(a & 0xff));
        devices.push_back({text, result->lpszServiceInstanceName ? narrow(result->lpszServiceInstanceName)
                                                                 : std::string(text)});
    }
    WSALookupServiceEnd(lookup);
    return devices;
}

} // namespace bt
