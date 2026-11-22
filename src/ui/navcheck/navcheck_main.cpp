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
#include "ui/ui_moment_row.h"
#include "ui/ui_text_row.h"
#include "ui_tokens.h"
#include "ui/ui_survey.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        int surveys = 0;   // documents whose contour survey was checked
        long rows_checked = 0;   // rows laid out and checked for overlap (--rows)
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

    // THE ROWS NEVER OVERLAP (the user, 2026-11-20: "it also must work and never overlap in every
    // game menu ui scale size"). Every rows page's real rows (OoTRecompiled --ui-rows-json), laid
    // into the page box the shell gives a page in that placement, at every interface size and
    // window, narrow or not as the shell decides (rows::stacked): in each row the label and the
    // value share no pixel, and neither, nor any choice, reaches past the row's right edge.
    struct RowsPage {
        std::string container;
        int list = 0;
        std::string name;
        std::vector<oot::ui::rows::Row> rows;
    };

    std::vector<RowsPage> read_rows(const fs::path& file) {
        std::vector<RowsPage> pages;
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            std::vector<std::string> parts;
            size_t from = 0;
            for (int field = 0; field < 3; ++field) {
                const size_t tab = line.find('\t', from);
                if (tab == std::string::npos) break;
                parts.push_back(line.substr(from, tab - from));
                from = tab + 1;
            }
            if (parts.size() != 3) continue;
            pages.push_back({ parts[0], std::atoi(parts[1].c_str()), parts[2], oot::ui::rows::from_json(line.substr(from)) });
        }
        return pages;
    }

    // The document under ui/ that holds an element with this id, read from the files themselves.
    fs::path document_with(const fs::path& ui, const std::string& id) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(ui, ec)) {
            if (entry.path().extension() != ".rml") continue;
            std::ifstream text(entry.path());
            const std::string markup((std::istreambuf_iterator<char>(text)), std::istreambuf_iterator<char>());
            if (markup.find("id=\"" + id + "\"") != std::string::npos) return entry.path();
        }
        return {};
    }

    // The page box's width the shell lays out at this size in the shell model's placement.
    float page_width(Rml::Context* ctx, const fs::path& shell) {
        Rml::ElementDocument* doc = ctx->LoadDocument(shell.string());
        if (doc == nullptr) return 0.0f;
        without_transitions(doc);
        doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        ctx->Update();
        Rml::Element* host = doc->GetElementById("sh-host");
        const float width = host != nullptr ? host->GetBox().GetSize(Rml::BoxArea::Border).x : 0.0f;
        doc->Close();
        ctx->Update();
        return width;
    }

    // Where an element's drawing starts on the left: the leftmost line of words, or box that draws
    // something of its own (a fill, a border, a decorator), inside it. Its own box is not counted:
    // a value's box spans the row whatever its words do. Infinity when nothing inside is drawn.
    float ink_left(Rml::Element* e) {
        float left = INFINITY;
        for (int c = 0; c < e->GetNumChildren(); ++c) {
            Rml::Element* child = e->GetChild(c);
            if (!child->IsVisible()) continue;
            if (auto* text = rmlui_dynamic_cast<Rml::ElementText*>(child)) {
                const float at = text->GetAbsoluteOffset(Rml::BoxArea::Border).x;
                for (const Rml::ElementText::Line& line : text->GetLines()) {
                    if (!line.text.empty()) left = std::min(left, at + line.position.x);
                }
                continue;
            }
            const Rml::Style::ComputedValues& look = child->GetComputedValues();
            const bool draws = look.background_color().alpha > 0 || look.has_decorator() ||
                               look.border_left_width() > 0.0f || look.border_top_width() > 0.0f;
            const Rml::Vector2f size = child->GetBox().GetSize(Rml::BoxArea::Border);
            if (draws && size.x >= 0.5f && size.y >= 0.5f) {
                left = std::min(left, child->GetAbsoluteOffset(Rml::BoxArea::Border).x);
            }
            left = std::min(left, ink_left(child));
        }
        return left;
    }

    void check_rows(Rml::Context* ctx, const fs::path& path, const RowsPage& page, float width, float dp,
                    const Where& w, Totals& t) {
        Rml::ElementDocument* doc = ctx->LoadDocument(path.string());
        if (doc == nullptr) {
            fail(t, w, "does not load");
            return;
        }
        without_transitions(doc);
        doc->SetClass("in-shell", true);
        doc->SetClass("narrow", oot::ui::rows::stacked(width, dp));
        doc->SetProperty(Rml::PropertyId::Position, Rml::Property(Rml::Style::Position::Absolute));
        doc->SetProperty(Rml::PropertyId::Left, Rml::Property(0.0f, Rml::Unit::PX));
        doc->SetProperty(Rml::PropertyId::Top, Rml::Property(0.0f, Rml::Unit::PX));
        doc->SetProperty(Rml::PropertyId::Width, Rml::Property(width, Rml::Unit::PX));
        const auto list = static_cast<oot::ui::rows::List>(page.list);
        oot::ui::rows::install(doc->GetElementById(page.container), list);
        // An action row's words show only while it is focused, so every action row is laid out
        // focused: its words are checked like any other value.
        std::vector<oot::ui::rows::Row> rows_shown = page.rows;
        for (oot::ui::rows::Row& r : rows_shown) {
            if (r.shape == oot::ui::rows::Action) r.focused = true;
        }
        oot::ui::rows::set(list, rows_shown);
        doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        ctx->Update();
        ctx->Update();

        struct Box {
            float l = 0, t = 0, r = 0, b = 0;
            bool empty() const { return r - l < 0.5f || b - t < 0.5f; }
        };
        const auto box_of = [](Rml::Element* e) {
            const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
            return Box{ at.x, at.y, at.x + size.x, at.y + size.y };
        };
        Rml::ElementList rows;
        doc->QuerySelectorAll(rows, ".row");
        for (Rml::Element* row : rows) {
            Rml::Element* label = row->QuerySelector(".label");
            Rml::Element* value = row->QuerySelector(".value");
            if (label == nullptr || value == nullptr) continue;
            const Box r = box_of(row);
            const float right = r.r - row->GetBox().GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Right) -
                                row->GetBox().GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Right) + 0.5f;
            const Box lb = box_of(label);
            const Box vb = box_of(value);
            const std::string what = page.name + " row \"" + label->GetInnerRML() + "\"";
            if (!lb.empty() && !vb.empty() && lb.l < vb.r && vb.l < lb.r && lb.t < vb.b && vb.t < lb.b) {
                fail(t, w, what + ": its label and its value overlap");
            }
            if (lb.r > right) fail(t, w, what + ": its label runs past the row");
            if (!vb.empty() && vb.r > right) fail(t, w, what + ": its value runs past the row");
            Rml::ElementList picks;
            row->QuerySelectorAll(picks, ".pick");
            for (Rml::Element* pick : picks) {
                if (box_of(pick).r > right) {
                    fail(t, w, what + ": a choice runs past the row");
                    break;
                }
            }

            // The value's parts share one line whenever that line fits in the row (the user,
            // 2026-11-20: "keeping all value options a row and stacking the label to its own
            // line"): every choice, or the stepper's arrows, ticks and words, side by side.
            std::vector<Box> parts;
            for (int c = 0; c < value->GetNumChildren(); ++c) {
                Rml::Element* part = value->GetChild(c);
                if (!part->IsVisible()) continue;
                if (part->IsClassSet("choices")) {
                    for (Rml::Element* pick : picks) parts.push_back(box_of(pick));
                }
                else {
                    parts.push_back(box_of(part));
                }
            }
            parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Box& b) { return b.empty(); }), parts.end());
            if (parts.size() > 1) {
                float width_sum = 0.0f, lowest_top = parts[0].t, highest_bottom = parts[0].b;
                for (const Box& p : parts) {
                    width_sum += p.r - p.l;
                    lowest_top = std::max(lowest_top, p.t);
                    highest_bottom = std::min(highest_bottom, p.b);
                }
                if (lowest_top >= highest_bottom && width_sum <= right - vb.l) {
                    fail(t, w, what + ": its value is broken over lines though it fits on one");
                }
            }

            // Stacked, the value starts under its label (the user, 2026-11-20: "lets align to the
            // left here for this text"): its first mark, a box or a line of words, at the label's
            // left edge.
            if (doc->IsClassSet("narrow")) {
                const float label_at = ink_left(label);
                const float value_at = ink_left(value);
                if (std::isfinite(label_at) && std::isfinite(value_at) && value_at > label_at + 2.0f) {
                    fail(t, w, what + ": its value does not start under its label");
                }
            }
        }
        doc->Close();
        ctx->Update();
    }

    // THE MOMENTS LIST'S ROWS (the user, 2026-11-20: "the thumbnails here are squished and the
    // location and date could fall to new row so all three text lines stack and the photo thumb
    // sits to left of them"). Rows built by the program's own builder, with long words, laid into
    // the page's box: the picture keeps its size, the words stay right of it and inside the row,
    // and in a narrow page the place and date sit under the name, starting where it starts.
    void check_moments(Rml::Context* ctx, const fs::path& path, float width, float dp, const Where& w, Totals& t) {
        Rml::ElementDocument* doc = ctx->LoadDocument(path.string());
        if (doc == nullptr) {
            fail(t, w, "does not load");
            return;
        }
        without_transitions(doc);
        const bool narrow = oot::ui::rows::stacked(width, dp);
        doc->SetClass("in-shell", true);
        doc->SetClass("narrow", narrow);
        doc->SetProperty(Rml::PropertyId::Position, Rml::Property(Rml::Style::Position::Absolute));
        doc->SetProperty(Rml::PropertyId::Left, Rml::Property(0.0f, Rml::Unit::PX));
        doc->SetProperty(Rml::PropertyId::Top, Rml::Property(0.0f, Rml::Unit::PX));
        doc->SetProperty(Rml::PropertyId::Width, Rml::Property(width, Rml::Unit::PX));
        Rml::Element* list = doc->GetElementById("moment-rows");
        if (list == nullptr) {
            fail(t, w, "no #moment-rows");
            doc->Close();
            return;
        }
        const oot::ui::moment_row::Parts samples[] = {
            { "a", "", "A place with a long name at 18:29", "Autosave, the newest", "A region with a long name \xC2\xB7 2026-11-20 18:34" },
            { "b", "", "Short at 12:00", "Slot 1", "Region \xC2\xB7 2026-11-20 02:17" },
            { "c", "", "Slot 3", "Empty", "" },
        };
        std::string rml;
        for (const auto& sample : samples) rml += oot::ui::moment_row::rml(sample);
        list->SetInnerRML(rml);
        doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        ctx->Update();
        ctx->Update();

        const auto left_of = [](Rml::Element* e) { return e->GetAbsoluteOffset(Rml::BoxArea::Border).x; };
        const auto right_of = [](Rml::Element* e) {
            return e->GetAbsoluteOffset(Rml::BoxArea::Border).x + e->GetBox().GetSize(Rml::BoxArea::Border).x;
        };
        const auto top_of = [](Rml::Element* e) { return e->GetAbsoluteOffset(Rml::BoxArea::Border).y; };
        const auto bottom_of = [](Rml::Element* e) {
            return e->GetAbsoluteOffset(Rml::BoxArea::Border).y + e->GetBox().GetSize(Rml::BoxArea::Border).y;
        };
        Rml::ElementList rows;
        doc->QuerySelectorAll(rows, ".row.mrow");
        if (rows.size() != std::size(samples)) fail(t, w, "the sample rows did not all lay out");
        for (Rml::Element* row : rows) {
            Rml::Element* thumb = row->QuerySelector(".thumb");
            Rml::Element* label = row->QuerySelector(".label");
            Rml::Element* value = row->QuerySelector(".value");
            if (thumb == nullptr || label == nullptr || value == nullptr) {
                fail(t, w, "a moment row lacks its picture, name or value");
                continue;
            }
            const std::string what = "moment row \"" + row->GetId() + "\"";
            const Rml::Vector2f picture = thumb->GetBox().GetSize(Rml::BoxArea::Content);
            if (picture.x < oot::ui::tokens::thumb_width * dp - 0.5f || picture.y < oot::ui::tokens::thumb_height * dp - 0.5f) {
                fail(t, w, what + ": its picture is squeezed to " + std::to_string(picture.x) + " by " + std::to_string(picture.y));
            }
            const float right = right_of(row) - row->GetBox().GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Right) -
                                row->GetBox().GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Right) + 0.5f;
            if (left_of(label) < right_of(thumb) || left_of(value) < right_of(thumb)) {
                fail(t, w, what + ": its words run under its picture");
            }
            if (right_of(label) > right || right_of(value) > right) fail(t, w, what + ": its words run past the row");
            if (narrow) {
                if (!value->GetInnerRML().empty() && top_of(value) < bottom_of(label) - 0.5f) {
                    fail(t, w, what + ": its place and date are not under its name");
                }
                const float label_at = ink_left(label);
                const float value_at = ink_left(value);
                if (std::isfinite(label_at) && std::isfinite(value_at) && std::abs(value_at - label_at) > 2.0f) {
                    fail(t, w, what + ": its place and date do not start under its name");
                }
            }
            else if (left_of(value) < right_of(label) && left_of(label) < right_of(value) &&
                     top_of(value) < bottom_of(label) && top_of(label) < bottom_of(value)) {
                fail(t, w, what + ": its name and its place overlap");
            }
            ++t.rows_checked;
        }
        doc->Close();
        ctx->Update();
    }

    // THE WARPS LIST'S ROWS (the user, 2026-11-22: "debgging has same row instead of stack issue").
    // Rows built by the program's own builder (ui_text_row.h), with long words, laid into the page's
    // box: in a narrow page the value sits under the label and starts where it starts; in a wide one
    // the two never overlap; neither runs past the row.
    void check_warps(Rml::Context* ctx, const fs::path& path, float width, float dp, const Where& w, Totals& t) {
        Rml::ElementDocument* doc = ctx->LoadDocument(path.string());
        if (doc == nullptr) {
            fail(t, w, "does not load");
            return;
        }
        without_transitions(doc);
        const bool narrow = oot::ui::rows::stacked(width, dp);
        doc->SetClass("in-shell", true);
        doc->SetClass("narrow", narrow);
        doc->SetProperty(Rml::PropertyId::Position, Rml::Property(Rml::Style::Position::Absolute));
        doc->SetProperty(Rml::PropertyId::Left, Rml::Property(0.0f, Rml::Unit::PX));
        doc->SetProperty(Rml::PropertyId::Top, Rml::Property(0.0f, Rml::Unit::PX));
        doc->SetProperty(Rml::PropertyId::Width, Rml::Property(width, Rml::Unit::PX));
        Rml::Element* list = doc->GetElementById("warp-rows");
        if (list == nullptr) {
            fail(t, w, "no #warp-rows");
            doc->Close();
            return;
        }
        const oot::ui::text_row::Parts samples[] = {
            { "wrow-0", "", "Time of day", "<span class=\"step prev\">&#8249;</span><span class=\"shown\">Sunrise</span>"
                                           "<span class=\"step\">&#8250;</span>" },
            { "wrow-1", "here", "A place with a long name, its second entrance", "Entrance 0x0123, room 4" },
            { "wrow-2", "", "Short", "Go" },
        };
        std::string rml;
        for (const auto& sample : samples) rml += oot::ui::text_row::rml(sample);
        list->SetInnerRML(rml);
        doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        ctx->Update();
        ctx->Update();

        const auto left_of = [](Rml::Element* e) { return e->GetAbsoluteOffset(Rml::BoxArea::Border).x; };
        const auto right_of = [](Rml::Element* e) {
            return e->GetAbsoluteOffset(Rml::BoxArea::Border).x + e->GetBox().GetSize(Rml::BoxArea::Border).x;
        };
        const auto top_of = [](Rml::Element* e) { return e->GetAbsoluteOffset(Rml::BoxArea::Border).y; };
        const auto bottom_of = [](Rml::Element* e) {
            return e->GetAbsoluteOffset(Rml::BoxArea::Border).y + e->GetBox().GetSize(Rml::BoxArea::Border).y;
        };
        Rml::ElementList rows;
        doc->QuerySelectorAll(rows, "#warp-rows .row");
        if (rows.size() != std::size(samples)) fail(t, w, "the sample warp rows did not all lay out");
        for (Rml::Element* row : rows) {
            Rml::Element* label = row->QuerySelector(".label");
            Rml::Element* value = row->QuerySelector(".value");
            if (label == nullptr || value == nullptr) {
                fail(t, w, "a warp row lacks its label or value");
                continue;
            }
            const std::string what = "warp row \"" + row->GetId() + "\"";
            const float right = right_of(row) - row->GetBox().GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Right) -
                                row->GetBox().GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Right) + 0.5f;
            if (right_of(label) > right || right_of(value) > right) fail(t, w, what + ": its words run past the row");
            if (narrow) {
                if (top_of(value) < bottom_of(label) - 0.5f) fail(t, w, what + ": its value is not under its label");
                const float label_at = ink_left(label);
                const float value_at = ink_left(value);
                if (std::isfinite(label_at) && std::isfinite(value_at) && std::abs(value_at - label_at) > 2.0f) {
                    fail(t, w, what + ": its value does not start under its label");
                }
            }
            else if (left_of(value) < right_of(label) && left_of(label) < right_of(value) &&
                     top_of(value) < bottom_of(label) && top_of(label) < bottom_of(value)) {
                fail(t, w, what + ": its label and value overlap");
            }
            ++t.rows_checked;
        }
        doc->Close();
        ctx->Update();
    }

    // THE CONTOUR SURVEY COVERS ITS GROUND (the user, 2026-11-20: maximized, the drawing stopped
    // part way and the right side had none). Every .survey in the document, laid by the program's
    // own survey::lay, has the ground it sits on (its parent) probed every 8 pixels and along the
    // far edges: each point must fall inside one of its .survey-tile copies. The probe knows nothing
    // of how the copies are counted.
    void check_survey(Rml::Context* ctx, System& sys, const fs::path& path, const Where& w, Totals& t) {
        sys.errors.clear();
        Rml::ElementDocument* doc = ctx->LoadDocument(path.string());
        if (doc == nullptr) {
            fail(t, w, "does not load");
            return;
        }
        without_transitions(doc);
        doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        ctx->Update();
        if (oot::ui::survey::lay(ctx)) {
            ctx->Update();
        }

        Rml::ElementList surveys;
        doc->QuerySelectorAll(surveys, ".survey");
        for (Rml::Element* survey : surveys) {
            Rml::Element* ground = survey->GetParentNode();
            const Rml::Vector2f origin = ground->GetAbsoluteOffset(Rml::BoxArea::Padding);
            const float width = ground->GetClientWidth();
            const float height = ground->GetClientHeight();
            Rml::ElementList tiles;
            survey->QuerySelectorAll(tiles, ".survey-tile");
            auto covered = [&](float x, float y) {
                return std::any_of(tiles.begin(), tiles.end(), [&](Rml::Element* tile) {
                    const Rml::Vector2f at = tile->GetAbsoluteOffset(Rml::BoxArea::Border);
                    const Rml::Vector2f size = tile->GetBox().GetSize(Rml::BoxArea::Border);
                    return x >= at.x && x < at.x + size.x && y >= at.y && y < at.y + size.y;
                });
            };
            std::vector<float> xs, ys;
            for (float x = 0.5f; x < width; x += 8.0f) xs.push_back(x);
            for (float y = 0.5f; y < height; y += 8.0f) ys.push_back(y);
            if (width >= 1.0f) xs.push_back(width - 0.5f);
            if (height >= 1.0f) ys.push_back(height - 0.5f);
            bool bare = false;
            for (float y : ys) {
                for (float x : xs) {
                    if (!covered(origin.x + x, origin.y + y)) {
                        fail(t, w, "the contour survey (" + std::to_string(static_cast<int>(width)) + " by " +
                                       std::to_string(static_cast<int>(height)) + ") is bare at " +
                                       std::to_string(static_cast<int>(x)) + "," + std::to_string(static_cast<int>(y)));
                        bare = true;
                        break;
                    }
                }
                if (bare) break;
            }
        }
        doc->Close();
        ctx->Update();
    }

}

