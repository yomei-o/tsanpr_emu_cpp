// WMI, as a COM object the guest can actually call.
//
// A guest that asks what machine it is running on reaches for WMI, and gets
// there through COM: CoCreateInstance hands back an IWbemLocator, and every
// step after that is a virtual call through a vtable in guest memory.  So
// answering CoCreateInstance is not enough - the object has to exist, with a
// vtable whose slots the guest can call.
//
// That is what this builds: each interface gets a vtable of hook addresses, and
// an object is two guest words - the vtable pointer and an id into the table
// here.  A method call arrives as an ordinary hook with `this` as its first
// argument, so the ABI glue that already handles stdcall and Microsoft x64
// handles COM without knowing what COM is.
//
// The emulator runs on the machine the guest is asking about, so the honest
// answer to "what is this machine" is the host's own - the same principle that
// has the guest inherit the host's environment and see its filesystem.
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "emulator.h"
#include "guest_printf.h"

#if defined(_WIN32)
#include <objbase.h>
#include <wbemidl.h>
#endif

namespace x86emu {
namespace {

// A property as WMI gave it: its VARIANT type, the number when it is one, and
// the text form for everything else.
struct Value {
    unsigned short vt = 8;  // VT_BSTR
    long long number = 0;
    std::string text;
};


#if defined(_WIN32)

std::string utf16_string_to_utf8_local(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    return out;
}

// Runs a WQL query against the host's own WMI and returns the rows as strings.
//
// The guest is asking what machine it is running on, and this is that machine -
// so the faithful answer is the host's, exactly as the host reports it.  Every
// value is converted to its string form, because that is what the caller reads
// out of the VARIANT the guest side builds.
//
// Answering nothing is a real answer too: a class with no instances (a virtual
// machine has no baseboard) has to come back empty rather than invented, or the
// guest sees a different machine than the one it is on.
bool host_wmi_query(const std::string& wql,
                    std::vector<std::map<std::string, Value>>& rows,
                    const std::vector<std::string>& columns) {
    static bool com_ready = false;
    if (!com_ready) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) return false;
        // Only meaningful once per process, and harmless when it has already
        // been set by whatever hosts the emulator.
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                             RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
        com_ready = true;
    }

    IWbemLocator* locator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, reinterpret_cast<void**>(&locator))))
        return false;

    IWbemServices* services = nullptr;
    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr,
                                        nullptr, &services);
    SysFreeString(ns);
    if (FAILED(hr)) {
        locator->Release();
        return false;
    }
    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                      EOAC_NONE);

    std::wstring wide(wql.begin(), wql.end());  // WQL is ASCII
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(wide.c_str());
    IEnumWbemClassObject* it = nullptr;
    hr = services->ExecQuery(language, query,
                             WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                             nullptr, &it);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(hr)) {
        services->Release();
        locator->Release();
        return false;
    }

    for (;;) {
        IWbemClassObject* row = nullptr;
        ULONG got = 0;
        if (it->Next(WBEM_INFINITE, 1, &row, &got) != WBEM_S_NO_ERROR || got == 0) break;
        std::map<std::string, Value> values;
        for (const std::string& column : columns) {
            std::wstring name(column.begin(), column.end());
            VARIANT v;
            VariantInit(&v);
            Value value;
            if (SUCCEEDED(row->Get(name.c_str(), 0, &v, nullptr, nullptr))) {
                // A property keeps the type WMI gave it.  Handing a caller a
                // string where it expects a number is the kind of difference
                // that turns into a wrong answer several steps later, with
                // nothing in between to say where it came from.
                value.vt = v.vt;
                switch (v.vt) {
                    case VT_I2: value.number = v.iVal; break;
                    case VT_I4: value.number = v.lVal; break;
                    case VT_UI1: value.number = v.bVal; break;
                    case VT_UI2: value.number = v.uiVal; break;
                    case VT_UI4: value.number = v.ulVal; break;
                    case VT_INT: value.number = v.intVal; break;
                    case VT_UINT: value.number = v.uintVal; break;
                    case VT_BOOL: value.number = v.boolVal ? -1 : 0; break;
                    default: break;
                }
                VARIANT as_text;
                VariantInit(&as_text);
                if (v.vt != VT_NULL && v.vt != VT_EMPTY &&
                    SUCCEEDED(VariantChangeType(&as_text, &v, 0, VT_BSTR)))
                    value.text = utf16_string_to_utf8_local(as_text.bstrVal);
                VariantClear(&as_text);
            }
            values[column] = value;
        }
        rows.push_back(std::move(values));
        row->Release();
    }
    it->Release();
    services->Release();
    locator->Release();
    return true;
}


