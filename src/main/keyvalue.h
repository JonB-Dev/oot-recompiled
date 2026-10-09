// The project's one file grammar: `key = value`, one per line, `#` comments, unknown keys
// ignored, every value clamped by whoever reads it. Written for the settings file in phase 24 and
// shared with the bindings file from phase 47, so the two files are parsed by one piece of code
// and a fix to it reaches both.
//
// Both files are hostile input (.scaffold/security/app-layer.md): nothing here throws, nothing
// here trusts a length, and a line that is not `key = value` is simply not a line.

#pragma once

#include <string>

namespace oot::keyvalue {

    // Splits `key = value`, trimming both. Returns false for a blank line, a comment, a line with
    // no `=`, or an empty key; the outputs are untouched in that case.
    bool parse_line(const std::string& line, std::string& key, std::string& value);

    // Reads a decimal integer with an optional sign. Garbage, an empty value, trailing text or a
    // value outside the int range all return `fallback`, which is what makes a half written file
    // lose nothing but the one value.
    int as_int(const std::string& value, int fallback);

} // namespace oot::keyvalue
