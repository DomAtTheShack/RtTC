#include "cat.h"

#include <stdexcept>

namespace {

void check(int status, const char* what) {
    if (status != RIG_OK) {
        // rigerror2: message without the debug trace
        throw std::runtime_error(std::string(what) + ": " + rigerror2(status));
    }
}

void setConf(RIG* rig, const char* name, const std::string& value) {
    check(rig_set_conf(rig, rig_token_lookup(rig, name), value.c_str()), name);
}

} // namespace

const std::vector<RadioModel>& commonRadios() {
    static const std::vector<RadioModel> radios = {
        {RIG_MODEL_IC7300,   "Icom IC-7300"},
#ifdef RIG_MODEL_IC7300MK2
        {RIG_MODEL_IC7300MK2, "Icom IC-7300MK2"},
#endif
        {RIG_MODEL_IC705,    "Icom IC-705"},
        {RIG_MODEL_IC7610,   "Icom IC-7610"},
        {RIG_MODEL_IC9700,   "Icom IC-9700"},
        {RIG_MODEL_IC7100,   "Icom IC-7100"},
        {RIG_MODEL_IC7000,   "Icom IC-7000"},
        {RIG_MODEL_IC9100,   "Icom IC-9100"},
        {RIG_MODEL_FTDX3000, "Yaesu FTDX-3000"},
        {RIG_MODEL_FTDX10,   "Yaesu FTDX-10"},
        {RIG_MODEL_FTDX101D, "Yaesu FTDX-101D"},
        {RIG_MODEL_FTDX5000, "Yaesu FTDX-5000"},
        {RIG_MODEL_FT991,    "Yaesu FT-991 / FT-991A"},
        {RIG_MODEL_FT710,    "Yaesu FT-710"},
        {RIG_MODEL_FT891,    "Yaesu FT-891"},
        {RIG_MODEL_FT817,    "Yaesu FT-817"},
        {RIG_MODEL_FT818,    "Yaesu FT-818"},
        {RIG_MODEL_FT857,    "Yaesu FT-857"},
        {RIG_MODEL_FT897,    "Yaesu FT-897"},
        {RIG_MODEL_TS590SG,  "Kenwood TS-590SG"},
        {RIG_MODEL_TS590S,   "Kenwood TS-590S"},
        {RIG_MODEL_TS890S,   "Kenwood TS-890S"},
        {RIG_MODEL_TS2000,   "Kenwood TS-2000"},
        {RIG_MODEL_TS480,    "Kenwood TS-480"},
        {RIG_MODEL_K3,       "Elecraft K3 / K3S"},
        {RIG_MODEL_KX3,      "Elecraft KX3 / KX2"},
        {RIG_MODEL_K4,       "Elecraft K4"},
        {RIG_MODEL_X6100,    "Xiegu X6100"},
#ifdef RIG_MODEL_X6200
        {RIG_MODEL_X6200,    "Xiegu X6200"},
#endif
        {RIG_MODEL_G90,      "Xiegu G90"},
        {RIG_MODEL_NETRIGCTL, "rigctld (host:port)"},
        {RIG_MODEL_DUMMY,    "Hamlib dummy rig"},
    };
    return radios;
}

std::string radioName(rig_model_t model) {
    for (const RadioModel& radio : commonRadios())
        if (radio.model == model) return radio.name;
    static const bool loaded = [] {
        rig_set_debug(RIG_DEBUG_NONE);
        return rig_load_all_backends() == RIG_OK;
    }();
    (void)loaded;
    const rig_caps* caps = rig_get_caps(model);
    if (!caps) return "";
    return std::string(caps->mfg_name) + " " + caps->model_name;
}

Cat::Cat(rig_model_t model, const std::string& port, int baud, const std::string& pttLine,
         const std::string& pttPort) {
    rig_set_debug(RIG_DEBUG_NONE);   // Keep Hamlib off the screen

    rig_ = rig_init(model);
    if (!rig_) {
        throw std::runtime_error("Hamlib has no rig model " + std::to_string(model));
    }

    try {
        // The dummy rig takes no port
        if (model != RIG_MODEL_DUMMY) {
            setConf(rig_, "rig_pathname", port);
        }
        if (model != RIG_MODEL_DUMMY && model != RIG_MODEL_NETRIGCTL) {
            setConf(rig_, "serial_speed", std::to_string(baud));
        }
        if (!pttLine.empty()) {
            setConf(rig_, "ptt_type", pttLine);
            setConf(rig_, "ptt_pathname", pttPort);
        }
        check(rig_open(rig_), ("open " + port).c_str());
    } catch (...) {
        rig_cleanup(rig_);
        throw;
    }
}

std::unique_ptr<Cat> openPtt(const Settings& s) {
    if (std::string problem = pttProblem(s); !problem.empty()) throw std::runtime_error(problem);
    switch (s.ptt) {
        case PttType::Vox:
            return nullptr;
        case PttType::Cat:
            return std::make_unique<Cat>(s.rigModel, s.catPort, s.catBaud);
        case PttType::Rts:
        case PttType::Dtr:
            // Only the line: Hamlib's dummy rig, with a real port for PTT
            return std::make_unique<Cat>(RIG_MODEL_DUMMY, "", 0, s.ptt == PttType::Rts ? "RTS" : "DTR",
                                         s.keyingPort());
        case PttType::Aioc:
            if (s.keyingPort().empty()) throw std::runtime_error("the AIOC cable wasn't found");
            // Hamlib turns RTS off when it opens a separate PTT port, so DTR alone keys it
            return std::make_unique<Cat>(RIG_MODEL_DUMMY, "", 0, "DTR", s.keyingPort());
    }
    return nullptr;
}

Cat::~Cat() {
    rig_close(rig_);
    rig_cleanup(rig_);
}

double Cat::frequency() {
    freq_t hz;
    check(rig_get_freq(rig_, RIG_VFO_CURR, &hz), "get frequency");
    return hz;
}

void Cat::setFrequency(double hz) {
    check(rig_set_freq(rig_, RIG_VFO_CURR, hz), "set frequency");
}

rmode_t Cat::mode() {
    rmode_t mode;
    pbwidth_t width;
    check(rig_get_mode(rig_, RIG_VFO_CURR, &mode, &width), "get mode");
    return mode;
}

void Cat::setMode(rmode_t mode, pbwidth_t width) {
    check(rig_set_mode(rig_, RIG_VFO_CURR, mode, width), "set mode");
}

bool Cat::transmitting() {
    ptt_t ptt;
    check(rig_get_ptt(rig_, RIG_VFO_CURR, &ptt), "get PTT");
    return ptt != RIG_PTT_OFF;
}

void Cat::setPtt(bool on) {
    check(rig_set_ptt(rig_, RIG_VFO_CURR, on ? RIG_PTT_ON : RIG_PTT_OFF), "set PTT");
}
