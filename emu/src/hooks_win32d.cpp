// The Windows surface a large third-party DLL reaches for that a compiler
// toolchain never does: one-time initialisation, processor topology, shell
// paths, path string helpers, and the COM entry points.
//
// These came out of bringing up a commercial inference DLL, which is a
// different shape of guest from the toolchains: it is built against a modern
// SDK, queries the machine it is running on, and touches COM on the way to its
// own licensing code.
// winsock2.h has to precede anything that reaches windows.h, which emulator.h
// does on a Windows host - so the network headers come first.
#if defined(_WIN32)
// GetIfTable2 lives behind a Vista-or-later target: iphlpapi.h only pulls in
// netioapi.h when NTDDI_VERSION says so.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif
#include <winsock2.h>
// netioapi.h only declares the MIB_IF_TABLE2 family when ws2ipdef.h has been
// seen first, which winsock2.h alone does not pull in.
#include <ws2tcpip.h>
#include <iphlpapi.h>
// Some older MinGW header sets ship a netioapi.h without MIB_IF_TABLE2 at all.
// There is no way to test for a typedef, so this is by toolchain, and the
// fallback is the empty table further down - which a guest reads as a machine
// with no interfaces configured.
#if defined(_MSC_VER) || defined(X86EMU_HAVE_IFTABLE2)
#define X86EMU_IFTABLE2 1
#include <netioapi.h>
#endif
// Device-interface (HID) enumeration, bridged to the host the same way the
// interface table is: a real machine has HID devices, and the licensing code
// folds the set it finds into its machine fingerprint.
#if defined(_MSC_VER) || defined(X86EMU_HAVE_SETUPAPI)
#define X86EMU_SETUPAPI 1
#include <setupapi.h>
#include <hidsdi.h>
#endif
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "emulator.h"
#include "guest_printf.h"

