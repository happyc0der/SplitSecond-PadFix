// Xbox button names in the game's prompts, in memory. Split/Second fills hint templates ("STEER *ACTION_TURN_L* AND
// TAP *ACTION_BREAK* TO DRIFT") with the bound input's display name from its English text table, and for a controller
// those names are DirectInput's: "Button 1", "Axis Z Pos", "Pov Button 1". The table is one block of NUL-terminated
// strings addressed by offset, so each name can be overwritten in place with a shorter one padded with NULs.
// SplitSecond-ButtonNames does the same to the file; doing it in memory also works where the file stores the table
// compressed (the Steam release) and leaves the game's files untouched.
//
// Memory also holds stale copies of the table in freed blocks, and a freed block can be handed out again at any
// moment. So the search only reads (and allocates nothing, so it can't churn the heap itself), and the writes happen
// with every other thread of the game suspended: each match is checked again while nothing else runs, then
// rewritten, and the threads resume (well under a millisecond). Overwriting a name inside a block that is free at
// that moment only changes dead bytes; nothing can reuse the block between the check and the write.
//
// A background thread does this once a second until the table turns up (about 2 s after start in both builds), then
// checks twice more, 10 s and 60 s later, in case another copy is loaded, and stops: no scanning during races.
#include "common.h"

#include <tlhelp32.h>

#include <cstring>
#include <thread>