// The columns a "SELECT a,b FROM c" asks for, so each can be read back by name.
std::vector<std::string> selected_columns(const std::string& wql) {
    size_t select = wql.find("SELECT ");
    size_t from = wql.find(" FROM ");
    if (select == std::string::npos || from == std::string::npos || from < select)
        return {};
    std::string list = wql.substr(select + 7, from - select - 7);
    std::vector<std::string> columns;
    size_t at = 0;
    while (at <= list.size()) {
        size_t comma = list.find(',', at);
        if (comma == std::string::npos) comma = list.size();
        std::string one = list.substr(at, comma - at);
        size_t b = one.find_first_not_of(" \t");
        size_t e2 = one.find_last_not_of(" \t");
        if (b != std::string::npos) columns.push_back(one.substr(b, e2 - b + 1));
        at = comma + 1;
    }
    return columns;
}

#endif  // _WIN32

#if !defined(_WIN32)
// The answers to serve where there is no WMI to ask, carried as data.
//
// One row per line: a substring the query must contain, then the properties.
//
//     Win32_BaseBoard                                    <- matches, no rows
//     Win32_Processor  Name=virt-9.1 ProcessorId=0000000000000000
//     GUID='{5C737FB0  DeviceID=0                        <- a WHERE clause row
//
// The match is a plain case-insensitive substring of the WQL, which is enough to
// pick a class and, where a query names one, a particular instance.  A line with
// no properties says "this query matches nothing", which is a real answer and the
// one Win32_BaseBoard gives on a virtual machine.
struct WmiRow {
    std::string match;
    std::map<std::string, Value> props;
};

std::vector<WmiRow>& wmi_table() {
    static std::vector<WmiRow> table;
    return table;
}

std::string wmi_lower(std::string v) {
    for (char& c : v)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return v;
}

void load_wmi_answers(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        std::fprintf(stderr, "[wmi] cannot open %s\n", path.c_str());
        return;
    }
    char line[2048];
    int n = 0;
    while (std::fgets(line, sizeof line, fp)) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        size_t b = text.find_first_not_of(" \t");
        if (b == std::string::npos || text[b] == '#') continue;
        text = text.substr(b);
        WmiRow row;
        size_t at = text.find_first_of(" \t");
        row.match = wmi_lower(text.substr(0, at));
        while (at != std::string::npos) {
            size_t s2 = text.find_first_not_of(" \t", at);
            if (s2 == std::string::npos) break;
            at = text.find_first_of(" \t", s2);
            std::string pair = text.substr(s2, at == std::string::npos ? at : at - s2);
            size_t eq = pair.find('=');
            if (eq == std::string::npos) continue;
            Value v;
            v.vt = 8;  // VT_BSTR: every property this serves is text
            v.text = pair.substr(eq + 1);
            row.props[pair.substr(0, eq)] = v;
        }
        wmi_table().push_back(std::move(row));
        ++n;
    }
    std::fclose(fp);
    std::fprintf(stderr, "[wmi] %s: %d row(s)\n", path.c_str(), n);
}
#endif  // !_WIN32


// HRESULTs a WMI caller distinguishes between.
constexpr uint64_t kOk = 0;
constexpr uint64_t kNotImpl = 0x80004001ull;
constexpr uint64_t kNoInterface = 0x80004002ull;
constexpr uint64_t kWbemNotFound = 0x80041002ull;
constexpr uint64_t kWbemFalse = 1;  // WBEM_S_FALSE: the enumeration is done

