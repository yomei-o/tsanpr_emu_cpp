// Winsock, for a guest that talks to the network - and gets a truthful "there
// is no network here".
//
// The emulator has no sockets: a guest may run in a browser tab, where the
// interface does not exist at all, and a save/restore would have to carry live
// connections across hosts.  So `WSAStartup` succeeds - a library is entitled
// to initialise the stack - and `socket` fails with WSAENETDOWN, which is a
// state every network client already has a path for.  A library that needs the
// network says so through its own error reporting rather than faulting.
//
// The pure computations (`htonl`, `inet_addr`) are done for real, because a
// caller uses them on addresses it never sends anywhere.
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include "emulator.h"

namespace x86emu {
namespace {

constexpr uint64_t kInvalidSocket = ~0ull;
constexpr uint64_t kSocketError = static_cast<uint64_t>(-1);
constexpr uint64_t kWsaeNetDown = 10050;
constexpr uint64_t kWsaeNotSock = 10038;
constexpr uint64_t kWsaHostNotFound = 11001;

uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

uint16_t bswap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

}  // namespace

// The ordinals a Winsock import table uses.  This is the Winsock 1.1 export
// list, which every ws2_32 still keeps at the same numbers, and an import by
// ordinal is the normal way to bind it - so without this table the guest's
// imports resolve to nothing at all.
const char* winsock_ordinal_name(uint32_t ordinal) {
    switch (ordinal) {
        case 1: return "accept";
        case 2: return "bind";
        case 3: return "closesocket";
        case 4: return "connect";
        case 5: return "getpeername";
        case 6: return "getsockname";
        case 7: return "getsockopt";
        case 8: return "htonl";
        case 9: return "htons";
        case 10: return "ioctlsocket";
        case 11: return "inet_addr";
        case 12: return "inet_ntoa";
        case 13: return "listen";
        case 14: return "ntohl";
        case 15: return "ntohs";
        case 16: return "recv";
        case 17: return "recvfrom";
        case 18: return "select";
        case 19: return "send";
        case 20: return "sendto";
        case 21: return "setsockopt";
        case 22: return "shutdown";
        case 23: return "socket";
        case 51: return "gethostbyaddr";
        case 52: return "gethostbyname";
        case 53: return "getprotobyname";
        case 54: return "getprotobynumber";
        case 55: return "getservbyname";
        case 56: return "getservbyport";
        case 57: return "gethostname";
        case 111: return "WSAGetLastError";
        case 112: return "WSASetLastError";
        case 113: return "WSACancelBlockingCall";
        case 114: return "WSAIsBlocking";
        case 115: return "WSAStartup";
        case 116: return "WSACleanup";
        case 151: return "__WSAFDIsSet";
        default: return nullptr;
    }
}

void Emulator::install_winsock_hooks() {
    auto win32 = [this](const char* name, int nargs, std::function<void(Emulator&)> fn) {
        add_hook(name, is64() ? 0 : nargs * 4, std::move(fn));
    };
    // Every call that would need a real socket fails the same way, and says so
    // through the error code the caller is about to ask for.
    auto no_socket = [&](const char* name, int nargs) {
        win32(name, nargs, [](Emulator& e) {
            e.set_last_error(kWsaeNotSock);
            e.set_result(kSocketError);
        });
    };

    // WSADATA's layout differs between the two bitnesses: the 64-bit form moves
    // the two counts and the vendor pointer ahead of the description strings so
    // the pointer stays aligned.
    win32("WSAStartup", 2, [](Emulator& e) {
        uint64_t p = e.arg_slot(1);
        uint16_t requested = static_cast<uint16_t>(e.arg_slot(0));
        if (p) {
            const char* desc = "x86emu Winsock (no network)";
            e.mem.write16(p + 0, requested ? requested : 0x0202);
            e.mem.write16(p + 2, 0x0202);
            if (e.is64()) {
                e.mem.write16(p + 4, 0);       // iMaxSockets, unused since 2.0
                e.mem.write16(p + 6, 0);       // iMaxUdpDg, likewise
                e.mem.write64(p + 8, 0);       // lpVendorInfo
                e.mem.write_cstring(p + 16, desc);
                e.mem.write8(p + 16 + 257, 0);  // szSystemStatus
            } else {
                e.mem.write_cstring(p + 4, desc);
                e.mem.write8(p + 4 + 257, 0);
                e.mem.write16(p + 4 + 257 + 129, 0);
                e.mem.write16(p + 4 + 257 + 129 + 2, 0);
                e.mem.write32(p + 4 + 257 + 129 + 4, 0);
            }
        }
        e.set_result(0);
    });
    win32("WSACleanup", 0, [](Emulator& e) { e.set_result(0); });
    win32("WSAGetLastError", 0, [](Emulator& e) { e.set_result(e.last_error()); });
    win32("WSASetLastError", 1, [](Emulator& e) {
        e.set_last_error(e.arg_slot(0));
        e.set_result(0);
    });
    win32("WSAIsBlocking", 0, [](Emulator& e) { e.set_result(0); });
    win32("WSACancelBlockingCall", 0, [](Emulator& e) { e.set_result(0); });
    win32("__WSAFDIsSet", 2, [](Emulator& e) { e.set_result(0); });

    // Creating a socket is where "no network" is reported, once, rather than at
    // every call that would have used it.
    auto no_network = [](Emulator& e) {
        e.set_last_error(kWsaeNetDown);
        e.set_result(kInvalidSocket);
    };
    win32("socket", 3, no_network);
    win32("WSASocketA", 6, no_network);
    win32("WSASocketW", 6, no_network);
    win32("accept", 3, no_network);

    no_socket("bind", 3);
    no_socket("closesocket", 1);
    no_socket("connect", 3);
    no_socket("getpeername", 3);
    no_socket("getsockname", 3);
    no_socket("getsockopt", 5);
    no_socket("ioctlsocket", 3);
    no_socket("listen", 2);
    no_socket("recv", 4);
    no_socket("recvfrom", 6);
    no_socket("send", 4);
    no_socket("sendto", 6);
    no_socket("setsockopt", 5);
    no_socket("shutdown", 2);
    // select over an empty set of ready descriptors: nothing happened, which is
    // what a timeout means, and is not an error.
    win32("select", 5, [](Emulator& e) { e.set_result(0); });

    // ---- name lookups -------------------------------------------------------
    auto not_found = [&](const char* name, int nargs) {
        win32(name, nargs, [](Emulator& e) {
            e.set_last_error(kWsaHostNotFound);
            e.set_result(0);
        });
    };
    not_found("gethostbyname", 1);
    not_found("gethostbyaddr", 3);
    not_found("getservbyname", 2);
    not_found("getservbyport", 2);
    not_found("getprotobyname", 1);
    not_found("getprotobynumber", 1);
    // getaddrinfo is the modern form of the same question, and gets the same
    // answer: there is no resolver here.  EAI_NONAME is what a name that does
    // not resolve returns, and a caller with an offline path takes it.
    auto no_address = [](Emulator& e) {
        if (e.arg_slot(3)) e.mem.write_sized(e.arg_slot(3), e.pointer_size(), 0);
        e.set_last_error(kWsaHostNotFound);
        e.set_result(kWsaHostNotFound);  // EAI_NONAME == WSAHOST_NOT_FOUND
    };
    win32("getaddrinfo", 4, no_address);
    win32("GetAddrInfoW", 4, no_address);
    win32("freeaddrinfo", 1, [](Emulator& e) { e.set_result(0); });
    win32("FreeAddrInfoW", 1, [](Emulator& e) { e.set_result(0); });
    win32("getnameinfo", 7, [](Emulator& e) {
        e.set_last_error(kWsaHostNotFound);
        e.set_result(kWsaHostNotFound);
    });
    win32("GetNameInfoW", 7, [](Emulator& e) {
        e.set_last_error(kWsaHostNotFound);
        e.set_result(kWsaHostNotFound);
    });
    // Parsing an address string is pure text work, but without a socket layer
    // there is nothing to hand the result to, so it reports the input invalid.
    win32("WSAStringToAddressW", 5, [](Emulator& e) {
        e.set_last_error(10022);  // WSAEINVAL
        e.set_result(kSocketError);
    });
    win32("WSAStringToAddressA", 5, [](Emulator& e) {
        e.set_last_error(10022);
        e.set_result(kSocketError);
    });
    win32("WSAAddressToStringW", 5, [](Emulator& e) {
        e.set_last_error(10022);
        e.set_result(kSocketError);
    });

    win32("gethostname", 2, [](Emulator& e) {
        if(std::getenv("EMU_NETLOG")) std::fprintf(stderr,"[net] gethostname called\n");
        const char* name = "localhost";
        if (e.arg_slot(0) && e.arg_slot(1) > std::strlen(name))
            e.mem.write_cstring(e.arg_slot(0), name);
        e.set_result(0);
    });

    // ---- byte order and address text ---------------------------------------
    win32("htonl", 1, [](Emulator& e) {
        e.set_result(bswap32(static_cast<uint32_t>(e.arg_slot(0))));
    });
    win32("ntohl", 1, [](Emulator& e) {
        e.set_result(bswap32(static_cast<uint32_t>(e.arg_slot(0))));
    });
    win32("htons", 1, [](Emulator& e) {
        e.set_result(bswap16(static_cast<uint16_t>(e.arg_slot(0))));
    });
    win32("ntohs", 1, [](Emulator& e) {
        e.set_result(bswap16(static_cast<uint16_t>(e.arg_slot(0))));
    });
    win32("inet_addr", 1, [](Emulator& e) {
        std::string s = e.arg_slot(0) ? e.mem.read_cstring(e.arg_slot(0)) : "";
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (std::sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 ||
            a > 255 || b > 255 || c > 255 || d > 255) {
            e.set_result(0xFFFFFFFFull);  // INADDR_NONE
            return;
        }
        // Network byte order: the first component is the low byte on a
        // little-endian host.
        e.set_result((d << 24) | (c << 16) | (b << 8) | a);
    });
    win32("inet_ntoa", 1, [](Emulator& e) {
        uint32_t v = static_cast<uint32_t>(e.arg_slot(0));
        char text[32];
        std::snprintf(text, sizeof text, "%u.%u.%u.%u", v & 0xFF, (v >> 8) & 0xFF,
                      (v >> 16) & 0xFF, (v >> 24) & 0xFF);
        // Real inet_ntoa answers a per-thread static buffer the caller may read
        // until its next call; a fresh guest string per call is the same
        // contract seen from outside, and never aliases.
        e.set_result(e.alloc_guest_string(text));
    });
}

}  // namespace x86emu
