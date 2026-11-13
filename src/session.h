// Packet sessions (direwolf), and writing packets to a file. Each returns on
// Esc or when done.
#ifndef RTC_SESSION_H
#define RTC_SESSION_H

#include <functional>
#include <string>
#include <vector>

// Writing packets to a file (gen_packets)

struct TransmitFormat {
    const char* name = "gen_packets";
    std::function<std::string(const std::string&)> format;   // Typed text -> stdin. Empty = as-is
    bool hideOutput = false;   // Show output only on failure
};

// Feeds typed lines to a program (gen_packets, writing a WAV file). Enter sends a line,
// Tab finishes, Esc stops. false if it failed
bool runFileTransmit(const std::vector<std::string>& args, const TransmitFormat& format);

// Packets (direwolf)

struct PacketSession {
    std::vector<std::string> direwolfArgs;
    std::string workingDir;                  // Where direwolf and its config are
    int kissPort = 0;
    std::string callsign;
    std::string destination = "CQ";          // Who typed packets are addressed to
    int baud = 1200;                        // For estimating airtime
    int fx25 = 0;                            // Settings::fx25: packet size and airtime
    int txDelayMs = 500;                     // Settings::txDelayMs: airtime
    bool showReceived = true;                // false: only sending (Tab finishes)
    bool canType = false;
    std::string text;                        // Sent first
    bool finishAfterText = false;
};

// One packet per line typed. Esc waits for sent packets to go out; Esc again stops now.
// transmitted is set if anything was sent. false if direwolf failed
bool runPacketSession(const PacketSession& session, bool& transmitted);

// Connected mode (docs/connected-mode.md): plain-text chat over AX.25 connections, using
// the same session code as RadioConnector, through direwolf's AGW port. Other stations can
// connect in; /c connects out. Typed lines go to the current session; lines starting with
// "/" are commands (/h lists them)
struct ConnectedSession {
    std::vector<std::string> direwolfArgs;
    std::string workingDir;
    int agwPort = 0;
    std::string callsign;
    std::string connectTo;     // Connected to at the start. Empty = wait for calls
    int fx25 = 0;              // Settings::fx25: text per frame
    int maxSessions = 4;
};

// Esc disconnects every session, then stops; Esc again stops now. transmitted is set if
// direwolf may have keyed the radio. false if direwolf failed
bool runConnectedSession(const ConnectedSession& session, bool& transmitted);

#endif //RTC_SESSION_H
