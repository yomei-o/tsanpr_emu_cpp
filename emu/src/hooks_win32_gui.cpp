// A headless window system: enough of USER32/GDI32 for a Win32 desktop program
// to start up, create its windows, and run whatever it does around them.
//
// There is no display here, so nothing is drawn and no input arrives.  What the
// hooks do provide is the part a program's own code depends on: window handles
// that stay distinct and can be looked up, the window procedure actually being
// called for WM_NCCREATE/WM_CREATE, properties and window text that read back
// what was stored, and a message loop that ends at once rather than blocking
// forever on a queue nothing will ever post to.
//
// That is the difference between "the GUI does not work" and "the program does
// not run": a tool whose real work happens on the way up gets to do it, and
// says what it did through its files and its exit code.
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "emulator.h"
#include "guest_printf.h"

namespace x86emu {
namespace {

// Handles come from one counter so a window, a DC and a brush are never
// confused for each other, and none of them is ever zero.
struct GuiState {
    uint64_t next_handle = 0x00110000;
    struct Window {
        uint64_t wndproc = 0;
        uint64_t parent = 0;
        std::string class_name;
        std::string text;
        std::map<std::string, uint64_t> props;
        std::map<int, uint64_t> longs;
    };
    std::map<uint64_t, Window> windows;
    std::map<std::string, uint64_t> classes;  // class name -> window procedure
    // Dialog controls, by dialog window and control id, so GetDlgItem finds
    // what the template said was there.
    std::map<uint64_t, std::map<uint32_t, uint64_t>> dialog_items;

