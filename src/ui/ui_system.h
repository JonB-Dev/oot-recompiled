#pragma once

#include <RmlUi/Core/SystemInterface.h>

// What RmlUi needs from the platform: a clock, somewhere to put its complaints, and since the
// ROM browser's path field (2026-09-19) the clipboard, so a path can be pasted into it and
// copied out of it. The cursor is deliberately not implemented: nothing needs a system cursor.
namespace oot::ui {

    class SystemInterface final : public Rml::SystemInterface {
    public:
        double GetElapsedTime() override;
        bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
        void SetClipboardText(const Rml::String& text) override;
        void GetClipboardText(Rml::String& text) override;
    };

} // namespace oot::ui
