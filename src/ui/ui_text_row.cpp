#include "ui_text_row.h"

#include "ui_markup.h"

namespace oot::ui::text_row {

    std::string rml(const Parts& parts) {
        std::string out = "<div class=\"row" + (parts.classes.empty() ? std::string() : " " + parts.classes) +
                          "\" id=\"" + parts.id + "\">";
        out += "<span class=\"text\">";
        out += "<span class=\"label\">" + markup::escape(parts.label) + "</span>";
        out += "<span class=\"value\">" + parts.value_rml + "</span>";
        out += "</span></div>";
        return out;
    }

} // namespace oot::ui::text_row
