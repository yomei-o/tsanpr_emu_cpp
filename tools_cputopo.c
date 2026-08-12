// What does GetLogicalProcessorInformationEx actually hand a guest?
//
// The hook that answers it writes SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX blocks
// into guest memory by hand, and the caller walks them by their own Size field.
// A block of the wrong length, or GroupCount at the wrong offset, is not a
// compile error and not a fault - it is onnxruntime reading a plausible number
// from the wrong place, which is the shape of every bug the licence bring-up
// found.  So this asks the guest side what it sees, and it can be run under the
// emulator on any host and natively on Windows to compare.
//
//   x86_64-w64-mingw32-gcc -O2 -o cputopo.exe tools_cputopo.c
//   ./x86emu cputopo.exe          # the hook's answer
//   cputopo.exe                   # a real Windows machine's answer
#include <windows.h>
#include <stdio.h>

static const char* rel_name(int r) {
    switch (r) {
        case RelationProcessorCore: return "ProcessorCore";
        case RelationNumaNode: return "NumaNode";
        case RelationCache: return "Cache";
        case RelationProcessorPackage: return "ProcessorPackage";
        case RelationGroup: return "Group";
        default: return "other";
    }
}

static void walk(LOGICAL_PROCESSOR_RELATIONSHIP want, const char* label) {
    DWORD len = 0;
    if (GetLogicalProcessorInformationEx(want, NULL, &len)) {
        printf("%-14s size query succeeded with len=%lu (expected failure)\n", label, len);
    }
    DWORD err = GetLastError();
    if (len == 0) {
        printf("%-14s nothing reported (len=0, err=%lu)\n", label, err);
        return;
    }
    if (err != ERROR_INSUFFICIENT_BUFFER)
        printf("%-14s note: size query set err=%lu, not 122\n", label, err);

    BYTE* buf = (BYTE*)malloc(len);
    DWORD have = len;
    if (!GetLogicalProcessorInformationEx(want, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &have)) {
        printf("%-14s fetch failed, err=%lu\n", label, GetLastError());
        free(buf);
        return;
    }
    printf("%-14s %lu bytes\n", label, have);

    DWORD off = 0;
    int n = 0;
    while (off < have) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* p =
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(buf + off);
        if (p->Size == 0 || off + p->Size > have) {
            printf("  [%d] +%lu BROKEN: Size=%lu, %lu bytes left\n",
                   n, off, p->Size, have - off);
            break;
        }
        printf("  [%d] +%-3lu %-16s Size=%-3lu", n, off, rel_name(p->Relationship), p->Size);
        switch (p->Relationship) {
            case RelationProcessorCore:
            case RelationProcessorPackage:
                printf("  flags=%u efficiency=%u groups=%u mask=0x%llx group=%u",
                       p->Processor.Flags, p->Processor.EfficiencyClass,
                       p->Processor.GroupCount,
                       (unsigned long long)p->Processor.GroupMask[0].Mask,
                       p->Processor.GroupMask[0].Group);
                break;
            case RelationNumaNode:
                printf("  node=%lu mask=0x%llx group=%u", p->NumaNode.NodeNumber,
                       (unsigned long long)p->NumaNode.GroupMask.Mask,
                       p->NumaNode.GroupMask.Group);
                break;
            case RelationCache:
                printf("  level=%u type=%d line=%u size=%lu", p->Cache.Level,
                       (int)p->Cache.Type, p->Cache.LineSize, p->Cache.CacheSize);
                break;
            case RelationGroup:
                printf("  maxgroups=%u active=%u  cpus max=%u active=%u mask=0x%llx",
                       p->Group.MaximumGroupCount, p->Group.ActiveGroupCount,
                       p->Group.GroupInfo[0].MaximumProcessorCount,
                       p->Group.GroupInfo[0].ActiveProcessorCount,
                       (unsigned long long)p->Group.GroupInfo[0].ActiveProcessorMask);
                break;
            default:
                break;
        }
        printf("\n");
        off += p->Size;
        n++;
    }
    if (off != have) printf("  walked %lu of %lu bytes\n", off, have);
    free(buf);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    walk(RelationAll, "RelationAll");
    walk(RelationProcessorCore, "Core");
    walk(RelationNumaNode, "NumaNode");
    walk(RelationProcessorPackage, "Package");
    walk(RelationGroup, "Group");
    walk(RelationCache, "Cache");

    DWORD highest = 0xdeadbeef;
    if (GetNumaHighestNodeNumber(&highest)) printf("highest numa node = %lu\n", highest);
    printf("GetActiveProcessorCount(ALL) = %lu\n",
           GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("dwNumberOfProcessors = %lu\n", si.dwNumberOfProcessors);
    return 0;
}
