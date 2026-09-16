// nsp_winwatch.h — event-driven window-change notifier.
//
// WindowWatch installs WinEvent hooks on the calling thread so the occlusion
// test (VideoRectObscured) runs the moment something in the z-order changes,
// rather than on a fixed slow poll.  The caller keeps calling TakeDirty() from
// its message-pump loop and re-runs the test whenever it returns true.
#pragma once

#include "nsp_common.h"

#include <cstdint>

namespace nsp {

class WindowWatch {
public:
    WindowWatch() = default;
    ~WindowWatch();  // calls Stop()

    WindowWatch(const WindowWatch&) = delete;
    WindowWatch& operator=(const WindowWatch&) = delete;

    // Installs the hooks on the CALLING thread, which must pump messages.
    // Returns false if any hook failed (nothing stays installed in that case)
    // or if a WindowWatch is already started on this thread.
    bool Start();

    // Uninstalls all hooks.  Safe to call more than once.
    void Stop();

    // Returns true if a relevant WinEvent arrived since the last call; clears
    // the flag.  Out-of-context hooks are dispatched from the same thread that
    // calls TakeDirty (the message pump), so no locking is needed.
    bool TakeDirty();

    // True between EVENT_SYSTEM_MOVESIZESTART and EVENT_SYSTEM_MOVESIZEEND. A
    // window being dragged moves without raising any event we hook, so the caller
    // re-runs the test on every iteration while this is true.
    bool MoveSizeActive() const;

    // Total relevant events accepted since Start().
    uint64_t EventsSeen() const;
};

}  // namespace nsp
