// Radio control through Hamlib.
#ifndef RTC_CAT_H
#define RTC_CAT_H

#include <memory>
#include <string>
#include <vector>

#include <hamlib/rig.h>

#include "../settings.h"

struct RadioModel {
    rig_model_t model;
    const char* name;
};

const std::vector<RadioModel>& commonRadios();
// "" if Hamlib doesn't know the model
std::string radioName(rig_model_t model);

// Every call throws std::runtime_error on failure
class Cat {
public:
    // model 1 = dummy rig, 2 = rigctld (port is host:port). pttLine "RTS" or "DTR": setPtt
    // uses that line of pttPort instead of a CAT command
    Cat(rig_model_t model, const std::string& port, int baud = 19200,
        const std::string& pttLine = "", const std::string& pttPort = "");
    ~Cat();

    Cat(const Cat&) = delete;
    Cat& operator=(const Cat&) = delete;

    double frequency();                 // Hz
    void setFrequency(double hz);

    rmode_t mode();
    void setMode(rmode_t mode, pbwidth_t width = RIG_PASSBAND_NORMAL);

    bool transmitting();
    void setPtt(bool on);

private:
    RIG* rig_;
};

// What keys the transmitter for these settings: the radio over CAT, or (RTS/DTR) only the
// keying port's line. nullptr for VOX. Throws std::runtime_error. direwolf does its own
// keying; this is for the tuning tone and unkeying after direwolf
std::unique_ptr<Cat> openPtt(const Settings& settings);

// Keys PTT while it exists
class PttGuard {
public:
    explicit PttGuard(Cat& cat) : cat_(cat) { cat_.setPtt(true); }
    ~PttGuard() {
        try { cat_.setPtt(false); } catch (...) {}
    }

    PttGuard(const PttGuard&) = delete;
    PttGuard& operator=(const PttGuard&) = delete;

private:
    Cat& cat_;
};


#endif //RTC_CAT_H
