// About's credits. See credits.h.

#include "ui/models/credits.h"

#include <cstdio>
#include <sstream>

namespace oot::ui::credits {

    namespace {

        std::vector<std::string> split(const std::string& line, char by) {
            std::vector<std::string> out;
            std::string part;
            std::istringstream in(line);
            while (std::getline(in, part, by)) {
                out.push_back(part);
            }
            return out;
        }

        // "github.com/google/fonts ofl/cinzel/Cinzel[wght].ttf" -> "github.com/google/fonts": the
        // repository or family a file came from, without the file's own path in it.
        std::string family_of(const std::string& source) {
            const size_t space = source.find(' ');
            return space == std::string::npos ? source : source.substr(0, space);
        }

        // "OFL-1.1 licenses/OFL-title.txt" -> "OFL-1.1".
        std::string license_of(const std::string& field) {
            const size_t space = field.find(' ');
            return space == std::string::npos ? field : field.substr(0, space);
        }

        bool ours(const std::string& license) {
            return license.rfind("GPL-3.0", 0) == 0;
        }

    }

    std::vector<Credit> from_manifest(const std::string& text) {
        std::vector<Credit> out;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const std::vector<std::string> f = split(line, '\t');
            if (f.size() < 4) {
                continue;
            }
            const std::string license = license_of(f[3]);
            if (ours(license)) {
                continue;
            }
            const std::string source = family_of(f[1]);
            Credit* found = nullptr;
            for (Credit& c : out) {
                if (c.source == source && c.license == license) {
                    found = &c;
                }
            }
            if (found == nullptr) {
                out.push_back({ source, license, {} });
                found = &out.back();
            }
            found->files.push_back(f[0]);
        }
        return out;
    }

    std::string describe(const Credit& credit) {
        std::string what = credit.source;
        if (what == "github.com/google/fonts") {
            what = "Typefaces from Google Fonts";
        }
        else if (what == "github.com/lucide-icons/lucide") {
            what = "Lucide icons";
        }
        return what + ", " + credit.license + " (" + std::to_string(credit.files.size()) +
               (credit.files.size() == 1 ? " file)" : " files)");
    }

    bool selftest(const std::string& manifest_text, int& passed) {
        bool ok = true;
        const auto check = [&](bool cond, const char* what) {
            if (cond) ++passed;
            else { std::printf("  credits: %s\n", what); ok = false; }
        };
        // Every entry that is not ours is credited, under its source and license, exactly once.
        const std::vector<Credit> credits = from_manifest(manifest_text);
        std::istringstream in(manifest_text);
        std::string line;
        int entries = 0;
        int credited = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const std::vector<std::string> f = split(line, '\t');
            if (f.size() < 4 || ours(license_of(f[3]))) continue;
            ++entries;
            int seen = 0;
            for (const Credit& c : credits) {
                for (const std::string& file : c.files) {
                    if (file == f[0] && c.license == license_of(f[3])) ++seen;
                }
            }
            if (seen == 1) ++credited;
        }
        check(entries > 0, "the manifest lists entries to credit");
        check(credited == entries, "every manifest entry not ours is credited exactly once");
        // And the shape of a line.
        const std::vector<Credit> one = from_manifest(
            "# a comment\nfonts/a.ttf\tgithub.com/google/fonts ofl/a/A.ttf\tabc\tOFL-1.1 licenses/OFL-a.txt\tff\tnote\n"
            "ui/ours.svg\tours, a tool\t-\tGPL-3.0 (this program's own)\tee\tnote\n");
        check(one.size() == 1 && one[0].files.size() == 1, "our own drawn files are not credits");
        check(one.size() == 1 && describe(one[0]) == "Typefaces from Google Fonts, OFL-1.1 (1 file)", "a credit reads as a line");
        return ok;
    }

}
