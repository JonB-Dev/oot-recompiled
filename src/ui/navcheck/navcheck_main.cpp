// ui_navcheck (the menus plan, phase 94; security/build-ci.md section 4): the gate that keeps
// navigation from regressing. Every interface document is laid out by the real RmlUi, with the
// real fonts and a renderer that draws nothing, at every window size and interface size the menus
// are checked at, and for each one:
//
//   1. every focusable element is reachable from the document's first focus by directions alone;
//   2. for every focusable element and each direction, the engine's target is the oracle's
//      (nav_oracle.cpp, the rule written a second time);
//   3. every layer says what Back does there (data-back);
//   4. a focused element looks different from an unfocused one (its own or a descendant's style
//      changes with the focus class);
//   5. in a list or a grid (a group whose items are all one size), moving one way and back
//      returns to the same element.
//
// A failure names the document, the size, the element and the direction, prints NAVCHECK_FAIL,
// and fails the build: CMake runs this before the game links.
//
// The check runs on freshly opened documents with no navigation memory, so "reachable" means by
// spatial moves alone; what memory changes (coming back to a list returns to the row you left) is
// the engine's own self test's business (ui_tests).

#include "ui/nav/nav.h"
#include "ui/nav/nav_oracle.h"
#include "ui/nav/nav_rml.h"
#include "ui/models/rows_model.h"
#include "ui/models/shell_model.h"
#include "ui/ui_fonts.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#ifndef OOT_SOURCE_DIR
#define OOT_SOURCE_DIR "."
#endif

namespace {

    using namespace oot::ui::nav;
    namespace fs = std::filesystem;

    // DOCUMENTS NOT YET MOVED ONTO THE ENGINE, in one place. Each phase that moves a surface
    // takes its line out; phase 116's exit criterion is this list being empty. A name here that
    // is no longer a document fails the check, so the list cannot go stale.
    const char* const EXEMPT[] = {
        "about.rml",
        "ask.rml",
        "browse.rml",
        "controls.rml",
        "counter.rml",
        "debug.rml",
        "files.rml",
        "frame.rml",
        "hint.rml",
        "hud.rml",
        "launch.rml",
        "letterbox.rml",
        "lighting.rml",
        "moments.rml",
        "recorder.rml",
        "settings.rml",
        "update.rml",
        "warps.rml",
    };

    // The check's own documents (src/ui/navcheck/fixtures/): the layouts the menus are built
    // from, so the gate has something to bite on before the first surface moves.
    // unreachable.rml is the planted failure; listing it here must fail the build.
    const char* const FIXTURES[] = {
        "patterns.rml",
    };

    // The focus class the shell sets on the focused element (nav_rml.h).
    constexpr const char* FOCUS_CLASS = "is-f";
    // The class that shows a layer (nav_rml.h). The check shows every layer so each is checked.
    constexpr const char* OPEN_CLASS = "is-open";

    struct Window {
        int w, h;
        bool launcher;   // the launcher window: its size is in dp, so it grows with the interface
    };

    // The default first, so the first failure printed is the one people see at their own size.
    // Every step of the Interface size row (phase 113), 100% first.
    const float INTERFACE_SIZES[] = { 1.0f, 0.75f, 0.9f, 1.15f, 1.3f, 1.5f };
    const Window WINDOWS[] = {
        { 1920, 1080, false },
        { 1280, 720, false },
        { 3440, 1440, false },
        { 960, 620, true },   // --launcher-width and --launcher-height at 100%
    };

    // ---------------------------------------------------------------------------------------
    // RmlUi with no window: a clock that does not move and a renderer that draws nothing
    // ---------------------------------------------------------------------------------------

    class System final : public Rml::SystemInterface {
    public:
        // Frozen, so an animation stays where it starts and two looks at an element compare
        // like with like.
        double GetElapsedTime() override { return 0.0; }

        bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
            if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT) {
                errors.push_back(message);
            }
            else if (report && type == Rml::Log::LT_WARNING) {
                std::printf("  rmlui: %s\n", message.c_str());
            }
            return true;
        }

        std::vector<std::string> errors;
        bool report = false;
    };

    class NullRender final : public Rml::RenderInterface {
    public:
        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return ++next; }
        void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
        void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
        // Bitmaps are not read by the real renderer either (ui_render.cpp, LoadTexture), so a
        // document lays out here exactly as it does there.
        Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override { return 0; }
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return ++next; }
        void ReleaseTexture(Rml::TextureHandle) override {}
        void EnableScissorRegion(bool) override {}
        void SetScissorRegion(Rml::Rectanglei) override {}

    private:
        uintptr_t next = 0;
    };

    // ---------------------------------------------------------------------------------------
    // Reporting
    // ---------------------------------------------------------------------------------------

    struct Totals {
        int documents = 0;
        long elements = 0;
        long moves = 0;
        int failures = 0;
    };

    struct Where {
        std::string document;   // the file name
        std::string size;       // "1920x1080 100%"
    };

    void fail(Totals& t, const Where& w, const std::string& what) {
        // Every size repeats a layout defect; the first few say enough.
        if (t.failures < 40) {
            std::printf("NAVCHECK_FAIL %s %s %s\n", w.document.c_str(), w.size.c_str(), what.c_str());
        }
        ++t.failures;
    }

    std::string name(const RmlModel& rm, int node) {
        if (node < 0) return "(none)";
        Rml::Element* e = rm.elements[node];
        const Rml::String id = e->GetId();
        if (!id.empty()) return "#" + id;
        return e->GetTagName() + "[" + std::to_string(node) + "]";
    }

    const char* dir_name(Dir d) {
        switch (d) {
            case Dir::Up: return "up";
            case Dir::Down: return "down";
            case Dir::Left: return "left";
            case Dir::Right: return "right";
        }
        return "?";
    }

    Dir opposite(Dir d) {
        switch (d) {
            case Dir::Up: return Dir::Down;
            case Dir::Down: return Dir::Up;
            case Dir::Left: return Dir::Right;
            case Dir::Right: return Dir::Left;
        }
        return d;
    }

    const Dir DIRS[] = { Dir::Up, Dir::Down, Dir::Left, Dir::Right };

    // ---------------------------------------------------------------------------------------
    // The model's questions
    // ---------------------------------------------------------------------------------------

    bool encloses(const Model& m, int ancestor, int node) {
        for (int p = m.nodes[node].parent; p >= 0; p = m.nodes[p].parent) {
            if (p == ancestor) return true;
        }
        return false;
    }

    int nearest_group(const Model& m, int node) {
        for (int p = m.nodes[node].parent; p >= 0; p = m.nodes[p].parent) {
            if (m.nodes[p].kind == Kind::Layer) return -1;
            if (m.nodes[p].kind == Kind::Group) return p;
        }
        return -1;
    }

    // A list or a grid: every item directly in the group is one size.
    bool uniform(const Model& m, int group) {
        bool have = false;
        Rect first{};
        for (size_t i = 0; i < m.nodes.size(); ++i) {
            const Node& n = m.nodes[i];
            if (n.kind != Kind::Item || nearest_group(m, static_cast<int>(i)) != group) continue;
            if (!have) { first = n.box; have = true; continue; }
            if (std::abs(n.box.w - first.w) > 1.0f || std::abs(n.box.h - first.h) > 1.0f) return false;
        }
        return have;
    }

    // The two boxes share a span across the axis of `d`.
    bool aligned(const Rect& a, const Rect& b, Dir d) {
        const bool horizontal = d == Dir::Left || d == Dir::Right;
        const float a0 = horizontal ? a.y : a.x, a1 = horizontal ? a.bottom() : a.right();
        const float b0 = horizontal ? b.y : b.x, b1 = horizontal ? b.bottom() : b.right();
        return std::min(a1, b1) - std::max(a0, b0) > 0.0f;
    }

    // ---------------------------------------------------------------------------------------
    // How an element looks: the properties a focus treatment changes, on it and inside it
    // ---------------------------------------------------------------------------------------

    const Rml::PropertyId LOOK[] = {
        Rml::PropertyId::Color, Rml::PropertyId::BackgroundColor, Rml::PropertyId::ImageColor,
        Rml::PropertyId::BorderTopColor, Rml::PropertyId::BorderRightColor,
        Rml::PropertyId::BorderBottomColor, Rml::PropertyId::BorderLeftColor,
        Rml::PropertyId::BorderTopWidth, Rml::PropertyId::BorderRightWidth,
        Rml::PropertyId::BorderBottomWidth, Rml::PropertyId::BorderLeftWidth,
        Rml::PropertyId::Opacity, Rml::PropertyId::Decorator, Rml::PropertyId::BoxShadow,
        Rml::PropertyId::Filter, Rml::PropertyId::FontWeight, Rml::PropertyId::TextDecoration,
        Rml::PropertyId::Transform, Rml::PropertyId::Visibility, Rml::PropertyId::Display,
    };

    void look(Rml::Element* e, std::vector<Rml::Property>& out) {
        for (Rml::PropertyId id : LOOK) {
            const Rml::Property* p = e->GetProperty(id);
            out.push_back(p != nullptr ? *p : Rml::Property());
        }
        for (int i = 0; i < e->GetNumChildren(); ++i) {
            look(e->GetChild(i), out);
        }
    }

    // A transition would leave a focused element mid-change at a clock that never moves.
    void without_transitions(Rml::Element* e) {
        e->SetProperty("transition", "none");
        for (int i = 0; i < e->GetNumChildren(); ++i) {
            without_transitions(e->GetChild(i));
        }
    }

    void show_layers(Rml::Element* e) {
        if (e->GetAttribute<Rml::String>("data-nav", "") == "layer") {
            e->SetClass(OPEN_CLASS, true);
        }
        for (int i = 0; i < e->GetNumChildren(); ++i) {
            show_layers(e->GetChild(i));
        }
    }

    // ---------------------------------------------------------------------------------------
    // One document at one size
    // ---------------------------------------------------------------------------------------

    // The model as it is while `item` has the focus: every scrolling list it sits in scrolled to
    // show it, outermost first, as the shell does. Scrolling moves everything inside a list by
    // one amount, so it is applied to the model rather than laid out again.
    Model scrolled_to(const Model& m, int item) {
        Model out = m;
        std::vector<int> lists;
        for (int g = m.nodes[item].parent; g >= 0 && m.nodes[g].kind != Kind::Layer; g = m.nodes[g].parent) {
            if (m.nodes[g].kind == Kind::Group && m.nodes[g].scrolls) lists.push_back(g);
        }
        for (auto it = lists.rbegin(); it != lists.rend(); ++it) {
            const Rect view = out.nodes[*it].box;
            const Rect row = out.nodes[item].box;
            const float delta = scroll_to_show(row.y, row.h, view.y, view.h, 0.0f, 0.0f);
            if (delta == 0.0f) continue;
            for (int i = 0; i < static_cast<int>(out.nodes.size()); ++i) {
                if (encloses(out, *it, i)) out.nodes[i].box.y -= delta;
            }
        }
        return out;
    }

    // `to` lies past `from` in `d`: a plain move, not a wrap.
    bool ahead(const Rect& from, const Rect& to, Dir d) {
        switch (d) {
            case Dir::Right: return to.x >= from.right() - 1.0f;
            case Dir::Left:  return to.right() <= from.x + 1.0f;
            case Dir::Down:  return to.y >= from.bottom() - 1.0f;
            case Dir::Up:    return to.bottom() <= from.y + 1.0f;
        }
        return false;
    }

    void check_state(const RmlModel& rm, const Model& m, const Where& w, Totals& t) {
        std::vector<int> items;
        for (int i = 0; i < static_cast<int>(m.nodes.size()); ++i) {
            if (reachable(m, i)) items.push_back(i);
        }
        if (items.empty()) return;
        const Memory none;

        // 1. Reachable from the first focus, by the engine's own moves.
        std::set<int> seen;
        std::vector<int> queue{ first_focus(m, none) };
        seen.insert(queue.front());
        while (!queue.empty()) {
            const int at = queue.back();
            queue.pop_back();
            const Model here = scrolled_to(m, at);
            for (Dir d : DIRS) {
                if (adjusts(here, at, d)) continue;
                const int to = move(here, none, at, d);
                if (to >= 0 && seen.insert(to).second) queue.push_back(to);
            }
        }
        for (int i : items) {
            if (!seen.count(i)) fail(t, w, name(rm, i) + " unreachable");
        }

        for (int i : items) {
            ++t.elements;
            const Model here = scrolled_to(m, i);
            for (Dir d : DIRS) {
                if (adjusts(here, i, d)) continue;
                // 2. The engine against the oracle.
                const int engine = move(here, none, i, d);
                const int oracle = oracle_move(here, i, d);
                ++t.moves;
                if (engine != oracle) {
                    fail(t, w, name(rm, i) + " " + dir_name(d) + " engine=" + name(rm, engine) + " oracle=" + name(rm, oracle));
                    continue;
                }
                // 5. Out and back, in a list or a grid: a plain move to an aligned neighbor in the
                // same group comes back. A wrap is not a move one way; it is left out.
                if (engine == i) continue;
                const int g = nearest_group(here, i);
                if (g < 0 || nearest_group(here, engine) != g || !uniform(here, g)) continue;
                const Rect& a = here.nodes[i].box;
                const Rect& b = here.nodes[engine].box;
                if (!ahead(a, b, d) || !aligned(a, b, d) || adjusts(here, engine, opposite(d))) continue;
                const int back = move(scrolled_to(m, engine), none, engine, opposite(d));
                if (back != i) {
                    fail(t, w, name(rm, i) + " " + dir_name(d) + " then " + dir_name(opposite(d)) + " lands on " + name(rm, back));
                }
            }
        }
    }

    // THE FOOTER'S PROMPTS NEVER SHRINK (shell_model::hold_prompt_size; the user, 2026-10-13: "Those
    // are already relatively small, so maybe don't scale those down"). A plate's width and a label's
    // text are measured at 100% (the first pass) and must be that size below 100% and grow with the
    // interface above it.
    struct PromptSize {
        float plate = 0.0f;
        float text = 0.0f;
    };

    void check_prompt_size(Rml::Context* ctx, Rml::ElementDocument* doc, const Where& w, Totals& t) {
        static PromptSize full;
        const float dp = ctx->GetDensityIndependentPixelRatio();
        oot::ui::shell_model::hold_prompt_size(doc, dp);
        ctx->Update();
        Rml::ElementList plates;
        Rml::ElementList labels;
        doc->GetElementsByClassName(plates, "pr-plate");
        doc->GetElementsByClassName(labels, "pr-label");
        PromptSize now;
        for (Rml::Element* e : plates) now.plate = std::max(now.plate, e->GetBox().GetSize(Rml::BoxArea::Border).x);
        for (Rml::Element* e : labels) now.text = std::max(now.text, e->GetComputedValues().font_size());
        // A system button's drawing stands at the plates' height and is as wide as its own drawing
        // says for that height: the renderer stretches an SVG to its box, so a width that does not
        // follow the drawing would squash its stone and word.
        Rml::ElementList arts;
        doc->GetElementsByClassName(arts, "pr-art");
        float plate_height = 0.0f;
        for (Rml::Element* e : plates) plate_height = std::max(plate_height, e->GetBox().GetSize(Rml::BoxArea::Border).y);
        int shown_arts = 0;
        for (Rml::Element* e : arts) {
            if (!e->IsVisible(true)) continue;   // an empty slot of the footer's fixed length
            ++shown_arts;
            const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
            Rml::Vector2f intrinsic;
            float ratio = 0.0f;
            e->GetIntrinsicDimensions(intrinsic, ratio);
            if (ratio <= 0.0f || std::fabs(size.y - plate_height) > 0.5f || std::fabs(size.x - size.y * ratio) > 0.5f) {
                fail(t, w, "a system button's drawing is " + std::to_string(size.x) + " by " + std::to_string(size.y) +
                           ", not " + std::to_string(plate_height * ratio) + " by " + std::to_string(plate_height));
            }
        }
        if (shown_arts == 0) {
            fail(t, w, "the footer has no system button drawing to measure");
        }
        if (now.plate <= 0.0f || now.text <= 0.0f) {
            fail(t, w, "the footer has no plate or label to measure");
            return;
        }
        if (std::fabs(dp - 1.0f) < 0.001f) {
            full = now;
            return;
        }
        if (full.plate <= 0.0f) return;
        const float want = std::max(dp, 1.0f);
        if (std::fabs(now.plate - full.plate * want) > 0.5f || std::fabs(now.text - full.text * want) > 0.25f) {
            fail(t, w, "the footer's prompts are " + std::to_string(now.plate) + " wide with text " + std::to_string(now.text) +
                       ", not " + std::to_string(full.plate * want) + " and " + std::to_string(full.text * want));
        }
        oot::ui::shell_model::hold_prompt_size(doc, 1.0f);
        ctx->Update();
    }

    void check_document(Rml::Context* ctx, System& sys, const fs::path& path, const Where& w, Totals& t, bool report) {
        sys.errors.clear();
        Rml::ElementDocument* doc = ctx->LoadDocument(path.string());
        if (doc == nullptr) {
            fail(t, w, "does not load");
            return;
        }
        without_transitions(doc);
        show_layers(doc);
        doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        ctx->Update();
        for (const std::string& e : sys.errors) {
            fail(t, w, "rmlui error: " + e);
        }

        const RmlModel rm = build(doc);
        const long elements_before = t.elements, moves_before = t.moves;

        // The document with every layer closed, then each layer open in turn over it.
        std::vector<int> states{ -1 };
        for (int i = 0; i < static_cast<int>(rm.model.nodes.size()); ++i) {
            if (rm.model.nodes[i].kind == Kind::Layer) states.push_back(i);
        }
        for (int s : states) {
            Model m = rm.model;
            for (int i = 0; i < static_cast<int>(m.nodes.size()); ++i) {
                if (m.nodes[i].kind == Kind::Layer) m.nodes[i].open = s >= 0 && (i == s || encloses(m, i, s));
            }
            // 3. Back is defined on every layer.
            if (s >= 0 && rm.elements[s]->GetAttribute<Rml::String>("data-back", "").empty()) {
                fail(t, w, name(rm, s) + " has no Back (data-back)");
            }
            check_state(rm, m, w, t);
        }

        // 4. The focus shows.
        for (int i = 0; i < static_cast<int>(rm.model.nodes.size()); ++i) {
            const Node& n = rm.model.nodes[i];
            if (n.kind != Kind::Item || !n.enabled) continue;
            Rml::Element* e = rm.elements[i];
            std::vector<Rml::Property> plain, focused;
            look(e, plain);
            e->SetClass(FOCUS_CLASS, true);
            ctx->Update();
            look(e, focused);
            e->SetClass(FOCUS_CLASS, false);
            ctx->Update();
            if (plain == focused) fail(t, w, name(rm, i) + " looks the same focused");
        }

        // 5. The footer's prompts keep their size below 100%.
        if (path.filename() == "shell.rml") check_prompt_size(ctx, doc, w, t);

        if (report) {
            std::printf("  %s %s: elements=%ld moves=%ld\n", w.document.c_str(), w.size.c_str(),
                        t.elements - elements_before, t.moves - moves_before);
        }
        doc->Close();
        ctx->Update();
    }

    std::string percent(float dp) {
        return std::to_string(static_cast<int>(dp * 100.0f + 0.5f)) + "%";
    }

}

