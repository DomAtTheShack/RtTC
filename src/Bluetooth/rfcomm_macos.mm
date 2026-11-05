// RFCOMM with IOBluetooth, after HTCommander's macOS build
// (AppDelegate.swift). IOBluetooth calls back on the run loop of the thread that opened
// the channel, so each connection runs its own run loop thread. Pair the radio in System
// Settings first. Untested on a radio.
#include "rfcomm.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

#include "bt_protocol.h"

namespace bt {
struct RfcommShared {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<uint8_t> received;
    bool closed = false;
};
} // namespace bt

@interface RtcRfcommDelegate : NSObject <IOBluetoothRFCOMMChannelDelegate> {
@public
    bt::RfcommShared* shared;
}
@end

@implementation RtcRfcommDelegate
- (void)rfcommChannelData:(IOBluetoothRFCOMMChannel*)channel data:(void*)data length:(size_t)length {
    (void)channel;
    std::lock_guard<std::mutex> lock(shared->mutex);
    auto* bytes = static_cast<const uint8_t*>(data);
    shared->received.insert(shared->received.end(), bytes, bytes + length);
    shared->changed.notify_all();
}

- (void)rfcommChannelClosed:(IOBluetoothRFCOMMChannel*)channel {
    (void)channel;
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->closed = true;
    shared->changed.notify_all();
}
@end

namespace bt {

namespace {

IOBluetoothSDPUUID* sdpUuid(const char* text) {
    uint8_t bytes[16];
    if (!parseUuid(text, bytes)) return nil;
    return [IOBluetoothSDPUUID uuidWithBytes:bytes length:16];
}

// Runs the run loop for up to seconds, or until done() is true
template <typename Done>
void runLoopUntil(double seconds, Done done) {
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (!done() && [until timeIntervalSinceNow] > 0) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
    }
}

} // namespace

struct Rfcomm::Impl {
    RfcommShared shared;
    std::thread thread;
    CFRunLoopRef loop = nullptr;
    IOBluetoothRFCOMMChannel* channel = nil;
    RtcRfcommDelegate* delegate = nil;
    bool connected = false;
    bool connectDone = false;
    std::string error;

    // Runs block on the connection's thread and waits for it
    void perform(void (^block)(void)) {
        if (!loop) return;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        CFRunLoopPerformBlock(loop, kCFRunLoopDefaultMode, ^{
            block();
            dispatch_semaphore_signal(done);
        });
        CFRunLoopWakeUp(loop);
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    }
};

Rfcomm::Rfcomm() : impl(std::make_unique<Impl>()) {}

Rfcomm::~Rfcomm() {
    close();
    if (impl->thread.joinable()) impl->thread.join();
}

