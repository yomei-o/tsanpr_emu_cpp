// The registry, read from the host.
//
// "There is no registry" is a real answer, and a runtime installed without one
// copes - which is why the emulator got away with it for a long time.  What it
// does not survive is a guest that keeps its *own* state there: it does not
// degrade, it concludes its state was never written, and reports whatever that
// means to it.
//
// So on a Windows host the keys are the host's own, for the same reason the
// guest inherits the host's environment, filesystem and WMI: the emulator runs
// on that machine, and a guest asking about the machine should be told the
// truth about it.
//
// Reads only.  A write would be a change to the host outside anything the user
// asked the emulator to do, and there is no way to undo it when the guest
// exits, so those still refuse.  Everywhere else - and when the host has no
// registry to bridge to - the answers fall back to "not present".
#include <cstring>
#include <map>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "emulator.h"
#include "guest_printf.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace x86emu {
namespace {

constexpr uint64_t kOk = 0;
constexpr uint64_t kFileNotFound = 2;
constexpr uint64_t kAccessDenied = 5;
constexpr uint64_t kMoreData = 234;
constexpr uint64_t kNoMoreItems = 259;

#if defined(_WIN32)

// Guest HKEYs are small opaque numbers of the emulator's own making, so a
// predefined root is recognised by value and an opened key by table lookup.
// Nothing in guest memory ever holds a host pointer.
struct RegistryState {
    std::map<uint64_t, HKEY> open;
    uint64_t next = 0x00210000;
};

HKEY predefined_root(uint64_t h) {
    switch (static_cast<uint32_t>(h)) {
        case 0x80000000u: return HKEY_CLASSES_ROOT;
        case 0x80000001u: return HKEY_CURRENT_USER;
        case 0x80000002u: return HKEY_LOCAL_MACHINE;
        case 0x80000003u: return HKEY_USERS;
        case 0x80000005u: return HKEY_CURRENT_CONFIG;
        default: return nullptr;
    }
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

std::string from_wide(const wchar_t* w, size_t len) {
    if (!w || len == 0) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0,
                                nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), &s[0], n, nullptr, nullptr);
    return s;
}

#else  // !_WIN32

// A registry the emulator keeps itself, loaded from regedit exports.
//
// On a host with no registry the honest answer used to be the only answer, and
// for a runtime that merely looks for optional settings it is a fine one.  It is
// not enough for a guest whose own state lives there: told "not present", it
// concludes its state was never written and reports whatever that means to it.
// Reading it from the same .reg files a Windows host would have been holding
// puts that state back without pretending anything about the host.

struct RegValue {
    uint32_t type = 0;  // REG_SZ = 1, REG_BINARY = 3, REG_DWORD = 4, ...
    std::vector<uint8_t> data;
};
// Full path, lowercased: "hkey_local_machine\\software\\policies\\...".  Value
// names are lowercased too; the default value is the empty name, as @ means.
using RegKeys = std::map<std::string, std::map<std::string, RegValue>>;

std::string reg_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// regedit writes UTF-16LE with a BOM; nothing in a key path or a value name
// needs more than the BMP, so this is the short version of the conversion.
std::string reg_text(const std::vector<uint8_t>& raw) {
    if (raw.size() < 2 || raw[0] != 0xFF || raw[1] != 0xFE)
        return std::string(raw.begin(), raw.end());  // already UTF-8
    std::string out;
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
        uint32_t c = static_cast<uint32_t>(raw[i]) | (static_cast<uint32_t>(raw[i + 1]) << 8);
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// REG_SZ in the registry is UTF-16 with its terminator, which is what a guest
// calling the W entry points expects to be handed back.
void reg_put_utf16(std::vector<uint8_t>& out, const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        uint32_t c = static_cast<uint8_t>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 1;
        if (n == 2 && i + 1 < s.size())
            c = ((c & 0x1F) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3F);
        else if (n == 3 && i + 2 < s.size())
            c = ((c & 0x0F) << 12) | ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 6) |
                (static_cast<uint8_t>(s[i + 2]) & 0x3F);
        out.push_back(static_cast<uint8_t>(c & 0xFF));
        out.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
        i += n;
    }
    out.push_back(0);
    out.push_back(0);
}

