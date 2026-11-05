// Bluetooth Classic RFCOMM connections, found by service UUID (SDP).
//   Linux    BlueZ sockets (libbluetooth for SDP), as HTCommander's Linux build does
//   Windows  Winsock AF_BTH: Windows does the SDP lookup from the UUID
//   macOS    IOBluetooth (rfcomm_macos.mm)
#ifndef RTC_RFCOMM_H
#define RTC_RFCOMM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bt {

struct Device {
    std::string address;   // AA:BB:CC:DD:EE:FF
    std::string name;
};

// Paired devices, radios with the audio service first where the OS says which have it
std::vector<Device> pairedDevices();

// One thread may read while another writes or closes it
class Rfcomm {
public:
    Rfcomm();
    ~Rfcomm();
    Rfcomm(const Rfcomm&) = delete;
    Rfcomm& operator=(const Rfcomm&) = delete;

    // address: AA:BB:CC:DD:EE:FF. uuid: the service's 128-bit UUID. Blocks for a few seconds
    bool connect(const std::string& address, const char* uuid);
    long read(uint8_t* buf, size_t size);   // Blocks. >0 bytes, 0 = closed, -1 = error
    bool write(const uint8_t* data, size_t size);
    void close();                           // A blocked read() returns 0
    const std::string& error() const { return lastError; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
    std::string lastError;
};

} // namespace bt

#endif //RTC_RFCOMM_H