bool Rfcomm::connect(const std::string& address, const char* uuid) {
    uint8_t check[6];
    if (!parseAddress(address, check)) {
        lastError = "\"" + address + "\" isn't a Bluetooth address";
        return false;
    }
    Impl* p = impl.get();
    std::string addressCopy = address;
    std::string uuidCopy = uuid;
    p->thread = std::thread([p, addressCopy, uuidCopy] {
        @autoreleasepool {
            p->loop = CFRunLoopGetCurrent();
            std::string error;
            IOBluetoothDevice* device =
                [IOBluetoothDevice deviceWithAddressString:[NSString stringWithUTF8String:addressCopy.c_str()]];
            IOBluetoothSDPUUID* service = sdpUuid(uuidCopy.c_str());
            BluetoothRFCOMMChannelID channelId = 0;
            if (!device || !service) {
                error = "no such Bluetooth device";
            } else {
                // Service records may be cached already; otherwise ask the radio
                IOBluetoothSDPServiceRecord* record = [device getServiceRecordForUUID:service];
                if (!record) {
                    [device performSDPQuery:nil];
                    runLoopUntil(10.0, [&] { return [device getServiceRecordForUUID:service] != nil; });
                    record = [device getServiceRecordForUUID:service];
                }
                if (!record || [record getRFCOMMChannelID:&channelId] != kIOReturnSuccess)
                    error = "the radio doesn't offer service " + uuidCopy;
            }
            if (error.empty()) {
                p->delegate = [[RtcRfcommDelegate alloc] init];
                p->delegate->shared = &p->shared;
                IOBluetoothRFCOMMChannel* channel = nil;
                IOReturn result = kIOReturnError;
                // The radio sometimes refuses the first try while it wakes up
                for (int attempt = 1; attempt <= 3 && result != kIOReturnSuccess; attempt++) {
                    if (attempt > 1) runLoopUntil(1.0, [] { return false; });
                    result = [device openRFCOMMChannelSync:&channel withChannelID:channelId delegate:p->delegate];
                }
                if (result == kIOReturnSuccess && channel) {
                    p->channel = channel;
                } else {
                    error = "RFCOMM channel " + std::to_string(channelId) + " didn't open (IOReturn " +
                            std::to_string(result) + ")";
                }
            }
            {
                std::lock_guard<std::mutex> lock(p->shared.mutex);
                p->error = error;
                p->connected = error.empty();
                p->connectDone = true;
                if (!p->connected) p->shared.closed = true;
                p->shared.changed.notify_all();
            }
            if (p->connected) {
                // Callbacks arrive here until close() stops the loop
                while (true) {
                    {
                        std::lock_guard<std::mutex> lock(p->shared.mutex);
                        if (p->shared.closed) break;
                    }
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, false);
                }
                [p->channel closeChannel];
                p->channel = nil;
            }
            p->loop = nullptr;
        }
    });
    std::unique_lock<std::mutex> lock(p->shared.mutex);
    p->shared.changed.wait(lock, [p] { return p->connectDone; });
    lastError = p->error;
    return p->connected;
}

long Rfcomm::read(uint8_t* buf, size_t size) {
    std::unique_lock<std::mutex> lock(impl->shared.mutex);
    impl->shared.changed.wait(lock, [this] { return !impl->shared.received.empty() || impl->shared.closed; });
    if (impl->shared.received.empty()) return 0;
    size_t n = std::min(size, impl->shared.received.size());
    std::copy(impl->shared.received.begin(), impl->shared.received.begin() + static_cast<long>(n), buf);
    impl->shared.received.erase(impl->shared.received.begin(), impl->shared.received.begin() + static_cast<long>(n));
    return static_cast<long>(n);
}

bool Rfcomm::write(const uint8_t* data, size_t size) {
    {
        std::lock_guard<std::mutex> lock(impl->shared.mutex);
        if (!impl->connected || impl->shared.closed) return false;
    }
    __block bool ok = true;
    Impl* p = impl.get();
    p->perform(^{
        if (!p->channel) {
            ok = false;
            return;
        }
        // writeSync takes at most the channel's MTU at a time
        size_t mtu = [p->channel getMTU];
        if (mtu == 0) mtu = 127;
        size_t pos = 0;
        while (ok && pos < size) {
            size_t n = std::min(mtu, size - pos);
            ok = [p->channel writeSync:const_cast<uint8_t*>(data + pos) length:static_cast<UInt16>(n)] ==
                 kIOReturnSuccess;
            pos += n;
        }
    });
    if (!ok) lastError = "Bluetooth write failed";
    return ok;
}

void Rfcomm::close() {
    std::lock_guard<std::mutex> lock(impl->shared.mutex);
    impl->shared.closed = true;
    impl->shared.changed.notify_all();
}

std::vector<Device> pairedDevices() {
    std::vector<Device> radios, others;
    @autoreleasepool {
        IOBluetoothSDPUUID* audio = sdpUuid(audioUuid);
        for (IOBluetoothDevice* device in [IOBluetoothDevice pairedDevices]) {
            std::string address = normalizeAddress([[device addressString] UTF8String]);
            if (address.empty()) continue;
            NSString* name = [device name];
            Device entry{address, name ? std::string([name UTF8String]) : address};
            ([device getServiceRecordForUUID:audio] ? radios : others).push_back(entry);
        }
    }
    radios.insert(radios.end(), others.begin(), others.end());
    return radios;
}

} // namespace bt
