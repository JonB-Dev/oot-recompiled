// The running executable's own digital signature (the user's ask of 2026-09-19: "Digitally signed
// by Jonathan Barnes" in the launcher's foot, and the signature's details in About).
//
// Windows' own trust verification is asked, the way Explorer asks it, with two deliberate
// differences: no interface (the verdict is ours to show), and NO NETWORK. The chain engine may
// only use what is already on the machine (WTD_CACHE_ONLY_URL_RETRIEVAL) and revocation is not
// checked, because the program makes no connections (CLAUDE.md). A signature whose chain would
// need a download to complete is therefore "not verified" here, and the About surface says so
// with Windows' own reason rather than pretending either way.

#include <cstdio>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include "main/signature.h"

namespace {

    std::string narrow(const wchar_t* text) {
        if (text == nullptr || *text == L'\0') {
            return {};
        }
        const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1) {
            return {};
        }
        std::string out(static_cast<size_t>(size - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
        return out;
    }

    std::string name_of(PCCERT_CONTEXT cert, DWORD flags) {
        wchar_t buffer[256] = {};
        const DWORD n = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, buffer, 256);
        return n > 1 ? narrow(buffer) : std::string();
    }

    // A file time as the local "YYYY-MM-DD HH:MM", or empty for a zero time.
    std::string local_time(const FILETIME& ft) {
        if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) {
            return {};
        }
        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
            return {};
        }
        char text[32] = {};
        std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u",
                      local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
        return text;
    }

    std::string windows_message(LONG code) {
        wchar_t* buffer = nullptr;
        const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, static_cast<DWORD>(code), 0,
                                       reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        std::string out = n > 0 ? narrow(buffer) : std::string();
        if (buffer != nullptr) {
            LocalFree(buffer);
        }
        while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) {
            out.pop_back();
        }
        if (out.empty()) {
            char hex[24] = {};
            std::snprintf(hex, sizeof(hex), "code 0x%08lX", static_cast<unsigned long>(code));
            out = hex;
        }
        return out;
    }

} // namespace

oot::signature::Info oot::signature::read_self() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        Info info;
        info.status = "Not signed";
        info.error = "the executable's path is unknown";
        return info;
    }
    return read_file(path);
}

oot::signature::Info oot::signature::read_file(const std::wstring& file_path) {
    Info info;

    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = file_path.c_str();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG verdict = WinVerifyTrust(nullptr, &action, &data);

    if (verdict == TRUST_E_NOSIGNATURE) {
        info.present = false;
        info.valid = false;
        info.status = "Not signed";
    }
    else {
        info.present = true;
        info.valid = (verdict == ERROR_SUCCESS);
        if (!info.valid) {
            info.error = windows_message(verdict);
        }
    }

    // The details, from the provider's state: the signer's certificate and the countersignature.
    if (CRYPT_PROVIDER_DATA* prov = WTHelperProvDataFromStateData(data.hWVTStateData)) {
        if (CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0)) {
            if (signer->csCertChain > 0) {
                if (CRYPT_PROVIDER_CERT* cert = WTHelperGetProvCertFromChain(signer, 0)) {
                    if (cert->pCert != nullptr) {
                        info.signer = name_of(cert->pCert, 0);
                        info.issuer = name_of(cert->pCert, CERT_NAME_ISSUER_FLAG);
                        info.valid_from = local_time(cert->pCert->pCertInfo->NotBefore);
                        info.valid_until = local_time(cert->pCert->pCertInfo->NotAfter);
                    }
                }
            }
            if (signer->csCounterSigners > 0 && signer->pasCounterSigners != nullptr) {
                info.signed_on = local_time(signer->pasCounterSigners[0].sftVerifyAsOf);
            }
        }
    }

    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);

    if (info.present) {
        const std::string who = info.signer.empty() ? std::string("an unknown signer") : info.signer;
        if (info.valid) {
            info.status = "Digitally signed by " + who;
        }
        else {
            info.status = "Signed by " + who + ", not verified offline";
        }
    }

    std::fprintf(stderr, "[signature] %s%s%s\n", info.status.c_str(),
                 info.error.empty() ? "" : ": ", info.error.c_str());
    return info;
}