// Which interface an object presents.  The id in the object's second word is an
// index into WmiState::objects, which says both the kind and what it holds.
enum class Iface { Locator, Services, Enumerator, ClassObject };

struct Object {
    Iface iface;
    // For an enumerator: the rows still to hand out, each a set of properties.
    std::vector<std::map<std::string, Value>> rows;
    size_t at = 0;
    // For a class object: the row it stands for.
    std::map<std::string, Value> row;
};

struct WmiState {
    // Hook addresses, per interface, in vtable slot order.
    std::map<Iface, std::vector<uint64_t>> slots;
    // The vtable's address in guest memory, once something has needed it.
    std::map<Iface, uint64_t> vtables;
    std::map<uint64_t, Object> objects;  // object address -> what it is
};

// A BSTR is a pointer to UTF-16 with its byte length in the four bytes before
// it; reading one only needs the terminator, which every BSTR also has.
std::string read_bstr(Emulator& e, uint64_t p) {
    if (!p) return "";
    return utf16_to_utf8(e, p, -1);
}

}  // namespace

void Emulator::install_wmi_hooks() {
#if !defined(_WIN32)
    for (const std::string& f : options().wmi_files) load_wmi_answers(f);
#endif
    auto state = std::make_shared<WmiState>();

    // Every slot is a hook; the ones with no implementation answer E_NOTIMPL,
    // which is a real answer a caller has to handle rather than a silent zero.
    auto slot = [this, state](Iface iface, int index, const char* name,
                              std::function<void(Emulator&)> fn) {
        // COM is stdcall on 32-bit, and `this` counts as an argument.
        char hook_name[64];
        std::snprintf(hook_name, sizeof hook_name, "__wmi_%d_%d_%s",
                      static_cast<int>(iface), index, name);
        uint64_t addr = add_hook(hook_name, 0, std::move(fn));
        auto& v = state->slots[iface];
        if (v.size() <= static_cast<size_t>(index)) v.resize(index + 1, 0);
        v[index] = addr;
    };
    auto not_impl = [this, state](Iface iface, int index) {
        char hook_name[64];
        std::snprintf(hook_name, sizeof hook_name, "__wmi_%d_%d_notimpl",
                      static_cast<int>(iface), index);
        uint64_t addr = add_hook(hook_name, 0,
                                 [](Emulator& e) { e.set_result(kNotImpl); });
        auto& v = state->slots[iface];
        if (v.size() <= static_cast<size_t>(index)) v.resize(index + 1, 0);
        v[index] = addr;
    };

    // Making an object: the vtable is built the first time an interface is
    // needed, because guest memory does not exist yet while hooks are installed.
    auto make_object = [state](Emulator& e, Iface iface) -> uint64_t {
        uint64_t ps = e.pointer_size();
        auto found = state->vtables.find(iface);
        if (found == state->vtables.end()) {
            const auto& s = state->slots[iface];
            std::vector<unsigned char> blank(s.size() * ps, 0);
            uint64_t vt = e.alloc_guest_data(blank.data(), blank.size());
            for (size_t i = 0; i < s.size(); ++i) e.mem.write_sized(vt + i * ps, ps, s[i]);
            found = state->vtables.emplace(iface, vt).first;
        }
        std::vector<unsigned char> blank(ps * 2, 0);
        uint64_t obj = e.alloc_guest_data(blank.data(), blank.size());
        e.mem.write_sized(obj, ps, found->second);
        e.mem.write_sized(obj + ps, ps, 0);
        state->objects[obj] = Object{iface, {}, 0, {}};
        return obj;
    };

    // ---- IUnknown, on every interface ---------------------------------------
    // Reference counting has nothing to free: an object lives in the table until
    // the process ends, and a guest that releases one simply stops using it.
    for (Iface iface : {Iface::Locator, Iface::Services, Iface::Enumerator,
                        Iface::ClassObject}) {
        slot(iface, 0, "QueryInterface", [](Emulator& e) {
            // Handing back the same object for any interface asked of it is
            // right here: each of these presents exactly one, and the caller
            // has already been told which by how it got the pointer.
            if (e.arg_slot(2)) e.mem.write_sized(e.arg_slot(2), e.pointer_size(), e.arg_slot(0));
            e.set_result(e.arg_slot(2) ? kOk : kNoInterface);
        });
        slot(iface, 1, "AddRef", [](Emulator& e) { e.set_result(2); });
        slot(iface, 2, "Release", [](Emulator& e) { e.set_result(1); });
    }

    // ---- IWbemLocator --------------------------------------------------------
    // ConnectServer(this, resource, user, password, locale, flags, authority,
    //               ctx, ppNamespace)
    slot(Iface::Locator, 3, "ConnectServer", [state, make_object](Emulator& e) {
        std::string ns = read_bstr(e, e.arg_slot(1));
        e.log_call("IWbemLocator::ConnectServer(%s)", ns.c_str());
        uint64_t out = e.arg_slot(8);
        if (!out) {
            e.set_result(0x80070057ull);  // E_INVALIDARG
            return;
        }
        e.mem.write_sized(out, e.pointer_size(), make_object(e, Iface::Services));
        e.set_result(kOk);
    });

    // ---- IWbemServices -------------------------------------------------------
    // Only the two calls that read something are implemented; the rest of the
    // twenty-six slots exist so a vtable index is never a hole.
    for (int i = 3; i <= 25; ++i) not_impl(Iface::Services, i);
    // GetObject(this, path, flags, ctx, ppObject, ppCallResult)
    slot(Iface::Services, 6, "GetObject", [state, make_object](Emulator& e) {
        e.log_call("IWbemServices::GetObject(%s)", read_bstr(e, e.arg_slot(1)).c_str());
        e.set_result(kWbemNotFound);
    });
    // ExecQuery(this, language, query, flags, ctx, ppEnum)
    slot(Iface::Services, 20, "ExecQuery", [state, make_object](Emulator& e) {
        std::string language = read_bstr(e, e.arg_slot(1));
        std::string query = read_bstr(e, e.arg_slot(2));
        e.log_call("IWbemServices::ExecQuery(%s: %s)", language.c_str(), query.c_str());
        uint64_t out = e.arg_slot(5);
        if (!out) {
            e.set_result(0x80070057ull);
            return;
        }
        uint64_t obj = make_object(e, Iface::Enumerator);
#if defined(_WIN32)
        // The host is the machine being asked about, so its own WMI answers.
        // A failure to reach it leaves the enumeration empty, which the guest
        // reads as "this class has no instances" - the same thing it would see
        // on a machine that genuinely has none.
        std::vector<std::map<std::string, Value>> rows;
        if (host_wmi_query(query, rows, selected_columns(query))) {
            e.log_call("  -> %u row(s) from the host", static_cast<unsigned>(rows.size()));
            state->objects[obj].rows = std::move(rows);
        }
#else
        // No WMI to ask: serve whatever the answer table has for this query.  With
        // no table the enumeration stays empty, exactly as before.
        std::string haystack = wmi_lower(query);
        std::vector<std::map<std::string, Value>> rows;
        bool matched = false;
        for (const WmiRow& row : wmi_table()) {
            if (row.match.empty() || haystack.find(row.match) == std::string::npos) continue;
            matched = true;
            if (!row.props.empty()) rows.push_back(row.props);
        }
        if (matched) {
            e.log_call("  -> %u row(s) from the answer table",
                       static_cast<unsigned>(rows.size()));
            state->objects[obj].rows = std::move(rows);
        }
#endif
        e.mem.write_sized(out, e.pointer_size(), obj);
        e.set_result(kOk);
    });

    // ---- IEnumWbemClassObject -------------------------------------------------
    for (int i = 3; i <= 7; ++i) not_impl(Iface::Enumerator, i);
    slot(Iface::Enumerator, 3, "Reset", [state](Emulator& e) {
        auto it = state->objects.find(e.arg_slot(0));
        if (it != state->objects.end()) it->second.at = 0;
        e.set_result(kOk);
    });
    // Next(this, timeout, count, apObjects, puReturned)
    slot(Iface::Enumerator, 4, "Next", [state, make_object](Emulator& e) {
        auto it = state->objects.find(e.arg_slot(0));
        uint64_t want = e.arg_slot(2), out = e.arg_slot(3), returned = e.arg_slot(4);
        uint64_t ps = e.pointer_size();
        uint64_t n = 0;
        if (it != state->objects.end()) {
            while (n < want && it->second.at < it->second.rows.size()) {
                uint64_t row_obj = make_object(e, Iface::ClassObject);
                state->objects[row_obj].row = it->second.rows[it->second.at++];
                if (out) e.mem.write_sized(out + n * ps, ps, row_obj);
                ++n;
            }
        }
        if (returned) e.mem.write32(returned, static_cast<uint32_t>(n));
        e.set_result(n == want ? kOk : kWbemFalse);
    });

    // ---- IWbemClassObject ------------------------------------------------------
    for (int i = 3; i <= 26; ++i) not_impl(Iface::ClassObject, i);
    // Get(this, name, flags, pVal, pType, plFlavor) - the property read.
    slot(Iface::ClassObject, 4, "Get", [state](Emulator& e) {
        auto it = state->objects.find(e.arg_slot(0));
        std::string name = e.arg_slot(1) ? utf16_to_utf8(e, e.arg_slot(1), -1) : "";
        e.log_call("IWbemClassObject::Get(%s)", name.c_str());
        if (it == state->objects.end()) {
            e.set_result(kWbemNotFound);
            return;
        }
        auto found = it->second.row.find(name);
        if (found == it->second.row.end()) {
            e.log_call("  %s -> (absent)", name.c_str());
            e.set_result(kWbemNotFound);
            return;
        }
        const Value& value = found->second;
        e.log_call("  %s = \"%s\" (vt %u)", name.c_str(), value.text.c_str(),
                   static_cast<unsigned>(value.vt));
        // A VARIANT: the type in the first word, the payload from offset 8.
        uint64_t v = e.arg_slot(3);
        bool numeric = value.vt != 8 && value.vt != 0 && value.vt != 1;
        if (v) {
            e.mem.write16(v, value.vt);
            e.mem.write16(v + 2, 0);
            e.mem.write16(v + 4, 0);
            e.mem.write16(v + 6, 0);
            if (numeric) {
                e.mem.write64(v + 8, static_cast<uint64_t>(value.number));
            } else {
                // A BSTR is a pointer to UTF-16 with its byte length in the four
                // bytes before it.
                std::u16string w = utf8_to_utf16(value.text);
                std::vector<unsigned char> blank((w.size() + 1) * 2 + 4, 0);
                uint64_t at = e.alloc_guest_data(blank.data(), blank.size());
                e.mem.write32(at, static_cast<uint32_t>(w.size() * 2));
                for (size_t i = 0; i <= w.size(); ++i)
                    e.mem.write16(at + 4 + i * 2, i < w.size() ? w[i] : 0);
                e.mem.write_sized(v + 8, e.pointer_size(), at + 4);
            }
        }
        // The CIM type the property is declared as, which a caller may switch on.
        uint32_t cim = 8;  // CIM_STRING
        switch (value.vt) {
            case 2: cim = 2; break;    // VT_I2   -> CIM_SINT16
            case 3: cim = 3; break;    // VT_I4   -> CIM_SINT32
            case 11: cim = 11; break;  // VT_BOOL -> CIM_BOOLEAN
            case 17: cim = 17; break;  // VT_UI1  -> CIM_UINT8
            case 18: cim = 18; break;  // VT_UI2  -> CIM_UINT16
            case 19: cim = 19; break;  // VT_UI4  -> CIM_UINT32
            default: break;
        }
        if (e.arg_slot(4)) e.mem.write32(e.arg_slot(4), cim);
        e.set_result(kOk);
    });

    // ---- the entry point ------------------------------------------------------
    // CoCreateInstance for CLSID_WbemLocator; anything else stays with the
    // "no such class registered" answer in hooks_win32d.cpp.
    wmi_create_locator_ = [state, make_object](Emulator& e) -> uint64_t {
        return make_object(e, Iface::Locator);
    };
}

}  // namespace x86emu
