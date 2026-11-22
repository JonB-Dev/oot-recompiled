#include "ui/ui_survey.h"

#include "ui_tokens.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace oot::ui::survey {

    namespace {

        constexpr const char* TILE_CLASS = "survey-tile";
        // What a survey was last covered for, so an unchanged one costs a string compare.
        constexpr const char* LAID_ATTRIBUTE = "data-laid";

        // One tile's side in whole pixels at this interface size.
        int tile_pixels(float dp_length, float dp) {
            return std::max(1, static_cast<int>(std::lround(dp_length * dp)));
        }

        int tiles_across(float length, int tile) {
            return std::max(1, static_cast<int>(std::ceil(length / static_cast<float>(tile))));
        }

        bool cover(Rml::Element* survey, float dp) {
            const int tile_w = tile_pixels(tokens::survey_width, dp);
            const int tile_h = tile_pixels(tokens::survey_height, dp);
            const int columns = tiles_across(survey->GetClientWidth(), tile_w);
            const int rows = tiles_across(survey->GetClientHeight(), tile_h);
            const Rml::String laid = std::to_string(columns) + "x" + std::to_string(rows) + " of " +
                                     std::to_string(tile_w) + "x" + std::to_string(tile_h);
            if (survey->GetAttribute<Rml::String>(LAID_ATTRIBUTE, "") == laid) {
                return false;
            }
            survey->SetAttribute(LAID_ATTRIBUTE, laid);

            const int count = columns * rows;
            while (survey->GetNumChildren() > count) {
                survey->RemoveChild(survey->GetLastChild());
            }
            while (survey->GetNumChildren() < count) {
                Rml::ElementPtr tile = survey->GetOwnerDocument()->CreateElement("div");
                tile->SetClass(TILE_CLASS, true);
                survey->AppendChild(std::move(tile));
            }
            auto px = [](int value) { return Rml::Property(static_cast<float>(value), Rml::Unit::PX); };
            for (int i = 0; i < count; ++i) {
                Rml::Element* tile = survey->GetChild(i);
                tile->SetProperty(Rml::PropertyId::Left, px((i % columns) * tile_w));
                tile->SetProperty(Rml::PropertyId::Top, px((i / columns) * tile_h));
                tile->SetProperty(Rml::PropertyId::Width, px(tile_w));
                tile->SetProperty(Rml::PropertyId::Height, px(tile_h));
            }
            return true;
        }

    } // namespace

    bool lay(Rml::Context* context) {
        if (context == nullptr) {
            return false;
        }
        const float dp = context->GetDensityIndependentPixelRatio();
        bool changed = false;
        for (int d = 0; d < context->GetNumDocuments(); ++d) {
            Rml::ElementList surveys;
            context->GetDocument(d)->QuerySelectorAll(surveys, ".survey");
            for (Rml::Element* survey : surveys) {
                changed = cover(survey, dp) || changed;
            }
        }
        return changed;
    }

} // namespace oot::ui::survey