bool parse_reg_file(const std::string& path, RegKeys& keys, std::string& err) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> raw;
    uint8_t buf[4096];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, fp)) > 0) raw.insert(raw.end(), buf, buf + got);
    std::fclose(fp);

    std::string text = reg_text(raw);
    // Join the trailing-backslash continuations a long hex: value is split over
    // before anything looks at a line.
    std::vector<std::string> lines;
    {
        std::string cur;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                while (!cur.empty() && (cur.back() == '\r' || cur.back() == ' ' || cur.back() == '\t'))
                    cur.pop_back();
                if (!cur.empty() && cur.back() == '\\') {
                    cur.pop_back();
                    continue;  // the next line is more of this value
                }
                lines.push_back(cur);
                cur.clear();
            } else {
                cur += text[i];
            }
        }
    }

    std::string key;
    int n_values = 0;
    for (std::string line : lines) {
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        line = line.substr(b);
        if (line[0] == ';') continue;
        if (line.compare(0, 8, "Windows ") == 0 || line.compare(0, 5, "REGED") == 0) continue;
        if (line[0] == '[') {
            size_t e = line.rfind(']');
            key = e == std::string::npos ? line.substr(1) : line.substr(1, e - 1);
            if (!key.empty() && key[0] == '-') { key.clear(); continue; }  // a deletion
            keys[reg_lower(key)];  // an existing key with no values is still a key
            continue;
        }
        if (key.empty()) continue;
        size_t eq = std::string::npos;
        if (line[0] == '@') {
            eq = line.find('=');
        } else if (line[0] == '"') {  // "name"=... - the name may contain an =
            size_t q = 1;
            while (q < line.size() && !(line[q] == '"' && line[q - 1] != '\\')) ++q;
            eq = line.find('=', q);
        }
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq), spec = line.substr(eq + 1);
        if (name == "@") {
            name.clear();
        } else if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
            name = name.substr(1, name.size() - 2);
        }

        RegValue v;
        if (!spec.empty() && spec[0] == '"') {  // REG_SZ
            std::string s;
            for (size_t i = 1; i < spec.size() && spec[i] != '"'; ++i) {
                if (spec[i] == '\\' && i + 1 < spec.size()) ++i;
                s += spec[i];
            }
            v.type = 1;
            reg_put_utf16(v.data, s);
        } else if (spec.compare(0, 6, "dword:") == 0) {
            uint32_t d = static_cast<uint32_t>(std::strtoul(spec.c_str() + 6, nullptr, 16));
            v.type = 4;
            for (int i = 0; i < 4; ++i) v.data.push_back(static_cast<uint8_t>((d >> (8 * i)) & 0xFF));
        } else if (spec.compare(0, 3, "hex") == 0) {
            size_t colon = spec.find(':');
            if (colon == std::string::npos) continue;
            v.type = 3;  // REG_BINARY unless hex(N): says otherwise
            if (spec[3] == '(') v.type = static_cast<uint32_t>(std::strtoul(spec.c_str() + 4, nullptr, 16));
            for (size_t i = colon + 1; i + 1 < spec.size() + 1; i += 3) {
                if (i + 1 >= spec.size()) break;
                v.data.push_back(static_cast<uint8_t>(std::strtoul(spec.substr(i, 2).c_str(), nullptr, 16)));
            }
        } else {
            continue;  // not a form regedit writes
        }
        keys[reg_lower(key)][reg_lower(name)] = std::move(v);
        ++n_values;
    }
    std::fprintf(stderr, "[registry] %s: %zu key(s), %d value(s)\n", path.c_str(),
                 keys.size(), n_values);
    return true;
}

// The predefined roots, spelled the way a .reg file spells them.
const char* reg_root_name(uint64_t h) {
    switch (static_cast<uint32_t>(h)) {
        case 0x80000000u: return "hkey_classes_root";
        case 0x80000001u: return "hkey_current_user";
        case 0x80000002u: return "hkey_local_machine";
        case 0x80000003u: return "hkey_users";
        case 0x80000005u: return "hkey_current_config";
        default: return nullptr;
    }
}

#endif  // _WIN32

}  // namespace

