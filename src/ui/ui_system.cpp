#include "ui/ui_system.h"

#include <chrono>
#include <cstdio>

#include <SDL.h>

namespace oot::ui {

    // The clipboard, through SDL, for the ROM browser's path field (2026-09-19): RmlUi asks
    // for it on a paste and hands it text on a copy. SDL's clipboard calls take the system
    // clipboard for the moment of the call, whatever thread asks.
    void SystemInterface::SetClipboardText(const Rml::String& text) {
        SDL_SetClipboardText(text.c_str());
    }

    void SystemInterface::GetClipboardText(Rml::String& text) {
        text.clear();
        if (char* clip = SDL_GetClipboardText()) {
            text = clip;
            SDL_free(clip);
        }
    }

    double SystemInterface::GetElapsedTime() {
        using clock = std::chrono::steady_clock;
        static const clock::time_point start = clock::now();
        return std::chrono::duration<double>(clock::now() - start).count();
    }

    bool SystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message) {
        // Routed to the console rather than swallowed. A stylesheet with a typo in it fails
        // quietly by default: the element simply does not appear, with nothing to say why, and
        // an evening goes into looking at the wrong file.
        const char* label = "info";
        switch (type) {
            case Rml::Log::LT_ERROR:   label = "error";   break;
            case Rml::Log::LT_ASSERT:  label = "assert";  break;
            case Rml::Log::LT_WARNING: label = "warning"; break;
            case Rml::Log::LT_INFO:    label = "info";    break;
            case Rml::Log::LT_DEBUG:   label = "debug";   break;
            default: break;
        }
        std::fprintf(stderr, "[rml %s] %s\n", label, message.c_str());

        // False would abort on an assert. Nothing about our interface is worth taking the game
        // down for.
        return true;
    }

} // namespace oot::ui