namespace padfix {
namespace {

struct Rename {
    const char* from;
    const char* to;
};
// Same list as SplitSecond-ButtonNames. Names assume the wired 360 layout this proxy produces.
const Rename kRenames[] = {
    {"Button 1", "A"},          {"Button 2", "B"},          {"Button 3", "X"},          {"Button 4", "Y"},
    {"Button 5", "LB"},         {"Button 6", "RB"},         {"Button 7", "VIEW"},       {"Button 8", "MENU"},
    {"Button 9", "LS CLICK"},   {"Button 10", "RS CLICK"},  {"Pov Button 1", "D-PAD"},
    {"Axis X Neg", "LS LEFT"},  {"Axis X Pos", "LS RIGHT"}, {"Axis Y Neg", "LS UP"},    {"Axis Y Pos", "LS DOWN"},
    {"Axis Z Pos", "LT"},       {"Axis Z Neg", "RT"},
    {"Right Axis X Neg", "RS LEFT"}, {"Right Axis X Pos", "RS RIGHT"},
    {"Right Axis Y Neg", "RS UP"},   {"Right Axis Y Pos", "RS DOWN"},
    {"RMenu", "RAlt"},
};
constexpr int kCount = static_cast<int>(sizeof(kRenames) / sizeof(kRenames[0]));
// Unchanged by the rename and unique to the input-name part of the table: marks the table in either state.
const char kAnchor[] = "\0Right Axis Z Pos";
constexpr size_t kAnchorLen = sizeof(kAnchor) - 1; // leading NUL + text (the trailing NUL is checked separately)
const char kRenamedMark[] = "\0RS CLICK";
constexpr size_t kRenamedMarkLen = sizeof(kRenamedMark) - 1;
constexpr size_t kWindow = 256 * 1024; // the English table is ~48 KB; search this far either side of the anchor

// Everything the scan uses lives in this DLL's image (never scanned: only private memory is), not on the heap.
char g_needle[kCount][24]; // "\0" + name
size_t g_needleLen[kCount];
struct Candidate {
    char* at; // first character of the name
    int idx;
};
constexpr int kMaxCandidates = 1024;
Candidate g_cand[kMaxCandidates];
int g_nCand = 0;
constexpr int kMaxThreads = 1024;
HANDLE g_threads[kMaxThreads];

size_t FindZ(const char* b, size_t n, const char* pat, size_t m) { // pat starts with NUL; match must end with NUL
    if (n < m + 1) return static_cast<size_t>(-1);
    const char first = pat[1];
    const char* p = b + 1;
    const char* end = b + n - m; // last position of pat[1] that leaves room for the trailing NUL
    while (p <= end) {
        p = static_cast<const char*>(memchr(p, first, static_cast<size_t>(end - p) + 1));
        if (!p) break;
        if (memcmp(p - 1, pat, m) == 0 && p[m - 1] == '\0') return static_cast<size_t>(p - 1 - b);
        ++p;
    }
    return static_cast<size_t>(-1);
}
// Reads run under SEH: the game can decommit memory while this thread reads it.
size_t SafeFindZ(const char* b, size_t n, const char* pat, size_t m) {
    __try {
        return FindZ(b, n, pat, m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<size_t>(-1);
    }
}
// Under suspension: still "\0name\0" at this address? Then write the new name padded with NULs.
bool SafeCheckAndWrite(char* at, int idx) {
    __try {
        if (memcmp(at - 1, g_needle[idx], g_needleLen[idx]) != 0 || at[g_needleLen[idx] - 1] != '\0') return false;
        const char* to = kRenames[idx].to;
        if (g_cfg.swapTriggers && !strcmp(kRenames[idx].from, "Axis Z Pos")) to = "RT";
        else if (g_cfg.swapTriggers && !strcmp(kRenames[idx].from, "Axis Z Neg")) to = "LT";
        size_t len = g_needleLen[idx] - 1, n = strlen(to);
        memcpy(at, to, n);
        memset(at + n, 0, len - n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Scannable(const MEMORY_BASIC_INFORMATION& m) {
    if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE) return false;
    DWORD p = m.Protect;
    if (p & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) return false; // never read GPU/uncached memory
    return (p & 0xFF) == PAGE_READWRITE;
}

// Read-only pass: every copy of the table that still has (most of) its original names adds those names to g_cand.
// Returns the number of such copies; *renamed is set when a copy that already has Xbox names is seen.
int Collect(bool* renamed) {
    g_nCand = 0;
    *renamed = false;
    int copies = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    for (char* p = static_cast<char*>(si.lpMinimumApplicationAddress); p < si.lpMaximumApplicationAddress;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(p, &m, sizeof(m))) break;
        char* base = static_cast<char*>(m.BaseAddress);
        char* next = base + m.RegionSize;
        if (Scannable(m)) {
            size_t off = 0;
            while (off < m.RegionSize) {
                size_t hit = SafeFindZ(base + off, m.RegionSize - off, kAnchor, kAnchorLen);
                if (hit == static_cast<size_t>(-1)) break;
                char* anchor = base + off + hit;
                size_t before = static_cast<size_t>(anchor - base), after = static_cast<size_t>(next - anchor);
                char* wb = anchor - (before < kWindow ? before : kWindow);
                char* we = anchor + (after < kWindow ? after : kWindow);
                size_t wn = static_cast<size_t>(we - wb);
                Candidate local[kCount];
                int found = 0;
                for (int i = 0; i < kCount; ++i) {
                    size_t f = SafeFindZ(wb, wn, g_needle[i], g_needleLen[i]);
                    if (f != static_cast<size_t>(-1)) local[found++] = {wb + f + 1, i};
                }
                if (found >= kCount - 4) { // the input names of the text table, not a stray copy of one string
                    ++copies;
                    for (int k = 0; k < found && g_nCand < kMaxCandidates; ++k) g_cand[g_nCand++] = local[k];
                } else if (found == 0 && SafeFindZ(wb, wn, kRenamedMark, kRenamedMarkLen) != static_cast<size_t>(-1)) {
                    *renamed = true; // e.g. Resident.ark already renamed by SplitSecond-ButtonNames
                }
                off += hit + kAnchorLen;
            }
        }
        p = next;
    }
    return copies;
}

// Suspend every other thread of the process, re-check and rewrite the candidates, resume. Returns names written.
int Commit() {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int nThreads = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok && nThreads < kMaxThreads; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (h) g_threads[nThreads++] = h;
    }
    CloseHandle(snap);
    // From here until every thread is resumed: no heap, no locks, no logging (a suspended thread may hold them).
    for (int i = 0; i < nThreads; ++i) {
        if (SuspendThread(g_threads[i]) == static_cast<DWORD>(-1)) {
            CloseHandle(g_threads[i]);
            g_threads[i] = nullptr;
            continue;
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        GetThreadContext(g_threads[i], &c); // returns once the suspension has actually taken effect
    }
    int written = 0;
    for (int k = 0; k < g_nCand; ++k) written += SafeCheckAndWrite(g_cand[k].at, g_cand[k].idx);
    for (int i = 0; i < nThreads; ++i) {
        if (!g_threads[i]) continue;
        ResumeThread(g_threads[i]);
        CloseHandle(g_threads[i]);
    }
    return written;
}

void Watch() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (int i = 0; i < kCount; ++i) {
        g_needle[i][0] = '\0';
        strcpy_s(g_needle[i] + 1, sizeof(g_needle[i]) - 1, kRenames[i].from);
        g_needleLen[i] = 1 + strlen(kRenames[i].from);
    }
    const DWORD start = GetTickCount();
    int total = 0, checksAfter = 0;
    for (;;) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed > 3 * 60 * 1000) {
            if (!total) Log("[names] text table not found within 3 minutes; prompts keep the game's names");
            else Log("[names] done");
            return;
        }
        DWORD t0 = GetTickCount();
        bool renamed = false;
        int copies = Collect(&renamed);
        if (copies) {
            DWORD scanMs = GetTickCount() - t0;
            int written = Commit();
            Log("[names] %.1f s after start: %d cop%s of the text table, %d names renamed (scan %lu ms; writes done with "
                "the game's threads paused)", elapsed / 1000.0, copies, copies == 1 ? "y" : "ies", written, scanMs);
            if (written > 0) total += written;
        } else if (renamed && !total) {
            Log("[names] the text table already has Xbox names (renamed in the game files); nothing to do");
            return;
        }
        if (total) {
            if (++checksAfter > 2) {
                Log("[names] done");
                return;
            }
            Sleep(checksAfter == 1 ? 10000 : 50000);
        } else {
            Sleep(1000);
        }
    }
}

} // namespace

void StartNameWatcher() {
    static bool started = false;
    if (started || !g_cfg.xboxNames) return;
    started = true;
    std::thread(Watch).detach();
}

} // namespace padfix
