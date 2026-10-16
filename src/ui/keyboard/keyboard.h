#pragma once

// THE ON-SCREEN KEYBOARD (the menus plan, phase 110): so a pad can type in the ROM browser's path
// field. This is its text and nothing else: the line being typed, the caret, Shift, and what each
// key does. Pure, so ui_tests types a path key by key; the shell draws it (assets/ui/keyboard.rml)
// and moves among its keys with the layout engine, the first surface on it.

#include <string>
#include <vector>

namespace oot::ui::keyboard {

    // The keys, row by row, as the document lays them out. A key is its character, or one of the
    // words below for a key that does something else.
    extern const char* const SHIFT;
    extern const char* const BACKSPACE;
    extern const char* const SPACE;
    extern const char* const DONE;
    const std::vector<std::vector<std::string>>& rows();

    class Line {
    public:
        explicit Line(std::string text = "") : text_(std::move(text)), caret_(text_.size()) {}

        // A key pressed: a character is typed at the caret (in capitals while Shift is on, which
        // then lets go), Shift toggles, Backspace takes the character before the caret, Space types
        // one. True when the key was Done.
        bool press(const std::string& key);
        void left();
        void right();

        const std::string& text() const { return text_; }
        size_t caret() const { return caret_; }
        bool shift() const { return shift_; }
        // The line as shown, with the caret drawn as a bar; given a width in characters, only
        // the part around the caret that fits, an ellipsis marking each side cut off.
        std::string shown() const;
        std::string shown(size_t width) const;

    private:
        std::string text_;
        size_t caret_;
        bool shift_ = false;
    };

    bool selftest(int& passed);

}
