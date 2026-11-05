// ansi.hpp - Terminal colors, menus and text input. Escape codes are skipped when stdout
// isn't a terminal or NO_COLOR is set.
#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <string>

#include "backend.h"

// printf format checking where the compiler supports it (GCC/Clang, not MSVC)
#if defined(__GNUC__) || defined(__clang__)
    #define ANSI_PRINTF_FORMAT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
    #define ANSI_PRINTF_FORMAT(fmt, args)
#endif

namespace ansi {

enum class Color {
    Default = 39,
    Black = 30, Red, Green, Yellow, Blue, Magenta, Cyan, White,
    Gray = 90, BrightRed, BrightGreen, BrightYellow, BrightBlue,
    BrightMagenta, BrightCyan, BrightWhite
};

// Styles can be combined: ansi::Bold | ansi::Underline
enum Style : unsigned {
    None      = 0,
    Bold      = 1 << 0,
    Dim       = 1 << 1,
    Italic    = 1 << 2,
    Underline = 1 << 3,
    Blink     = 1 << 4,
    Inverse   = 1 << 5,
};

inline Style operator|(Style a, Style b) {
    return static_cast<Style>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

// ---- Enable / disable --------------------------------------------------------

inline bool& enabledFlag() {
    static bool enabled = [] {
        bool on = os::isTerminal(stdout) && std::getenv("NO_COLOR") == nullptr;
        if (on) os::enableAnsiEscapes();   // Windows consoles need this before escapes work
        return on;
    }();
    return enabled;
}

inline bool enabled() { return enabledFlag(); }
inline void setEnabled(bool on) { enabledFlag() = on; }

// Writes a raw escape sequence only when escapes are enabled
inline void esc(const char* seq) {
    if (enabled()) std::fputs(seq, stdout);
}

// ---- Colors and styles -------------------------------------------------------

inline void reset() { esc("\033[0m"); }

inline void setColor(Color fg, Color bg = Color::Default) {
    if (!enabled()) return;
    std::printf("\033[%d;%dm", static_cast<int>(fg), static_cast<int>(bg) + 10);
}

// 24-bit color, supported by most modern terminals (Windows Terminal included)
inline void setRGB(int r, int g, int b) {
    if (!enabled()) return;
    std::printf("\033[38;2;%d;%d;%dm", r, g, b);
}

inline void setStyle(Style style) {
    if (style & Bold)      esc("\033[1m");
    if (style & Dim)       esc("\033[2m");
    if (style & Italic)    esc("\033[3m");
    if (style & Underline) esc("\033[4m");
    if (style & Blink)     esc("\033[5m");
    if (style & Inverse)   esc("\033[7m");
}

// ---- Printing (printf-style) -------------------------------------------------

inline void vprint(Color fg, Style style, const char* fmt, va_list args) {
    setColor(fg);
    setStyle(style);
    std::vprintf(fmt, args);
    reset();
}

ANSI_PRINTF_FORMAT(2, 3)
inline void print(Color fg, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(fg, None, fmt, args);
    va_end(args);
}

ANSI_PRINTF_FORMAT(3, 4)
inline void print(Color fg, Style style, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(fg, style, fmt, args);
    va_end(args);
}

// ---- Screen and cursor -------------------------------------------------------

inline void clearScreen() { esc("\033[2J\033[H"); }  // Clear and move cursor to top-left
inline void clearLine()   { esc("\033[2K\r"); }      // Clear current line, cursor to column 1
inline void clearToEnd()  { esc("\033[0J"); }        // Clear from cursor to end of screen

inline void moveTo(int row, int col) {               // 1-based coordinates
    if (!enabled()) return;
    std::printf("\033[%d;%dH", row, col);
}

inline void hideCursor() { esc("\033[?25l"); }
inline void showCursor() { esc("\033[?25h"); }

inline void flush() { std::fflush(stdout); }

// ---- Keyboard input ----------------------------------------------------------

// Raw mode: keys arrive one at a time, without waiting for Enter and without echo
inline void setRawMode(bool enable) { os::setRawMode(enable); }

enum class Key { Up, Down, PageUp, PageDown, Home, End, Enter, Back, Digit, Other };

struct KeyPress {
    Key key;
    int digit = 0;   // For Key::Digit
};

// Reads one key press; raw mode must be on. A lone ESC is the Esc key
inline KeyPress readKey() {
    int c = os::readInputByte();
    switch (c) {
        case -1:                      // stdin closed
        case 3:                       // Ctrl-C (menus turn off the interrupt key)
        case 'q': case 'Q':
        case 8: case 127:             // Backspace
            return {Key::Back};
        case '\r': case '\n': case ' ':
            return {Key::Enter};
        case 'k': return {Key::Up};
        case 'j': return {Key::Down};
        case 27: {
            if (!os::waitForInput(50)) return {Key::Back};
            int next = os::readInputByte();
            if (next != '[' && next != 'O') return {Key::Other};
            std::string params;
            int final = os::readInputByte();
            while (final != -1 && (final < 0x40 || final > 0x7e)) {
                params += static_cast<char>(final);
                final = os::readInputByte();
            }
            switch (final) {
                case 'A': return {Key::Up};
                case 'B': return {Key::Down};
                case 'C': return {Key::Enter};    // Right arrow goes into an item
                case 'D': return {Key::Back};     // Left arrow comes back out
                case 'H': return {Key::Home};
                case 'F': return {Key::End};
                case '~':
                    if (params == "5") return {Key::PageUp};
                    if (params == "6") return {Key::PageDown};
                    if (params == "1" || params == "7") return {Key::Home};
                    if (params == "4" || params == "8") return {Key::End};
                    return {Key::Other};
                default:  return {Key::Other};
            }
        }
        default:
            if (c >= '0' && c <= '9') return {Key::Digit, c - '0'};
            return {Key::Other};
    }
}

// Waits for any key. Swallows the rest of an escape sequence so it isn't read later
inline void waitKey(const char* message = "Press any key") {
    setRawMode(true);
    os::setInterruptKey(false);
    print(Color::Gray, "\n%s", message);
    flush();
    int c = os::readInputByte();
    if (c == 27) {
        while (os::waitForInput(30)) os::readInputByte();
    }
    std::printf("\n");
    setRawMode(false);                // Also turns the interrupt key back on
}

// ---- Selection menu ----------------------------------------------------------

inline const char* toCStr(const char* s)        { return s; }
inline const char* toCStr(const std::string& s) { return s.c_str(); }

// Cuts text to fit in width columns, counting UTF-8 characters rather than bytes
inline std::string fitWidth(const std::string& text, int width) {
    if (width < 1) return "";
    int chars = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
        if (++chars > width) return text.substr(0, i);
    }
    return text;
}

namespace detail {
// Ends a menu line, clearing whatever a longer line from the last frame left there
inline void endLine() {
    esc("\033[K");
    std::printf("\n");
}
} // namespace detail

// Menu. label(i) gives option i's text. Returns the chosen index, or -1 on Esc.
// Typing a number jumps to that item, and picks it once no more digits could follow.
template <class LabelFn>
int selectWith(const std::string& title, int count, LabelFn label, int initial = 0,
               const std::string& subtitle = "") {
    if (count <= 0) return -1;
    int selection = (initial >= 0 && initial < count) ? initial : 0;
    int top = 0;
    int typed = 0;                    // The number typed so far
    auto lastDigit = std::chrono::steady_clock::now();
    int numberWidth = static_cast<int>(std::to_string(count).size());
    int result = -1;
    bool done = false;

    setRawMode(true);
    os::setInterruptKey(false);
    hideCursor();
    clearScreen();
    while (!done) {
        os::TerminalSize size = os::terminalSize();
        int headerLines = subtitle.empty() ? 2 : 3;
        int visible = size.rows - headerLines - 3;    // 2 scroll markers + the help line
        if (visible < 3) visible = 3;
        if (selection < top) top = selection;
        if (selection >= top + visible) top = selection - visible + 1;
        if (top > count - visible) top = count - visible > 0 ? count - visible : 0;

        esc("\033[H");                                // Draw over the last frame: no flicker
        print(Color::BrightRed, Bold, "%s", fitWidth(title, size.cols - 1).c_str());
        detail::endLine();
        if (!subtitle.empty()) {
            print(Color::Gray, "%s", fitWidth(subtitle, size.cols - 1).c_str());
            detail::endLine();
        }
        detail::endLine();

        if (top > 0) print(Color::Gray, "   ^ %d more", top);
        detail::endLine();
        int end = top + visible < count ? top + visible : count;
        for (int i = top; i < end; i++) {
            char number[16];
            std::snprintf(number, sizeof number, "%*d  ", numberWidth, i + 1);
            std::string text = fitWidth(number + std::string(toCStr(label(i))), size.cols - 4);
            if (i == selection) {
                print(Color::BrightRed, "-> ");
                print(Color::BrightGreen, Bold | Inverse, "%s", text.c_str());
            } else {
                print(Color::BrightGreen, "   %s", text.c_str());
            }
            detail::endLine();
        }
        if (end < count) print(Color::Gray, "   v %d more", count - end);
        detail::endLine();
        print(Color::Gray, "%s", fitWidth("Up/Down move  Enter select  Esc back  "
                                          "type a number to jump", size.cols - 1).c_str());
        esc("\033[K\033[J");                          // Clear anything left below
        flush();

        KeyPress press = readKey();
        if (press.key != Key::Digit) typed = 0;
        switch (press.key) {
            case Key::Up:       selection = (selection - 1 + count) % count; break;
            case Key::Down:     selection = (selection + 1) % count;         break;
            case Key::PageUp:   selection = selection > visible ? selection - visible : 0; break;
            case Key::PageDown: selection = selection + visible < count ? selection + visible
                                                                        : count - 1; break;
            case Key::Home:     selection = 0;         break;
            case Key::End:      selection = count - 1; break;
            case Key::Enter:    result = selection; done = true; break;
            case Key::Back:     done = true; break;
            case Key::Digit: {
                auto now = std::chrono::steady_clock::now();
                if (now - lastDigit > std::chrono::milliseconds(1200)) typed = 0;
                lastDigit = now;
                typed = typed * 10 + press.digit;
                if (typed > count) typed = press.digit;      // Too big: start a new number
                if (typed >= 1 && typed <= count) {
                    selection = typed - 1;
                    if (typed * 10 > count) {                // No more digits could follow
                        result = selection;
                        done = true;
                    }
                } else {
                    typed = 0;
                }
                break;
            }
            case Key::Other:    break;
        }
    }
    showCursor();
    clearScreen();
    setRawMode(false);                // Also turns the interrupt key back on
    return result;
}

// Any container of strings: C arrays, std::array, std::vector<const char*>, std::vector<std::string>
template <class Container>
int select(const std::string& title, const Container& options, int initial = 0,
           const std::string& subtitle = "") {
    return selectWith(title, static_cast<int>(std::size(options)),
                      [&](int i) { return std::begin(options)[i]; }, initial, subtitle);
}

// Inline list: ansi::select("Mode", {"Receive", "Transmit", "Duplex"})
inline int select(const std::string& title, std::initializer_list<const char*> options,
                  int initial = 0, const std::string& subtitle = "") {
    return selectWith(title, static_cast<int>(options.size()),
                      [&](int i) { return options.begin()[i]; }, initial, subtitle);
}

// ---- Text input --------------------------------------------------------------

// Edits value. Enter saves (true), Esc cancels (false), Ctrl-U clears.
inline bool inputLine(const std::string& title, const std::string& prompt, std::string& value) {
    std::string text = value;
    setRawMode(true);
    os::setInterruptKey(false);
    bool accepted = false;
    for (;;) {
        clearScreen();
        print(Color::BrightRed, Bold, "%s\n", title.c_str());
        print(Color::Gray, "%s\n\n", prompt.c_str());
        print(Color::BrightGreen, "> %s", text.c_str());
        flush();

        int c = os::readInputByte();
        if (c == '\r' || c == '\n') { accepted = true; break; }
        if (c == -1 || c == 3) break;
        if (c == 27) {
            if (!os::waitForInput(50)) break;                 // Esc on its own: cancel
            while (os::waitForInput(30)) os::readInputByte(); // Arrow keys etc.: ignore
            continue;
        }
        if (c == 21) { text.clear(); continue; }              // Ctrl-U
        if (c == 8 || c == 127) {
            // Remove one whole UTF-8 character
            while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
                text.pop_back();
            if (!text.empty()) text.pop_back();
            continue;
        }
        if (c >= 32) text += static_cast<char>(c);
    }
    clearScreen();
    setRawMode(false);
    if (accepted) value = text;
    return accepted;
}

} // namespace ansi
