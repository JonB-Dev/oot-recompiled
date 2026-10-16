// The shell's data model. See shell_model.h.

#include "ui/models/shell_model.h"

#include <algorithm>

#include <RmlUi/Core.h>

namespace oot::ui::shell_model {

    namespace {

        constexpr const char* MODEL = "shell";

        State g_state;
        std::vector<Prompt> g_guide;
        Rml::DataModelHandle g_handle;

        bool same(const Section& a, const Section& b) {
            return a.id == b.id && a.name == b.name && a.glyph == b.glyph && a.slot == b.slot && a.current == b.current;
        }

        bool same(const Prompt& a, const Prompt& b) {
            return a.kind == b.kind && a.key == b.key && a.shape == b.shape && a.tint == b.tint &&
                   a.label == b.label && a.action == b.action;
        }

        template <typename T>
        bool same(const std::vector<T>& a, const std::vector<T>& b) {
            if (a.size() != b.size()) {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i) {
                if (!same(a[i], b[i])) {
                    return false;
                }
            }
            return true;
        }

    }

    bool create(Rml::Context* context) {
        if (context == nullptr) {
            return false;
        }
        Rml::DataModelConstructor model = context->CreateDataModel(MODEL);
        if (!model) {
            return false;
        }
        if (auto s = model.RegisterStruct<Section>()) {
            s.RegisterMember("id", &Section::id);
            s.RegisterMember("name", &Section::name);
            s.RegisterMember("glyph", &Section::glyph);
            s.RegisterMember("slot", &Section::slot);
            s.RegisterMember("current", &Section::current);
        }
        if (auto p = model.RegisterStruct<Prompt>()) {
            p.RegisterMember("kind", &Prompt::kind);
            p.RegisterMember("key", &Prompt::key);
            p.RegisterMember("shape", &Prompt::shape);
            p.RegisterMember("tint", &Prompt::tint);
            p.RegisterMember("label", &Prompt::label);
            p.RegisterMember("action", &Prompt::action);
        }
        model.RegisterArray<std::vector<Section>>();
        model.RegisterArray<std::vector<Prompt>>();
        model.Bind("mode", &g_state.mode);
        model.Bind("placement", &g_state.placement);
        model.Bind("title", &g_state.title);
        model.Bind("version", &g_state.version);
        model.Bind("held", &g_state.held);
        model.Bind("sections", &g_state.sections);
        model.Bind("prompts", &g_state.prompts);
        g_guide.assign(GUIDE_SLOTS, Prompt{});
        model.Bind("guide", &g_guide);
        g_handle = model.GetModelHandle();
        return true;
    }

    void set(const State& wanted) {
        // THE FOOTER NEVER SHRINKS (phase 111): it is always PROMPT_SLOTS long, the unused slots
        // empty and hidden by the stylesheet. A list that shrank had RmlUi evaluate the removed
        // prompts' bindings once more ("prompts[3].tint" out of bounds) before dropping them.
        State next = wanted;
        next.prompts.resize(std::max(next.prompts.size(), PROMPT_SLOTS));
        const auto mark = [](bool changed, const char* name) {
            if (changed && g_handle) {
                g_handle.DirtyVariable(name);
            }
        };
        mark(next.mode != g_state.mode, "mode");
        mark(next.placement != g_state.placement, "placement");
        mark(next.title != g_state.title, "title");
        mark(next.version != g_state.version, "version");
        mark(next.held != g_state.held, "held");
        mark(!same(next.sections, g_state.sections), "sections");
        mark(!same(next.prompts, g_state.prompts), "prompts");
        g_state = next;
    }

    const State& get() {
        return g_state;
    }

    void set_guide(const std::vector<Prompt>& prompts) {
        std::vector<Prompt> next = prompts;
        next.resize(std::max(next.size(), GUIDE_SLOTS));
        if (!same(next, g_guide)) {
            g_guide = std::move(next);
            if (g_handle) {
                g_handle.DirtyVariable("guide");
            }
        }
    }

    void release() {
        g_handle = Rml::DataModelHandle();
        g_state = State{};
        g_guide.clear();
    }

    void hold_prompt_size(Rml::ElementDocument* doc, float dp) {
        if (doc != nullptr) {
            doc->SetClass("prompts-full-size", dp < 0.999f);
        }
    }

}
