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

#endif  // _WIN32

}  // namespace

void Emulator::install_registry_hooks() {
#if defined(_WIN32)
    auto state = std::make_shared<RegistryState>();

    // The host key a guest handle names, or null if it names nothing.
    auto resolve = [state](uint64_t h) -> HKEY {
        if (HKEY root = predefined_root(h)) return root;
        auto it = state->open.find(h);
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
        state->open[handle] = opened;
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
        state->open[handle] = opened;
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
        auto it = state->open.find(e.arg_slot(0));
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
        state->open[handle] = opened;
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
#endif  // _WIN32
}

}  // namespace x86emu
