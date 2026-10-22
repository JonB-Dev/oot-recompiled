// The interface's vectors. See ui_vectors.h.

#include "ui_vectors.h"

#include <lunasvg.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace oot::ui::vectors {

    int selftest(const std::string& assets_dir) {
        namespace fs = std::filesystem;
        // The prompts' and the brand's drawings too (2026-10-13): every pad button is drawn now, and a
        // drawing the library cannot read would leave its prompt blank.
        const std::vector<fs::path> folders = { fs::path(assets_dir) / "icons", fs::path(assets_dir) / "ui" / "medallion",
                                                fs::path(assets_dir) / "ui" / "prompts", fs::path(assets_dir) / "ui" / "brand" };
        int drawn = 0;
        bool ok = true;

        for (const fs::path& folder : folders) {
            if (!fs::is_directory(folder)) {
                std::printf("  %s is missing\n", folder.string().c_str());
                ok = false;
                continue;
            }
            for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
                if (entry.path().extension() != ".svg") continue;
                const std::string path = entry.path().string();
                auto doc = lunasvg::Document::loadFromFile(path);
                if (!doc) {
                    std::printf("  %s would not parse\n", path.c_str());
                    ok = false;
                    continue;
                }
                // 1x and 2x of the document's own size, the way the interface size row will ask.
                for (int scale = 1; scale <= 2; ++scale) {
                    const int w = static_cast<int>(doc->width()) * scale;
                    const int h = static_cast<int>(doc->height()) * scale;
                    lunasvg::Bitmap bitmap = doc->renderToBitmap(w, h);
                    if (bitmap.isNull()) {
                        std::printf("  %s drew no bitmap at %dx\n", path.c_str(), scale);
                        ok = false;
                        continue;
                    }
                    // Something must be painted: a parse that yields an empty picture is how a
                    // file that references a missing gradient fails, silently.
                    const uint8_t* pixels = bitmap.data();
                    bool painted = false;
                    for (int y = 0; y < bitmap.height() && !painted; ++y) {
                        const uint8_t* row = pixels + static_cast<size_t>(y) * bitmap.stride();
                        for (int x = 0; x < bitmap.width(); ++x) {
                            if (row[x * 4 + 3] != 0) { painted = true; break; }
                        }
                    }
                    if (!painted) {
                        std::printf("  %s is blank at %dx\n", path.c_str(), scale);
                        ok = false;
                    }
                }
                ++drawn;
            }
        }
        std::printf("%s %d\n", ok ? "SVG_OK" : "SVG_FAIL", drawn);
        return ok ? 0 : 1;
    }

}
