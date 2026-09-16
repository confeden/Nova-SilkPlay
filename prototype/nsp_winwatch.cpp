// nsp_winwatch.cpp — event-driven window-change notifier.

#include "nsp_winwatch.h"

namespace nsp {
namespace {

// State lives here because WinEvent callbacks carry no context pointer.
// All access happens on the thread that calls Start/Stop/TakeDirty and that
// same thread's message pump dispatches the WINEVENT_OUTOFCONTEXT callbacks —
// so plain (non-atomic) variables are correct, no locking is needed.
bool             g_started    = false;
bool             g_dirty      = false;
bool             g_moveSize   = false;
uint64_t         g_eventsSeen = 0;

// One handle per hooked event range.
static constexpr int kNumHooks = 5;
HWINEVENTHOOK    g_hooks[kNumHooks] = {};

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild,
                           DWORD /*idEventThread*/, DWORD /*dwmsEventTime*/) {
    // We only care about whole windows, not controls or the cursor.
    if (!hwnd) return;

    // Show/Hide/Cloaked/Uncloaked: ignore child controls — only top-level
    // windows can be occluders, and child controls fire constantly inside
    // every application (e.g. every list-box row becoming visible).
    if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE ||
        event == EVENT_OBJECT_CLOAKED || event == EVENT_OBJECT_UNCLOAKED) {
        if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
        if (IsWindow(hwnd) && GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    } else {
        // System events (FOREGROUND, MOVESIZE*, MINIMIZE*): Windows always
        // passes OBJID_WINDOW/CHILDID_SELF for these, but guard anyway.
        if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    }

    // Track move/size so the caller can avoid redundant queries mid-drag.
    if (event == EVENT_SYSTEM_MOVESIZESTART) g_moveSize = true;
    else if (event == EVENT_SYSTEM_MOVESIZEEND) g_moveSize = false;

    g_dirty = true;
    ++g_eventsSeen;
}

}  // namespace

// ------------------------------------------------------------- WindowWatch

WindowWatch::~WindowWatch() { Stop(); }

bool WindowWatch::Start() {
    if (g_started) return false;

    // EVENT_OBJECT_LOCATIONCHANGE is intentionally omitted: it fires for
    // OBJID_CURSOR on every mouse move — thousands of times a second with a
    // high-rate mouse — and every one would be marshalled into our render
    // thread's message queue.  The events below already cover every case that
    // can change which windows cover the video rect.
    //
    // WINEVENT_SKIPOWNPROCESS keeps our own overlay showing and hiding from
    // re-triggering the occlusion test in a feedback loop.
    struct Range { UINT lo, hi; };
    static constexpr Range kRanges[kNumHooks] = {
        { EVENT_SYSTEM_FOREGROUND,   EVENT_SYSTEM_FOREGROUND   },
        { EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND },
        { EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND },
        { EVENT_OBJECT_SHOW,         EVENT_OBJECT_HIDE         },
        { EVENT_OBJECT_CLOAKED,      EVENT_OBJECT_UNCLOAKED    },
    };

    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    for (int i = 0; i < kNumHooks; ++i) {
        g_hooks[i] = SetWinEventHook(kRanges[i].lo, kRanges[i].hi,
                                     nullptr,       // hmodWinEventProc: out-of-context
                                     WinEventProc,
                                     0, 0,          // any pid, any thread
                                     flags);
        if (!g_hooks[i]) {
            // Partial install is worse than none — unwind and report failure.
            for (int j = 0; j < i; ++j) {
                UnhookWinEvent(g_hooks[j]);
                g_hooks[j] = nullptr;
            }
            return false;
        }
    }

    g_started    = true;
    g_dirty      = false;
    g_moveSize   = false;
    g_eventsSeen = 0;
    return true;
}

void WindowWatch::Stop() {
    if (!g_started) return;
    for (int i = 0; i < kNumHooks; ++i) {
        if (g_hooks[i]) {
            UnhookWinEvent(g_hooks[i]);
            g_hooks[i] = nullptr;
        }
    }
    g_started  = false;
    g_moveSize = false;
}

bool WindowWatch::TakeDirty() {
    if (!g_dirty) return false;
    g_dirty = false;
    return true;
}

bool WindowWatch::MoveSizeActive() const { return g_moveSize; }

uint64_t WindowWatch::EventsSeen() const { return g_eventsSeen; }

}  // namespace nsp
