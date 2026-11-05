// session.h - Packet sessions (direwolf), and writing packets to a file. Each returns on
// Esc or when done.
#ifndef RTC_SESSION_H
#define RTC_SESSION_H

#include <functional>
#include <string>
#include <vector>

// ---- Writing packets to a file (gen_packets) ---------------------------------------

struct TransmitFormat {
    const char* name = "gen_packets";
    std::function<std::string(const std::string&)> format;   // Typed text -> stdin. Empty = as-is
    bool hideOutput = false;   // Show output only on failure
};

// Feeds typed lines to a program (gen_packets, writing a WAV file). Enter sends a line,
// Tab finishes, Esc stops. false if it failed
bool runFileTransmit(const std::vector<std::string>& args, const TransmitFormat& format);

// ---- Packets (direwolf) ----------------------------------------------------------

struct PacketSession {
    std::vector<std::string> direwolfArgs;
    std::string workingDir;                  // Where direwolf and its config are
    int kissPort = 0;
    std::string callsign;
    std::string destination = "CQ";          // Who typed packets are addressed to
    int baud = 1200;                        // For estimating airtime
    int fx25 = 0;                            // Settings::fx25: packet size and airtime
    bool showReceived = true;                // Receive and Duplex
    bool canType = false;                    // Transmit and Duplex
    std::string text;                        // Sent first
    bool finishAfterText = false;
};

// One packet per line typed. Esc waits for sent packets to go out; Esc again stops now.
// transmitted is set if anything was sent. false if direwolf failed
bool runPacketSession(const PacketSession& session, bool& transmitted);

#endif //RTC_SESSION_H
