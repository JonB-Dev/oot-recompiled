#include "game/game_state.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>

namespace {

    // The one address. From OUR data symbol table, OoTRecompSyms/oot.ntsc-1.0.datasyms.toml, not
    // from the reference project: its save context is at a different address in a different game,
    // and copying one would produce a plausible reading of unrelated memory.
    constexpr uint32_t G_SAVE_CONTEXT = 0x8011A5D0;

    // Offsets inside SaveContext, taken from the decompilation's own offset comments rather than
    // counted by hand. The nested ones are the second figure in those comments, which is already
    // relative to the start of `Save`:
    //
    //     SaveContext.save                              0x0000
    //     Save.entranceIndex                            0x0000
    //     Save.linkAge                                  0x0004
    //     Save.info.playerData.healthCapacity           0x002E
    //     Save.info.playerData.health                   0x0030
    //     Save.info.playerData.rupees                   0x0034
    //     Save.info.playerData.savedSceneId             0x0066
    //     SaveContext.gameMode                          0x135C
    constexpr uint32_t OFF_ENTRANCE        = 0x0000;
    // Save.dayTime and Save.nightFlag, straight out of the decomp's own save.h (0x0C and 0x10
    // inside Save, which is the head of SaveContext). The clock is read because the user's report
    // of the Kakariko fault is that the TIME CHANGES on its own, and a symptom you can only see
    // as a lighting shift is one you cannot count.
    constexpr uint32_t OFF_DAY_TIME        = 0x000C;
    constexpr uint32_t OFF_NIGHT_FLAG      = 0x0010;
    constexpr uint32_t OFF_LINK_AGE        = 0x0004;
    constexpr uint32_t OFF_HEALTH_CAPACITY = 0x002E;
    constexpr uint32_t OFF_HEALTH          = 0x0030;
    constexpr uint32_t OFF_RUPEES          = 0x0034;
    constexpr uint32_t OFF_SAVED_SCENE     = 0x0066;
    constexpr uint32_t OFF_GAME_MODE       = 0x135C;
    /* THE ARMED CLOCK. gSaveContext.nextDayTime, outside the saved block, at 0x1416 in this
       revision (upstream/oot/include/save.h). The game checks it on EVERY scene init and, when it
       is anything but NEXT_TIME_NONE, forces dayTime and skyboxTime to it (z_play.c:395). So a
       value left armed here makes the clock jump on every transition and stay there, which is
       exactly the reported "when I go outside the whole time changes". Read so it can be SEEN
       rather than argued about. */
    // 65535 (0xFFFF, the game's NEXT_TIME_NONE) means nothing is armed. Any other value in the
    // recording's next_day_time column is the clock waiting to be forced at the next scene init.
    constexpr uint32_t OFF_NEXT_DAY_TIME   = 0x1416;

    // gSegments, from our own data symbol table. Sixteen entries of physical addresses.
    constexpr uint32_t G_SEGMENTS = 0x80120C38;

    // gRegEditor, a POINTER to the register editor block the game allocates at boot; from our
    // data symbols. R_UPDATE_RATE is SREG(30), which is data[1 * 96 + 30] of s16 after the block's
    // five s32 header fields: 0x14 + 126 * 2 = 0x110. The same arithmetic the recompiled patch
    // performs, checked against it.
    constexpr uint32_t G_REG_EDITOR_PTR = 0x8011BA00;
    constexpr uint32_t OFF_UPDATE_RATE = 0x110;

    // sSunDepthTestX and sSunDepthTestY in z_kankyo.c, two halfwords, from the data symbol table.
    constexpr uint32_t G_SUN_TEST_X = 0x8011BCD6;
    constexpr uint32_t G_SUN_TEST_Y = 0x8011BCD8;

    // The largest offset we touch. Used to refuse a read that would run off the end of what the
    // runtime maps, which is the mistake that produced this session's one real crash.
    constexpr uint32_t HIGHEST_OFFSET = OFF_NEXT_DAY_TIME + sizeof(uint16_t);

