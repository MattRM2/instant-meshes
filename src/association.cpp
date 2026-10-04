/*
    association.cpp: .imd files opened by Instant Meshes (see association.h)
*/

#include "association.h"

#if defined(_WIN32)
#  if !defined(NOMINMAX)
#    define NOMINMAX
#  endif
#  if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shlobj.h>

namespace {

const char *ProgId = "InstantMeshes.Project";

std::string executable() {
    char path[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameA(nullptr, path, (DWORD) sizeof path);
    return n > 0 && n < sizeof path ? std::string(path, n) : std::string();
}

/* HKEY_CURRENT_USER\Software\Classes\<key>, default value 'value' */
bool set(const std::string &key, const std::string &value, std::string &error) {
    HKEY h;
    const std::string full = "Software\\Classes\\" + key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, full.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) !=
        ERROR_SUCCESS) {
        error = "cannot write the registry key " + full;
        return false;
    }
    const LONG r = RegSetValueExA(h, nullptr, 0, REG_SZ, (const BYTE *) value.c_str(), (DWORD) value.size() + 1);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS) {
        error = "cannot write the registry value of " + full;
        return false;
    }
    return true;
}

std::string get(const std::string &key) {
    char buf[2048];
    DWORD size = sizeof buf;
    const std::string full = "Software\\Classes\\" + key;
    if (RegGetValueA(HKEY_CURRENT_USER, full.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return std::string();
    return buf;
}

} // namespace

bool register_imd_files(std::string &error) {
    const std::string exe = executable();
    if (exe.empty()) {
        error = "cannot find the path of the executable";
        return false;
    }
    const std::string id = ProgId;
    if (!set(".imd", id, error) || !set(id, "Instant Meshes project", error) ||
        !set(id + "\\DefaultIcon", "\"" + exe + "\",-101", error) ||
        !set(id + "\\shell\\open\\command", "\"" + exe + "\" \"%1\"", error))
        return false;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool imd_files_registered() {
    const std::string exe = executable();
    return get(".imd") == ProgId &&
           get(std::string(ProgId) + "\\shell\\open\\command") == "\"" + exe + "\" \"%1\"";
}

#else

bool register_imd_files(std::string &error) {
    error = "file associations are set by the desktop environment on this system";
    return false;
}

bool imd_files_registered() { return false; }

#endif
