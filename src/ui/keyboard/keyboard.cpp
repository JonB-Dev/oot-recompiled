// The on-screen keyboard's text. See keyboard.h.

#include "ui/keyboard/keyboard.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace oot::ui::keyboard {

    const char* const SHIFT = "Shift";
    const char* const BACKSPACE = "Backspace";
    const char* const SPACE = "Space";
    const char* const DONE = "Done";

    // Every character a Windows path needs (a drive's colon, both slashes, the dot, the usual
    // punctuation of folder names), the digits and the letters.
    const std::vector<std::vector<std::string>>& rows() {
        static const std::vector<std::vector<std::string>> ROWS = {
            { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "_" },
            { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\\", "/" },
            { "a", "s", "d", "f", "g", "h", "j", "k", "l", ":", ".", "," },
            { SHIFT, "z", "x", "c", "v", "b", "n", "m", "(", ")", "'", BACKSPACE },
            { SPACE, DONE },
        };
        return ROWS;
    }

    bool Line::press(const std::string& key) {
        if (key == DONE) {
            return true;
        }
        if (key == SHIFT) {
            shift_ = !shift_;
            return false;
        }
        if (key == BACKSPACE) {
            if (caret_ > 0) {
                text_.erase(caret_ - 1, 1);
                --caret_;
            }
            return false;
        }
        std::string typed = key == SPACE ? std::string(" ") : key;
        if (shift_ && typed.size() == 1) {
            typed[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(typed[0])));
            shift_ = false;
        }
        text_.insert(caret_, typed);
        caret_ += typed.size();
        return false;
    }

    void Line::left() {
        if (caret_ > 0) {
            --caret_;
        }
    }

    void Line::right() {
        if (caret_ < text_.size()) {
            ++caret_;
        }
    }

    std::string Line::shown() const {
        return text_.substr(0, caret_) + "|" + text_.substr(caret_);
    }

    std::string Line::shown(size_t width) const {
        static const std::string ELLIPSIS = "\xE2\x80\xA6";
        if (text_.size() + 1 <= width || width < 8) {
            return shown();
        }
        // The caret's bar and an ellipsis each side take three of the width.
        const size_t room = width - 3;
        const size_t begin = caret_ > room ? caret_ - room : 0;
        const size_t end = std::min(text_.size(), begin + room);
        return (begin > 0 ? ELLIPSIS : "") + text_.substr(begin, caret_ - begin) + "|" +
               text_.substr(caret_, end - caret_) + (end < text_.size() ? ELLIPSIS : "");
    }

    bool selftest(int& passed) {
        bool ok = true;
        const auto check = [&](bool cond, const char* what) {
            if (cond) ++passed;
            else { std::printf("  keyboard: %s\n", what); ok = false; }
        };
        // A path, typed key by key.
        Line line;
        const char* keys[] = { "Shift", "c", ":", "\\", "Shift", "g", "a", "m", "e", "s", "\\", "o", "o", "t", ".", "z", "6", "4" };
        bool done = false;
        for (const char* k : keys) done = line.press(k) || done;
        check(line.text() == "C:\\Games\\oot.z64", "a path typed key by key, Shift making one capital each");
        check(!done, "no key but Done is Done");
        line.press(BACKSPACE);
        line.press(BACKSPACE);
        check(line.text() == "C:\\Games\\oot.z", "Backspace takes the character before the caret");
        line.left();
        line.left();
        line.press("x");
        check(line.text() == "C:\\Games\\ootx.z" && line.shown() == "C:\\Games\\ootx|.z", "the caret moves, and typing goes in at it");
        line.press(SPACE);
        check(line.text() == "C:\\Games\\ootx .z", "Space types a space");
        check(line.press(DONE), "Done is Done");
        bool every = true;
        for (const auto& row : rows()) {
            for (const std::string& k : row) every = every && !k.empty();
        }
        check(every && rows().size() == 5, "every key has a name, five rows");
        Line long_line("C:\\Users\\someone\\a very long folder name\\another one\\rom.z64");
        const std::string tail = long_line.shown(20);
        check(tail.size() <= 20 + 3 && tail.find("rom.z64|") != std::string::npos && tail.compare(0, 3, "\xE2\x80\xA6") == 0,
              "a long line shows its end at the caret, the cut start marked");
        long_line.left();
        for (int i = 0; i < 60; ++i) long_line.left();
        const std::string head = long_line.shown(20);
        check(head.compare(0, 4, "|C:\\") == 0 && head.find("\xE2\x80\xA6") != std::string::npos,
              "with the caret at the start the line shows its start, the cut end marked");
        return ok;
    }

}