int main(int argc, char** argv) {
    fs::path assets = fs::path(OOT_SOURCE_DIR) / "assets";
    fs::path fixtures = fs::path(OOT_SOURCE_DIR) / "src" / "ui" / "navcheck" / "fixtures";
    bool report = false;
    std::vector<std::string> also;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--report") == 0) report = true;
        else if (std::strcmp(argv[i], "--assets") == 0 && i + 1 < argc) assets = argv[++i];
        else if (std::strcmp(argv[i], "--fixtures") == 0 && i + 1 < argc) fixtures = argv[++i];
        // A fixture checked once, by hand, beyond the list (the planted failure's proof).
        else if (std::strcmp(argv[i], "--also") == 0 && i + 1 < argc) also.push_back(argv[++i]);
        else {
            std::printf("NAVCHECK_FAIL usage: ui_navcheck [--report] [--assets dir] [--fixtures dir] [--also fixture.rml]\n");
            return 2;
        }
    }

    // What is checked: the fixtures, then every document not exempt.
    std::vector<fs::path> documents;
    for (const char* f : FIXTURES) documents.push_back(fixtures / f);
    for (const std::string& f : also) documents.push_back(fixtures / f);
    std::set<std::string> exempt(std::begin(EXEMPT), std::end(EXEMPT));
    std::set<std::string> present;
    const fs::path ui = assets / "ui";
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(ui, ec)) {
        if (entry.path().extension() != ".rml") continue;
        const std::string file = entry.path().filename().string();
        present.insert(file);
        if (!exempt.count(file)) documents.push_back(entry.path());
    }
    if (ec || present.empty()) {
        std::printf("NAVCHECK_FAIL no documents under %s\n", ui.string().c_str());
        return 1;
    }

    Totals t;
    std::string exempt_list;
    for (const std::string& e : exempt) {
        if (!present.count(e)) {
            std::printf("NAVCHECK_FAIL %s is exempt but is not a document; take it off the list\n", e.c_str());
            ++t.failures;
        }
        exempt_list += (exempt_list.empty() ? "" : ",") + e;
    }

    System sys;
    sys.report = report;
    NullRender render;
    Rml::SetSystemInterface(&sys);
    Rml::SetRenderInterface(&render);
    Rml::Initialise();   // RmlUi's own name, spelled as its authors spell it (lang-ok)
    oot::ui::fonts::load(assets.string());

    Rml::Context* ctx = Rml::CreateContext("navcheck", Rml::Vector2i(1920, 1080));
    if (ctx == nullptr) {
        std::printf("NAVCHECK_FAIL RmlUi would not make a context\n");
        Rml::Shutdown();
        return 1;
    }
    // The rows' data model (phase 95), as the shell makes it, so the documents that bind it load
    // as they do in the program; and its one template, drawn and read back, before any document.
    oot::ui::rows::create(ctx);
    // The shell's (phase 96), with a menu's worth of sections and prompts, so shell.rml is laid
    // out with its dial and footer as the program draws them.
    oot::ui::shell_model::create(ctx);
    {
        oot::ui::shell_model::State shell;
        shell.mode = "game";
        shell.placement = "left";
        shell.title = "Settings";
        shell.version = "0.0.0";
        shell.held = true;
        const char* ids[] = { "settings", "lighting", "hud", "controls", "moments", "about" };
        const int slots[] = { 0, 1, 2, 99, -2, -1 };
        for (int i = 0; i < 6; ++i) shell.sections.push_back({ ids[i], ids[i], "info", slots[i], i == 0 });
        shell.prompts.push_back({ "face", "A", "", "xbox-a", "Select", "accept" });
        shell.prompts.push_back({ "face", "", "circle", "ps-circle", "Back", "back" });
        shell.prompts.push_back({ "plate", "LB", "", "", "", "dial-prev" });
        shell.prompts.push_back({ "plate", "RB", "", "", "Change setting", "dial-next" });
        // The pad prompts' drawings (2026-10-13): the d-pad in the medallion, a system button whole.
        shell.prompts.push_back({ "medal", "", "dpad-left-right", "", "Change", "change" });
        shell.prompts.push_back({ "art", "", "sys-options", "", "Close", "close" });
        oot::ui::shell_model::set(shell);
    }
    int rows_passed = 0;
    if (const int rows_failed = oot::ui::rows::selftest(ctx, rows_passed); rows_failed > 0) {
        std::printf("NAVCHECK_FAIL the rows template: %d of %d checks failed\n", rows_failed, rows_failed + rows_passed);
        t.failures += rows_failed;
    }
    for (const fs::path& doc : documents) {
        if (!fs::exists(doc)) {
            std::printf("NAVCHECK_FAIL %s is listed but missing\n", doc.filename().string().c_str());
            ++t.failures;
            continue;
        }
        ++t.documents;
        for (float dp : INTERFACE_SIZES) {
            for (const Window& win : WINDOWS) {
                const int w = win.launcher ? static_cast<int>(win.w * dp + 0.5f) : win.w;
                const int h = win.launcher ? static_cast<int>(win.h * dp + 0.5f) : win.h;
                ctx->SetDimensions(Rml::Vector2i(w, h));
                ctx->SetDensityIndependentPixelRatio(dp);
                const Where where{ doc.filename().string(), std::to_string(w) + "x" + std::to_string(h) + " " + percent(dp) };
                check_document(ctx, sys, doc, where, t, report);
            }
        }
    }
    Rml::Shutdown();

    if (t.failures > 0) {
        std::printf("NAVCHECK_FAIL %d problem%s\n", t.failures, t.failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("NAVCHECK_OK documents=%d elements=%ld moves=%ld rows=%d exempt=%s\n",
                t.documents, t.elements, t.moves, rows_passed, exempt_list.c_str());
    return 0;
}