namespace x86emu {
namespace {

uint64_t unix_to_filetime(int64_t t) {
    return (static_cast<uint64_t>(t) + 11644473600ull) * 10000000ull;
}

void write_system_time(Emulator& e, uint64_t p, const std::tm& tm, int ms) {
    if (!p) return;
    e.mem.write16(p + 0, static_cast<uint16_t>(tm.tm_year + 1900));
    e.mem.write16(p + 2, static_cast<uint16_t>(tm.tm_mon + 1));
    e.mem.write16(p + 4, static_cast<uint16_t>(tm.tm_wday));
    e.mem.write16(p + 6, static_cast<uint16_t>(tm.tm_mday));
    e.mem.write16(p + 8, static_cast<uint16_t>(tm.tm_hour));
    e.mem.write16(p + 10, static_cast<uint16_t>(tm.tm_min));
    e.mem.write16(p + 12, static_cast<uint16_t>(tm.tm_sec));
    e.mem.write16(p + 14, static_cast<uint16_t>(ms));
}

void write_utf16(Emulator& e, uint64_t dst, const std::string& s) {
    std::u16string w = utf8_to_utf16(s);
    for (size_t i = 0; i <= w.size(); ++i)
        e.mem.write16(dst + i * 2, i < w.size() ? w[i] : 0);
}

// Write a wide string into a caller's buffer, answering the way the
// GetXxxDirectory family does: the length without the terminator on success,
// the length *with* it when the buffer is too small.
void write_wide_dir(Emulator& e, uint64_t buf, uint64_t chars, const std::string& s) {
    std::u16string w = utf8_to_utf16(s);
    if (!buf || w.size() + 1 > chars) {
        e.set_result(w.size() + 1);
        return;
    }
    for (size_t i = 0; i <= w.size(); ++i)
        e.mem.write16(buf + i * 2, i < w.size() ? w[i] : 0);
    e.set_result(w.size());
}

void write_narrow_dir(Emulator& e, uint64_t buf, uint64_t size, const std::string& s) {
    if (!buf || s.size() + 1 > size) {
        e.set_result(s.size() + 1);
        return;
    }
    e.mem.write_cstring(buf, s);
    e.set_result(s.size());
}

}  // namespace

void Emulator::install_win32_dll_hooks() {
    auto win32 = [this](const char* name, int nargs, std::function<void(Emulator&)> fn) {
        add_hook(name, is64() ? 0 : nargs * 4, std::move(fn));
    };
    auto ret0 = [&](const char* name, int nargs) {
        win32(name, nargs, [](Emulator& e) { e.set_result(0); });
    };
    auto ret1 = [&](const char* name, int nargs) {
        win32(name, nargs, [](Emulator& e) { e.set_result(1); });
    };

    // ---- one-time initialisation -------------------------------------------
    // An INIT_ONCE is one pointer-sized word the caller owns.  Real Windows
    // packs a state into its low bits; nothing may read it except through these
    // calls, so the emulator is free to use 0 = untouched, 1 = in progress,
    // 2 = done.  A hook runs to completion before any other thread is scheduled,
    // so the check and the store here are atomic without saying so.
    win32("InitOnceBeginInitialize", 4, [](Emulator& e) {
        uint64_t once = e.arg_slot(0);
        uint64_t pending = e.arg_slot(2);
        uint64_t ctx = e.arg_slot(3);
        uint64_t state = once ? e.mem.read_sized(once, e.pointer_size()) : 0;
        bool first = state != 2;
        if (first && once) e.mem.write_sized(once, e.pointer_size(), 1);
        if (pending) e.mem.write32(pending, first ? 1 : 0);
        if (ctx) e.mem.write_sized(ctx, e.pointer_size(), 0);
        e.set_result(1);
    });
    win32("InitOnceComplete", 3, [](Emulator& e) {
        uint64_t once = e.arg_slot(0);
        if (once) e.mem.write_sized(once, e.pointer_size(), 2);
        e.set_result(1);
    });

    // ---- processor topology -------------------------------------------------
    // A guest asks these to size its thread pool.  Answering "one group, one
    // node, processor zero" is a truthful description of a cooperative
    // scheduler that interprets one instruction stream.
    win32("GetLogicalProcessorInformationEx", 3, [](Emulator& e) {
        // Refusing (ERROR_NOT_SUPPORTED) is documented, but onnxruntime's
        // InitializeCpuInfo does not cope: it logs "error code 50" and then leaves
        // a half-built structure whose failed field (HRESULT 0x80070032) is later
        // dereferenced as a pointer, crashing recognition.  Bridge to the host so
        // the CPU topology is real.  The SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX
        // blocks are self-contained POD (sizes + GROUP_AFFINITY masks, no
        // pointers), so a raw byte copy into guest memory is valid.
#if defined(_WIN32)
        LOGICAL_PROCESSOR_RELATIONSHIP rel =
            static_cast<LOGICAL_PROCESSOR_RELATIONSHIP>(static_cast<DWORD>(e.arg_slot(0)));
        uint64_t gbuf = e.arg_slot(1);
        uint64_t glen = e.arg_slot(2);
        if (glen == 0) { e.set_last_error(87); e.set_result(0); return; }  // ERROR_INVALID_PARAMETER
        uint32_t guest_cap = e.mem.read32(glen);
        DWORD needed = 0;
        GetLogicalProcessorInformationEx(rel, nullptr, &needed);  // learn the size
        std::vector<unsigned char> host(needed ? needed : 1);
        DWORD have = needed;
        if (needed == 0 ||
            !GetLogicalProcessorInformationEx(
                rel, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(host.data()), &have)) {
            e.set_last_error(GetLastError());
            e.set_result(0);
            return;
        }
        e.mem.write32(glen, have);  // always report the required size
        if (gbuf == 0 || guest_cap < have) {
            e.set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
            e.set_result(0);
            return;
        }
        for (DWORD i = 0; i < have; ++i) e.mem.write8(gbuf + i, host[i]);
        e.set_result(1);
#else
        e.set_last_error(50);  // ERROR_NOT_SUPPORTED
        e.set_result(0);
#endif
    });
    win32("GetNumaHighestNodeNumber", 1, [](Emulator& e) {
        if (e.arg_slot(0)) e.mem.write32(e.arg_slot(0), 0);
        e.set_result(1);
    });
    win32("GetCurrentProcessorNumberEx", 1, [](Emulator& e) {
        uint64_t p = e.arg_slot(0);  // PROCESSOR_NUMBER: group, number, reserved
        if (p) {
            e.mem.write16(p, 0);
            e.mem.write8(p + 2, 0);
            e.mem.write8(p + 3, 0);
        }
        e.set_result(0);
    });
    win32("GetThreadGroupAffinity", 2, [](Emulator& e) {
        uint64_t p = e.arg_slot(1);  // GROUP_AFFINITY: mask, group, reserved[3]
        if (p) {
            e.mem.write64(p, 1);
            e.mem.write16(p + 8, 0);
            e.mem.write16(p + 10, 0);
            e.mem.write16(p + 12, 0);
            e.mem.write16(p + 14, 0);
        }
        e.set_result(1);
    });
    ret1("SetThreadGroupAffinity", 3);
    win32("ProcessIdToSessionId", 2, [](Emulator& e) {
        if (e.arg_slot(1)) e.mem.write32(e.arg_slot(1), 1);
        e.set_result(1);
    });
    win32("HeapQueryInformation", 5, [](Emulator& e) {
        // HeapCompatibilityInformation: 0 = the standard heap, which is what the
        // emulator's allocator behaves like.
        if (e.arg_slot(2) && e.arg_slot(3) >= 4) e.mem.write32(e.arg_slot(2), 0);
        if (e.arg_slot(4)) e.mem.write_sized(e.arg_slot(4), e.pointer_size(), 4);
        e.set_result(1);
    });
    // Depth lives in the low 16 bits of the header's first word (both x86 and the
    // x64 HeaderX64 layout put Depth there).
    win32("QueryDepthSList", 1, [](Emulator& e) {
        uint64_t head = e.arg_slot(0);
        e.set_result(head ? (e.mem.read_sized(head, 8) & 0xFFFF) : 0);
    });

    // ---- system directories -------------------------------------------------
    // Bridged to the host so the CASE matches the real machine: Windows returns
    // "C:\WINDOWS" (upper) here, and a guest that folds the path into a license
    // check compares it case-sensitively.  A hard-coded "C:\Windows" diverged.
    auto host_dir = [](bool system) -> std::string {
#if defined(_WIN32)
        wchar_t buf[MAX_PATH];
        UINT n = system ? GetSystemDirectoryW(buf, MAX_PATH) : GetWindowsDirectoryW(buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::string s;  // a system directory path is ASCII
            for (UINT i = 0; i < n; ++i) s.push_back(static_cast<char>(buf[i] & 0xff));
            return s;
        }
#endif
        return system ? "C:\\Windows\\System32" : "C:\\Windows";
    };
    win32("GetSystemDirectoryA", 2, [host_dir](Emulator& e) {
        write_narrow_dir(e, e.arg_slot(0), e.arg_slot(1), host_dir(true));
    });
    win32("GetSystemDirectoryW", 2, [host_dir](Emulator& e) {
        write_wide_dir(e, e.arg_slot(0), e.arg_slot(1), host_dir(true));
    });
    win32("GetWindowsDirectoryW", 2, [host_dir](Emulator& e) {
        write_wide_dir(e, e.arg_slot(0), e.arg_slot(1), host_dir(false));
    });
    win32("GetWindowsDirectoryA", 2, [host_dir](Emulator& e) {
        write_narrow_dir(e, e.arg_slot(0), e.arg_slot(1), host_dir(false));
    });
    // SHGetFolderPathW(hwnd, csidl, token, flags, path) - the folders a guest
    // stores its own data in.  CSIDL_APPDATA is 0x1A, LOCAL_APPDATA 0x1C,
    // COMMON_APPDATA 0x23; anything else gets the profile directory.
    win32("SHGetFolderPathW", 5, [](Emulator& e) {
        uint32_t csidl = static_cast<uint32_t>(e.arg_slot(1)) & 0xFFu;
        std::string home = "C:\\Users\\Default";
        if (const std::string* v = e.getenv("USERPROFILE")) home = *v;
        std::string path;
        if (csidl == 0x23) {
            path = "C:\\ProgramData";
            if (const std::string* v = e.getenv("ProgramData")) path = *v;
        } else if (csidl == 0x1C) {
            path = home + "\\AppData\\Local";
            if (const std::string* v = e.getenv("LOCALAPPDATA")) path = *v;
        } else if (csidl == 0x1A) {
            path = home + "\\AppData\\Roaming";
            if (const std::string* v = e.getenv("APPDATA")) path = *v;
        } else {
            path = home;
        }
        if (e.arg_slot(4)) write_utf16(e, e.arg_slot(4), path);
        e.set_result(0);  // S_OK
    });

    // ---- time ---------------------------------------------------------------
    win32("FileTimeToSystemTime", 2, [](Emulator& e) {
        uint64_t ft = e.arg_slot(0) ? e.mem.read64(e.arg_slot(0)) : 0;
        int ms = static_cast<int>((ft / 10000ull) % 1000ull);
        std::time_t t = static_cast<std::time_t>(ft / 10000000ull - 11644473600ull);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        write_system_time(e, e.arg_slot(1), tm, ms);
        e.set_result(1);
    });
    // The emulator keeps everything in UTC, so "local time" is the same time.
    win32("SystemTimeToTzSpecificLocalTime", 3, [](Emulator& e) {
        uint64_t in = e.arg_slot(1), out = e.arg_slot(2);
        if (in && out)
            for (int i = 0; i < 16; i += 2) e.mem.write16(out + i, e.mem.read16(in + i));
        e.set_result(1);
    });
    // (locale, flags, systemtime, format, buffer, size[, calendar]) - a fixed
    // ISO-ish rendering, which is what a log line wants and no caller parses.
    auto format_datetime = [](Emulator& e, bool date) {
        uint64_t st = e.arg_slot(2);
        char text[64] = {0};
        if (st) {
            if (date)
                std::snprintf(text, sizeof text, "%04u-%02u-%02u",
                              e.mem.read16(st + 0), e.mem.read16(st + 2),
                              e.mem.read16(st + 6));
            else
                std::snprintf(text, sizeof text, "%02u:%02u:%02u",
                              e.mem.read16(st + 8), e.mem.read16(st + 10),
                              e.mem.read16(st + 12));
        }
        std::string s = text;
        uint64_t buf = e.arg_slot(4), chars = e.arg_slot(5);
        if (!buf || chars == 0) {
            e.set_result(s.size() + 1);
            return;
        }
        if (s.size() + 1 > chars) {
            e.set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
            e.set_result(0);
            return;
        }
        write_utf16(e, buf, s);
        e.set_result(s.size() + 1);
    };
    win32("GetDateFormatW", 6, [format_datetime](Emulator& e) { format_datetime(e, true); });
    win32("GetTimeFormatW", 6, [format_datetime](Emulator& e) { format_datetime(e, false); });

    win32("GetUserDefaultLocaleName", 2, [](Emulator& e) {
        // The emulator's CRT hooks format in the C locale, so saying so is the
        // truthful answer rather than inventing a region.
        const char* name = "en-US";
        std::u16string w = utf8_to_utf16(name);
        if (!e.arg_slot(0) || e.arg_slot(1) < w.size() + 1) {
            e.set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
            e.set_result(0);
            return;
        }
        write_utf16(e, e.arg_slot(0), name);
        e.set_result(w.size() + 1);
    });
    win32("GetLocalTime", 1, [](Emulator& e) {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        write_system_time(e, e.arg_slot(0), tm, 0);
        e.set_result(0);
    });
    // Everything here is UTC, so the local form is the same eight words.
    win32("FileTimeToLocalFileTime", 2, [](Emulator& e) {
        if (e.arg_slot(0) && e.arg_slot(1))
            e.mem.write64(e.arg_slot(1), e.mem.read64(e.arg_slot(0)));
        e.set_result(1);
    });
    // No .ini file exists, so every key falls back to the caller's default -
    // which is the contract, and what a program on a fresh install sees.
    // (lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize, lpFileName).
    // nSize is a DWORD, and a caller writes it as one - the top half of that
    // stack slot keeps whatever was there before, so it has to be masked off.
    win32("GetPrivateProfileStringW", 6, [](Emulator& e) {
        uint64_t def = e.arg_slot(2), buf = e.arg_slot(3);
        uint64_t size = static_cast<uint32_t>(e.arg_slot(4));
        std::string value = def ? utf16_to_utf8(e, def, -1) : "";
        std::u16string w = utf8_to_utf16(value);
        if (!buf || size == 0) {
            e.set_result(0);
            return;
        }
        size_t n = w.size() + 1 > size ? static_cast<size_t>(size - 1) : w.size();
        for (size_t i = 0; i < n; ++i) e.mem.write16(buf + i * 2, w[i]);
        e.mem.write16(buf + n * 2, 0);
        e.set_result(n);
    });
    // ShellExecute answers a value above 32 for success.  Nothing is launched -
    // there is no shell - and a caller that only checks the threshold carries on.
    win32("ShellExecuteW", 6, [](Emulator& e) {
        e.log_call("ShellExecute(%s)",
                   e.arg_slot(2) ? utf16_to_utf8(e, e.arg_slot(2), -1).c_str() : "");
        e.set_result(42);
    });
    ret0("MiniDumpWriteDump", 7);

    // ---- files --------------------------------------------------------------
    win32("GetFileAttributesExA", 3, [](Emulator& e) {
        FileTable::Stat st;
        if (FileTable::stat_path(e.mem.read_cstring(e.arg_slot(0)), st) != 0) {
            e.set_last_error(2);
            e.set_result(0);
            return;
        }
        uint64_t out = e.arg_slot(2);
        if (out) {
            uint64_t ticks = unix_to_filetime(st.mtime);
            e.mem.write32(out, st.is_dir ? 0x10u : 0x80u);
            e.mem.write64(out + 4, ticks);
            e.mem.write64(out + 12, ticks);
            e.mem.write64(out + 20, ticks);
            e.mem.write32(out + 28, static_cast<uint32_t>(st.size >> 32));
            e.mem.write32(out + 32, static_cast<uint32_t>(st.size));
        }
        e.set_result(1);
    });
    // Advisory locks on a file only one process has open: taking one always
    // succeeds, and releasing one that was never contended is a no-op.
    ret1("LockFileEx", 6);
    ret1("UnlockFileEx", 5);
    ret1("CancelIo", 1);
    win32("OpenFileMappingA", 3, [](Emulator& e) {
        e.set_last_error(2);  // ERROR_FILE_NOT_FOUND: no named section exists
        e.set_result(0);
    });
    ret0("ReadConsoleA", 5);

    // ---- path string helpers (shlwapi) --------------------------------------
    // These operate on the caller's buffer and answer pointers into it, so they
    // are pure string arithmetic on guest addresses.
    win32("PathFindNextComponentW", 1, [](Emulator& e) {
        uint64_t p = e.arg_slot(0);
        if (!p) {
            e.set_result(0);
            return;
        }
        uint64_t at = p;
        while (uint16_t c = e.mem.read16(at)) {
            if (c == u'\\' || c == u'/') {
                e.set_result(at + 2);
                return;
            }
            at += 2;
        }
        e.set_result(at);  // the terminator, which is what "no next" means here
    });
    win32("PathRemoveBackslashW", 1, [](Emulator& e) {
        uint64_t p = e.arg_slot(0);
        if (!p) {
            e.set_result(0);
            return;
        }
        uint64_t at = p, last = 0;
        while (e.mem.read16(at)) {
            last = at;
            at += 2;
        }
        if (last && e.mem.read16(last) == u'\\') {
            e.mem.write16(last, 0);
            e.set_result(last);
            return;
        }
        e.set_result(at);
    });
    // "C:\" and "\\server\share\" are roots; everything after one is the path.
    win32("PathSkipRootW", 1, [](Emulator& e) {
        uint64_t p = e.arg_slot(0);
        if (!p) {
            e.set_result(0);
            return;
        }
        uint16_t c0 = e.mem.read16(p), c1 = e.mem.read16(p + 2);
        if (c1 == u':' && e.mem.read16(p + 4) == u'\\') {
            e.set_result(p + 6);
            return;
        }
        if (c0 == u'\\' && c1 == u'\\') {
            uint64_t at = p + 4;
            for (int seen = 0; seen < 2;) {
                uint16_t c = e.mem.read16(at);
                if (!c) break;
                at += 2;
                if (c == u'\\') ++seen;
            }
            e.set_result(at);
            return;
        }
        e.set_result(0);
    });

    // ---- user32 -------------------------------------------------------------
    win32("wsprintfA", 2, [](Emulator& e) {
        Args a(e, 2);
        std::string s = format_guest(e, e.arg_slot(1), a, false);
        e.mem.write_cstring(e.arg_slot(0), s);
        e.set_result(s.size());
    });
    // A guest with no window station is a service, which is a state a library
    // has to handle - and the emulator genuinely has no desktop.
    ret0("GetProcessWindowStation", 0);
    ret0("GetUserObjectInformationW", 5);
    // RECT is four 32-bit longs.
    win32("CopyRect", 2, [](Emulator& e) {
        uint64_t dst = e.arg_slot(0), src = e.arg_slot(1);
        if (dst && src)
            for (int i = 0; i < 16; i += 4) e.mem.write32(dst + i, e.mem.read32(src + i));
        e.set_result(1);
    });
    win32("InflateRect", 3, [](Emulator& e) {
        uint64_t r = e.arg_slot(0);
        int32_t dx = static_cast<int32_t>(e.arg_slot(1));
        int32_t dy = static_cast<int32_t>(e.arg_slot(2));
        if (r) {
            e.mem.write32(r + 0, static_cast<uint32_t>(static_cast<int32_t>(e.mem.read32(r + 0)) - dx));
            e.mem.write32(r + 4, static_cast<uint32_t>(static_cast<int32_t>(e.mem.read32(r + 4)) - dy));
            e.mem.write32(r + 8, static_cast<uint32_t>(static_cast<int32_t>(e.mem.read32(r + 8)) + dx));
            e.mem.write32(r + 12, static_cast<uint32_t>(static_cast<int32_t>(e.mem.read32(r + 12)) + dy));
        }
        e.set_result(1);
    });
    // A message box with no one to click it answers as if its *default* button
    // were pressed - what hitting Enter does - rather than always IDOK, which is
    // not even among the answers a Yes/No box can give.  The text goes to the
    // log either way: a guest that puts one up is usually saying something the
    // run needs to know.
    win32("MessageBoxW", 4, [](Emulator& e) {
        std::string text = e.arg_slot(1) ? utf16_to_utf8(e, e.arg_slot(1), -1) : "";
        std::string caption = e.arg_slot(2) ? utf16_to_utf8(e, e.arg_slot(2), -1) : "";
        uint32_t type = static_cast<uint32_t>(e.arg_slot(3));
        uint64_t answer = 1;  // IDOK
        switch (type & 0xF) {
            case 1: answer = 1; break;   // MB_OKCANCEL         -> IDOK
            case 2: answer = 3; break;   // MB_ABORTRETRYIGNORE -> IDABORT
            case 3: answer = 6; break;   // MB_YESNOCANCEL      -> IDYES
            case 4: answer = 6; break;   // MB_YESNO            -> IDYES
            case 5: answer = 4; break;   // MB_RETRYCANCEL      -> IDRETRY
            default: answer = 1; break;  // MB_OK and the rest
        }
        // MB_DEFBUTTON2 moves the default to the second button, which on a
        // Yes/No box is No.
        if ((type & 0xF00) == 0x100 && (type & 0xF) == 4) answer = 7;  // IDNO
        e.log_call("MessageBox(%s: %s) -> %llu", caption.c_str(), text.c_str(),
                   static_cast<unsigned long long>(answer));
        e.set_result(answer);
    });

    // ---- security descriptors -----------------------------------------------
    // There is one user here and no access check to fail, so a descriptor is a
    // structure the guest fills in and hands to a call that ignores it.  Zeroing
    // the revision word is enough for the guest's own reads to be consistent.
    win32("InitializeSecurityDescriptor", 2, [](Emulator& e) {
        uint64_t p = e.arg_slot(0);
        if (p) {
            e.mem.write8(p, static_cast<uint8_t>(e.arg_slot(1)));  // Revision
            e.mem.write8(p + 1, 0);                                // Sbz1
            e.mem.write16(p + 2, 0);                               // Control
            for (int i = 0; i < 4; ++i)
                e.mem.write_sized(p + 4 + i * e.pointer_size(), e.pointer_size(), 0);
        }
        e.set_result(1);
    });
    ret1("SetSecurityDescriptorDacl", 4);

    // ---- the event log ------------------------------------------------------
    // A service logs here when it has no console.  Routing the strings to the
    // call log keeps them visible without inventing an event log.
    win32("RegisterEventSourceW", 2, [](Emulator& e) { e.set_result(1); });
    ret1("DeregisterEventSource", 1);
    // (source, type, category, id, sid, count, size, strings, raw)
    win32("ReportEventW", 9, [](Emulator& e) {
        uint64_t count = e.arg_slot(5), strings = e.arg_slot(7);
        for (uint64_t i = 0; i < count && strings; ++i) {
            uint64_t p = e.mem.read_sized(strings + i * e.pointer_size(), e.pointer_size());
            if (p) e.log_call("ReportEvent: %s", utf16_to_utf8(e, p, -1).c_str());
        }
        e.set_result(1);
    });

    // ---- CryptoAPI: keys and signatures -------------------------------------
    // Hashing and random bytes are implemented (hooks_win32c.cpp); a *key* is
    // not, because the private half would have to come from a real key store
    // this emulator has no equivalent of.  Failing with NTE_BAD_KEYSET is what a
    // machine with no key container answers, and a caller that treats a key
    // store as optional takes its other path.
    auto no_keyset = [&](const char* name, int nargs) {
        win32(name, nargs, [](Emulator& e) {
            e.set_last_error(0x80090016);  // NTE_BAD_KEYSET
            e.set_result(0);
        });
    };
    no_keyset("CryptGetUserKey", 3);
    no_keyset("CryptExportKey", 6);
    no_keyset("CryptSignHashW", 6);
    no_keyset("CryptDecrypt", 6);
    no_keyset("CryptSetHashParam", 4);
    no_keyset("CryptGetProvParam", 5);
    ret1("CryptDestroyKey", 1);
    win32("CryptEnumProvidersW", 6, [](Emulator& e) {
        e.set_last_error(259);  // ERROR_NO_MORE_ITEMS: the enumeration is empty
        e.set_result(0);
    });

    // ---- the certificate stores ---------------------------------------------
    // Opening a store succeeds and finds nothing, which is a real state: a
    // machine whose "MY" store holds no certificates.
    win32("CertOpenStore", 5, [](Emulator& e) { e.set_result(1); });
    win32("CertOpenSystemStoreW", 2, [](Emulator& e) { e.set_result(1); });
    ret1("CertCloseStore", 2);
    ret0("CertEnumCertificatesInStore", 2);
    ret0("CertFindCertificateInStore", 6);
    ret0("CertDuplicateCertificateContext", 1);
    ret1("CertFreeCertificateContext", 1);
    ret0("CertGetCertificateContextProperty", 4);

    // ---- device enumeration and HID ----------------------------------------
    // A licensing library looks for a USB dongle by walking the HID device
    // interfaces.  There are no devices here, and an enumeration that finds none
    // is an ordinary outcome on a machine with nothing plugged in - so these
    // report emptiness rather than failure, and the guest goes on to whatever it
    // does without one.
    win32("HidD_GetHidGuid", 1, [](Emulator& e) {
        // GUID_DEVINTERFACE_HID {4D1E55B2-F16F-11CF-88CB-001111000030}
        uint64_t p = e.arg_slot(0);
        if (p) {
            e.mem.write32(p, 0x4D1E55B2u);
            e.mem.write16(p + 4, 0xF16F);
            e.mem.write16(p + 6, 0x11CF);
            static const uint8_t tail[8] = {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30};
            for (int i = 0; i < 8; ++i) e.mem.write8(p + 8 + i, tail[i]);
        }
        e.set_result(0);
    });
#if defined(X86EMU_SETUPAPI)
    // A HID handle opened through CreateFile lives in the host-handle table; its
    // attributes and strings are the host device's own, which is what a licensing
    // scan reads while deciding whether a device is its dongle.
    win32("HidD_GetAttributes", 2, [](Emulator& e) {
        void* host = lookup_host_handle(e.arg_slot(0));
        uint64_t out = e.arg_slot(1);
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        BOOL ok = host && out &&
                  HidD_GetAttributes(reinterpret_cast<HANDLE>(host), &attr);
        if (ok) {
            e.mem.write32(out + 0, attr.Size);
            e.mem.write16(out + 4, attr.VendorID);
            e.mem.write16(out + 6, attr.ProductID);
            e.mem.write16(out + 8, attr.VersionNumber);
        }
        e.set_result(ok ? 1 : 0);
    });
    // The string queries take (handle, buffer, byte-length) and fill a wide
    // string; forward each to the host device.
    auto hid_string = [](Emulator& e, int which) {
        void* host = lookup_host_handle(e.arg_slot(0));
        uint64_t buf = e.arg_slot(1);
        uint64_t len = e.arg_slot(2);
        if (!host || !buf || !len) { e.set_result(0); return; }
        std::vector<wchar_t> tmp(len / 2 + 1, 0);
        HANDLE h = reinterpret_cast<HANDLE>(host);
        BOOL ok = FALSE;
        switch (which) {
            case 0: ok = HidD_GetProductString(h, tmp.data(), static_cast<ULONG>(len)); break;
            case 1: ok = HidD_GetManufacturerString(h, tmp.data(), static_cast<ULONG>(len)); break;
            case 2: ok = HidD_GetSerialNumberString(h, tmp.data(), static_cast<ULONG>(len)); break;
        }
        if (ok)
            for (uint64_t i = 0; i + 1 < len; i += 2)
                e.mem.write16(buf + i, static_cast<uint16_t>(tmp[i / 2]));
        e.set_result(ok ? 1 : 0);
    };
    win32("HidD_GetProductString", 3, [hid_string](Emulator& e) { hid_string(e, 0); });
    win32("HidD_GetManufacturerString", 3, [hid_string](Emulator& e) { hid_string(e, 1); });
    win32("HidD_GetSerialNumberString", 3, [hid_string](Emulator& e) { hid_string(e, 2); });
#else
    ret0("HidD_GetAttributes", 2);
    ret0("HidD_GetProductString", 3);
#endif
    ret0("HidD_GetFeature", 3);
    ret0("HidD_SetFeature", 3);
    ret0("HidD_GetPreparsedData", 2);
    ret1("HidD_FreePreparsedData", 1);
    ret0("HidD_FlushQueue", 1);
    win32("HidP_GetCaps", 2, [](Emulator& e) {
        e.set_result(0xC0110001ull);  // HIDP_STATUS_INVALID_PREPARSED_DATA
    });
    // SetupDiGetClassDevs answers a set of present devices.  Bridged to the host
    // so the guest sees the machine's real HID interfaces: a licensing library
    // walks them looking for a dongle, and folds what it finds into its machine
    // fingerprint.  Reporting an empty set made that fingerprint diverge from the
    // host's on a real machine (a VM with no HID devices hid the difference).
    // The host HDEVINFO pointer is the token the guest passes back; it is opaque
    // to the guest and only ever handed to the other SetupDi calls here.
#if defined(X86EMU_SETUPAPI)
    auto read_guid = [](Emulator& e, uint64_t p, GUID& g) {
        for (int i = 0; i < 16; ++i)
            reinterpret_cast<uint8_t*>(&g)[i] = static_cast<uint8_t>(e.mem.read_sized(p + i, 1));
    };
    auto get_class_devs = [read_guid](Emulator& e, bool wide) {
        GUID g{};
        uint64_t gp = e.arg_slot(0);
        if (gp) read_guid(e, gp, g);
        DWORD flags = static_cast<DWORD>(e.arg_slot(3));
        HDEVINFO h = SetupDiGetClassDevsW(gp ? &g : nullptr, nullptr, nullptr, flags);
        (void)wide;
        e.set_result(h == INVALID_HANDLE_VALUE ? ~0ull
                                               : static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h)));
    };
    win32("SetupDiGetClassDevsA", 4, [get_class_devs](Emulator& e) { get_class_devs(e, false); });
    win32("SetupDiGetClassDevsW", 4, [get_class_devs](Emulator& e) { get_class_devs(e, true); });
    win32("SetupDiEnumDeviceInterfaces", 5, [read_guid](Emulator& e) {
        HDEVINFO h = reinterpret_cast<HDEVINFO>(static_cast<uintptr_t>(e.arg_slot(0)));
        GUID g{};
        uint64_t gp = e.arg_slot(2);
        if (gp) read_guid(e, gp, g);
        DWORD idx = static_cast<DWORD>(e.arg_slot(3));
        uint64_t out = e.arg_slot(4);
        SP_DEVICE_INTERFACE_DATA did{};
        did.cbSize = sizeof(did);
        BOOL ok = SetupDiEnumDeviceInterfaces(h, nullptr, gp ? &g : nullptr, idx, &did);
        if (ok && out) {
            // MSVC lays SP_DEVICE_INTERFACE_DATA out the same in guest and host:
            // cbSize@0, InterfaceClassGuid@4, Flags@20, Reserved@24.  Reserved is
            // the host's own handle to this interface and must round-trip to the
            // detail call, so it is echoed back verbatim.
            e.mem.write32(out + 0, did.cbSize);
            for (int i = 0; i < 16; ++i)
                e.mem.write8(out + 4 + i, reinterpret_cast<uint8_t*>(&did.InterfaceClassGuid)[i]);
            e.mem.write32(out + 20, did.Flags);
            e.mem.write64(out + 24, static_cast<uint64_t>(did.Reserved));
        }
        if (!ok) e.set_last_error(GetLastError());
        e.set_result(ok ? 1 : 0);
    });
    // The detail is a { DWORD cbSize; char/WCHAR DevicePath[]; } whose path sits
    // at offset 4 in both the A and W forms and in both guest and host, so the
    // host's own answer copies across whole.  Calling the same A/W variant the
    // guest did yields exactly the bytes the native process would get.
    auto get_detail = [read_guid](Emulator& e, bool wide) {
        HDEVINFO h = reinterpret_cast<HDEVINFO>(static_cast<uintptr_t>(e.arg_slot(0)));
        uint64_t didp = e.arg_slot(1);
        SP_DEVICE_INTERFACE_DATA did{};
        did.cbSize = sizeof(did);
        if (didp) {
            did.cbSize = static_cast<DWORD>(e.mem.read_sized(didp + 0, 4));
            read_guid(e, didp + 4, did.InterfaceClassGuid);
            did.Flags = static_cast<DWORD>(e.mem.read_sized(didp + 20, 4));
            did.Reserved = static_cast<ULONG_PTR>(e.mem.read_sized(didp + 24, 8));
        }
        uint64_t detail = e.arg_slot(2), size = e.arg_slot(3), reqp = e.arg_slot(4);
        DWORD need = 0;
        std::vector<uint8_t> buf;
        BOOL ok;
        if (wide) {
            SetupDiGetDeviceInterfaceDetailW(h, &did, nullptr, 0, &need, nullptr);
            buf.resize(need ? need : sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 8);
            auto* d = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
            d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            DWORD got = 0;
            ok = SetupDiGetDeviceInterfaceDetailW(h, &did, d, static_cast<DWORD>(buf.size()), &got, nullptr);
            if (got) need = got;
        } else {
            SetupDiGetDeviceInterfaceDetailA(h, &did, nullptr, 0, &need, nullptr);
            buf.resize(need ? need : sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 8);
            auto* d = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buf.data());
            d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            DWORD got = 0;
            ok = SetupDiGetDeviceInterfaceDetailA(h, &did, d, static_cast<DWORD>(buf.size()), &got, nullptr);
            if (got) need = got;
        }
        if (!detail || size < need) {
            if (reqp) e.mem.write32(reqp, need);
            e.set_last_error(ok ? 122 : GetLastError());  // ERROR_INSUFFICIENT_BUFFER
            e.set_result(0);
            return;
        }
        // Rewrite cbSize to the guest's own convention (offset of DevicePath),
        // then copy the path bytes that follow it straight across.
        e.mem.write32(detail + 0, wide ? 8u : 6u);
        for (DWORD i = 4; i < need; ++i) e.mem.write8(detail + i, buf[i]);
        if (reqp) e.mem.write32(reqp, need);
        e.set_result(ok ? 1 : 0);
    };
    win32("SetupDiGetDeviceInterfaceDetailA", 6, [get_detail](Emulator& e) { get_detail(e, false); });
    win32("SetupDiGetDeviceInterfaceDetailW", 6, [get_detail](Emulator& e) { get_detail(e, true); });
    win32("SetupDiDestroyDeviceInfoList", 1, [](Emulator& e) {
        HDEVINFO h = reinterpret_cast<HDEVINFO>(static_cast<uintptr_t>(e.arg_slot(0)));
        if (h && h != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(h);
        e.set_result(1);
    });
#else
    // No host SetupAPI: report an empty but valid set, as before.
    win32("SetupDiGetClassDevsA", 4, [](Emulator& e) { e.set_result(1); });
    win32("SetupDiGetClassDevsW", 4, [](Emulator& e) { e.set_result(1); });
    win32("SetupDiEnumDeviceInterfaces", 5, [](Emulator& e) {
        e.set_last_error(259);  // ERROR_NO_MORE_ITEMS
        e.set_result(0);
    });
    win32("SetupDiGetDeviceInterfaceDetailA", 6, [](Emulator& e) {
        e.set_last_error(259);
        e.set_result(0);
    });
    win32("SetupDiGetDeviceInterfaceDetailW", 6, [](Emulator& e) {
        e.set_last_error(259);
        e.set_result(0);
    });
    ret1("SetupDiDestroyDeviceInfoList", 1);
#endif

    // ---- network interfaces -------------------------------------------------
    // GetIfTable2 is how a guest reads the machine's MAC addresses.  The host is
    // that machine, so its own table is the answer - and MIB_IF_ROW2 has the
    // same layout in a 64-bit guest as in the 64-bit host, so the rows carry
    // across whole.  Without a real table a guest sees a machine with no
    // network interfaces, which is a different machine from the one it is on.
    // GetIfTable2Ex takes a level - MibIfTableNormal or MibIfTableRaw - and the
    // two do not return the same rows.  Passing it through matters to a caller
    // that hashes what comes back.
    auto if_table = [](Emulator& e, uint64_t out, int level) {
        if (!out) {
            e.set_result(87);  // ERROR_INVALID_PARAMETER
            return;
        }
        uint64_t table = 0;
#if defined(X86EMU_IFTABLE2)
        if (e.is64()) {
            MIB_IF_TABLE2* host = nullptr;
            if (GetIfTable2Ex(static_cast<MIB_IF_TABLE_LEVEL>(level), &host) == NO_ERROR &&
                host) {
                size_t bytes = offsetof(MIB_IF_TABLE2, Table) +
                               static_cast<size_t>(host->NumEntries) * sizeof(MIB_IF_ROW2);
                table = e.alloc_guest_data(host, bytes);
                e.log_call("GetIfTable2(level %d) -> %u interface(s) from the host",
                           level, static_cast<unsigned>(host->NumEntries));
                if (std::getenv("EMU_IFTABLE_LOG")) {
                    for (unsigned i = 0; i < host->NumEntries; ++i) {
                        const MIB_IF_ROW2& r = host->Table[i];
                        char mac[64]; int p = 0;
                        for (unsigned k = 0; k < r.PhysicalAddressLength && k < 32; ++k)
                            p += std::snprintf(mac + p, sizeof(mac) - p, "%02x", r.PhysicalAddress[k]);
                        mac[p] = 0;
                        e.log_call("  if[%2u] idx=%u type=%u guid={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} maclen=%u mac=%s",
                            i, (unsigned)r.InterfaceIndex, (unsigned)r.Type,
                            (unsigned long)r.InterfaceGuid.Data1, r.InterfaceGuid.Data2, r.InterfaceGuid.Data3,
                            r.InterfaceGuid.Data4[0], r.InterfaceGuid.Data4[1], r.InterfaceGuid.Data4[2], r.InterfaceGuid.Data4[3],
                            r.InterfaceGuid.Data4[4], r.InterfaceGuid.Data4[5], r.InterfaceGuid.Data4[6], r.InterfaceGuid.Data4[7],
                            (unsigned)r.PhysicalAddressLength, mac);
                    }
                }
                FreeMibTable(host);
            }
        }
#endif
        if (!table) {
            // No table to be had: an empty one, which the caller frees the same
            // way and reads as a machine with nothing configured.
            static const unsigned char empty[16] = {0};
            table = e.alloc_guest_data(empty, sizeof empty);
        }
        e.mem.write_sized(out, e.pointer_size(), table);
        e.set_result(0);  // NO_ERROR
    };
    win32("GetIfTable2", 1, [if_table](Emulator& e) {
        if_table(e, e.arg_slot(0), 0);  // MibIfTableNormal
    });
    win32("GetIfTable2Ex", 2, [if_table](Emulator& e) {
        if_table(e, e.arg_slot(1), static_cast<int>(e.arg_slot(0)));
    });
    // The table lives in guest memory the emulator owns, so there is nothing to
    // give back.
    win32("FreeMibTable", 1, [](Emulator& e) { e.set_result(0); });

    // ---- COM ----------------------------------------------------------------
    // Initialising an apartment is bookkeeping the emulator can agree to; the
    // moment a guest asks for an actual object it has to be told there is none,
    // and REGDB_E_CLASSNOTREG is the answer a machine without that component
    // registered gives.
    ret0("CoInitializeEx", 2);   // S_OK
    ret0("CoInitializeSecurity", 9);
    ret0("CoSetProxyBlanket", 8);
    win32("CoUninitialize", 0, [](Emulator& e) { e.set_result(0); });
    win32("CoCreateInstance", 5, [](Emulator& e) {
        // Which class was asked for is the whole story when a guest then reports
        // a feature missing, so the CLSID goes to the call log.
        uint64_t c = e.arg_slot(0);
        if (c)
            e.log_call("CoCreateInstance({%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X})",
                       e.mem.read32(c), e.mem.read16(c + 4), e.mem.read16(c + 6),
                       e.mem.read8(c + 8), e.mem.read8(c + 9), e.mem.read8(c + 10),
                       e.mem.read8(c + 11), e.mem.read8(c + 12), e.mem.read8(c + 13),
                       e.mem.read8(c + 14), e.mem.read8(c + 15));
        // CLSID_WbemLocator {4590F811-1D3A-11D0-891F-00AA004B2E24} is WMI, and
        // the emulator has an implementation of it (hooks_wmi.cpp) because a
        // guest asking what machine it runs on deserves the host's own answer.
        bool is_wbem_locator = c && e.mem.read32(c) == 0x4590F811u &&
                               e.mem.read16(c + 4) == 0x1D3A &&
                               e.mem.read16(c + 6) == 0x11D0;
        if (is_wbem_locator && e.wmi_create_locator_ && e.arg_slot(4)) {
            e.mem.write_sized(e.arg_slot(4), e.pointer_size(), e.wmi_create_locator_(e));
            e.set_result(0);  // S_OK
            return;
        }
        if (e.arg_slot(4)) e.mem.write_sized(e.arg_slot(4), e.pointer_size(), 0);
        e.set_result(0x80040154ull);  // REGDB_E_CLASSNOTREG
    });
}

}  // namespace x86emu