int main(int argc, char** argv) {
    fs::path assets = fs::path(OOT_SOURCE_DIR) / "assets";
    fs::path fixtures = fs::path(OOT_SOURCE_DIR) / "src" / "ui" / "navcheck" / "fixtures";
    bool report = false;
    std::vector<std::string> also;
    fs::path rows_file;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--report") == 0) report = true;
        else if (std::strcmp(argv[i], "--assets") == 0 && i + 1 < argc) assets = argv[++i];
        else if (std::strcmp(argv[i], "--fixtures") == 0 && i + 1 < argc) fixtures = argv[++i];
        // A fixture checked once, by hand, beyond the list (the planted failure's proof).
        else if (std::strcmp(argv[i], "--also") == 0 && i + 1 < argc) also.push_back(argv[++i]);
        // Every rows page's real rows, from OoTRecompiled --ui-rows-json, checked for overlap.
        else if (std::strcmp(argv[i], "--rows") == 0 && i + 1 < argc) rows_file = argv[++i];
        else {
            std::printf("NAVCHECK_FAIL usage: ui_navcheck [--report] [--assets dir] [--fixtures dir] [--also fixture.rml] [--rows rows.txt]\n");
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
        shell.bar = true;   // in the game the window's bar is over the menu, which starts under it
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

    // The contour survey in every document that lays one, exempt or not, with the shell in the
    // launcher's own window, where its ground is widest.
    {
        oot::ui::shell_model::State shell = oot::ui::shell_model::get();
        shell.mode = "launcher";
        shell.placement = "launcher";
        oot::ui::shell_model::set(shell);
    }
    for (const std::string& file : present) {
        const fs::path doc = ui / file;
        std::ifstream text(doc);
        const std::string markup((std::istreambuf_iterator<char>(text)), std::istreambuf_iterator<char>());
        if (markup.find("\"survey\"") == std::string::npos) continue;
        ++t.surveys;
        for (float dp : INTERFACE_SIZES) {
            for (const Window& win : WINDOWS) {
                const int w = win.launcher ? static_cast<int>(win.w * dp + 0.5f) : win.w;
                const int h = win.launcher ? static_cast<int>(win.h * dp + 0.5f) : win.h;
                ctx->SetDimensions(Rml::Vector2i(w, h));
                ctx->SetDensityIndependentPixelRatio(dp);
                check_survey(ctx, sys, doc, Where{ file, std::to_string(w) + "x" + std::to_string(h) + " " + percent(dp) }, t);
            }
        }
    }

    // The rows pages, docked in the game and in the launcher's own window. The file is the
    // program's own (--ui-rows-json, after each link); before the first link there is none yet.
    if (!rows_file.empty() && !fs::exists(rows_file)) {
        std::printf("navcheck: no rows file yet at %s; the rows are checked after the program links\n",
                    rows_file.string().c_str());
    }
    else if (!rows_file.empty()) {
        const std::vector<RowsPage> pages = read_rows(rows_file);
        if (pages.empty()) {
            std::printf("NAVCHECK_FAIL no rows read from %s\n", rows_file.string().c_str());
            ++t.failures;
        }
        for (const char* placement : { "left", "launcher" }) {
            oot::ui::shell_model::State shell = oot::ui::shell_model::get();
            shell.placement = placement;
            shell.mode = std::strcmp(placement, "launcher") == 0 ? "launcher" : "game";
            shell.bar = shell.mode == "game";
            oot::ui::shell_model::set(shell);
            for (float dp : INTERFACE_SIZES) {
                for (const Window& win : WINDOWS) {
                    const int w = win.launcher ? static_cast<int>(win.w * dp + 0.5f) : win.w;
                    const int h = win.launcher ? static_cast<int>(win.h * dp + 0.5f) : win.h;
                    ctx->SetDimensions(Rml::Vector2i(w, h));
                    ctx->SetDensityIndependentPixelRatio(dp);
                    const float width = page_width(ctx, ui / "shell.rml");
                    for (const RowsPage& page : pages) {
                        const fs::path doc = document_with(ui, page.container);
                        const Where where{ page.name + " (" + placement + ")",
                                           std::to_string(w) + "x" + std::to_string(h) + " " + percent(dp) };
                        if (doc.empty() || width <= 0.0f) {
                            fail(t, where, "no document or no page box for " + page.container);
                            continue;
                        }
                        check_rows(ctx, doc, page, width, dp, where, t);
                        t.rows_checked += static_cast<long>(page.rows.size());
                    }
                    if (width > 0.0f) {
                        check_moments(ctx, ui / "moments.rml", width, dp,
                                      Where{ std::string("moments (") + placement + ")",
                                             std::to_string(w) + "x" + std::to_string(h) + " " + percent(dp) }, t);
                        check_warps(ctx, ui / "warps.rml", width, dp,
                                    Where{ std::string("warps (") + placement + ")",
                                           std::to_string(w) + "x" + std::to_string(h) + " " + percent(dp) }, t);
                    }
                }
            }
        }
    }
    Rml::Shutdown();

    if (t.failures > 0) {
        std::printf("NAVCHECK_FAIL %d problem%s\n", t.failures, t.failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("NAVCHECK_OK documents=%d elements=%ld moves=%ld rows=%d surveys=%d laid_rows=%ld exempt=%s\n",
                t.documents, t.elements, t.moves, rows_passed, t.surveys, t.rows_checked, exempt_list.c_str());
    return 0;
}
