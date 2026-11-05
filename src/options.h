// Choices for the settings menus.
#ifndef RTC_OPTIONS_H
#define RTC_OPTIONS_H

struct Option {
    float value;
    const char* label;
};

const Option catBaudRates[] = {
    {4800.0f,   "4800"},
    {9600.0f,   "9600"},
    {19200.0f,  "19200"},
    {38400.0f,  "38400"},
    {115200.0f, "115200"},
};

#endif //RTC_OPTIONS_H