void Emulator::install_registry_hooks() {
#if defined(_WIN32)
    auto state = std::make_shared<RegistryState>();

    // The host key a guest handle names, or null if it names nothing.
    // The low 32 bits identify the handle: an HKEY is pointer-sized, but this
    // guest keeps one in a 32-bit slot and hands it back with whatever was in the
    // upper half (0x412D6177_00310000 was one, seen on the non-Windows path where
    // the same guest code runs).  These numbers are the emulator's own and stay
    // small precisely so that losing the upper half loses nothing.
    auto resolve = [state](uint64_t h) -> HKEY {
        if (HKEY root = predefined_root(h)) return root;
        auto it = state->open.find(h & 0xFFFFFFFFull);
        return it == state->open.end() ? nullptr : it->second;
    };
    auto read_name = [](Emulator& e, uint64_t p, bool wide) -> std::string {
        if (!p) return "";
        return wide ? utf16_to_utf8(e, p, -1) : e.mem.read_cstring(p);
    };

    // RegOpenKeyEx(hKey, subkey, options, desired, phkResult)
    auto open_key = [state, resolve, read_name](Emulator& e, bool wide) {
        HKEY parent = resolve(e.arg_slot(0));
        std::string sub = read_name(e, e.arg_slot(1), wide);
        uint64_t out = e.arg_slot(4);
        if (!parent || !out) {
            e.log_call("RegOpenKeyEx(%s) -> no such root", sub.c_str());
            e.set_result(kFileNotFound);
            return;
        }
        HKEY opened = nullptr;
        // KEY_READ regardless of what was asked for: this bridge never writes,
        // and asking the host for write access would fail on keys a read would
        // have succeeded on.
        LONG r = RegOpenKeyExW(parent, to_wide(sub).c_str(), 0, KEY_READ, &opened);
        if (r != ERROR_SUCCESS) {
            e.log_call("RegOpenKeyEx(%s) -> %ld", sub.c_str(), static_cast<long>(r));
            e.set_result(static_cast<uint64_t>(r));
            return;
        }
        uint64_t handle = state->next += 0x10;
        state->open[handle & 0xFFFFFFFFull] = opened;
        e.log_call("RegOpenKeyEx(%s) -> 0x%llX", sub.c_str(),
                   static_cast<unsigned long long>(handle));
        e.mem.write_sized(out, e.pointer_size(), handle);
        e.set_result(kOk);
    };
    add_hook("RegOpenKeyExW", 0, [open_key](Emulator& e) { open_key(e, true); });
    add_hook("RegOpenKeyExA", 0, [open_key](Emulator& e) { open_key(e, false); });
    add_hook("RegOpenKeyW", 0, [state, resolve, read_name](Emulator& e) {
        // The old three-argument form: no options, no access mask.
        HKEY parent = resolve(e.arg_slot(0));
        std::string sub = read_name(e, e.arg_slot(1), true);
        uint64_t out = e.arg_slot(2);
        HKEY opened = nullptr;
        if (!parent || !out ||
            RegOpenKeyExW(parent, to_wide(sub).c_str(), 0, KEY_READ, &opened) !=
                ERROR_SUCCESS) {
            e.set_result(kFileNotFound);
            return;
        }
        uint64_t handle = state->next += 0x10;
        state->open[handle & 0xFFFFFFFFull] = opened;
        e.mem.write_sized(out, e.pointer_size(), handle);
        e.set_result(kOk);
    });

    // RegQueryValueEx(hKey, valueName, reserved, pType, pData, pcbData)
    auto query_value = [resolve, read_name](Emulator& e, bool wide) {
        HKEY key = resolve(e.arg_slot(0));
        std::string name = read_name(e, e.arg_slot(1), wide);
        if (std::getenv("EMU_FP_TRACE") && name.size() == 32 &&
            name.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
            uint64_t sp = e.cpu().regs[RSP];
            std::fprintf(stderr, "[fp] RegQueryValueEx(%s) rsp=%016llx\n",
                         name.c_str(), (unsigned long long)sp);
            for (uint64_t off = 0; off <= 0x340; off += 8) {
                uint64_t v = e.mem.read64(sp + off);
                if (v >= 0x140100000ull && v < 0x14231b470ull)
                    std::fprintf(stderr, "  [rsp+%03llx] %016llx  dll+%llx\n",
                                 (unsigned long long)off, (unsigned long long)v,
                                 (unsigned long long)(v - 0x140100000ull));
            }
        }
        uint64_t type_out = e.arg_slot(3), data_out = e.arg_slot(4);
        uint64_t size_out = e.arg_slot(5);
        if (!key) {
            e.set_result(kFileNotFound);
            return;
        }
        // Diagnostic: when a 32-hex fingerprint value name misses, redirect to
        // whatever real 32-hex value already exists in this key (the machine's
        // own registered license), to test whether the fingerprint mismatch is
        // the ONLY thing standing between the emulated engine and a full run.
        if (std::getenv("EMU_FP_REDIRECT") && name.size() == 32 &&
            name.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
            LONG probe = RegQueryValueExW(key, to_wide(name).c_str(), nullptr, nullptr, nullptr, nullptr);
            if (probe != ERROR_SUCCESS) {
                for (DWORD idx = 0;; ++idx) {
                    wchar_t vn[256]; DWORD vlen = 256;
                    LONG er = RegEnumValueW(key, idx, vn, &vlen, nullptr, nullptr, nullptr, nullptr);
                    if (er != ERROR_SUCCESS) break;
                    std::string cand = from_wide(vn, vlen);
                    if (cand.size() == 32 &&
                        cand.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
                        std::fprintf(stderr, "[redirect] %s -> existing %s\n", name.c_str(), cand.c_str());
                        name = cand;
                        break;
                    }
                }
            }
        }
        // Ask the host how big it is, then read it whole: the guest's buffer
        // size is only used to decide what it gets back, so a value that grew
        // between the two calls cannot truncate anything here.
        DWORD type = 0, bytes = 0;
        LONG r = RegQueryValueExW(key, to_wide(name).c_str(), nullptr, &type, nullptr,
                                  &bytes);
        if (r != ERROR_SUCCESS) {
            e.log_call("RegQueryValueEx(%s) -> %ld", name.c_str(), static_cast<long>(r));
            e.set_result(static_cast<uint64_t>(r));
            return;
        }
        std::vector<unsigned char> value(bytes ? bytes : 1, 0);
        DWORD got = bytes;
        r = RegQueryValueExW(key, to_wide(name).c_str(), nullptr, &type, value.data(),
                             &got);
        if (r != ERROR_SUCCESS) {
            e.set_result(static_cast<uint64_t>(r));
            return;
        }
        // A narrow caller wants the string forms in its own code page; every
        // other type is bytes and passes through untouched.
        std::string narrow;
        if (!wide && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            narrow = from_wide(reinterpret_cast<const wchar_t*>(value.data()),
                               got / sizeof(wchar_t));
            if (!narrow.empty() && narrow.back() == '\0') narrow.pop_back();
            value.assign(narrow.begin(), narrow.end());
            value.push_back('\0');
            got = static_cast<DWORD>(value.size());
        }
        if (type_out) e.mem.write32(type_out, type);
        uint64_t room = size_out ? e.mem.read32(size_out) : 0;
        if (size_out) e.mem.write32(size_out, got);
        if (!data_out) {
            e.set_result(kOk);  // a size query
            return;
        }
        if (room < got) {
            e.set_result(kMoreData);
            return;
        }
        e.mem.write(data_out, value.data(), got);
        std::string shown;
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            shown = wide ? from_wide(reinterpret_cast<const wchar_t*>(value.data()),
                                     got / sizeof(wchar_t))
                         : std::string(reinterpret_cast<const char*>(value.data()),
                                       got ? got - 1 : 0);
            while (!shown.empty() && shown.back() == '\0') shown.pop_back();
        }
        e.log_call("RegQueryValueEx(%s) -> type %u, %u of %u byte(s) into %u \"%s\"",
                   name.c_str(), static_cast<unsigned>(type),
                   static_cast<unsigned>(got), static_cast<unsigned>(bytes),
                   static_cast<unsigned>(room), shown.c_str());
        // Diagnostic: report the guest address the value landed at, so a read
        // watchpoint can catch where the licensing code consumes it.
        if (std::getenv("EMU_REGADDR"))
            std::fprintf(stderr, "[regaddr] value \"%s\" data at guest %llX (%u bytes)\n",
                         name.c_str(), (unsigned long long)data_out, static_cast<unsigned>(got));
        e.set_result(kOk);
    };
    add_hook("RegQueryValueExW", 0, [query_value](Emulator& e) { query_value(e, true); });
    add_hook("RegQueryValueExA", 0, [query_value](Emulator& e) { query_value(e, false); });

    // RegEnumKeyEx(hKey, index, name, pcchName, reserved, class, pcchClass, ft)
    add_hook("RegEnumKeyExW", 0, [resolve](Emulator& e) {
        HKEY key = resolve(e.arg_slot(0));
        uint64_t name_out = e.arg_slot(2), size_out = e.arg_slot(3);
        if (!key || !name_out || !size_out) {
            e.set_result(kNoMoreItems);
            return;
        }
        wchar_t name[256];
        DWORD chars = 256;
        LONG r = RegEnumKeyExW(key, static_cast<DWORD>(e.arg_slot(1)), name, &chars,
                               nullptr, nullptr, nullptr, nullptr);
        if (r != ERROR_SUCCESS) {
            e.set_result(static_cast<uint64_t>(r));
            return;
        }
        uint64_t room = e.mem.read32(size_out);
        if (room < chars + 1) {
            e.set_result(kMoreData);
            return;
        }
        for (DWORD i = 0; i <= chars; ++i)
            e.mem.write16(name_out + i * 2, i < chars ? name[i] : 0);
        e.mem.write32(size_out, chars);
        e.set_result(kOk);
    });

    // RegQueryInfoKey(hKey, class, pcchClass, reserved, pcSubKeys, pcbMaxSubKeyLen,
    //                 pcbMaxClassLen, pcValues, pcbMaxValueNameLen,
    //                 pcbMaxValueLen, pcbSecurityDescriptor, pftLastWriteTime)
    add_hook("RegQueryInfoKeyW", 0, [resolve](Emulator& e) {
        HKEY key = resolve(e.arg_slot(0));
        if (!key) {
            e.set_result(kFileNotFound);
            return;
        }
        DWORD subkeys = 0, max_subkey = 0, values = 0, max_name = 0, max_value = 0;
        LONG r = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, &max_subkey,
                                  nullptr, &values, &max_name, &max_value, nullptr,
                                  nullptr);
        if (r != ERROR_SUCCESS) {
            e.set_result(static_cast<uint64_t>(r));
            return;
        }
        if (e.arg_slot(4)) e.mem.write32(e.arg_slot(4), subkeys);
        if (e.arg_slot(5)) e.mem.write32(e.arg_slot(5), max_subkey);
        if (e.arg_slot(7)) e.mem.write32(e.arg_slot(7), values);
        if (e.arg_slot(8)) e.mem.write32(e.arg_slot(8), max_name);
        if (e.arg_slot(9)) e.mem.write32(e.arg_slot(9), max_value);
        e.set_result(kOk);
    });

    add_hook("RegCloseKey", 0, [state](Emulator& e) {
        auto it = state->open.find(e.arg_slot(0) & 0xFFFFFFFFull);
        if (it != state->open.end()) {
            RegCloseKey(it->second);
            state->open.erase(it);
        }
        e.set_result(kOk);
    });

    // ---- writing -------------------------------------------------------------
    // A write outlives the run and nothing here undoes it, so it is refused
    // unless the emulator was started with --registry-write.  With the flag, an
    // installer running as a guest installs for real - which is the point of
    // running one at all.
    if (!options().registry_write) {
        for (const char* name : {"RegCreateKeyW", "RegCreateKeyExW", "RegCreateKeyExA",
                                 "RegSetValueExW", "RegSetValueExA", "RegDeleteKeyW",
                                 "RegDeleteKeyExW", "RegDeleteValueW", "RegSaveKeyW",
                                 "RegLoadKeyW"}) {
            add_hook(name, 0, [](Emulator& e) { e.set_result(kAccessDenied); });
        }
        return;
    }

    // RegCreateKeyEx(hKey, subkey, reserved, class, options, desired, security,
    //                phkResult, pdwDisposition)
    auto create_key = [state, resolve, read_name](Emulator& e, bool wide) {
        HKEY parent = resolve(e.arg_slot(0));
        std::string sub = read_name(e, e.arg_slot(1), wide);
        uint64_t out = e.arg_slot(7), disposition = e.arg_slot(8);
        if (!parent || !out) {
            e.set_result(kFileNotFound);
            return;
        }
        HKEY opened = nullptr;
        DWORD made = 0;
        LONG r = RegCreateKeyExW(parent, to_wide(sub).c_str(), 0, nullptr, 0,
                                 KEY_READ | KEY_WRITE, nullptr, &opened, &made);
        e.log_call("RegCreateKeyEx(%s) -> %ld", sub.c_str(), static_cast<long>(r));
        if (r != ERROR_SUCCESS) {
            e.set_result(static_cast<uint64_t>(r));
            return;
        }
        uint64_t handle = state->next += 0x10;
        state->open[handle & 0xFFFFFFFFull] = opened;
        e.mem.write_sized(out, e.pointer_size(), handle);
        if (disposition) e.mem.write32(disposition, made);
        e.set_result(kOk);
    };
    add_hook("RegCreateKeyExW", 0, [create_key](Emulator& e) { create_key(e, true); });
    add_hook("RegCreateKeyExA", 0, [create_key](Emulator& e) { create_key(e, false); });

    // RegSetValueEx(hKey, valueName, reserved, type, pData, cbData)
    auto set_value = [resolve, read_name](Emulator& e, bool wide) {
        HKEY key = resolve(e.arg_slot(0));
        std::string name = read_name(e, e.arg_slot(1), wide);
        DWORD type = static_cast<DWORD>(e.arg_slot(3));
        uint64_t data = e.arg_slot(4);
        DWORD bytes = static_cast<DWORD>(e.arg_slot(5));
        if (!key) {
            e.set_result(kFileNotFound);
            return;
        }
        std::vector<unsigned char> value(bytes ? bytes : 1, 0);
        if (data && bytes) e.mem.read(data, value.data(), bytes);
        // A narrow caller's string has to reach the registry as the wide form
        // the registry stores, or every later read comes back as mojibake.
        std::wstring widened;
        if (!wide && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            std::string narrow(reinterpret_cast<const char*>(value.data()), bytes);
            while (!narrow.empty() && narrow.back() == '\0') narrow.pop_back();
            widened = to_wide(narrow);
            value.assign(reinterpret_cast<const unsigned char*>(widened.c_str()),
                         reinterpret_cast<const unsigned char*>(widened.c_str()) +
                             (widened.size() + 1) * sizeof(wchar_t));
            bytes = static_cast<DWORD>(value.size());
        }
        LONG r = RegSetValueExW(key, to_wide(name).c_str(), 0, type, value.data(), bytes);
        e.log_call("RegSetValueEx(%s, type %u, %u byte(s)) -> %ld", name.c_str(),
                   static_cast<unsigned>(type), static_cast<unsigned>(bytes),
                   static_cast<long>(r));
        e.set_result(static_cast<uint64_t>(r));
    };
    add_hook("RegSetValueExW", 0, [set_value](Emulator& e) { set_value(e, true); });
    add_hook("RegSetValueExA", 0, [set_value](Emulator& e) { set_value(e, false); });

    add_hook("RegDeleteValueW", 0, [resolve, read_name](Emulator& e) {
        HKEY key = resolve(e.arg_slot(0));
        std::string name = read_name(e, e.arg_slot(1), true);
        if (!key) {
            e.set_result(kFileNotFound);
            return;
        }
        LONG r = RegDeleteValueW(key, to_wide(name).c_str());
        e.log_call("RegDeleteValue(%s) -> %ld", name.c_str(), static_cast<long>(r));
        e.set_result(static_cast<uint64_t>(r));
    });
    // Deleting a whole key is a bigger step than any installer here needs.
    for (const char* name : {"RegDeleteKeyW", "RegDeleteKeyExW", "RegSaveKeyW",
                             "RegLoadKeyW"}) {
        add_hook(name, 0, [](Emulator& e) { e.set_result(kAccessDenied); });
    }
