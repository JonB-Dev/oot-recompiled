// The interface's faces. See ui_fonts.h.

#include "ui_fonts.h"

#include <RmlUi/Core.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#include <cstdio>
#include <filesystem>
#include <set>
#include <vector>

namespace oot::ui::fonts {

    namespace {

        struct Fallback {
            const char* path;
            int weight;
        };

        struct Face {
            const char* file;
            const char* family;
            std::vector<int> weights;
            // System faces registered under this family when the bundled file is missing. The
            // nearest Windows has to each: a bold sans for the condensed display face, the system
            // sans for the body, Consolas for the mono.
            std::vector<Fallback> fallbacks;
        };

        const std::vector<Face>& faces() {
            static const std::vector<Face> list = {
                { "display.ttf", "Big Shoulders Display", { 800, 900 },
                  { { "C:/Windows/Fonts/segoeuib.ttf", 800 }, { "C:/Windows/Fonts/seguibl.ttf", 900 } } },
                { "body.ttf", "Bricolage Grotesque", { 400, 600 },
                  { { "C:/Windows/Fonts/segoeui.ttf", 400 }, { "C:/Windows/Fonts/seguisb.ttf", 600 } } },
                { "mono.ttf", "JetBrains Mono", { 400, 700 },
                  { { "C:/Windows/Fonts/consola.ttf", 400 }, { "C:/Windows/Fonts/consolab.ttf", 700 } } },
            };
            return list;
        }

        std::string bundled(const std::string& assets_dir, const Face& face) {
            return assets_dir + "/fonts/" + face.file;
        }

    }

    void load(const std::string& assets_dir) {
        int ours = 0, fallen = 0;
        for (const Face& face : faces()) {
            const std::string path = bundled(assets_dir, face);
            // One call registers every named instance of a variable face, each under the family
            // the file names, with its own weight.
            if (std::filesystem::exists(path) && Rml::LoadFontFace(path)) {
                ++ours;
                continue;
            }
            for (const Fallback& f : face.fallbacks) {
                if (std::filesystem::exists(f.path) &&
                    Rml::LoadFontFace(f.path, face.family, Rml::Style::FontStyle::Normal,
                                      static_cast<Rml::Style::FontWeight>(f.weight))) {
                    ++fallen;
                }
            }
            std::fprintf(stderr, "[ui] %s is missing, so %s is drawn in a system face\n", path.c_str(), face.family);
        }
        std::fprintf(stderr, "[ui] fonts: %d of ours, %d system fallbacks\n", ours, fallen);
        if (ours == 0 && fallen == 0) {
            std::fprintf(stderr, "[ui] no font could be loaded, so the interface will have no text at all\n");
        }
    }

    int selftest(const std::string& assets_dir) {
        FT_Library library = nullptr;
        if (FT_Init_FreeType(&library) != 0) {
            std::printf("FONTS_FAIL FreeType would not start\n");
            return 1;
        }
        const FT_ULong wght = FT_MAKE_TAG('w', 'g', 'h', 't');
        bool ok = true;
        std::string report;

        for (const Face& face : faces()) {
            const std::string path = bundled(assets_dir, face);
            if (!std::filesystem::exists(path)) {
                std::string line = std::string(face.family) + " missing, falling back to";
                for (const Fallback& f : face.fallbacks) {
                    const bool there = std::filesystem::exists(f.path);
                    ok = ok && there;
                    line += std::string(" ") + f.path + (there ? "" : " (ABSENT)");
                }
                std::printf("  %s\n", line.c_str());
                report += std::string(" ") + face.file + "=system";
                continue;
            }

            FT_Face ft = nullptr;
            if (FT_New_Face(library, path.c_str(), 0, &ft) != 0) {
                std::printf("  %s: FreeType could not open %s\n", face.family, path.c_str());
                ok = false;
                continue;
            }
            const std::string family = ft->family_name ? ft->family_name : "";
            std::set<int> have;
            FT_MM_Var* mm = nullptr;
            if (FT_Get_MM_Var(ft, &mm) == 0) {
                for (FT_UInt a = 0; a < mm->num_axis; ++a) {
                    if (mm->axis[a].tag != wght) continue;
                    for (FT_UInt n = 0; n < mm->num_namedstyles; ++n) {
                        have.insert(static_cast<int>(mm->namedstyle[n].coords[a] >> 16));
                    }
                }
                FT_Done_MM_Var(library, mm);
            }
            FT_Done_Face(ft);

            std::string weights;
            for (int w : face.weights) {
                const bool present = have.count(w) > 0;
                ok = ok && present;
                weights += (weights.empty() ? "" : ", ") + std::to_string(w) + (present ? "" : " MISSING");
            }
            if (family != face.family) {
                ok = false;
                std::printf("  %s names its family '%s', not '%s'\n", face.file, family.c_str(), face.family);
            }
            std::printf("  %s: %s (%s) of %zu named instances\n", face.file, family.c_str(), weights.c_str(), have.size());
            report += std::string(" ") + face.file + "=" + family;
        }
        FT_Done_FreeType(library);

        std::printf("%s%s\n", ok ? "FONTS_OK" : "FONTS_FAIL", report.c_str());
        return ok ? 0 : 1;
    }

}
