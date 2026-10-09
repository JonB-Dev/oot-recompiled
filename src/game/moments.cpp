// Saved moments, the native side (phases 74 and 75, .scaffold/states/).
//
// The game side (patches/harness_warp.c, in the patched Play_Main) asks once a frame whether a
// capture is wanted, judges whether the moment is safe, and when it is, hands over the addresses
// of the game's save block, a context block it filled and a place record it filled. This side
// reads those bytes out of RDRAM, adds the header, asks the recorder for one frame as the
// thumbnail, and writes the slot file atomically. Nothing here knows a game type: the blocks are
// opaque words whose sizes the patch asserted against the real structs.
#include "game/moments.h"

#include "game/game_state.h"
#include "main/places.h"
#include "main/recorder.h"
#include "ui/ui_settings.h"
#include "build_info.h"

#include <miniz/miniz.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace oot::moments {

    namespace {

        // The request, one at a time. A slot of -1 is no request. The lifetime is in play ticks
        // (the game's own updates, twenty a second), because the patch answers per update.
        std::atomic<int> g_pending_slot{ -1 };
        std::atomic<uint32_t> g_pending_until_tick{ 0 };
        std::atomic<uint32_t> g_pending_delay{ 0 };
        // The autosave's clock (phase 78): the last capture of any kind, the last manual one, and
        // the harness's override of the interval.
        std::atomic<uint32_t> g_last_capture_tick{ 0 };
        std::atomic<uint32_t> g_last_manual_tick{ 0 };
        std::atomic<uint32_t> g_autosave_override{ 0 };
        constexpr uint32_t TICKS_PER_MINUTE = 20 * 60;
        constexpr uint32_t MANUAL_QUIET_TICKS = 30 * 20;   // the manual save wins for half a minute

        // ONE PRESS, ONE MOMENT (the user, 2026-09-27, with a screenshot of three identical
        // moments in slots 4, 5 and 6 a second apart, and three more from the day before).
        //
        // WHY A GUARD RATHER THAN A FIX AT THE BUTTON. The quick keys are the only EDGE consumers
        // the game has: everything else reads the pad's state per frame, where a bounce is
        // invisible. An input bound to an axis or a hat, which is what a trigger, a shoulder on
        // some pads and a d-pad direction are, does not cross its threshold once on the way down;
        // it crosses, wobbles and crosses again, and every crossing is another edge. Three edges a
        // tenth of a second apart, each with time to complete before the next, is three moments in
        // three different slots, because quick save takes the first EMPTY one.
        //
        // The press cannot be made clean at the source without inventing a hysteresis the rest of
        // the program does not need, so the refusal lives here, where the cost of being wrong is
        // one ignored press rather than a control that feels sticky. Two seconds is far longer
        // than any bounce and far shorter than a person meaning it twice.
        constexpr uint32_t QUICK_AGAIN_TICKS = 2 * 20;
        std::atomic<uint32_t> g_last_quick_tick{ 0 };

        // True when a quick key has already been acted on inside the window. The moment is
        // recorded only when it lets one THROUGH, so a control that bounces cannot walk the
        // window forward by being refused over and over.
        bool quick_too_soon() {
            const uint32_t now = oot::game_state::play_ticks();
            const uint32_t last = g_last_quick_tick.load(std::memory_order_relaxed);
            if (last != 0 && now >= last && now - last < QUICK_AGAIN_TICKS) {
                return true;
            }
            g_last_quick_tick.store(now == 0 ? 1 : now, std::memory_order_relaxed);
            return false;
        }
        // Save on quit (phase 78b): whether a quit is waiting on its capture, and until when.
        std::atomic<bool> g_quit_waiting{ false };
        std::atomic<long long> g_quit_deadline_ms{ 0 };
        std::atomic<bool> g_start_resume_done{ false };
        long long now_ms() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        std::atomic<uint32_t> g_first_play_tick{ 0 };   // 0 until play is first seen
        std::atomic<int> g_last_reason{ 0 };
        std::mutex g_mutex;
        std::string g_pending_name;

        // The line waiting for the interface to show. Its own lock rather than g_mutex, because
        // it is written from the game thread while a capture finishes and read from the render
        // thread once a frame, and neither should ever wait on the other.
        std::mutex g_notice_mutex;
        std::string g_notice;

        // The staged resume: the file read and checked, waiting for a safe frame.
        std::atomic<int> g_load_slot{ -1 };
        std::atomic<uint32_t> g_load_until_tick{ 0 };
        std::vector<uint32_t> g_staged_save;
        std::vector<uint32_t> g_staged_context;
        Place g_staged_place{};
        int g_staged_slot = -1;

        const char* reason_text(int reason) {
            switch (reason) {
                case 1: return "the game is not running";
                case 2: return "a scene transition is in progress";
                case 3: return "a cutscene is playing";
                case 4: return "a text box is open";
                case 5: return "the pause menu is open";
                case 6: return "not in play";
                case 7: return "the player is not in the scene";
                case 8: return "the player is out of health";
                default: return "the moment was not safe";
            }
        }

        // ROTATE THE AUTOSAVES BEFORE ONE IS WRITTEN (2026-09-25, the user: keep the most recent
        // five). The oldest is dropped, every survivor moves one step back, and slot 0 is left
        // free for the capture about to happen. Renames rather than copies: a rename cannot half
        // succeed the way a copy can, and the thumbnail beside each moment moves with it.
        void rotate_autosaves() {
            static const char* const Extensions[] = { ".moment", ".rgba", ".png" };
            for (int slot = AUTOSAVE_OLDEST; slot < AUTOSAVE_SLOT; ++slot) {
                for (const char* extension : Extensions) {
                    std::error_code ec;
                    const std::filesystem::path older = slot_file(slot, extension);
                    const std::filesystem::path newer = slot_file(slot + 1, extension);
                    std::filesystem::remove(older, ec);
                    if (std::filesystem::is_regular_file(newer, ec)) {
                        std::filesystem::rename(newer, older, ec);
                    }
                }
            }
        }

        uint64_t wall_clock_s() {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        std::string clock_text(uint32_t day_time) {
            // The game's clock is a u16 wrapping over the day: 0x0000 midnight, 0x8000 noon.
            const uint32_t minutes = (day_time * 1440u) / 65536u;
            char text[8];
            std::snprintf(text, sizeof(text), "%02u:%02u", minutes / 60, minutes % 60);
            return text;
        }

        void copy_text(char* out, size_t size, const std::string& text) {
            std::memset(out, 0, size);
            const size_t n = text.size() < size - 1 ? text.size() : size - 1;
            std::memcpy(out, text.data(), n);
        }

        // Write to a temporary name beside the target, then rename over it, so a slot is never
        // half a file. The rename replaces an existing slot in one step.
        bool write_atomically(const std::filesystem::path& target, const std::vector<uint8_t>& bytes) {
            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);
            const std::filesystem::path temp = target.string() + ".writing";
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                if (!out) {
                    return false;
                }
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!out.good()) {
                    return false;
                }
            }
            std::filesystem::rename(temp, target, ec);
            if (ec) {
                std::filesystem::remove(target, ec);
                std::filesystem::rename(temp, target, ec);
            }
            return !ec;
        }

    } // namespace

    std::filesystem::path slot_file(int slot, const char* extension) {
        // The player's slots are "1" to "8"; the autosaves are "auto" (the newest, and the name
        // this has always written) then "auto2" to "auto5" going back in time.
        std::string stem;
        if (slot > AUTOSAVE_SLOT) {
            stem = std::to_string(slot);
        }
        else if (slot == AUTOSAVE_SLOT) {
            stem = "auto";
        }
        else {
            stem = "auto" + std::to_string(1 - slot);
        }
        return oot::places::moments() / (stem + extension);
    }

    void request_capture(int slot, uint32_t lifetime_ticks, const std::string& name, uint32_t delay_ticks) {
        if (!slot_exists(slot)) {
            std::fprintf(stderr, "MOMENT_REFUSED slot %d does not exist\n", slot);
            return;
        }
        // The autosave's own request rotates first, so the four before it survive and the oldest
        // goes. A player's slot is written where they asked and nothing moves.
        if (slot == AUTOSAVE_SLOT) {
            rotate_autosaves();
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending_name = name;
        }
        g_last_reason.store(0, std::memory_order_relaxed);
        g_pending_delay.store(delay_ticks, std::memory_order_relaxed);
        g_first_play_tick.store(0, std::memory_order_relaxed);
        g_pending_until_tick.store(oot::game_state::play_ticks() + lifetime_ticks + delay_ticks, std::memory_order_relaxed);
        g_pending_slot.store(slot, std::memory_order_release);
        if (slot != AUTOSAVE_SLOT) {
            g_last_manual_tick.store(oot::game_state::play_ticks(), std::memory_order_relaxed);
        }
        std::fprintf(stderr, "[moments] capture requested for slot %d\n", slot);
    }

    void notice(const char* text) {
        std::lock_guard<std::mutex> lock(g_notice_mutex);
        // The newest wins. Two of these cannot usefully queue: they last a second and a half
        // each, and a person who saved and resumed inside that wants to be told the second thing.
        g_notice = (text != nullptr) ? text : "";
    }

    bool take_notice(std::string& out) {
        std::lock_guard<std::mutex> lock(g_notice_mutex);
        if (g_notice.empty()) {
            return false;
        }
        out = g_notice;
        g_notice.clear();
        return true;
    }

    void quick_save() {
        // A capture already asked for and not yet written: a second press here would find the
        // slot it chose still empty and either overwrite the request or, once the first lands,
        // take the next slot along. Either way the person pressed once.
        if (g_pending_slot.load(std::memory_order_acquire) >= 0) {
            std::fprintf(stderr, "[moments] quick save ignored: one is already waiting to be taken\n");
            return;
        }
        if (quick_too_soon()) {
            std::fprintf(stderr, "[moments] quick save ignored: one was taken a moment ago\n");
            return;
        }
        for (int slot = 1; slot <= SLOT_COUNT; ++slot) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(slot_file(slot, ".moment"), ec)) {
                request_capture(slot, 60, "");
                return;
            }
        }

        // EVERY SLOT HOLDS ONE, SO THE OLDEST GIVES WAY (the user's choice when asked,
        // 2026-09-28). This printed a line to the log and did nothing else, and that is how a
        // mapped button came to be dead: he pressed it, nothing happened on screen, and the only
        // record was in a file he had no reason to open (he took it for the new bounce guard
        // refusing every press, which it was not). All eight of his slots were full, three of
        // them the duplicates the bounce itself had left the day before.
        //
        // His instruction of the 23rd was that quick save takes the first empty slot and never
        // overwrites. That is relaxed HERE ONLY, at his word, and only once there is nowhere left
        // to go: the newest eight presses are always on disk, and a moment worth keeping is kept by
        // moving it out of the eight rather than by hoping the button stays broken. The toast says
        // only that the state was saved, because a rolling quick save is ordinary and the slot it
        // reused is not news (his call, 2026-09-28).
        int oldest = -1;
        uint64_t when = 0;
        for (int slot = 1; slot <= SLOT_COUNT; ++slot) {
            Header header{};
            std::vector<uint32_t> save_words;
            std::vector<uint32_t> context_words;
            std::string why;
            // A slot that will not read goes first: it is no use to anyone as it stands.
            if (!read_slot(slot_file(slot, ".moment"), header, save_words, context_words, why)) {
                oldest = slot;
                break;
            }
            if ((oldest < 0) || (header.captured_at < when)) {
                oldest = slot;
                when = header.captured_at;
            }
        }
        std::fprintf(stderr, "[moments] every slot holds a moment; the oldest, slot %d, gives way\n", oldest);
        request_capture(oldest, 60, "");
    }

    // The slot holding the newest readable moment, or -1.
    int newest_slot() {
        int newest = -1;
        uint64_t when = 0;
        for (int slot = AUTOSAVE_OLDEST; slot <= SLOT_COUNT; ++slot) {
            Header header{};
            std::vector<uint32_t> save_words;
            std::vector<uint32_t> context_words;
            std::string why;
            std::error_code ec;
            const std::filesystem::path file = slot_file(slot, ".moment");
            if (!std::filesystem::is_regular_file(file, ec) || !read_slot(file, header, save_words, context_words, why)) {
                continue;
            }
            if (header.captured_at >= when) {
                when = header.captured_at;
                newest = slot;
            }
        }
        return newest;
    }

    void quick_load() {
        // The same bounce, and worse here: a second resume mid transition is the kind of thing
        // that leaves the game somewhere nobody asked to be.
        if (g_load_slot.load(std::memory_order_acquire) >= 0) {
            std::fprintf(stderr, "[moments] quick resume ignored: one is already staged\n");
            return;
        }
        if (quick_too_soon()) {
            std::fprintf(stderr, "[moments] quick resume ignored: one was asked for a moment ago\n");
            return;
        }
        const int newest = newest_slot();
        if (newest < 0) {
            std::fprintf(stderr, "MOMENT_REFUSED there is no moment to resume\n");
            return;
        }
        request_load(newest);
    }

    int take_boot_load() {
        // Once per launch, whatever comes of it: the console logo asks before its first frame.
        if (g_start_resume_done.exchange(true, std::memory_order_acq_rel)) {
            return -1;
        }
        if (oot::ui::settings().resume_on_start != 1) {
            return -1;
        }
        const int newest = newest_slot();
        if (newest < 0) {
            std::fprintf(stderr, "[moments] resume on start: no moment to resume, the title runs\n");
            return -1;
        }
        if (!request_load(newest)) {
            return -1;
        }
        uint32_t file_plus_one = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            file_plus_one = g_staged_place.reserved;
        }
        if (file_plus_one == 0) {
            // A moment from before the file number was kept: a boot resume could not send a later
            // save to the right file. It stays staged and resumes once a file is loaded, as the
            // in play path always did.
            std::fprintf(stderr, "[moments] resume on start: slot %d predates the file number; it resumes once a file is loaded\n", newest);
            return -1;
        }
        std::fprintf(stderr, "[moments] resume on start from the console logo: slot %d, file %u\n", newest, file_plus_one - 1);
        return newest;
    }

    void set_autosave_override(uint32_t ticks) {
        g_autosave_override.store(ticks, std::memory_order_relaxed);
    }

    bool on_quit_requested(bool game_started) {
        if (g_quit_waiting.load(std::memory_order_acquire)) {
            return true;   // already waiting; a second close does not start a second capture
        }
        if (!game_started || oot::ui::settings().save_on_quit != 1) {
            return false;
        }
        const oot::game_state::Snapshot now = oot::game_state::read();
        if (!now.valid || now.game_mode != 0) {
            return false;   // the title, the file select: nothing worth keeping
        }
        // Three seconds of game updates for a safe frame, and a wall clock deadline a little
        // longer, so a person who closes the window mid cutscene still gets out.
        request_capture(AUTOSAVE_SLOT, 3 * 20, "");
        g_quit_deadline_ms.store(now_ms() + 4000, std::memory_order_relaxed);
        g_quit_waiting.store(true, std::memory_order_release);
        std::fprintf(stderr, "[moments] quit waits for a moment to be kept\n");
        return true;
    }

    bool quit_pending() {
        return g_quit_waiting.load(std::memory_order_acquire);
    }

    bool quit_ready() {
        if (!g_quit_waiting.load(std::memory_order_acquire)) {
            return true;
        }
        const bool pending = g_pending_slot.load(std::memory_order_acquire) >= 0;
        if (!pending || now_ms() > g_quit_deadline_ms.load(std::memory_order_relaxed)) {
            if (pending) {
                std::fprintf(stderr, "[moments] no safe frame came; quitting without a moment\n");
                g_pending_slot.store(-1, std::memory_order_release);
            }
            g_quit_waiting.store(false, std::memory_order_release);
            return true;
        }
        return false;
    }

    // The autosave interval in game updates from the row (0 off), or the harness's override.
    uint32_t autosave_interval_ticks() {
        const uint32_t over = g_autosave_override.load(std::memory_order_relaxed);
        if (over > 0) {
            return over;
        }
        static const uint32_t minutes[4] = { 0, 5, 10, 15 };
        const int row = oot::ui::settings().autosave;
        return (row > 0 && row < 4) ? minutes[row] * TICKS_PER_MINUTE : 0u;
    }

    int take_pending_capture() {
        int slot = g_pending_slot.load(std::memory_order_acquire);
        if (slot < 0) {
            // NOTHING ASKED FOR: is the autosave due? On the interval since the last capture of
            // any kind, not within half a minute of a manual save (the manual save wins), and only
            // once play has been seen. It becomes an ordinary request for the autosave's own slot,
            // judged safe by the patch like any other, lapsing after a minute of unsafe frames.
            const uint32_t interval = autosave_interval_ticks();
            if (interval == 0) {
                return -1;
            }
            const uint32_t ticks = oot::game_state::play_ticks();
            if (g_last_capture_tick.load(std::memory_order_relaxed) == 0) {
                const oot::game_state::Snapshot now = oot::game_state::read();
                if (now.valid && now.game_mode == 0) {
                    g_last_capture_tick.store(ticks == 0 ? 1 : ticks, std::memory_order_relaxed);
                }
                return -1;
            }
            const uint32_t since = ticks - g_last_capture_tick.load(std::memory_order_relaxed);
            const uint32_t quiet = ticks - g_last_manual_tick.load(std::memory_order_relaxed);
            if (since < interval || quiet < MANUAL_QUIET_TICKS) {
                return -1;
            }
            request_capture(AUTOSAVE_SLOT, 60 * 20, "");
            slot = AUTOSAVE_SLOT;
        }
        // Held until the game is in ordinary play, exactly as a pending warp is held: the patch
        // runs during the title demo and the file select too, and a capture there is not a
        // moment anyone asked for. The patch makes the finer judgment once it has the state.
        const oot::game_state::Snapshot now = oot::game_state::read();
        if (!now.valid || now.game_mode != 0) {
            if (oot::game_state::play_ticks() > g_pending_until_tick.load(std::memory_order_relaxed)) {
                g_pending_slot.store(-1, std::memory_order_release);
                std::fprintf(stderr, "MOMENT_REFUSED %s\n", reason_text(6));
            }
            return -1;
        }
        // The delay counts from the first update seen in play, not from launch: the intro's
        // length is nobody's to predict.
        const uint32_t ticks = oot::game_state::play_ticks();
        if (g_first_play_tick.load(std::memory_order_relaxed) == 0) {
            g_first_play_tick.store(ticks == 0 ? 1 : ticks, std::memory_order_relaxed);
        }
        if (ticks < g_first_play_tick.load(std::memory_order_relaxed) + g_pending_delay.load(std::memory_order_relaxed)) {
            return -1;
        }
        return slot;
    }

    void note_refused(int slot, int reason) {
        // The request STAYS PENDING while the reason is transient (a transition mid way, a text
        // box open) and lapses only when its lifetime runs out, so a capture asked for a frame
        // before a door still happens on the far side of it. The reason is logged when it
        // changes, and once more as the final word if a request lapses. Each request lapses on
        // its own clock, and only while it is actually pending.
        if (g_last_reason.exchange(reason, std::memory_order_relaxed) != reason) {
            std::fprintf(stderr, "[moments] slot %d waiting: %s\n", slot, reason_text(reason));
        }
        const uint32_t ticks = oot::game_state::play_ticks();
        if (g_pending_slot.load(std::memory_order_acquire) >= 0 &&
            ticks > g_pending_until_tick.load(std::memory_order_relaxed)) {
            g_pending_slot.store(-1, std::memory_order_release);
            std::fprintf(stderr, "MOMENT_REFUSED %s\n", reason_text(reason));
        }
        if (g_load_slot.load(std::memory_order_acquire) >= 0 &&
            ticks > g_load_until_tick.load(std::memory_order_relaxed)) {
            g_load_slot.store(-1, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_staged_slot = -1;
            }
            std::fprintf(stderr, "MOMENT_REFUSED %s\n", reason_text(reason));
        }
    }

    void on_captured(int slot, const std::vector<uint32_t>& save_words,
                     const std::vector<uint32_t>& context_words, const Place& place) {
        g_pending_slot.store(-1, std::memory_order_release);

        if (save_words.size() * 4 != SAVE_SIZE || context_words.size() * 4 != CONTEXT_SIZE) {
            std::fprintf(stderr, "MOMENT_REFUSED the game handed over %zu and %zu bytes, not %u and %u\n",
                         save_words.size() * 4, context_words.size() * 4, SAVE_SIZE, CONTEXT_SIZE);
            return;
        }

        Header header{};
        std::memcpy(header.magic, MAGIC, sizeof(MAGIC));
        header.layout_version = LAYOUT_VERSION;
        header.total_size = FILE_SIZE;
        copy_text(header.program_version, sizeof(header.program_version), oot::build_info::version);
        header.captured_at = wall_clock_s();
        header.entrance = static_cast<uint32_t>(place.entrance);
        header.scene = place.scene;
        header.room = place.room;
        header.position[0] = place.position[0];
        header.position[1] = place.position[1];
        header.position[2] = place.position[2];
        header.yaw = place.yaw;
        header.link_age = place.link_age;
        header.day_time = place.day_time;
        // The game's file number plus one, from the patch (0: a moment from before it was kept).
        header.reserved = place.reserved;
        header.flags = is_autosave(slot) ? FLAG_AUTOSAVE : 0u;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const std::string name = g_pending_name.empty()
                ? ("Scene " + std::to_string(place.scene) + " at " + clock_text(place.day_time))
                : g_pending_name;
            copy_text(header.name, sizeof(header.name), name);
        }
        header.save_size = SAVE_SIZE;
        header.context_size = CONTEXT_SIZE;

        // The thumbnail is asked for FIRST, so its presence bit is honest by the time the header
        // is written: the recorder takes the next presented frame, which is this one or the next.
        const std::filesystem::path png = slot_file(slot, ".png");
        if (oot::recorder::snapshot(png, THUMBNAIL_WIDTH)) {
            header.flags |= FLAG_THUMBNAIL;
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(FILE_SIZE);
        const uint8_t* h = reinterpret_cast<const uint8_t*>(&header);
        bytes.insert(bytes.end(), h, h + sizeof(Header));
        const uint8_t* s = reinterpret_cast<const uint8_t*>(save_words.data());
        bytes.insert(bytes.end(), s, s + SAVE_SIZE);
        const uint8_t* c = reinterpret_cast<const uint8_t*>(context_words.data());
        bytes.insert(bytes.end(), c, c + CONTEXT_SIZE);
        const uint32_t crc = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, bytes.data(), bytes.size()));
        const uint8_t* cr = reinterpret_cast<const uint8_t*>(&crc);
        bytes.insert(bytes.end(), cr, cr + CRC_SIZE);

        const std::filesystem::path target = slot_file(slot, ".moment");
        if (!write_atomically(target, bytes)) {
            std::fprintf(stderr, "MOMENT_REFUSED the slot file could not be written: %s\n", target.string().c_str());
            return;
        }
        g_last_capture_tick.store(oot::game_state::play_ticks(), std::memory_order_relaxed);
        std::fprintf(stderr, is_autosave(slot) ? "MOMENT_AUTOSAVED %d %zu\n" : "MOMENT_WRITTEN %d %zu\n", slot, bytes.size());
        // HERE rather than where the key was pressed: a capture waits for a frame it is safe to
        // take, so saying "saved" on the press would be saying it before it was true. The
        // autosave says nothing, because nobody asked it to happen and a toast every few minutes
        // during play is an irritation rather than an answer.
        if (!is_autosave(slot)) {
            // ONE LINE, WHATEVER HAPPENED TO THE SLOT (the user, 2026-09-28: "the saved over moment
            // N is not needed ... rolling quick saves are nomal. we dont need the extra details").
            // It named the slot it had replaced for a while, on the reasoning that a press which
            // overwrites something should say so. His answer is that a rolling save is unremarkable,
            // and a shorter line is also the better one to read mid-play.
            notice("State saved");
        }
    }

    bool request_load(int slot) {
        if (!slot_exists(slot)) {
            std::fprintf(stderr, "MOMENT_REFUSED slot %d does not exist\n", slot);
            return false;
        }
        Header header{};
        std::vector<uint32_t> save_words;
        std::vector<uint32_t> context_words;
        std::string why;
        const std::filesystem::path file = slot_file(slot, ".moment");
        if (!read_slot(file, header, save_words, context_words, why)) {
            std::fprintf(stderr, "MOMENT_REFUSED %s: %s\n", why.c_str(), file.string().c_str());
            return false;
        }
        Place place{};
        place.position[0] = header.position[0];
        place.position[1] = header.position[1];
        place.position[2] = header.position[2];
        place.yaw = header.yaw;
        place.room = header.room;
        place.scene = header.scene;
        place.entrance = static_cast<int32_t>(header.entrance);
        place.link_age = header.link_age;
        place.day_time = header.day_time;
        place.reserved = (header.reserved >= 1 && header.reserved <= 3) ? static_cast<uint32_t>(header.reserved) : 0u;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_staged_save = std::move(save_words);
            g_staged_context = std::move(context_words);
            g_staged_place = place;
            g_staged_slot = slot;
        }
        g_last_reason.store(0, std::memory_order_relaxed);
        g_load_until_tick.store(oot::game_state::play_ticks() + 3000, std::memory_order_relaxed);
        g_load_slot.store(slot, std::memory_order_release);
        std::fprintf(stderr, "[moments] resume staged from slot %d (%s, entrance 0x%04X)\n", slot, header.name, header.entrance);
        return true;
    }

    int take_pending_load() {
        int slot = g_load_slot.load(std::memory_order_acquire);
        if (slot < 0) {
            // RESUME ON START (phase 78b): once, the first time play is seen with the row on and
            // nothing else asked for, the newest moment is staged, exactly as quick load stages it.
            // "Once a file is loaded" is the honest shape: the game's title and file select are
            // the game's, and the moment replaces the loaded file's running state, not its file.
            if (!g_start_resume_done.load(std::memory_order_acquire) && oot::ui::settings().resume_on_start == 1) {
                const oot::game_state::Snapshot now = oot::game_state::read();
                if (now.valid && now.game_mode == 0) {
                    g_start_resume_done.store(true, std::memory_order_release);
                    std::fprintf(stderr, "[moments] resuming the newest moment on start\n");
                    quick_load();
                    slot = g_load_slot.load(std::memory_order_acquire);
                }
            }
            if (slot < 0) {
                return -1;
            }
        }
        const oot::game_state::Snapshot now = oot::game_state::read();
        if (!now.valid || now.game_mode != 0) {
            if (oot::game_state::play_ticks() > g_load_until_tick.load(std::memory_order_relaxed)) {
                g_load_slot.store(-1, std::memory_order_release);
                std::fprintf(stderr, "MOMENT_REFUSED %s\n", reason_text(6));
            }
            return -1;
        }
        return slot;
    }

    bool take_staged(std::vector<uint32_t>& save_words, std::vector<uint32_t>& context_words, Place& place) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_staged_slot < 0) {
            return false;
        }
        save_words = g_staged_save;
        context_words = g_staged_context;
        place = g_staged_place;
        return true;
    }

    void note_resumed() {
        int slot = -1;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            slot = g_staged_slot;
            g_staged_slot = -1;
            g_staged_save.clear();
            g_staged_context.clear();
        }
        g_load_slot.store(-1, std::memory_order_release);
        std::fprintf(stderr, "MOMENT_RESUMED %d\n", slot);
        // The game has taken the state by the time this runs, so the line is true when it shows.
        notice("State restored");
    }

    bool read_slot(const std::filesystem::path& file, Header& header, std::vector<uint32_t>& save_words,
                   std::vector<uint32_t>& context_words, std::string& why) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (ec) {
            why = "the file could not be read";
            return false;
        }
        if (size != FILE_SIZE) {
            why = "the file is not the size a moment is";
            return false;
        }
        std::vector<uint8_t> bytes(FILE_SIZE);
        {
            std::ifstream in(file, std::ios::binary);
            if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(FILE_SIZE))) {
                why = "the file could not be read";
                return false;
            }
        }
        uint32_t stored = 0;
        std::memcpy(&stored, bytes.data() + FILE_SIZE - CRC_SIZE, CRC_SIZE);
        const uint32_t crc = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, bytes.data(), FILE_SIZE - CRC_SIZE));
        if (crc != stored) {
            why = "the file is damaged (its checksum does not match)";
            return false;
        }
        std::memcpy(&header, bytes.data(), sizeof(Header));
        if (std::memcmp(header.magic, MAGIC, sizeof(MAGIC)) != 0) {
            why = "the file is not a moment";
            return false;
        }
        if (header.layout_version != LAYOUT_VERSION) {
            why = "the file was written by a version this program does not read";
            return false;
        }
        if (header.total_size != FILE_SIZE || header.save_size != SAVE_SIZE || header.context_size != CONTEXT_SIZE) {
            why = "the file's own sizes do not match its layout";
            return false;
        }
        // The name is displayed, so it is terminated whatever the file says.
        header.name[sizeof(header.name) - 1] = '\0';
        header.program_version[sizeof(header.program_version) - 1] = '\0';
        if (header.room < -1 || header.room > 64 || header.link_age < 0 || header.link_age > 1) {
            why = "the file names a room or an age that does not exist";
            return false;
        }
        for (float v : header.position) {
            if (!(v > -100000.0f && v < 100000.0f)) {
                why = "the file places the player outside any scene";
                return false;
            }
        }
        save_words.resize(SAVE_SIZE / 4);
        std::memcpy(save_words.data(), bytes.data() + sizeof(Header), SAVE_SIZE);
        context_words.resize(CONTEXT_SIZE / 4);
        std::memcpy(context_words.data(), bytes.data() + sizeof(Header) + SAVE_SIZE, CONTEXT_SIZE);
        return true;
    }

    int dump(const std::filesystem::path& file) {
        Header header{};
        std::vector<uint32_t> save_words;
        std::vector<uint32_t> context_words;
        std::string why;
        if (!read_slot(file, header, save_words, context_words, why)) {
            std::printf("MOMENT_REFUSED %s: %s\n", why.c_str(), file.string().c_str());
            return 1;
        }
        std::printf("moment %s\n", file.string().c_str());
        std::printf("  name         %s\n", header.name);
        std::printf("  written by   %s\n", header.program_version);
        std::printf("  captured at  %llu\n", static_cast<unsigned long long>(header.captured_at));
        std::printf("  entrance     0x%04X\n", header.entrance);
        std::printf("  scene        %d\n", header.scene);
        std::printf("  room         %d\n", header.room);
        std::printf("  position     %.1f %.1f %.1f\n", header.position[0], header.position[1], header.position[2]);
        std::printf("  yaw          %d\n", header.yaw);
        std::printf("  link age     %s\n", header.link_age == 1 ? "child" : "adult");
        std::printf("  day time     %s (0x%04X)\n", clock_text(header.day_time).c_str(), header.day_time);
        std::printf("  thumbnail    %s\n", (header.flags & FLAG_THUMBNAIL) ? "yes" : "no");
        std::printf("  autosave     %s\n", (header.flags & FLAG_AUTOSAVE) ? "yes" : "no");
        std::printf("  save block   %u bytes, context %u bytes, checksum ok\n", header.save_size, header.context_size);
        return 0;
    }

} // namespace oot::moments