#else  // !_WIN32
    // Nothing given, nothing registered: the answers stay "not present", which is
    // what they were before and what hooks_win32_fs.cpp still says.
    if (options().registry_files.empty()) return;

    struct State {
        RegKeys keys;
        // Guest handle -> full lowercased path.  Keyed by the low 32 bits: an HKEY
        // is pointer-sized, but this guest keeps one in a 32-bit slot and hands it
        // back with whatever was in the upper half (0x412D6177_00310000 was one).
        // The emulator mints these numbers itself and keeps them small precisely so
        // that losing the upper half loses nothing.
        std::map<uint32_t, std::string> open;
        uint32_t next = 0x00310000;
    };
    auto state = std::make_shared<State>();
    for (const std::string& f : options().registry_files) {
        std::string err;
        if (!parse_reg_file(f, state->keys, err)) std::fprintf(stderr, "[registry] %s\n", err.c_str());
    }
    if (state->keys.empty()) return;

    auto path_of = [state](uint64_t h) -> std::string {
        if (const char* r = reg_root_name(h)) return r;
        auto it = state->open.find(static_cast<uint32_t>(h));
        return it == state->open.end() ? std::string() : it->second;
    };
    auto read_name = [](Emulator& e, uint64_t p, bool wide) -> std::string {
        if (!p) return std::string();
        return wide ? utf16_to_utf8(e, p, -1) : e.mem.read_cstring(p);
    };
    // A key is there if it holds values or if anything below it does - an export
    // names only the keys that have something, and a guest may open a parent.
    auto key_exists = [state](const std::string& p) {
        if (state->keys.count(p)) return true;
        std::string below = p + "\\";
        auto it = state->keys.lower_bound(below);
        return it != state->keys.end() && it->first.compare(0, below.size(), below) == 0;
    };

    // RegOpenKeyEx(hKey, subkey, options, desired, phkResult)
    auto open_key = [state, path_of, read_name, key_exists](Emulator& e, bool wide) {
        std::string parent = path_of(e.arg_slot(0));
        std::string sub = read_name(e, e.arg_slot(1), wide);
        uint64_t out = e.arg_slot(4);
        std::string full = parent;
        if (!sub.empty()) full += (full.empty() ? "" : "\\") + reg_lower(sub);
        if (parent.empty() || !out || !key_exists(full)) {
            e.log_call("RegOpenKeyEx(%s) -> not present", sub.c_str());
            e.set_result(kFileNotFound);
            return;
        }
        uint32_t h = state->next++;
        state->open[h] = full;
        e.mem.write_sized(out, e.pointer_size(), h);
        e.log_call("RegOpenKeyEx(%s) -> 0x%llX [%s]", sub.c_str(),
                   static_cast<unsigned long long>(h), full.c_str());
        e.set_result(kOk);
    };
    add_hook("RegOpenKeyExW", 0, [open_key](Emulator& e) { open_key(e, true); });
    add_hook("RegOpenKeyExA", 0, [open_key](Emulator& e) { open_key(e, false); });

    // RegQueryValueEx(hKey, name, reserved, lpType, lpData, lpcbData).  A null
    // lpData is the size query every caller makes first.
    auto query_value = [state, path_of, read_name](Emulator& e, bool wide) {
        std::string key = path_of(e.arg_slot(0));
        std::string name = reg_lower(read_name(e, e.arg_slot(1), wide));
        uint64_t ptype = e.arg_slot(3), pdata = e.arg_slot(4), pcb = e.arg_slot(5);
        auto k = state->keys.find(key);
        if (key.empty() || k == state->keys.end()) {
            e.log_call("RegQueryValueEx(%s) -> no such key '%s' (handle 0x%llX)", name.c_str(),
                       key.c_str(), static_cast<unsigned long long>(e.arg_slot(0)));
            e.set_result(kFileNotFound);
            return;
        }
        auto v = k->second.find(name);
        if (v == k->second.end()) {
            e.log_call("RegQueryValueEx(%s) -> not present", name.c_str());
            e.set_result(kFileNotFound);
            return;
        }
        uint32_t need = static_cast<uint32_t>(v->second.data.size());
        uint32_t cap = pcb ? e.mem.read32(pcb) : 0;
        if (ptype) e.mem.write32(ptype, v->second.type);
        if (pcb) e.mem.write32(pcb, need);
        if (!pdata) {
            e.log_call("RegQueryValueEx(%s) -> type %u, %u bytes (size query)", name.c_str(),
                       v->second.type, need);
            e.set_result(kOk);
            return;
        }
        if (cap < need) { e.set_result(kMoreData); return; }
        for (uint32_t i = 0; i < need; ++i) e.mem.write8(pdata + i, v->second.data[i]);
        e.log_call("RegQueryValueEx(%s) -> type %u, %u bytes into 0x%llX", name.c_str(),
                   v->second.type, need, static_cast<unsigned long long>(pdata));
        e.set_result(kOk);
    };
    add_hook("RegQueryValueExW", 0, [query_value](Emulator& e) { query_value(e, true); });
    add_hook("RegQueryValueExA", 0, [query_value](Emulator& e) { query_value(e, false); });

    // RegEnumValueW(hKey, index, name, pcchName, reserved, lpType, lpData, lpcbData)
    add_hook("RegEnumValueW", 0, [state, path_of](Emulator& e) {
        auto k = state->keys.find(path_of(e.arg_slot(0)));
        uint32_t index = static_cast<uint32_t>(e.arg_slot(1));
        if (k == state->keys.end() || index >= k->second.size()) {
            e.set_result(kNoMoreItems);
            return;
        }
        auto it = k->second.begin();
        std::advance(it, index);
        uint64_t pname = e.arg_slot(2), pcch = e.arg_slot(3), ptype = e.arg_slot(5),
                 pdata = e.arg_slot(6), pcb = e.arg_slot(7);
        // The name count is in characters and excludes the terminator going in,
        // and excludes it coming back out too.
        uint32_t chars = static_cast<uint32_t>(it->first.size());
        if (pcch && e.mem.read32(pcch) <= chars) { e.set_result(kMoreData); return; }
        if (pname)
            for (uint32_t i = 0; i <= chars; ++i)
                e.mem.write16(pname + i * 2, i < chars ? static_cast<uint8_t>(it->first[i]) : 0);
        if (pcch) e.mem.write32(pcch, chars);
        if (ptype) e.mem.write32(ptype, it->second.type);
        uint32_t need = static_cast<uint32_t>(it->second.data.size());
        uint32_t cap = pcb ? e.mem.read32(pcb) : 0;
        if (pcb) e.mem.write32(pcb, need);
        if (pdata) {
            if (cap < need) { e.set_result(kMoreData); return; }
            for (uint32_t i = 0; i < need; ++i) e.mem.write8(pdata + i, it->second.data[i]);
        }
        e.set_result(kOk);
    });

    // RegQueryInfoKey(hKey, class, pcchClass, reserved, pcSubKeys, pcbMaxSubKeyLen,
    //                 pcbMaxClassLen, pcValues, pcbMaxValueNameLen, pcbMaxValueLen, ...)
    add_hook("RegQueryInfoKeyW", 0, [state, path_of](Emulator& e) {
        std::string key = path_of(e.arg_slot(0));
        auto k = state->keys.find(key);
        uint32_t values = 0, max_name = 0, max_value = 0;
        if (k != state->keys.end()) {
            values = static_cast<uint32_t>(k->second.size());
            for (const auto& v : k->second) {
                if (v.first.size() > max_name) max_name = static_cast<uint32_t>(v.first.size());
                if (v.second.data.size() > max_value)
                    max_value = static_cast<uint32_t>(v.second.data.size());
            }
        }
        uint32_t subkeys = 0, max_subkey = 0;
        std::string below = key + "\\";
        for (auto it = state->keys.lower_bound(below);
             it != state->keys.end() && it->first.compare(0, below.size(), below) == 0; ++it) {
            std::string rest = it->first.substr(below.size());
            size_t cut = rest.find('\\');
            if (cut == std::string::npos) {
                ++subkeys;
                if (rest.size() > max_subkey) max_subkey = static_cast<uint32_t>(rest.size());
            }
        }
        if (e.arg_slot(2)) e.mem.write32(e.arg_slot(2), 0);
        if (e.arg_slot(4)) e.mem.write32(e.arg_slot(4), subkeys);
        if (e.arg_slot(5)) e.mem.write32(e.arg_slot(5), max_subkey);
        if (e.arg_slot(7)) e.mem.write32(e.arg_slot(7), values);
        if (e.arg_slot(8)) e.mem.write32(e.arg_slot(8), max_name);
        if (e.arg_slot(9)) e.mem.write32(e.arg_slot(9), max_value);
        e.set_result(kOk);
    });

    add_hook("RegCloseKey", 0, [state](Emulator& e) {
        state->open.erase(static_cast<uint32_t>(e.arg_slot(0)));
        e.set_result(kOk);
    });

    // Writing is refused for the same reason the Windows bridge refuses it: it
    // outlives the run and nothing here undoes it.  --registry-write is about the
    // host's registry, and there is none - a write would have to go back into the
    // .reg file, which is a different feature.
    for (const char* name : {"RegCreateKeyW", "RegCreateKeyExW", "RegCreateKeyExA",
                             "RegSetValueExW", "RegSetValueExA", "RegDeleteKeyW",
                             "RegDeleteKeyExW", "RegDeleteValueW", "RegSaveKeyW",
                             "RegLoadKeyW"}) {
        add_hook(name, 0, [](Emulator& e) { e.set_result(kAccessDenied); });
    }
#endif  // _WIN32
}

}  // namespace x86emu
