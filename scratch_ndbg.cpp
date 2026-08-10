// Minimal native debugger: launch a PE, break at tsanpr.dll RVAs, dump the same
// register+memory view the emulator's EMU_REGS prints, so native and emu state
// can be diffed at the same instruction.  Reads the DLL load base from the debug
// event, so it needs no ASLR patch.
//
// usage: ndbg <program> <rva1> [rva2 ...]     (RVAs are hex, into tsanpr.dll)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

static const char* kRegNames[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                     "r8","r9","r10","r11","r12","r13","r14","r15"};

static uint64_t reg_by_index(const CONTEXT& c, int i) {
    switch (i) {
        case 0: return c.Rax; case 1: return c.Rcx; case 2: return c.Rdx; case 3: return c.Rbx;
        case 4: return c.Rsp; case 5: return c.Rbp; case 6: return c.Rsi; case 7: return c.Rdi;
        case 8: return c.R8; case 9: return c.R9; case 10: return c.R10; case 11: return c.R11;
        case 12: return c.R12; case 13: return c.R13; case 14: return c.R14; case 15: return c.R15;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: ndbg <program> <rva> [rva...]\n"
                                         "       ndbg <program> --trace <startRVA> <N>\n"); return 2; }
    std::string program = argv[1];
    std::vector<uint64_t> rvas;
    bool traceMode = false; uint64_t traceStart = 0; uint64_t traceBudget = 20000;
    // --poke <trigRVA> <targetRVA> <dwordVal>: at the first hit of trigRVA, write
    // dwordVal to dllBase+targetRVA, then continue normally (used to force
    // __isa_available so native takes the same ISA path as the emulator).
    bool pokeMode = false; uint64_t pokeTrig = 0, pokeTarget = 0; uint32_t pokeVal = 0;
    if (std::string(argv[2]) == "--trace") {
        traceMode = true;
        traceStart = std::strtoull(argv[3], nullptr, 16);
        if (argc > 4) traceBudget = std::strtoull(argv[4], nullptr, 0);
        // optional: --trace <start> <N> <pokeTargetRVA> <pokeVal> — poke at trace start
        if (argc > 6) { pokeTarget = std::strtoull(argv[5], nullptr, 16);
                        pokeVal = (uint32_t)std::strtoull(argv[6], nullptr, 16); pokeMode = true; }
        rvas.push_back(traceStart);
    } else if (std::string(argv[2]) == "--poke") {
        pokeMode = true;
        pokeTrig = std::strtoull(argv[3], nullptr, 16);
        pokeTarget = std::strtoull(argv[4], nullptr, 16);
        pokeVal = (uint32_t)std::strtoull(argv[5], nullptr, 16);
        rvas.push_back(pokeTrig);
    } else {
        for (int i = 2; i < argc; ++i) rvas.push_back(std::strtoull(argv[i], nullptr, 16));
    }

    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(program.c_str(), nullptr, nullptr, nullptr, FALSE,
                        DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }
    HANDLE hProc = pi.hProcess;

    uint64_t dllBase = 0;
    std::map<uint64_t, uint8_t> orig;                 // absolute addr -> original byte
    std::map<DWORD, uint64_t> pendingRearm;           // thread id -> addr to re-arm
    std::map<uint64_t, int> hitCount;

    auto setBp = [&](uint64_t addr) {
        uint8_t b; SIZE_T n;
        if (!ReadProcessMemory(hProc, (void*)addr, &b, 1, &n)) return;
        orig[addr] = b;
        uint8_t cc = 0xCC;
        WriteProcessMemory(hProc, (void*)addr, &cc, 1, &n);
        FlushInstructionCache(hProc, (void*)addr, 1);
    };

    DEBUG_EVENT ev{};
    bool armed = false;
    int totalHits = 0;
    bool tracing = false;          // trace mode active (single-stepping + logging)
    uint64_t traceLeft = traceBudget;
    uint64_t stepCap = 40000000;   // hard cap on total steps
    uint64_t stepoverBp = 0;       // one-shot bp planted to skip a non-DLL excursion
    uint8_t  stepoverOrig = 0;
    while (WaitForDebugEvent(&ev, INFINITE)) {
        DWORD cont = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
            case LOAD_DLL_DEBUG_EVENT: {
                if (ev.u.LoadDll.hFile) {
                    char path[MAX_PATH] = {0};
                    DWORD r = GetFinalPathNameByHandleA(ev.u.LoadDll.hFile, path, MAX_PATH, 0);
                    std::string p = (r > 0 && r < MAX_PATH) ? path : "";
                    for (auto& ch : p) ch = (char)tolower((unsigned char)ch);
                    if (!armed && p.find("tsanpr.dll") != std::string::npos) {
                        dllBase = (uint64_t)ev.u.LoadDll.lpBaseOfDll;
                        std::fprintf(stderr, "[ndbg] tsanpr.dll base=%llx\n", (unsigned long long)dllBase);
                        for (uint64_t rva : rvas) setBp(dllBase + rva);
                        armed = true;
                    }
                    if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
                }
                break;
            }
            case EXCEPTION_DEBUG_EVENT: {
                const EXCEPTION_RECORD& er = ev.u.Exception.ExceptionRecord;
                DWORD code = er.ExceptionCode;
                DWORD tid = ev.dwThreadId;
                if (code == EXCEPTION_SINGLE_STEP && tracing) {
                    HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
                    CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
                    GetThreadContext(hThread, &ctx);
                    uint64_t rip = ctx.Rip;
                    bool inDll = rip >= dllBase && rip < dllBase + 0x3000000ULL;
                    // Step over non-DLL excursions (kernel/CRT I/O): single-stepping
                    // through them can hit millions of instructions. When execution
                    // leaves the DLL, plant a one-shot breakpoint at the first DLL
                    // return address on the stack and run at full speed back to it.
                    if (!inDll && stepoverBp == 0) {
                        unsigned char sb[0x800]; SIZE_T got = 0; uint64_t ret = 0;
                        if (ReadProcessMemory(hProc,(void*)ctx.Rsp,sb,sizeof(sb),&got))
                            for (SIZE_T k=0;k+8<=got;k+=8){uint64_t q;memcpy(&q,sb+k,8);
                                if(q>=dllBase&&q<dllBase+0x3000000ULL){ret=q;break;}}
                        if (ret) {
                            uint8_t o; SIZE_T n;
                            if (ReadProcessMemory(hProc,(void*)ret,&o,1,&n)) {
                                stepoverOrig=o; stepoverBp=ret; uint8_t cc=0xCC;
                                WriteProcessMemory(hProc,(void*)ret,&cc,1,&n);
                                FlushInstructionCache(hProc,(void*)ret,1);
                                ctx.EFlags &= ~0x100u; SetThreadContext(hThread,&ctx);
                                CloseHandle(hThread);
                                ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
                                continue;
                            }
                        }
                    }
                    if (inDll && traceLeft > 0) {
                        uint64_t r[16] = {ctx.Rax,ctx.Rcx,ctx.Rdx,ctx.Rbx,ctx.Rsp,ctx.Rbp,ctx.Rsi,ctx.Rdi,
                                          ctx.R8,ctx.R9,ctx.R10,ctx.R11,ctx.R12,ctx.R13,ctx.R14,ctx.R15};
                        std::fprintf(stderr, "IT %llx", (unsigned long long)(rip - dllBase));
                        for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %llx", (unsigned long long)r[i]);
                        if (getenv("NDBG_XMM")) {
                            M128A* xh = &ctx.Xmm0;
                            for (int i = 0; i < 16; ++i)
                                std::fprintf(stderr, " x%d=%016llx%016llx", i,
                                             (unsigned long long)xh[i].High, (unsigned long long)xh[i].Low);
                        }
                        std::fprintf(stderr, "\n");
                        // At the wcslen "done" point, also log the measured wide
                        // string by CONTENT (r8=start), for a desync-robust diff.
                        if (rip - dllBase == 0x204b6f3ULL) {
                            uint64_t s = ctx.R8; unsigned char wb[130]={0}; SIZE_T g=0;
                            ReadProcessMemory(hProc,(void*)s,wb,130,&g);
                            std::fprintf(stderr, "WSTR r8=%llx \"", (unsigned long long)s);
                            for (int k=0;k<64;k++){unsigned c=wb[k*2]; if(c==0&&wb[k*2+1]==0)break;
                                std::fprintf(stderr,"%c",(c>=32&&c<127)?(int)c:'.');}
                            std::fprintf(stderr, "\"\n");
                        }
                        if (rip - dllBase == 0x14ed076ULL) {
                            uint64_t s = ctx.Rbx; unsigned char wb[82]={0}; SIZE_T g=0;
                            ReadProcessMemory(hProc,(void*)s,wb,82,&g);
                            std::fprintf(stderr, "STOI rbx=%llx \"", (unsigned long long)s);
                            for (int k=0;k<40;k++){unsigned c=wb[k*2]; if(c==0&&wb[k*2+1]==0)break;
                                std::fprintf(stderr,"%c",(c>=32&&c<127)?(int)c:'.');}
                            std::fprintf(stderr, "\"\n");
                        }
                        --traceLeft;
                    }
                    if (traceLeft > 0 && --stepCap > 0) {
                        ctx.EFlags |= 0x100; SetThreadContext(hThread, &ctx);
                    } else {
                        std::fprintf(stderr, "[ndbg] trace done (left=%llu)\n", (unsigned long long)traceLeft);
                        CloseHandle(hThread); TerminateProcess(hProc, 0);
                        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
                        continue;
                    }
                    CloseHandle(hThread);
                    ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
                    continue;
                }
                if (code == EXCEPTION_BREAKPOINT) {
                    uint64_t addr = (uint64_t)er.ExceptionAddress;
                    // A one-shot step-over breakpoint: we've run back into the DLL
                    // after a non-DLL excursion. Restore the byte and resume tracing.
                    if (tracing && stepoverBp && addr == stepoverBp) {
                        HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
                        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL; GetThreadContext(hThread,&ctx);
                        WriteProcessMemory(hProc,(void*)stepoverBp,&stepoverOrig,1,nullptr);
                        FlushInstructionCache(hProc,(void*)stepoverBp,1);
                        // Log the return-site instruction so the trace stays aligned
                        // with the emulator (which logs it as a normal step).
                        if (traceLeft > 0 && stepoverBp >= dllBase && stepoverBp < dllBase+0x3000000ULL) {
                            uint64_t r[16]={ctx.Rax,ctx.Rcx,ctx.Rdx,ctx.Rbx,ctx.Rsp,ctx.Rbp,ctx.Rsi,ctx.Rdi,
                                            ctx.R8,ctx.R9,ctx.R10,ctx.R11,ctx.R12,ctx.R13,ctx.R14,ctx.R15};
                            std::fprintf(stderr, "IT %llx", (unsigned long long)(stepoverBp - dllBase));
                            for (int i=0;i<16;i++) std::fprintf(stderr," %llx",(unsigned long long)r[i]);
                            std::fprintf(stderr, "\n"); --traceLeft;
                        }
                        ctx.Rip = stepoverBp; ctx.EFlags |= 0x100; SetThreadContext(hThread,&ctx);
                        stepoverBp = 0; CloseHandle(hThread);
                        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
                        continue;
                    }
                    auto it = orig.find(addr);
                    if (traceMode && it != orig.end() && addr == dllBase + traceStart && !tracing) {
                        // Begin tracing here: remove the INT3, back up RIP, single-step.
                        HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
                        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
                        GetThreadContext(hThread, &ctx);
                        if (pokeMode) {
                            uint64_t tgt = dllBase + pokeTarget; uint32_t before = 0; SIZE_T n = 0;
                            ReadProcessMemory(hProc, (void*)tgt, &before, 4, &n);
                            WriteProcessMemory(hProc, (void*)tgt, &pokeVal, 4, &n);
                            std::fprintf(stderr, "[ndbg] poke +%llx: %u -> %u\n",
                                         (unsigned long long)pokeTarget, before, pokeVal);
                        }
                        WriteProcessMemory(hProc, (void*)addr, &it->second, 1, nullptr);
                        FlushInstructionCache(hProc, (void*)addr, 1);
                        ctx.Rip = addr;
                        ctx.EFlags |= 0x100;
                        SetThreadContext(hThread, &ctx);
                        tracing = true;
                        std::fprintf(stderr, "[ndbg] trace start at +%llx\n", (unsigned long long)traceStart);
                        CloseHandle(hThread);
                        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
                        continue;
                    }
                    if (pokeMode && it != orig.end() && addr == dllBase + pokeTrig) {
                        // Poke the target dword, then step over and re-arm this bp
                        // (leave it armed harmlessly; the write is idempotent).
                        uint64_t tgt = dllBase + pokeTarget;
                        uint32_t before = 0; SIZE_T n = 0;
                        ReadProcessMemory(hProc, (void*)tgt, &before, 4, &n);
                        WriteProcessMemory(hProc, (void*)tgt, &pokeVal, 4, &n);
                        std::fprintf(stderr, "[ndbg] poke +%llx: %u -> %u\n",
                                     (unsigned long long)pokeTarget, before, pokeVal);
                        HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
                        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL; GetThreadContext(hThread, &ctx);
                        WriteProcessMemory(hProc, (void*)addr, &it->second, 1, nullptr);
                        FlushInstructionCache(hProc, (void*)addr, 1);
                        ctx.Rip = addr; ctx.EFlags |= 0x100; SetThreadContext(hThread, &ctx);
                        pendingRearm[tid] = addr; CloseHandle(hThread);
                        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
                        continue;
                    }
                    if (it != orig.end()) {
                        // Our breakpoint.  Dump state, then step over and re-arm.
                        HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
                        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
                        GetThreadContext(hThread, &ctx);
                        uint64_t rva = addr - dllBase;
                        int hc = ++hitCount[addr]; ++totalHits;
                        std::fprintf(stderr, "[hit] rva=%llx rip=%llx #%d\n",
                                     (unsigned long long)rva, (unsigned long long)addr, hc);
                        for (int i = 0; i < 16; ++i) {
                            uint64_t v = reg_by_index(ctx, i);
                            std::fprintf(stderr, "  %-3s=%016llx", kRegNames[i], (unsigned long long)v);
                            if (v > 0x10000 && v < 0x7fffffffffffULL) {
                                unsigned char buf[160]; SIZE_T got = 0;
                                if (ReadProcessMemory(hProc, (void*)v, buf, sizeof(buf), &got) && got) {
                                    std::fprintf(stderr, "  N\"");
                                    for (SIZE_T k = 0; k < got; ++k)
                                        std::fprintf(stderr, "%c", (buf[k] >= 32 && buf[k] < 127) ? (int)buf[k] : (buf[k] == 0 ? '#' : '.'));
                                    std::fprintf(stderr, "\"");
                                }
                                // Interpret as MSVC std::string: data@+0/inline, size@+0x10, cap@+0x18
                                uint64_t sz=0, cap=0; SIZE_T g2=0;
                                if (ReadProcessMemory(hProc,(void*)(v+0x10),&sz,8,&g2) &&
                                    ReadProcessMemory(hProc,(void*)(v+0x18),&cap,8,&g2) &&
                                    sz<=cap && cap<0x10000 && sz<0x2000) {
                                    uint64_t data=v; if (cap>15) ReadProcessMemory(hProc,(void*)v,&data,8,&g2);
                                    unsigned char sb[128]={0}; SIZE_T g3=0;
                                    ReadProcessMemory(hProc,(void*)data,sb,sz<127?(SIZE_T)sz:127,&g3);
                                    std::fprintf(stderr,"  str(sz=%llu)=\"",(unsigned long long)sz);
                                    for (SIZE_T k=0;k<g3;k++) std::fprintf(stderr,"%c",(sb[k]>=32&&sb[k]<127)?(int)sb[k]:'.');
                                    std::fprintf(stderr,"\"");
                                }
                            }
                            std::fprintf(stderr, "\n");
                        }
                        if (getenv("NDBG_TREE")) {
                            // Recursive std::string dump from rcx (depth 2), to diff
                            // by CONTENT against the emulator's EMU_TREE.
                            uint64_t root = ctx.Rcx;
                            std::fprintf(stderr, "[tree] rcx=%llx reachable std::strings:\n",(unsigned long long)root);
                            auto tryS = [&](uint64_t o, const char* tag){
                                uint64_t sz=0,cap=0; SIZE_T g=0;
                                if(!ReadProcessMemory(hProc,(void*)(o+0x10),&sz,8,&g)) return;
                                if(!ReadProcessMemory(hProc,(void*)(o+0x18),&cap,8,&g)) return;
                                if(!(sz>=1&&sz<512&&cap>=sz&&cap<0x10000)) return;
                                uint64_t nd=o; if(cap>15) if(!ReadProcessMemory(hProc,(void*)o,&nd,8,&g))return;
                                unsigned char b2[2]={0}; ReadProcessMemory(hProc,(void*)nd,b2,2,&g);
                                bool wide=(b2[1]==0&&b2[0]>=32&&b2[0]<127);
                                unsigned char sb[210]={0}; SIZE_T step=wide?2:1;
                                ReadProcessMemory(hProc,(void*)nd,sb,(SIZE_T)(sz*step<210?sz*step:210),&g);
                                std::fprintf(stderr,"[tree] %s @%llx sz=%llu %s\"",tag,(unsigned long long)o,(unsigned long long)sz,wide?"W":"N");
                                for(uint64_t k=0;k<sz&&k<100;k++){unsigned c=sb[k*step];std::fprintf(stderr,"%c",(c>=32&&c<127)?(int)c:'.');}
                                std::fprintf(stderr,"\"\n");
                            };
                            for(uint64_t off=0;off<0x400;off+=8){
                                uint64_t here=root+off; tryS(here,"obj");
                                uint64_t p=0; SIZE_T g=0;
                                if(ReadProcessMemory(hProc,(void*)here,&p,8,&g) && p>0x10000 && p<0x7fffffffffffULL)
                                    for(uint64_t o2=0;o2<0x200;o2+=8) tryS(p+o2,"ptr");
                            }
                        }
                        // Stack: print any qword that looks like a tsanpr.dll
                        // return address, to reveal the caller chain.
                        {
                            unsigned char sb[0x400]; SIZE_T got = 0;
                            if (ReadProcessMemory(hProc, (void*)ctx.Rsp, sb, sizeof(sb), &got)) {
                                std::fprintf(stderr, "  stack dll returns:");
                                for (SIZE_T k = 0; k + 8 <= got; k += 8) {
                                    uint64_t q; memcpy(&q, sb + k, 8);
                                    if (q >= dllBase && q < dllBase + 0x3000000ULL)
                                        std::fprintf(stderr, " +%llx", (unsigned long long)(q - dllBase));
                                }
                                std::fprintf(stderr, "\n");
                            }
                        }
                        // restore original, back up RIP, single-step to re-arm
                        WriteProcessMemory(hProc, (void*)addr, &it->second, 1, nullptr);
                        FlushInstructionCache(hProc, (void*)addr, 1);
                        ctx.Rip = addr;
                        ctx.EFlags |= 0x100;  // trap flag
                        SetThreadContext(hThread, &ctx);
                        pendingRearm[tid] = addr;
                        CloseHandle(hThread);
                        cont = DBG_CONTINUE;
                    } else {
                        // Initial system breakpoint, or someone else's: continue.
                        cont = DBG_CONTINUE;
                    }
                } else if (code == EXCEPTION_SINGLE_STEP) {
                    auto pr = pendingRearm.find(tid);
                    if (pr != pendingRearm.end()) {
                        uint8_t cc = 0xCC;
                        WriteProcessMemory(hProc, (void*)pr->second, &cc, 1, nullptr);
                        FlushInstructionCache(hProc, (void*)pr->second, 1);
                        pendingRearm.erase(pr);
                        cont = DBG_CONTINUE;
                    } else {
                        cont = DBG_EXCEPTION_NOT_HANDLED;
                    }
                } else {
                    // App's own exceptions (incl. C++ 0xE06D7363): let the app handle them.
                    cont = ev.u.Exception.dwFirstChance ? DBG_EXCEPTION_NOT_HANDLED : DBG_CONTINUE;
                }
                break;
            }
            case OUTPUT_DEBUG_STRING_EVENT:
                cont = DBG_CONTINUE; break;
            case EXIT_PROCESS_DEBUG_EVENT:
                std::fprintf(stderr, "[ndbg] process exited code %lu, total hits=%d\n",
                             ev.u.ExitProcess.dwExitCode, totalHits);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                return 0;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    return 0;
}
