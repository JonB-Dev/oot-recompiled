#include "main/install.h"
#include "main/signature.h"
#include "main/updates.h"
#include "main/video.h"

namespace oot::video {
Status status() { return {}; }
void toggle_record() {}
void toggle_pause() {}
bool wants_pictures() { return false; }
bool wants_picture(uint32_t, uint32_t, uint32_t&, uint32_t&, uint64_t&) { return false; }
bool records_window() { return false; }
void submit_picture(const uint8_t*, uint32_t, uint32_t, uint32_t, uint64_t, std::function<void()> done) {
    if (done) done();
}
bool wants_audio() { return false; }
void submit_audio(const int16_t*, std::size_t, uint32_t) {}
void submit_mic(const int16_t*, std::size_t) {}
void request_at_play_ticks(const uint32_t*, int) {}
void tick(uint32_t) {}
void renderer_gone() {}
void shutdown() {}
}

namespace oot::updates {
void start_if_enabled() {}
void tick() {}
State state() { return State::Idle; }
std::string available() { return {}; }
std::string line() { return {}; }
void start_download() {}
bool plate(std::string&, std::string&) { return false; }
bool install_pending() { return false; }
bool install() { return false; }
}

namespace oot::install {
Options options() { return {}; }
bool set_options(const Options&) { return false; }
void register_for(const std::filesystem::path&) {}
void unregister_self() {}
std::string remove_program() { return "No installer is used by this local macOS build."; }
std::filesystem::path shortcut_path() { return {}; }
std::filesystem::path desktop_shortcut_path() { return {}; }
}

namespace oot::signature {
Info read_self() { return {}; }
Info read_file(const std::wstring&) { return {}; }
}
