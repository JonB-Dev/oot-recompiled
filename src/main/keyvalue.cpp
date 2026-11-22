#include "main/keyvalue.h"

namespace oot::keyvalue {

    bool parse_line(const std::string& line, std::string& key, std::string& value) {
        if (line.empty() || line[0] == '#') {
            return false;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            return false;
        }
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);

        auto trim = [](std::string& s) {
            const size_t begin = s.find_first_not_of(" \t\r\n");
            const size_t end = s.find_last_not_of(" \t\r\n");
            s = (begin == std::string::npos) ? std::string() : s.substr(begin, end - begin + 1);
        };
        trim(k);
        trim(v);
        if (k.empty()) {
            return false;
        }
        key = k;
        value = v;
        return true;
    }

    // Hand rolled rather than std::stoi, which THROWS on garbage. The whole point of these files
    // is that garbage in them costs one value and never the program. Digits only, no sign, and a
    // value past five digits is garbage too: exactly the reading the settings file has had since
    // phase 24 (moved here from ui_settings.cpp unchanged), so nothing in it reads differently.
    int as_int(const std::string& value, int fallback) {
        if (value.empty()) {
            return fallback;
        }
        int result = 0;
        for (char c : value) {
            if (c < '0' || c > '9') {
                return fallback;
            }
            if (result > 10000) {
                return fallback;
            }
            result = result * 10 + (c - '0');
        }
        return result;
    }

} // namespace oot::keyvalue