    // The runtime maps emulated 0x80000000 at rdram + 0, and stores 32 bit words in host order.
    // Sub-word reads are swizzled, which is not a detail that can be skipped: a halfword read
    // without the XOR returns a plausible number from the wrong half of the word, so health would
    // come back as something like 0x0300 instead of 3 and nothing would look obviously broken.
    constexpr uint32_t RAM_BASE = 0x80000000;

    std::atomic<uint8_t*> g_rdram{ nullptr };

    // Written by the command line, read by the game thread once per frame.
    std::atomic<int32_t> g_pending_warp{ -1 };

    /* HOW LONG AN INTERACTIVE REQUEST STAYS ALIVE.
       A warp or a time of day asked for by a person is held until the game is actually playing,
       because the same per frame entry point runs during the title demo. That hold used to have
       no end to it: a request the game could not honor stayed armed and fired at the next
       opportunity, which might be a scene later. A teleport with no cause and a clock that jumps
       on arriving somewhere are both exactly that, and both were reported.
       Three seconds is long enough to cover a transition and short enough that nothing can
       ambush anybody. The QUEUED sweep is deliberately not subject to this. */
    constexpr std::chrono::milliseconds PENDING_LIFETIME{ 3000 };
    std::atomic<int64_t> g_warp_set_at{ 0 };
    std::atomic<int64_t> g_day_time_set_at{ 0 };

    int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // True when a request set at `at` has been waiting longer than anyone would still expect it.
    bool lapsed(const std::atomic<int64_t>& at) {
        return now_ms() - at.load(std::memory_order_relaxed) > PENDING_LIFETIME.count();
    }

    std::mutex g_warp_mutex;
    std::deque<int32_t> g_warp_queue;

    // Frames to leave between warps. The patch asks once per game frame, and this game runs
    // its logic at twenty frames a second, so the default is a few seconds: enough for the
    // scene to finish loading and settle before it is torn down again. Too short and the sweep
    // tests the loader over and over rather than the scenes. A harness that wants to DO
    // something in each scene sets a longer one with --dwell.
    int g_warp_dwell_frames = 120;
    int g_frames_since_warp = 0;

    // The transition the warps use, as the game numbers its types; 2 is its fade to black.
    std::atomic<int32_t> g_warp_transition{ 2 };

    int32_t read_word(const uint8_t* rdram, uint32_t address) {
        return *reinterpret_cast<const int32_t*>(rdram + (address - RAM_BASE));
    }

    int16_t read_half(const uint8_t* rdram, uint32_t address) {
        return *reinterpret_cast<const int16_t*>(rdram + ((address ^ 2) - RAM_BASE));
    }

} // namespace

namespace oot::game_state {

    void bind(uint8_t* rdram) {
        g_rdram.store(rdram, std::memory_order_release);
    }

    bool bound() {
        return g_rdram.load(std::memory_order_acquire) != nullptr;
    }