    uint64_t alloc() { return next_handle += 0x10; }
};

uint64_t read_ptr(Emulator& e, uint64_t p) {
    return e.mem.read_sized(p, e.pointer_size());
}

std::string wide_or_atom(Emulator& e, uint64_t p) {
    // A class may be named by an atom, which is a small integer in the pointer
    // slot rather than an address.  Naming it "#<atom>" keeps one lookup table.
    if (p > 0xFFFF) return utf16_to_utf8(e, p, -1);
    char text[16];
    std::snprintf(text, sizeof text, "#%u", static_cast<unsigned>(p));
    return text;
}


// A string or an ordinal, as a dialog template spells them: 0x0000 is an empty
// string, 0xFFFF means the next word is an ordinal, anything else is the first
// unit of a NUL-terminated UTF-16 string.  Returns the text and advances `at`.
std::string read_sz_or_ord(Emulator& e, uint64_t& at) {
    uint16_t first = e.mem.read16(at);
    if (first == 0x0000) {
        at += 2;
        return "";
    }
    if (first == 0xFFFF) {
        uint16_t ordinal = e.mem.read16(at + 2);
        at += 4;
        // The predefined control classes, which templates name by number.
        switch (ordinal) {
            case 0x0080: return "Button";
            case 0x0081: return "Edit";
            case 0x0082: return "Static";
            case 0x0083: return "ListBox";
            case 0x0084: return "ScrollBar";
            case 0x0085: return "ComboBox";
            default: break;
        }
        char text[16];
        std::snprintf(text, sizeof text, "#%u", static_cast<unsigned>(ordinal));
        return text;
    }
    std::u16string w;
    for (;;) {
        uint16_t c = e.mem.read16(at);
        at += 2;
        if (!c) break;
        w += static_cast<char16_t>(c);
    }
    return utf16_string_to_utf8(w);
}

struct DialogItem {
    uint32_t id;
    std::string cls;
    std::string text;
};

std::vector<DialogItem> parse_dialog_template(Emulator& e, uint64_t at,
                                              std::string& caption) {
    std::vector<DialogItem> items_out;
    if (!at) return items_out;
    // The extended form announces itself with 0xFFFF in the second word; the two
    // layouts differ in field order and width all the way down.
    bool extended = e.mem.read16(at + 2) == 0xFFFF;
    uint32_t style = 0;
    uint16_t items = 0;
    uint64_t p = at;
    if (extended) {
        style = e.mem.read32(p + 12);
        items = e.mem.read16(p + 16);
        p += 26;
    } else {
        style = e.mem.read32(p);
        items = e.mem.read16(p + 8);
        p += 18;
    }
    read_sz_or_ord(e, p);                          // menu
    read_sz_or_ord(e, p);                          // window class
    caption = read_sz_or_ord(e, p);                // caption
    constexpr uint32_t kSetFont = 0x00000040;      // DS_SETFONT
    if (style & kSetFont) {
        p += extended ? 6 : 2;                     // point size (+ weight, italic, charset)
        read_sz_or_ord(e, p);                      // typeface
    }
    for (uint16_t i = 0; i < items; ++i) {
        p = (p + 3) & ~uint64_t(3);                // each item starts DWORD-aligned
        uint32_t id;
        if (extended) {
            id = e.mem.read32(p + 20);
            p += 24;
        } else {
            id = e.mem.read16(p + 16);
            p += 18;
        }
        std::string cls = read_sz_or_ord(e, p);
        std::string text = read_sz_or_ord(e, p);
        uint16_t extra = e.mem.read16(p);
        p += 2 + extra;
        items_out.push_back(DialogItem{id, cls, text});
    }
    return items_out;
}

}  // namespace

void Emulator::install_gui_hooks() {
    auto win32 = [this](const char* name, int nargs, std::function<void(Emulator&)> fn) {
        add_hook(name, is64() ? 0 : nargs * 4, std::move(fn));
    };
    auto ret0 = [&](const char* name, int nargs) {
        win32(name, nargs, [](Emulator& e) { e.set_result(0); });
    };
    auto ret1 = [&](const char* name, int nargs) {
        win32(name, nargs, [](Emulator& e) { e.set_result(1); });
    };
    auto gui = std::make_shared<GuiState>();

    // ---- window classes ------------------------------------------------------
    // WNDCLASSEXW is laid out around two pointer-sized fields before the extras,
    // so the window procedure and the class name move with the bitness.
    win32("RegisterClassExW", 1, [gui](Emulator& e) {
        uint64_t p = e.arg_slot(0);
        if (!p) {
            e.set_result(0);
            return;
        }
        uint64_t ps = e.pointer_size();
        uint64_t wndproc = read_ptr(e, p + 8);
        uint64_t name_at = e.is64() ? p + 64 : p + 40;
        std::string name = wide_or_atom(e, read_ptr(e, name_at));
        (void)ps;
        gui->classes[name] = wndproc;
        e.log_call("RegisterClass(%s, proc=0x%llX)", name.c_str(),
                   static_cast<unsigned long long>(wndproc));
        e.set_result(0xC000 + (gui->classes.size() & 0xFF));  // a plausible atom
    });
    win32("RegisterClassW", 1, [gui](Emulator& e) {
        uint64_t p = e.arg_slot(0);
        if (!p) {
            e.set_result(0);
            return;
        }
        uint64_t wndproc = read_ptr(e, p + 4 + (e.is64() ? 4 : 0));
        uint64_t name_at = e.is64() ? p + 56 : p + 36;
        gui->classes[wide_or_atom(e, read_ptr(e, name_at))] = wndproc;
        e.set_result(0xC000 + (gui->classes.size() & 0xFF));
    });
    ret1("UnregisterClassW", 2);

    // ---- windows -------------------------------------------------------------
    // Creating a window runs the class's own procedure for WM_NCCREATE and
    // WM_CREATE, which is where a program does its setup - so this is the one
    // place the hook has to call back into the guest rather than answer for it.
    win32("CreateWindowExW", 12, [gui](Emulator& e) {
        std::string cls = wide_or_atom(e, e.arg_slot(1));
        uint64_t hwnd = gui->alloc();
        GuiState::Window w;
        w.class_name = cls;
        w.parent = e.arg_slot(8);
        if (e.arg_slot(2)) w.text = utf16_to_utf8(e, e.arg_slot(2), -1);
        auto found = gui->classes.find(cls);
        w.wndproc = found == gui->classes.end() ? 0 : found->second;
        gui->windows[hwnd] = w;
        e.log_call("CreateWindow(%s, \"%s\") = 0x%llX", cls.c_str(), w.text.c_str(),
                   static_cast<unsigned long long>(hwnd));

        if (w.wndproc) {
            // CREATESTRUCTW, in the caller's own layout, so lpCreateParams -
            // the pointer a program passes itself through - arrives intact.
            uint64_t ps = e.pointer_size();
            std::vector<unsigned char> cs(e.is64() ? 80 : 48, 0);
            uint64_t at = e.alloc_guest_data(cs.data(), cs.size());
            e.mem.write_sized(at + 0 * ps, ps, e.arg_slot(11));  // lpCreateParams
            e.mem.write_sized(at + 1 * ps, ps, e.arg_slot(10));  // hInstance
            e.mem.write_sized(at + 2 * ps, ps, e.arg_slot(9));   // hMenu
            e.mem.write_sized(at + 3 * ps, ps, e.arg_slot(8));   // hwndParent
            e.mem.write32(at + 4 * ps + 0, static_cast<uint32_t>(e.arg_slot(7)));  // cy
            e.mem.write32(at + 4 * ps + 4, static_cast<uint32_t>(e.arg_slot(6)));  // cx
            e.mem.write32(at + 4 * ps + 8, static_cast<uint32_t>(e.arg_slot(5)));  // y
            e.mem.write32(at + 4 * ps + 12, static_cast<uint32_t>(e.arg_slot(4))); // x
            e.mem.write32(at + 4 * ps + 16, static_cast<uint32_t>(e.arg_slot(3))); // style
            e.mem.write_sized(at + 4 * ps + 24, ps, e.arg_slot(2));  // lpszName
            e.mem.write_sized(at + 5 * ps + 24, ps, e.arg_slot(1));  // lpszClass
            e.call_guest(w.wndproc, {hwnd, 0x0081, 0, at});  // WM_NCCREATE
            e.call_guest(w.wndproc, {hwnd, 0x0001, 0, at});  // WM_CREATE
        }
        e.set_result(hwnd);
    });
    win32("DestroyWindow", 1, [gui](Emulator& e) {
        gui->windows.erase(e.arg_slot(0));
        e.set_result(1);
    });
    win32("DefWindowProcW", 4, [](Emulator& e) { e.set_result(0); });
    win32("SendMessageW", 4, [gui](Emulator& e) {
        auto it = gui->windows.find(e.arg_slot(0));
        if (it != gui->windows.end() && it->second.wndproc) {
            e.set_result(e.call_guest(it->second.wndproc,
                                      {e.arg_slot(0), e.arg_slot(1), e.arg_slot(2),
                                       e.arg_slot(3)}));
            return;
        }
        e.set_result(0);
    });
    ret0("PostMessageW", 4);
    win32("GetParent", 1, [gui](Emulator& e) {
        auto it = gui->windows.find(e.arg_slot(0));
        e.set_result(it == gui->windows.end() ? 0 : it->second.parent);
    });
    win32("FindWindowW", 2, [](Emulator& e) { e.set_result(0); });
    win32("GetDlgItem", 2, [gui](Emulator& e) {
        auto d = gui->dialog_items.find(e.arg_slot(0));
        if (d == gui->dialog_items.end()) {
            e.set_result(0);
            return;
        }
        auto it = d->second.find(static_cast<uint32_t>(e.arg_slot(1)));
        e.set_result(it == d->second.end() ? 0 : it->second);
    });
    // (dialog, id, buffer, chars)
    win32("GetDlgItemTextW", 4, [gui](Emulator& e) {
        std::string text;
        auto d = gui->dialog_items.find(e.arg_slot(0));
        if (d != gui->dialog_items.end()) {
            auto it = d->second.find(static_cast<uint32_t>(e.arg_slot(1)));
            if (it != d->second.end()) text = gui->windows[it->second].text;
        }
        std::u16string w = utf8_to_utf16(text);
        uint64_t buf = e.arg_slot(2), max = e.arg_slot(3);
        if (!buf || max == 0) {
            e.set_result(0);
            return;
        }
        size_t n = w.size() + 1 > max ? static_cast<size_t>(max - 1) : w.size();
        for (size_t i = 0; i < n; ++i) e.mem.write16(buf + i * 2, w[i]);
        e.mem.write16(buf + n * 2, 0);
        e.set_result(n);
    });
    win32("SetDlgItemTextW", 3, [gui](Emulator& e) {
        auto d = gui->dialog_items.find(e.arg_slot(0));
        if (d != gui->dialog_items.end()) {
            auto it = d->second.find(static_cast<uint32_t>(e.arg_slot(1)));
            if (it != d->second.end()) {
                std::string text =
                    e.arg_slot(2) ? utf16_to_utf8(e, e.arg_slot(2), -1) : "";
                gui->windows[it->second].text = text;
                e.log_call("dialog control %u <- \"%s\"",
                           static_cast<unsigned>(e.arg_slot(1)), text.c_str());
            }
        }
        e.set_result(1);
    });

    // Window text reads back what was stored, because a program that sets it and
    // then reads it is talking to itself, not to a display.
    win32("SetWindowTextW", 2, [gui](Emulator& e) {
        auto it = gui->windows.find(e.arg_slot(0));
        if (it != gui->windows.end())
            it->second.text = e.arg_slot(1) ? utf16_to_utf8(e, e.arg_slot(1), -1) : "";
        e.set_result(1);
    });
    win32("GetWindowTextW", 3, [gui](Emulator& e) {
        auto it = gui->windows.find(e.arg_slot(0));
        std::string s = it == gui->windows.end() ? "" : it->second.text;
        std::u16string w = utf8_to_utf16(s);
        uint64_t buf = e.arg_slot(1), max = e.arg_slot(2);
        if (!buf || max == 0) {
            e.set_result(0);
            return;
        }
        size_t n = w.size() + 1 > max ? static_cast<size_t>(max - 1) : w.size();
        for (size_t i = 0; i < n; ++i) e.mem.write16(buf + i * 2, w[i]);
        e.mem.write16(buf + n * 2, 0);
        e.set_result(n);
    });
    win32("GetWindowTextLengthW", 1, [gui](Emulator& e) {
        auto it = gui->windows.find(e.arg_slot(0));
        e.set_result(it == gui->windows.end() ? 0 : utf8_to_utf16(it->second.text).size());
    });

    // Properties and the window longs are per-window storage a program uses to
    // hang its own object off a handle; losing them is how a window procedure
    // ends up dereferencing null.
    win32("SetPropW", 3, [gui](Emulator& e) {
        auto it = gui->windows.find(e.arg_slot(0));
        if (it != gui->windows.end())
            it->second.props[wide_or_atom(e, e.arg_slot(1))] = e.arg_slot(2);
        e.set_result(1);
    });
    win32("GetPropW", 2, [gui](Emulator& e) {
        auto it = gui->windows.find(e.arg_slot(0));
        if (it == gui->windows.end()) {
            e.set_result(0);
            return;
        }
        auto p = it->second.props.find(wide_or_atom(e, e.arg_slot(1)));
        e.set_result(p == it->second.props.end() ? 0 : p->second);
    });
    win32("RemovePropW", 2, [gui](Emulator& e) {
        auto it = gui->windows.find(e.arg_slot(0));
        if (it == gui->windows.end()) {
            e.set_result(0);
            return;
        }
        std::string key = wide_or_atom(e, e.arg_slot(1));
        auto p = it->second.props.find(key);
        uint64_t v = p == it->second.props.end() ? 0 : p->second;
        it->second.props.erase(key);
        e.set_result(v);
    });
    auto set_window_long = [gui](Emulator& e) {
        auto it = gui->windows.find(e.arg_slot(0));
        int index = static_cast<int>(static_cast<int32_t>(e.arg_slot(1)));
        uint64_t old = 0;
        if (it != gui->windows.end()) {
            auto f = it->second.longs.find(index);
            if (f != it->second.longs.end()) old = f->second;
            it->second.longs[index] = e.arg_slot(2);
            // GWLP_WNDPROC is -4: subclassing has to take effect, or messages go
            // to the wrong procedure.
            if (index == -4) it->second.wndproc = e.arg_slot(2);
        }
        e.set_result(old);
    };
    win32("SetWindowLongW", 3, set_window_long);
    win32("SetWindowLongPtrW", 3, set_window_long);
    auto get_window_long = [gui](Emulator& e) {
        auto it = gui->windows.find(e.arg_slot(0));
        if (it == gui->windows.end()) {
            e.set_result(0);
            return;
        }
        auto f = it->second.longs.find(static_cast<int>(static_cast<int32_t>(e.arg_slot(1))));
        e.set_result(f == it->second.longs.end() ? 0 : f->second);
    };
    win32("GetWindowLongW", 2, get_window_long);
    win32("GetWindowLongPtrW", 2, get_window_long);

    // ---- the message loop ----------------------------------------------------
    // Nothing will ever post a message here, so GetMessage answering 0 - the
    // WM_QUIT result - ends the loop instead of hanging the guest forever.
    win32("GetMessageW", 4, [](Emulator& e) { e.set_result(0); });
    ret0("PeekMessageW", 5);
    ret0("TranslateMessage", 1);
    ret0("DispatchMessageW", 1);
    ret0("TranslateAcceleratorW", 3);
    win32("PostQuitMessage", 1, [](Emulator& e) { e.set_result(0); });
    // A modal dialog cannot be shown, but its procedure still gets WM_INITDIALOG,
    // which is where a dialog does its setup.  The result is IDCANCEL: the
    // dialog was dismissed without a choice, which is true.
    win32("DialogBoxIndirectParamW", 5, [gui](Emulator& e) {
        // A dialog template carries the whole of a program's user interface -
        // its caption and every control's id, class and text.  Reading it out
        // is what makes a dialog-driven guest inspectable at all when there is
        // no screen: otherwise the run says only that a dialog happened.
        std::string caption;
        std::vector<DialogItem> items = parse_dialog_template(e, e.arg_slot(1), caption);
        e.log_call("dialog \"%s\" with %u control(s)", caption.c_str(),
                   static_cast<unsigned>(items.size()));
        uint64_t proc = e.arg_slot(3);
        if (!proc) {
            e.set_result(2);  // IDCANCEL
            return;
        }
        uint64_t hwnd = gui->alloc();
        gui->windows[hwnd].text = caption;
        // The controls have to exist as windows: a dialog procedure reaches its
        // own controls through GetDlgItem, and a null handle there stops a
        // dialog-driven program before it does anything.
        for (const DialogItem& item : items) {
            uint64_t child = gui->alloc();
            GuiState::Window w;
            w.class_name = item.cls;
            w.text = item.text;
            w.parent = hwnd;
            gui->windows[child] = w;
            gui->dialog_items[hwnd][item.id] = child;
            e.log_call("  control id %u  %s  \"%s\"", static_cast<unsigned>(item.id),
                       item.cls.c_str(), item.text.c_str());
        }
        e.call_guest(proc, {hwnd, 0x0110, 0, e.arg_slot(4)});  // WM_INITDIALOG
        // Then whatever the run was told to press.  The notification code is
        // BN_CLICKED (0), so wParam is just the control id.
        for (uint32_t id : e.options().dialog_commands) {
            e.log_call("dialog command %u", static_cast<unsigned>(id));
            e.call_guest(proc, {hwnd, 0x0111, id, 0});  // WM_COMMAND
        }
        e.set_result(2);  // IDCANCEL
    });
    ret1("EndDialog", 2);

    // ---- geometry and appearance --------------------------------------------
    // A window with no display still has a size a program lays itself out
    // against; a plausible one keeps the arithmetic sane.
    auto zero_rect = [](Emulator& e, uint64_t r, int w, int h) {
        if (!r) return;
        e.mem.write32(r + 0, 0);
        e.mem.write32(r + 4, 0);
        e.mem.write32(r + 8, static_cast<uint32_t>(w));
        e.mem.write32(r + 12, static_cast<uint32_t>(h));
    };
    win32("GetClientRect", 2, [zero_rect](Emulator& e) {
        zero_rect(e, e.arg_slot(1), 800, 600);
        e.set_result(1);
    });
    win32("GetWindowRect", 2, [zero_rect](Emulator& e) {
        zero_rect(e, e.arg_slot(1), 800, 600);
        e.set_result(1);
    });
    win32("SetRectEmpty", 1, [zero_rect](Emulator& e) {
        zero_rect(e, e.arg_slot(0), 0, 0);
        e.set_result(1);
    });
    win32("OffsetRect", 3, [](Emulator& e) {
        uint64_t r = e.arg_slot(0);
        int32_t dx = static_cast<int32_t>(e.arg_slot(1));
        int32_t dy = static_cast<int32_t>(e.arg_slot(2));
        if (r) {
            for (int i = 0; i < 16; i += 8) {
                e.mem.write32(r + i, static_cast<uint32_t>(
                                         static_cast<int32_t>(e.mem.read32(r + i)) + dx));
                e.mem.write32(r + i + 4, static_cast<uint32_t>(
                                             static_cast<int32_t>(e.mem.read32(r + i + 4)) + dy));
            }
        }
        e.set_result(1);
    });
    ret1("MapWindowPoints", 4);
    ret1("MoveWindow", 6);
    ret1("SetWindowPos", 7);
    ret1("InvalidateRect", 3);
    ret1("ShowWindow", 2);
    ret1("EnableWindow", 2);
    ret1("IsWindowEnabled", 1);
    ret1("SetForegroundWindow", 1);
    ret0("SetFocus", 1);
    ret0("SetCursor", 1);
    ret1("LoadCursorW", 2);
    ret1("LoadImageW", 6);
    ret0("KillTimer", 2);
    ret1("SetTimer", 4);
    ret0("GetProcessDefaultLayout", 1);
    ret1("SetProcessDefaultLayout", 1);
    // SystemParametersInfo is asked for metrics a program lays out against; the
    // caller's buffer is left as it found it, which reads as "all zero".
    ret1("SystemParametersInfoW", 4);
    win32("GetSystemMetrics", 1, [](Emulator& e) {
        switch (static_cast<uint32_t>(e.arg_slot(0))) {
            case 0: e.set_result(1920); break;   // SM_CXSCREEN
            case 1: e.set_result(1080); break;   // SM_CYSCREEN
            default: e.set_result(0); break;
        }
    });

    // ---- painting ------------------------------------------------------------
    // Every drawing call succeeds and draws nothing.  A device context and a
    // GDI object still have to be distinct non-zero handles, because a program
    // selects one into another and checks the result.
    win32("BeginPaint", 2, [gui](Emulator& e) {
        uint64_t ps = e.pointer_size();
        uint64_t p = e.arg_slot(1);  // PAINTSTRUCT: hdc, fErase, rcPaint, ...
        uint64_t hdc = gui->alloc();
        if (p) {
            e.mem.write_sized(p, ps, hdc);
            e.mem.write32(p + ps, 0);
            for (int i = 0; i < 16; i += 4) e.mem.write32(p + ps + 4 + i, 0);
        }
        e.set_result(hdc);
    });
    ret1("EndPaint", 2);
    win32("GetDC", 1, [gui](Emulator& e) { e.set_result(gui->alloc()); });
    win32("GetWindowDC", 1, [gui](Emulator& e) { e.set_result(gui->alloc()); });
    ret1("ReleaseDC", 2);
    ret1("FillRect", 3);
    ret1("DrawTextW", 5);
    win32("CreateCompatibleDC", 1, [gui](Emulator& e) { e.set_result(gui->alloc()); });
    win32("CreateCompatibleBitmap", 3, [gui](Emulator& e) { e.set_result(gui->alloc()); });
    win32("CreateFontIndirectW", 1, [gui](Emulator& e) { e.set_result(gui->alloc()); });
    win32("CreatePen", 3, [gui](Emulator& e) { e.set_result(gui->alloc()); });
    win32("CreateSolidBrush", 1, [gui](Emulator& e) { e.set_result(gui->alloc()); });
    win32("GetStockObject", 1, [gui](Emulator& e) { e.set_result(gui->alloc()); });
    win32("SelectObject", 2, [gui](Emulator& e) { e.set_result(gui->alloc()); });
    ret1("DeleteDC", 1);
    ret1("DeleteObject", 1);
    ret1("BitBlt", 9);
    ret0("SetBkColor", 2);
    ret0("SetBkMode", 2);
    ret0("SetTextColor", 2);
    ret0("SetLayout", 2);
    ret1("SetStretchBltMode", 2);

    // ---- common controls and dialogs ----------------------------------------
    ret1("InitCommonControlsEx", 1);
    win32("ImageList_Create", 5, [gui](Emulator& e) { e.set_result(gui->alloc()); });
    ret1("ImageList_Destroy", 1);
    ret1("ImageList_Remove", 2);
    // No one can pick a file - but a caller has already written its suggested
    // name into lpstrFile, which is exactly what a person clicking Save without
    // changing anything would accept.  So the dialog "succeeds" with that name
    // when there is one, and reports cancellation when there is not.  Refusing
    // unconditionally means a program that saves anything never saves it.
    auto file_dialog = [](Emulator& e) {
        uint64_t p = e.arg_slot(0);
        if (!p) {
            e.set_result(0);
            return;
        }
        uint64_t ps = e.pointer_size();
        uint64_t file_at = e.is64() ? p + 48 : p + 28;
        uint64_t name = e.mem.read_sized(file_at, ps);
        if (!name || !e.mem.read16(name)) {
            e.log_call("file dialog with no suggested name -> cancelled");
            e.set_result(0);
            return;
        }
        std::string chosen = utf16_to_utf8(e, name, -1);
        // nFileOffset and nFileExtension point into that string; a caller uses
        // them to split the directory from the file.
        size_t slash = chosen.find_last_of("\/");
        size_t dot = chosen.find_last_of('.');
        uint64_t flags_at = e.is64() ? p + 96 : p + 52;
        e.mem.write16(flags_at + 4,
                      static_cast<uint16_t>(slash == std::string::npos ? 0 : slash + 1));
        e.mem.write16(flags_at + 6,
                      static_cast<uint16_t>(dot == std::string::npos ? 0 : dot + 1));
        e.log_call("file dialog -> \"%s\"", chosen.c_str());
        e.set_result(1);
    };
    win32("GetSaveFileNameW", 1, file_dialog);
    win32("GetOpenFileNameW", 1, file_dialog);
    ret0("CommDlgExtendedError", 0);

    // ---- GDI+ ----------------------------------------------------------------
    // Startup succeeds with a token the caller hands back at shutdown.
    win32("GdiplusStartup", 3, [gui](Emulator& e) {
        if (e.arg_slot(0)) e.mem.write_sized(e.arg_slot(0), e.pointer_size(), gui->alloc());
        e.set_result(0);  // Gdiplus::Ok
    });
    win32("GdiplusShutdown", 1, [](Emulator& e) { e.set_result(0); });
}

}  // namespace x86emu
