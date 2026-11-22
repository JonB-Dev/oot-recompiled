#pragma once

#include <string>

namespace oot::ui::markup {

    // Text into a document goes through here: RmlUi reads RML, so a name that happens to hold
    // an ampersand or an angle bracket (a pad's name is whatever its maker wrote, a file's name
    // is whatever the user typed) has to be escaped or it would be parsed as markup.
    inline std::string escape(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (const char c : text) {
            switch (c) {
                case '&':  out += "&amp;"; break;
                case '<':  out += "&lt;"; break;
                case '>':  out += "&gt;"; break;
                default:   out += c; break;
            }
        }
        return out;
    }

} // namespace oot::ui::markup