    Snapshot read() {
        Snapshot s;

        const uint8_t* rdram = g_rdram.load(std::memory_order_acquire);
        if (rdram == nullptr) {
            return s;
        }

        // Nothing here should ever be out of range, since the address is a constant from our own
        // symbol table. Checked anyway, because an unchecked read against a guard page is what
        // this project already lost an evening to, and the check costs one comparison.
        if (G_SAVE_CONTEXT < RAM_BASE || (G_SAVE_CONTEXT - RAM_BASE) + HIGHEST_OFFSET < HIGHEST_OFFSET) {
            return s;
        }

        s.entrance_index   = read_word(rdram, G_SAVE_CONTEXT + OFF_ENTRANCE);
        s.link_age         = read_word(rdram, G_SAVE_CONTEXT + OFF_LINK_AGE);
        s.game_mode        = read_word(rdram, G_SAVE_CONTEXT + OFF_GAME_MODE);
        s.next_day_time    = static_cast<int32_t>(read_half(rdram, G_SAVE_CONTEXT + OFF_NEXT_DAY_TIME));
        s.health_capacity  = read_half(rdram, G_SAVE_CONTEXT + OFF_HEALTH_CAPACITY);
        s.health           = read_half(rdram, G_SAVE_CONTEXT + OFF_HEALTH);
        s.rupees           = read_half(rdram, G_SAVE_CONTEXT + OFF_RUPEES);
        s.saved_scene      = read_half(rdram, G_SAVE_CONTEXT + OFF_SAVED_SCENE);
        s.cic_id           = read_word(rdram, 0x80000310u);
        s.boot_magic0      = static_cast<uint32_t>(read_word(rdram, 0x800088A0u));
        for (uint32_t i = 0; i < 16; ++i) {
            s.segment[i] = static_cast<uint32_t>(read_word(rdram, G_SEGMENTS + i * 4));
        }

        // Through a pointer the game owns, so it is checked before it is followed: null before
        // the game allocates it, and never trusted to point anywhere but the cached view of the
        // console's memory.
        s.sun_test_x = read_half(rdram, G_SUN_TEST_X);
        s.sun_test_y = read_half(rdram, G_SUN_TEST_Y);

        const uint32_t reg_editor = static_cast<uint32_t>(read_word(rdram, G_REG_EDITOR_PTR));
        if (reg_editor >= RAM_BASE && reg_editor + OFF_UPDATE_RATE + 2 < RAM_BASE + 0x00800000 &&
            (reg_editor & 3) == 0) {
            s.update_rate = read_half(rdram, reg_editor + OFF_UPDATE_RATE);
        }

        s.day_time   = read_half(rdram, G_SAVE_CONTEXT + OFF_DAY_TIME);
        s.night_flag = read_word(rdram, G_SAVE_CONTEXT + OFF_NIGHT_FLAG);

        s.valid            = true;
        return s;
    }

    // The time of day the debug menu has asked for, or -1 when there is none. Same shape as the
    // pending warp above and for the same reason: the value is SET from whatever thread the
    // interface runs on and APPLIED by the patch on the game thread, so nothing here ever writes
    // into emulated memory behind the game's back.
    std::atomic<int32_t> g_pending_day_time{ -1 };

    // Game thread only, like the rest of this file's counters.
    uint32_t g_play_ticks_value = 0;

    uint32_t play_ticks() {
        return g_play_ticks_value;
    }

    // Written from the VI thread and read from the present thread, so it is atomic. The counters
    // above are game thread only and deliberately are not.
    std::atomic<uint32_t> g_vi_value{ 0 };

    uint32_t vi_count() {
        return g_vi_value.load(std::memory_order_relaxed);
    }

    void note_vi() {
        g_vi_value.fetch_add(1, std::memory_order_relaxed);
    }

    void note_play_tick() {
        ++g_play_ticks_value;
    }

    uint32_t g_dropped_frames_value = 0;

    uint32_t dropped_frames() {
        return g_dropped_frames_value;
    }

    void note_dropped_frame(uint32_t which, int32_t overrun) {
        ++g_dropped_frames_value;

        // Logged at most once a second: a scene that overflows does so on every frame, and one
        // line a second names the pool and the size of the overrun, which is what sizing the
        // pools needs, without turning the log into a scroll of the same line.
        static int64_t last_said_ms = -1000000;
        const int64_t now = now_ms();
        if (now - last_said_ms < 1000) {
            return;
        }
        last_said_ms = now;

        const char* names[] = { "opaque", "translucent", "overlay", "work" };
        char pools[64];
        int used = 0;
        for (int bit = 0; bit < 4 && used < static_cast<int>(sizeof(pools)) - 1; ++bit) {
            if (which & (1u << bit)) {
                used += std::snprintf(pools + used, sizeof(pools) - static_cast<size_t>(used), "%s%s",
                                      used ? ", " : "", names[bit]);
            }
        }
        std::fprintf(stderr, "[gfx] frame dropped: the %s list overflowed by %d bytes (%u dropped so far)\n",
                     used ? pools : "(none?)", overrun, g_dropped_frames_value);
    }

    void set_pending_day_time(int32_t day_time) {
        g_day_time_set_at.store(now_ms(), std::memory_order_relaxed);
        g_pending_day_time.store(day_time, std::memory_order_relaxed);
    }

