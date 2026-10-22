// The update check: the ONE network connection this program makes.
//
// Read `.scaffold/security/edge-serverside.md` before changing anything here. It is the threat
// model for this file and it was written before this file was, because "no network at all" used to
// be a property of this program and stopped being one on the user's explicit decision.
//
// WHAT IT DOES: one HTTPS GET of a fixed URL, once at launch, only when the setting is on. It
// reads a small manifest, compares the version in it with this build's, and reports whether a
// newer one exists. When one does, and this copy is INSTALLED rather than portable, it fetches
// that version's setup file in the background (one more GET, same host, a name from the
// manifest checked against the fixed prefix), verifies it twice, and keeps it in the data
// folder for the next start. It never replaces the program while it is running: the install
// happens on the next launch, before the window opens, or when the person presses F5 or
// clicks the plate. The background fetch is the user's decision of 2026-09-23 ("it can
// download in bg and say to the user the new update will install next restart"), replacing
// the earlier rule that a key press came first. `.scaffold/security/edge-serverside.md`
// carries the change.
//
// WHAT IT CANNOT DO, by construction rather than by policy:
//   - reach any host but the one compiled in. There is no code path that takes a URL.
//   - send anything about the machine, the user or their copy of the game. It is a GET.
//   - run before the person has answered the first run question, because the setting it reads
//     defaults to off and the prompt is what turns it on.
//   - stop somebody playing. Every failure is a silent no-op: no network, a refused name, a 404,
//     a truncated read, a manifest full of garbage. It runs off the main thread and its answer
//     arrives when it arrives, or never.

#pragma once

#include <string>

namespace oot::updates {

    // The public release page's manifest for this program. A compile time constant, quoted here
    // so the one place a host name appears is visible in a header rather than buried.
    constexpr const wchar_t* HOST = L"releases.jonbarnes.dev";
    constexpr const wchar_t* PATH = L"/oot-recompiled/latest.yml";

    // The publisher the downloaded file's Authenticode signature must name. "Is signed" is not
    // sufficient and never was: plenty of malware is signed by somebody. This is the second of
    // the two independent checks, the first being the SHA-512 from the manifest.
    constexpr const char* PUBLISHER = "Jonathan Barnes";

    enum class State {
        // Never asked, because the setting is off or the check has not run yet.
        Idle,
        Checking,
        // The answer came back and this build is the newest named.
        Current,
        // A newer version exists. `available` holds it.
        Newer,
        // Something did not work. Deliberately indistinguishable to the user from Current: a
        // failed check is not news, and a program that reports its own plumbing at somebody who
        // wanted to play is worse than one that says nothing.
        Failed,

        // The states of the INSTALL, which only ever begins because a person asked for it.
        Downloading,
        // Downloaded, digest matched, signature verified and naming our publisher. Ready to run.
        Ready,
        // Downloaded and REFUSED. This one IS reported, loudly, because a file that arrived from
        // the network and failed verification is the one thing here worth interrupting somebody
        // about. The file is deleted before this is set.
        Refused,
    };

    // Start the check on its own thread, if the setting is on and it has not already run this
    // session. Returns immediately. Safe to call when the setting is off: it does nothing.
    void start_if_enabled();

    // Called often (the interface's poll), cheap, and the reason the check is not only a launch
    // event: if the setting is on and no check has run, it starts one (the row was switched on
    // mid session); if the last check found nothing, or failed, and four hours have passed, it
    // checks again (the user, 2026-09-23: "the same update check every 4 hours, or on launch,
    // just like my others"). It never interrupts a fetch, a ready file or a refusal.
    void tick();

    State state();

    // The newer version, when state() is Newer. Empty otherwise.
    std::string available();

    // One line for the launcher's foot, or empty when there is nothing worth saying.
    std::string line();

    // Fetch the newer version and check it, on its own thread. Called by the check itself once
    // it has found a newer version (installed copies only), and harmless to call again. Does
    // nothing unless state() is Newer.
    void start_download();

    // What the in-game plate shows, or false when there is nothing worth a plate: the newer
    // version being fetched, a verified file waiting for the next start, or a download that
    // failed verification and was deleted. `value` is the short figure, `note` the words beside.
    bool plate(std::string& value, std::string& note);

    // AT BOOT, BEFORE THE WINDOW: if a setup file left by a previous session waits in the
    // updates folder, names a version newer than this one, and carries a trusted signature by
    // our publisher, run it and return true, in which case the caller exits so the installer
    // can replace the files and start the new version. Anything in that folder that is not
    // exactly that is deleted. Portable copies never install this way.
    bool install_pending();

    // Run the verified setup file and return true if it started, in which case the caller should
    // quit so the installer can replace the files. THE INSTALL IS THE SETUP FILE: running it is
    // the update, which is the whole reason the packaging work came first. Refuses unless
    // state() is Ready.
    bool install();

} // namespace oot::updates
