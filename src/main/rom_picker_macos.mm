#import <AppKit/AppKit.h>

#include <filesystem>

namespace oot::launch {
std::filesystem::path choose_rom_with_native_picker() {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = @"Choose your Ocarina of Time USA 1.0 ROM";
        panel.message = @"The file is checked before use and stays where you keep it.";
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        if ([panel runModal] != NSModalResponseOK || panel.URL == nil) return {};
        const char* path = panel.URL.fileSystemRepresentation;
        return path ? std::filesystem::path(path) : std::filesystem::path{};
    }
}
}