    int32_t take_pending_day_time() {
        if (g_pending_day_time.load(std::memory_order_relaxed) < 0) {
            return -1;
        }
        // Held until the game is actually playing, exactly as the warp is: the title demo runs
        // through the same per frame entry point, and setting the clock there would be setting
        // the demo's clock.
        const Snapshot now = read();
        if (!now.valid || now.game_mode != 0) {
            // Lapse rather than wait. See PENDING_LIFETIME.
            if (lapsed(g_day_time_set_at)) {
                g_pending_day_time.store(-1, std::memory_order_relaxed);
            }
            return -1;
        }
        return g_pending_day_time.exchange(-1, std::memory_order_relaxed);
    }

    // How many warps the patch has taken, pending or queued. The frozen capture (shots.h) arms
    // only once the harness's warp has actually happened, so a run whose target is the file's
    // own scene freezes the scene the warp made and not the one the file opened in.
    std::atomic<uint32_t> g_warps_taken{ 0 };

    void set_pending_warp(int32_t entrance) {
        g_warp_set_at.store(now_ms(), std::memory_order_relaxed);
        g_pending_warp.store(entrance, std::memory_order_relaxed);
    }

    int32_t take_pending_warp() {
        if (g_pending_warp.load(std::memory_order_relaxed) < 0) {
            return -1;
        }

        // HOLD IT UNTIL THE GAME IS ACTUALLY PLAYING. The patch that takes this runs from the
        // game's per frame entry point, and that entry point is running during the title demo
        // too. The first attempt warped straight out of the demo, which is not what anyone asking
        // for a scene means, and it made the result useless for sweeping scenes because the file
        // was never loaded.
        //
        // Game mode 0 is ordinary play. The title demo is 1 and the file select is 2, both seen
        // in a state trace rather than assumed.
        const Snapshot now = read();
        if (!now.valid || now.game_mode != 0) {
            // Lapse rather than wait. See PENDING_LIFETIME.
            if (lapsed(g_warp_set_at)) {
                g_pending_warp.store(-1, std::memory_order_relaxed);
            }
            return -1;
        }

        const int32_t taken = g_pending_warp.exchange(-1, std::memory_order_relaxed);
        if (taken >= 0) {
            g_warps_taken.fetch_add(1, std::memory_order_relaxed);
        }
        return taken;
    }

    uint32_t warps_taken() {
        return g_warps_taken.load(std::memory_order_relaxed);
    }

    void queue_pending_warp(int32_t entrance) {
        std::lock_guard<std::mutex> lock(g_warp_mutex);
        g_warp_queue.push_back(entrance);
    }

    void set_warp_dwell(int frames) {
        // Set once from the command line before the game starts, so no lock is needed. Clamped
        // because a dwell of zero would warp every frame and never let a scene finish loading.
        g_warp_dwell_frames = frames < 20 ? 20 : frames;
    }

    void set_warp_transition(int32_t type) {
        // The game has 56 types; anything else is its fade to black rather than a number it
        // would read as garbage.
        g_warp_transition.store((type >= 0 && type < 56) ? type : 2, std::memory_order_relaxed);
    }

    int32_t warp_transition() {
        return g_warp_transition.load(std::memory_order_relaxed);
    }

    int pending_warp_count() {
        std::lock_guard<std::mutex> lock(g_warp_mutex);
        return static_cast<int>(g_warp_queue.size());
    }

    // Called once per game frame by the patch, so it is also the frame counter for the dwell.
    int32_t take_queued_warp() {
        const Snapshot now = read();
        if (!now.valid || now.game_mode != 0) {
            return -1;
        }

        if (++g_frames_since_warp < g_warp_dwell_frames) {
            return -1;
        }

        std::lock_guard<std::mutex> lock(g_warp_mutex);
        if (g_warp_queue.empty()) {
            return -1;
        }
        const int32_t next = g_warp_queue.front();
        g_warp_queue.pop_front();
        g_frames_since_warp = 0;
        g_warps_taken.fetch_add(1, std::memory_order_relaxed);
        return next;
    }

} // namespace oot::game_state
