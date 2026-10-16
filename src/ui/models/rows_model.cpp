// The rows as data. See rows_model.h.

#include "ui/models/rows_model.h"

#include <RmlUi/Core.h>

#include <array>
#include <cstdio>

namespace oot::ui::rows {

    namespace {

        constexpr const char* MODEL = "rows";
        constexpr std::array<const char*, 5> NAMES = { "launch", "settings", "lighting", "hud", "debug" };

        std::array<std::vector<Row>, 5> g_lists;
        Rml::DataModelHandle g_handle;

        size_t at(List list) {
            return static_cast<size_t>(list);
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
        if (auto choice = model.RegisterStruct<Choice>()) {
            choice.RegisterMember("label", &Choice::label);
            choice.RegisterMember("chosen", &Choice::chosen);
        }
        model.RegisterArray<std::vector<Choice>>();
        model.RegisterArray<std::vector<int>>();
        if (auto row = model.RegisterStruct<Row>()) {
            row.RegisterMember("n", &Row::n);
            row.RegisterMember("label", &Row::label);
            row.RegisterMember("value", &Row::value);
            row.RegisterMember("shape", &Row::shape);
            row.RegisterMember("low", &Row::low);
            row.RegisterMember("high", &Row::high);
            row.RegisterMember("on", &Row::on);
            row.RegisterMember("note", &Row::note);
            row.RegisterMember("section", &Row::section);
            row.RegisterMember("focused", &Row::focused);
            row.RegisterMember("icon", &Row::icon);
            row.RegisterMember("choices", &Row::choices);
            row.RegisterMember("ticks", &Row::ticks);
        }
        model.RegisterArray<std::vector<Row>>();
        for (size_t i = 0; i < NAMES.size(); ++i) {
            model.Bind(NAMES[i], &g_lists[i]);
        }
        g_handle = model.GetModelHandle();
        return true;
    }

    // THE ONE ROW TEMPLATE. What the string builders wrote per row (stepper_html, check_html,
    // action_html, retired in phase 95), written once and bound: a divider when the row carries a
    // section, the label, the value drawn by its shape, the note under it. The ids are the ones the
    // shell's clicks and focus have always read; an id a shape does not use is left empty, so a
    // check's next- id and a stepper's never both exist.
    void install(Rml::Element* container, List list) {
        if (container == nullptr) {
            return;
        }
        const std::string name = NAMES[at(list)];
        container->SetInnerRML(
            "<div class=\"rowset\" data-for=\"row : " + name + "\">"
                "<div class=\"divider\" data-if=\"row.section != ''\"><span class=\"divider-label\">{{row.section}}</span></div>"
                "<div class=\"row\" data-attr-id=\"'row-' + row.n\" data-class-noted=\"row.note != ''\" data-class-acting=\"row.shape == 3\" data-class-on=\"row.focused\""
                    " data-class-plated=\"row.icon != ''\">"
                    "<span class=\"mi\" data-if=\"row.icon != ''\"><span class=\"mi-ic\" data-style-decorator=\"'svg(../icons/' + row.icon + '.svg)'\"></span></span>"
                    "<span class=\"label\" data-attr-id=\"'label-' + row.n\">{{row.label}}</span>"
                    "<span class=\"value\" data-attr-id=\"'value-' + row.n\">"
                        "<span class=\"choices\" data-if=\"row.shape == 1 && row.choices.size > 0\">"
                            "<span class=\"pick\" data-for=\"c, ci : row.choices\" data-class-in=\"c.chosen\""
                                " data-attr-id=\"'pick-' + row.n + '-' + ci\">{{c.label}}</span>"
                        "</span>"
                        "<span class=\"step\" data-if=\"row.shape == 1 && row.choices.size == 0\" data-class-spent=\"row.low\""
                            " data-attr-id=\"row.shape == 1 ? 'prev-' + row.n : ''\">&#8249;</span>"
                        "<span class=\"ticks\" data-if=\"row.shape == 1 && row.ticks.size > 0\">"
                            "<span class=\"tk\" data-for=\"t : row.ticks\" data-class-in=\"t == 1\"></span>"
                        "</span>"
                        "<span class=\"shown\" data-if=\"row.shape == 1 && row.choices.size == 0\">{{row.value}}</span>"
                        "<span class=\"step\" data-if=\"row.shape == 1 && row.choices.size == 0\" data-class-spent=\"row.high\""
                            " data-attr-id=\"row.shape == 1 ? 'next-' + row.n : ''\">&#8250;</span>"
                        "<span class=\"shown\" data-if=\"row.shape == 2\"><span class=\"check\" data-class-ticked=\"row.on\""
                            " data-attr-id=\"row.shape == 2 ? 'next-' + row.n : ''\"><span class=\"tick\"></span></span></span>"
                        "<span class=\"step-gap\" data-if=\"row.shape == 2\"></span>"
                        "<span class=\"act\" data-if=\"row.shape == 3\">{{row.value}}</span>"
                        "<span data-if=\"row.shape == 4\">{{row.value}}</span>"
                    "</span>"
                    "<div class=\"row-note\" data-if=\"row.note != ''\">{{row.note}}</div>"
                "</div>"
            "</div>");
    }

    void set(List list, std::vector<Row> rows) {
        g_lists[at(list)] = std::move(rows);
        if (g_handle) {
            g_handle.DirtyVariable(NAMES[at(list)]);
        }
    }

    void focus(List list, int n) {
        bool changed = false;
        for (Row& row : g_lists[at(list)]) {
            const bool focused = row.n == n;
            changed = changed || (row.focused != focused);
            row.focused = focused;
        }
        if (changed && g_handle) {
            g_handle.DirtyVariable(NAMES[at(list)]);
        }
    }

