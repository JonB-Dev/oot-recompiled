// ui_tests (the menus plan, phase 90 onward): the interface's pure logic, tested headless by the
// build. No SDL, no RmlUi, no window, no renderer: each module that can be tested this way
// exposes a selftest, and this runs them all. Prints UI_TESTS_OK <n> tests, or the failures and
// UI_TESTS_FAIL, and the build stops on a failure (CMakeLists.txt, ui_tests_run).

#include "ui/input/ui_input_core.h"
#include "ui/nav/nav.h"
#include "ui/nav/nav_values.h"
#include "ui/nav/nav_mouse.h"
#include "ui/shell/shell.h"
#include "ui/models/credits.h"
#include "ui/prompts/prompts.h"
#include "ui/prompts/photo_guide.h"
#include "ui/keyboard/keyboard.h"
#include "ui/sound/ui_sound.h"
#include "ui/sound/game_queue.h"
#include "ui/ui_gradient.h"

#include <cstdio>
#include <fstream>
#include <sstream>

int main() {
    int passed = 0;
    bool ok = true;

    ok = oot::ui::input::selftest(passed) && ok;
    ok = oot::ui::nav::selftest(passed) && ok;
    ok = oot::ui::nav::value_selftest(passed) && ok;
    ok = oot::ui::nav::mouse_selftest(passed) && ok;
    ok = oot::ui::menu::selftest(passed) && ok;
    ok = oot::ui::prompts::selftest(passed) && ok;
    ok = oot::ui::prompts::photo_guide::selftest(passed) && ok;
    ok = oot::ui::keyboard::selftest(passed) && ok;
    ok = oot::ui::sound::selftest(passed) && ok;
    ok = oot::ui::sound::game::selftest(passed) && ok;

    // About's credits (phase 106): every entry of the bundle's license manifest is credited.
    {
        std::ifstream manifest(std::string(OOT_SOURCE_DIR) + "/assets/licenses/MANIFEST.txt", std::ios::binary);
        std::stringstream text;
        text << manifest.rdbuf();
        ok = oot::ui::credits::selftest(text.str(), passed) && ok;
    }

    // The gradient ramps (phase 87): their own test prints GRADIENT_OK or the failures.
    if (oot::ui::gradient::selftest() == 0) {
        ++passed;
    }
    else {
        ok = false;
    }

    if (!ok) {
        std::printf("UI_TESTS_FAIL\n");
        return 1;
    }
    std::printf("UI_TESTS_OK %d tests\n", passed);
    return 0;
}
