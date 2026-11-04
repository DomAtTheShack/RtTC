// rfcomm_linux.cpp - RFCOMM with BlueZ: SDP finds the channel for the UUID, then a plain
// RFCOMM socket connects to it (HTCommander's Linux plugin does the same). The radio
// must be paired first, e.g. with bluetoothctl or the desktop's Bluetooth settings.
#include "rfcomm.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../backend.h"
#include "bt_protocol.h"

namespace bt {

namespace {

// The RFCOMM channel offering the service, or -1
int findChannel(const bdaddr_t& target, const char* uuidText, std::string& error) {
    uint8_t bytes[16];
    if (!parseUuid(uuidText, bytes)) {
        error = std::string("bad UUID ") + uuidText;
        return -1;
    }
    uuid_t uuid;
    sdp_uuid128_create(&uuid, bytes);

    bdaddr_t any{};   // BDADDR_ANY, which is a macro C++ dislikes
    sdp_session_t* session = sdp_connect(&any, &target, SDP_RETRY_IF_BUSY);
    if (!session) {
        error = std::string("can't reach the radio for its service list: ") + std::strerror(errno);
        return -1;
    }
    sdp_list_t* search = sdp_list_append(nullptr, &uuid);
    uint32_t range = 0x0000ffff;
    sdp_list_t* attributes = sdp_list_append(nullptr, &range);
    sdp_list_t* responses = nullptr;
    int channel = -1;
    if (sdp_service_search_attr_req(session, search, SDP_ATTR_REQ_RANGE, attributes, &responses) == 0) {
        for (sdp_list_t* r = responses; r && channel < 0; r = r->next) {
            auto* record = static_cast<sdp_record_t*>(r->data);
            sdp_list_t* protocols = nullptr;
            if (sdp_get_access_protos(record, &protocols) == 0) {
                int port = sdp_get_proto_port(protocols, RFCOMM_UUID);
                if (port > 0) channel = port;
                for (sdp_list_t* p = protocols; p; p = p->next)
                    sdp_list_free(static_cast<sdp_list_t*>(p->data), nullptr);
                sdp_list_free(protocols, nullptr);
            }
        }
    }
    for (sdp_list_t* r = responses; r; r = r->next) sdp_record_free(static_cast<sdp_record_t*>(r->data));
    sdp_list_free(responses, nullptr);
    sdp_list_free(search, nullptr);
    sdp_list_free(attributes, nullptr);
    sdp_close(session);
    if (channel < 0) error = std::string("the radio doesn't offer service ") + uuidText;
    return channel;
}

} // namespace

struct Rfcomm::Impl {
    std::atomic<int> fd{-1};
};

Rfcomm::Rfcomm() : impl(std::make_unique<Impl>()) {}

Rfcomm::~Rfcomm() {
    close();
    int fd = impl->fd.exchange(-1);
    if (fd >= 0) ::close(fd);
}

bool Rfcomm::connect(const std::string& address, const char* uuid) {
    uint8_t bytes[6];
    if (!parseAddress(address, bytes)) {
        lastError = "\"" + address + "\" isn't a Bluetooth address";
        return false;
    }
    bdaddr_t target;
    for (int i = 0; i < 6; i++) target.b[i] = bytes[5 - i];   // bdaddr_t is least significant first

    // The radio sometimes refuses the first try while it wakes up
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (attempt > 1) std::this_thread::sleep_for(std::chrono::seconds(1));
        int channel = findChannel(target, uuid, lastError);
        if (channel < 0) continue;
        int fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
        if (fd < 0) {
            lastError = std::string("no Bluetooth socket: ") + std::strerror(errno);
            return false;
        }
        sockaddr_rc addr{};
        addr.rc_family = AF_BLUETOOTH;
        addr.rc_bdaddr = target;
        addr.rc_channel = static_cast<uint8_t>(channel);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
            impl->fd = fd;
            lastError.clear();
            return true;
        }
        lastError = "RFCOMM channel " + std::to_string(channel) + ": " + std::strerror(errno);
        ::close(fd);
    }
    return false;
}

long Rfcomm::read(uint8_t* buf, size_t size) {
    int fd = impl->fd;
    if (fd < 0) return 0;
    ssize_t n;
    do { n = ::recv(fd, buf, size, 0); } while (n < 0 && errno == EINTR);
    if (n < 0) lastError = std::strerror(errno);
    return n;
}

bool Rfcomm::write(const uint8_t* data, size_t size) {
    int fd = impl->fd;
    while (fd >= 0 && size > 0) {
        ssize_t n = ::send(fd, data, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            lastError = std::strerror(errno);
            return false;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return fd >= 0;
}

void Rfcomm::close() {
    int fd = impl->fd;
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}

std::vector<Device> pairedDevices() {
    // bluetoothctl prints "Device AA:BB:CC:DD:EE:FF Name". Older versions spell the
    // command paired-devices
    std::vector<Device> devices;
    auto collect = [&](const std::vector<std::string>& argv) {
        std::string output;
        os::runAndPipe(argv, [&](std::string_view text) { output += text; });
        size_t start = 0;
        while (start < output.size()) {
            size_t end = output.find('\n', start);
            if (end == std::string::npos) end = output.size();
            std::string line = output.substr(start, end - start);
            start = end + 1;
            size_t at = line.find("Device ");
            if (at == std::string::npos || line.size() < at + 24) continue;
            std::string address = normalizeAddress(line.substr(at + 7, 17));
            if (address.empty()) continue;
            std::string name = line.size() > at + 25 ? line.substr(at + 25) : address;
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            devices.push_back({address, name});
        }
    };
    collect({"/usr/bin/env", "bluetoothctl", "devices", "Paired"});
    if (devices.empty()) collect({"/usr/bin/env", "bluetoothctl", "paired-devices"});

    // Radios first: their service list has the audio UUID
    std::vector<Device> radios, others;
    for (const Device& device : devices) {
        std::string info;
        os::runAndPipe({"/usr/bin/env", "bluetoothctl", "info", device.address},
                       [&](std::string_view text) { info += text; });
        (info.find("39144315") != std::string::npos ? radios : others).push_back(device);
    }
    radios.insert(radios.end(), others.begin(), others.end());
    return radios;
}

} // namespace bt
