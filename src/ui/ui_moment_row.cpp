#include "ui_moment_row.h"

#include "ui_markup.h"

namespace oot::ui::moment_row {

    std::string rml(const Parts& parts) {
        using markup::escape;
        std::string out = "<div class=\"row mrow\" id=\"mrow-" + parts.id + "\">";
        out += parts.picture.empty() ? std::string("<div class=\"thumb empty\"></div>")
                                     : "<img class=\"thumb\" src=\"" + escape(parts.picture) + "\"/>";
        out += "<span class=\"text\">";
        out += "<span class=\"label\">" + escape(parts.title) + "<span class=\"slot\">" + escape(parts.slot) + "</span></span>";
        out += "<span class=\"value\">" + escape(parts.where) + "</span>";
        out += "</span></div>";
        return out;
    }

} // namespace oot::ui::moment_row
