// The running executable's own digital signature, read once at start for the launcher's foot and
// the About surface. Knows Windows; knows nothing of the game.

#pragma once

#include <string>

namespace oot::signature {

    struct Info {
        // An Authenticode signature is attached to the file.
        bool present = false;
        // And Windows trusts it, checked without the network (a chain that would need a download
        // to complete is reported as not verified, never fetched).
        bool valid = false;
        // One line for a foot: "Digitally signed by <name>", "Not signed", or the reason.
        std::string status;
        std::string signer;       // the certificate's subject, as Windows displays it
        std::string issuer;       // the certificate's issuer
        std::string signed_on;    // the countersignature's time, local, or empty
        std::string valid_from;   // the certificate's validity
        std::string valid_until;
        std::string error;        // when present and not valid: Windows' reason
    };

    // The executable this code is running in.
    Info read_self();

    // Any file on disk, by path. Used by the updater to check what it downloaded BEFORE anything
    // runs it, which is the whole point of signing: "is signed" is not sufficient, so the caller
    // must also check that `signer` is the publisher it expects (plenty of malware is signed by
    // somebody). See .scaffold/security/edge-serverside.md.
    Info read_file(const std::wstring& path);

} // namespace oot::signature
