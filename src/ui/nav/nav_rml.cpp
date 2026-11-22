// A laid-out RmlUi document as the navigation model. See nav_rml.h.

#include "ui/nav/nav_rml.h"

#include <RmlUi/Core.h>

namespace oot::ui::nav {

    namespace {

        bool is_item(Rml::Element* e) {
            const Rml::String& tag = e->GetTagName();
            if (tag == "button" || tag == "input" || tag == "select") return true;
            if (e->HasAttribute("data-act") || e->HasAttribute("data-value")) return true;
            return e->GetAttribute<Rml::String>("data-nav", "") == "item";
        }

        bool scrolls(Rml::Element* e) {
            const Rml::Style::Overflow y = e->GetComputedValues().overflow_y();
            return y == Rml::Style::Overflow::Auto || y == Rml::Style::Overflow::Scroll;
        }

        Rect box_of(Rml::Element* e) {
            const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
            return Rect{ at.x, at.y, size.x, size.y };
        }

        void walk(Rml::Element* e, int parent, RmlModel& out) {
            const Rml::ComputedValues& cv = e->GetComputedValues();
            if (cv.display() == Rml::Style::Display::None || cv.visibility() == Rml::Style::Visibility::Hidden) {
                return;
            }
            const Rml::String nav = e->GetAttribute<Rml::String>("data-nav", "");
            int here = parent;
            if (nav == "layer" || nav == "group" || is_item(e)) {
                Node n;
                n.parent = parent;
                n.box = box_of(e);
                if (nav == "layer") {
                    n.kind = Kind::Layer;
                }
                else if (nav == "group") {
                    n.kind = Kind::Group;
                    n.wrap = e->HasAttribute("data-nav-wrap");
                    n.scrolls = scrolls(e);
                }
                else {
                    n.kind = Kind::Item;
                    n.value = e->HasAttribute("data-value");
                    n.enabled = !e->HasAttribute("disabled") && !e->HasAttribute("data-disabled");
                }
                const int index = out.model.add(n);
                out.elements.push_back(e);
                out.index[e] = index;
                // An item is a leaf to navigation: what is inside a button is part of the button.
                if (n.kind == Kind::Item) return;
                here = index;
            }
            for (int i = 0; i < e->GetNumChildren(); ++i) {
                walk(e->GetChild(i), here, out);
            }
        }

    }

    RmlModel build(Rml::ElementDocument* document) {
        RmlModel out;
        if (document != nullptr) {
            for (int i = 0; i < document->GetNumChildren(); ++i) {
                walk(document->GetChild(i), -1, out);
            }
        }
        return out;
    }

}