    void release() {
        g_handle = Rml::DataModelHandle();
        for (std::vector<Row>& list : g_lists) {
            list.clear();
        }
    }

    int selftest(Rml::Context* context, int& passed) {
        int failed = 0;
        auto check = [&](bool ok, const char* what) {
            if (ok) ++passed;
            else { std::printf("  rows: %s\n", what); ++failed; }
        };
        if (context == nullptr) {
            check(false, "no context");
            return failed;
        }
        if (!context->GetDataModel(MODEL)) {
            create(context);
        }
        Rml::ElementDocument* doc = context->LoadDocumentFromMemory(
            "<rml><head><style>body { display: block; }</style></head>"
            "<body><div id=\"list\" data-model=\"rows\"></div></body></rml>", "rows-selftest");
        if (doc == nullptr) {
            check(false, "the test document would not load");
            return failed;
        }
        install(doc->GetElementById("list"), List::Settings);
        doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);

        std::vector<Row> rows(4);
        rows[0].n = 0; rows[0].label = "Window"; rows[0].value = "Windowed"; rows[0].shape = Stepper; rows[0].low = true;
        rows[0].section = "Place";
        rows[1].n = 1; rows[1].label = "Hearts"; rows[1].shape = Check; rows[1].on = true;
        rows[2].n = 3; rows[2].label = "Controls"; rows[2].value = "Click or press Enter to open"; rows[2].shape = Action;
        rows[3].n = 4; rows[3].label = "ROM"; rows[3].value = "Not found & <unread>"; rows[3].shape = Text;
        rows[3].note = "What it means";
        set(List::Settings, rows);
        focus(List::Settings, 1);
        context->Update();

        const auto el = [doc](const char* id) { return doc->GetElementById(id); };
        const auto shown = [](Rml::Element* e) {
            for (; e != nullptr; e = e->GetParentNode()) {
                if (e->GetComputedValues().display() == Rml::Style::Display::None) return false;
            }
            return true;
        };
        const auto text = [](Rml::Element* e) { return e != nullptr ? e->GetInnerRML() : Rml::String("(none)"); };

        check(el("row-0") != nullptr && el("row-1") != nullptr && el("row-3") != nullptr && el("row-4") != nullptr,
              "every row has its row- id from n");
        check(el("row-2") == nullptr, "no row- id for a place the list skips");
        check(text(el("label-0")) == "Window", "the label is the model's");
        check(el("prev-0") != nullptr && el("prev-0")->IsClassSet("spent"), "a stepper at its first option shows its prev- step spent");
        check(el("next-0") != nullptr && !el("next-0")->IsClassSet("spent"), "and its next- step live");
        check(el("next-1") != nullptr && el("next-1")->IsClassSet("check") && el("next-1")->IsClassSet("ticked"),
              "a check carries next- on the ticked box");
        check(el("prev-1") == nullptr, "a check has no prev- id");
        check(el("row-1")->IsClassSet("on") && !el("row-0")->IsClassSet("on"), "the focused row alone is on");
        check(el("row-4")->IsClassSet("noted") && !el("row-0")->IsClassSet("noted"), "a row with a note is noted");
        Rml::Element* note = el("row-4")->QuerySelector(".row-note");
        check(note != nullptr && shown(note) && note->GetInnerRML() == "What it means", "the note is drawn under its row");
        Rml::Element* quiet = el("row-0")->QuerySelector(".row-note");
        check(quiet != nullptr && !shown(quiet), "a row without a note draws none");
        Rml::Element* act = el("row-3")->QuerySelector(".act");
        check(act != nullptr && shown(act) && act->GetInnerRML() == "Click or press Enter to open", "an action's words are in its act span");
        Rml::Element* divider = el("row-0")->GetParentNode()->QuerySelector(".divider");
        check(divider != nullptr && shown(divider) && text(divider->QuerySelector(".divider-label")) == "Place",
              "a row with a section has its divider before it");
        Rml::Element* none = el("row-1")->GetParentNode()->QuerySelector(".divider");
        check(none != nullptr && !shown(none), "a row without one has none shown");
        // The text row's words sit in the value's last span as one text node, as written.
        Rml::Element* plain = el("value-4") != nullptr ? el("value-4")->GetLastChild() : nullptr;
        Rml::ElementText* words = (plain != nullptr && plain->GetNumChildren() == 1)
            ? rmlui_dynamic_cast<Rml::ElementText*>(plain->GetFirstChild()) : nullptr;
        check(words != nullptr && words->GetText() == "Not found & <unread>", "plain words are text, never parsed as markup");

        rows.resize(2);
        set(List::Settings, rows);
        context->Update();
        check(el("row-3") == nullptr && el("row-4") == nullptr && el("row-1") != nullptr, "a row taken away goes");

        doc->Close();
        context->Update();
        set(List::Settings, {});
        return failed;
    }

    std::string dump_line(const Row& row) {
        static const char* const SHAPES[] = { "?", "stepper", "check", "action", "text" };
        const char* shape = (row.shape >= 1 && row.shape <= 4) ? SHAPES[row.shape] : SHAPES[0];
        std::string out = std::to_string(row.n) + " | " + row.label + " | " + shape + " | " + row.value;
        if (row.shape == Stepper) {
            out += std::string(" | ") + (row.low ? "low" : "-") + " " + (row.high ? "high" : "-");
        }
        if (row.shape == Check) {
            out += row.on ? " | ticked" : " | clear";
        }
        if (!row.note.empty()) {
            out += " | note: " + row.note;
        }
        if (!row.section.empty()) {
            out += " | after divider: " + row.section;
        }
        return out;
    }

}
