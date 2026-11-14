#include "link_watch.h"

namespace connector {

namespace {

constexpr size_t recentFrames = 8;   // A resend comes within the window (at most 7 frames)

// Unnumbered frame types, without the P/F bit
constexpr uint8_t SABME = 0x6F, SABM = 0x2F, DM = 0x0F, FRMR = 0x87;

} // namespace

void LinkWatch::setPeer(const std::string& call, bool open) { peers[call].open = open; }

void LinkWatch::removePeer(const std::string& call) { peers.erase(call); }

std::vector<std::string> LinkWatch::heard(const packet::Bytes& frame) {
    std::vector<std::string> problems;
    auto header = packet::decodeHeader(frame);
    // Only frames from a station we have a session with, to us, as direwolf acts on them
    // (a copy heard before a digipeater repeated it would look like a resend)
    if (!header || header->destination != own || !header->repeated) return problems;
    auto it = peers.find(header->source);
    if (it == peers.end()) return problems;
    Peer& peer = it->second;
    const std::string& call = header->source;
    size_t at = header->controlAt;
    uint8_t c = frame[at];

    if ((c & 0x01) == 0) {
        // I frame. N(S) is in bits 1-3 (modulo 8) or 1-7 (modulo 128); its low bits and the
        // info say whether we've had it. The info starts 2 or 3 bytes after the control
        // byte, so skip 3: the same either way
        std::string key(1, static_cast<char>(c & 0x0E));
        if (frame.size() > at + 3) key.append(frame.begin() + static_cast<long>(at + 3), frame.end());
        for (const std::string& seen : peer.recent) {
            if (seen == key) {
                problems.push_back(call + " sent the same frame again: our acknowledgement didn't reach them");
                return problems;
            }
        }
        peer.recent.push_back(key);
        if (peer.recent.size() > recentFrames) peer.recent.pop_front();
        return problems;
    }

    if ((c & 0x03) == 0x01) {
        // S frame: one control byte (modulo 8) or two (modulo 128), P/F in each's own place
        bool modulo128 = frame.size() >= at + 2;
        bool pf = modulo128 ? (frame[at + 1] & 0x01) : (c & 0x10);
        int type = (c >> 2) & 0x03;
        if (type == 1) problems.push_back(call + " is busy and asked us to wait (RNR)");
        if (type == 2) problems.push_back(call + " missed a frame from us and asked for it again (REJ)");
        if (type == 3) problems.push_back(call + " missed a frame from us and asked for it again (SREJ)");
        if (pf && header->command)
            problems.push_back(call + " got no answer from us in time and asked again");
        if (pf && header->response)
            problems.push_back("No answer from " + call + " in time: we asked again, and they answered");
        return problems;
    }

    // U frame
    uint8_t type = c & static_cast<uint8_t>(~0x10);
    if (type == SABM || type == SABME) {
        if (peer.open)
            problems.push_back(call + " asked to connect again on an open link: our answer didn't reach "
                               "them, or they restarted the link");
        else
            problems.push_back(call + " is trying to connect to us at the same time");
    } else if (type == DM) {
        problems.push_back(peer.open ? call + " says the link isn't connected (DM)"
                                     : call + " refused the connection (DM)");
    } else if (type == FRMR) {
        problems.push_back(call + " rejected a frame as invalid (FRMR)");
    }
    return problems;
}

std::vector<std::string> LinkWatch::outstanding(const std::string& call, int count, Clock::time_point now) {
    std::vector<std::string> problems;
    auto it = peers.find(call);
    if (it == peers.end() || count < 0) return problems;
    Peer& peer = it->second;
    if (count == 0 || count < peer.waiting) {   // Acknowledged: start over
        peer.waitingSince = now;
        peer.stallReported = false;
    } else if (peer.waiting == 0) {
        peer.waitingSince = now;
    }
    peer.waiting = count;
    if (count > 0 && !peer.stallReported && now - peer.waitingSince >= stallLimit) {
        peer.stallReported = true;
        problems.push_back("No answer from " + call + " for " + std::to_string(stallLimit.count()) + " s (" +
                           std::to_string(count) + (count == 1 ? " frame" : " frames") +
                           " not acknowledged)");
    }
    return problems;
}

} // namespace connector
