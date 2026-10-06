// nsp_mediawatch.h - is the browser playing media? Asked of Windows, not of the page (D30).
//
// Chrome publishes every playing <video>/<audio> with sound to the system media transport
// controls (the media flyout by the volume indicator). The session manager answers "is a
// Chrome session Playing" for any process to ask, without touching the browser. It gates the
// one costly thing the windowed path does while nothing is engaged: capturing the browser
// window to find the video in it (P11). Browsing with nothing playing captures nothing.
//
// A muted-by-the-page autoplay preview publishes no session, which is the right answer too:
// it is not what the user is watching.
#pragma once
#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

namespace nsp {

enum class MediaState {
    kUnknown,  // not asked yet, or the session manager is unavailable: callers decide
    kIdle,     // no browser session, or one that is paused/stopped
    kPlaying,  // a browser session reports Playing
};

const char* MediaStateName(MediaState s);

class MediaWatch {
public:
    MediaWatch();
    ~MediaWatch();
    MediaWatch(const MediaWatch&) = delete;
    MediaWatch& operator=(const MediaWatch&) = delete;

    // `appIdSub`: case-insensitive substring of the session's AppUserModelId ("chrome").
    void Start(const std::wstring& appIdSub);
    void Stop();
    MediaState State() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nsp
