// Spots what shouldn't happen on a healthy AX.25 connected-mode link, for RadioConnectorCli's
// connected screen. It sees what direwolf hears (AGW raw frames) and direwolf's count of our
// frames not yet acknowledged ('Y'). direwolf doesn't report its own connected-mode frames,
// so our resends are seen through the other station's answers: an answer with the F bit set
// means we asked again, as do no answers at all while frames wait.
#ifndef RTC_LINK_WATCH_H
#define RTC_LINK_WATCH_H

#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "packet.h"

namespace connector {

class LinkWatch {
public:
    using Clock = std::chrono::steady_clock;

    // No answer while frames wait this long is reported
    static constexpr std::chrono::seconds stallLimit{10};

    explicit LinkWatch(std::string own) : own(std::move(own)) {}

    // A station we have a session with, opening (false) or open (true). Callsigns normalized
    void setPeer(const std::string& call, bool open);
    void removePeer(const std::string& call);

    // A poll after the link has been quiet this long is direwolf checking the link is still
    // there (T3, every 5 minutes by default), not a resend
    static constexpr std::chrono::seconds idleLimit{60};

    // A frame direwolf heard, without the FCS. Returns what went wrong, if anything. A
    // keep-alive poll on a quiet link isn't a problem: it goes in notes, if given. now's
    // default turns that check off
    std::vector<std::string> heard(const packet::Bytes& frame, Clock::time_point now = {},
                                   std::vector<std::string>* notes = nullptr);
    // direwolf's count of our frames call hasn't acknowledged yet
    std::vector<std::string> outstanding(const std::string& call, int count, Clock::time_point now);

private:
    struct Peer {
        bool open = false;
        std::deque<std::string> recent;   // Recent I frames: N(S)'s low bits and the info
        int waiting = 0;                  // Frames not acknowledged
        Clock::time_point waitingSince{};
        bool stallReported = false;
        Clock::time_point lastFrame{};    // When we last heard from them; {} = not yet
    };

    std::string own;
    std::map<std::string, Peer> peers;
};

} // namespace connector

#endif //RTC_LINK_WATCH_H
