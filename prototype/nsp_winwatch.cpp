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
// A SECOND, independent dirty flag for EVENT_SYSTEM_FOREGROUND only. The game gate
// needs to know the foreground changed, and it must not consume g_dirty: that one
// belongs to the occlusion test, and a shared flag would let whichever consumer
// polled first swallow the other's notification.
bool             g_fgDirty    = false;
bool             g_moveSize   = false;
// WHO is being dragged and SINCE WHEN. MOVESIZEEND is posted by the window
// being moved, so a window destroyed mid-drag - a crashed browser, a display
// unplugged, a session switch - never sends it. The latch then stayed true for
// the rest of the run, and the re-engage guard in main.cpp reads it: frame
// generation never came back until the program was restarted, silently (beta
// audit, 2026-09-20). A drag nobody ended is over.
HWND             g_moveSizeHwnd = nullptr;
int64_t          g_moveSizeQpc = 0;
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
    if (event == EVENT_SYSTEM_MOVESIZESTART) {
        g_moveSize = true;
        g_moveSizeHwnd = hwnd;
        g_moveSizeQpc = QpcNow();
    } else if (event == EVENT_SYSTEM_MOVESIZEEND) {
        g_moveSize = false;
        g_moveSizeHwnd = nullptr;
    }

    // No new hook: EVENT_SYSTEM_FOREGROUND is already the first range in kRanges,
    // and WINEVENT_SKIPOWNPROCESS keeps our own Settings window out of it.
    if (event == EVENT_SYSTEM_FOREGROUND) g_fgDirty = true;

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
    g_fgDirty    = false;
    g_moveSize   = false;
    g_moveSizeHwnd = nullptr;
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
    g_fgDirty  = false;
    g_moveSize = false;
    g_moveSizeHwnd = nullptr;
}

bool WindowWatch::TakeDirty() {
    if (!g_dirty) return false;
    g_dirty = false;
    return true;
}

bool WindowWatch::TakeForegroundChanged() {
    if (!g_fgDirty) return false;
    g_fgDirty = false;
    return true;
}

bool WindowWatch::MoveSizeActive() const {
    if (!g_moveSize) return false;
    // Five seconds is far longer than any drag the caller cares about - it only
    // uses this to raise the occlusion cadence and to hold off a re-engage - and
    // far shorter than "for ever", which is what the bug was.
    if (!IsWindow(g_moveSizeHwnd) || QpcToSec(QpcNow() - g_moveSizeQpc) > 5.0) {
        Log("window watch: a move/size that began %.1f s ago never ended%s - clearing it",
            QpcToSec(QpcNow() - g_moveSizeQpc),
            IsWindow(g_moveSizeHwnd) ? "" : " (its window is gone)");
        g_moveSize = false;
        g_moveSizeHwnd = nullptr;
        return false;
    }
    return true;
}

uint64_t WindowWatch::EventsSeen() const { return g_eventsSeen; }

}  // namespace nsp
