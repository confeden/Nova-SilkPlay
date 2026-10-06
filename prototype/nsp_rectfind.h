// nsp_rectfind.h - the video rect of a WINDOWED browser, found from outside (P11, D30).
//
// No extension and no page channel (D30), and UI Automation does not expose Chrome's web
// content to an outside client at all (measured 2026-10-06: the tree stops at the render
// widget, 0 elements below it). What DOES see the video is DWM: every capture of the
// browser window carries the rects that changed since the last one, and a playing video
// damages exactly its own rect, frame after frame. MEASURED on windowed YouTube on the
// 1080p secondary (tools/yt-check/windowed_probe.py, 2026-10-06):
//
//   default layout   [18,171 1087x455] 138-150 times in 8 s; the page's own rect for the
//                    <video> rounded out to whole pixels
//   scrolled 300 px  [18,96 1087x200]  - the visible part of the element, clipped by the
//                    viewport, again exactly
//   theater mode     [54,158 1460x611] 107-149 times, and a four-rect ring around it (the
//                    player's black surround) 68-93 times
//   dark theme       ambient mode adds [0,95 1380x762] - a rect CONTAINING the video - 93
//                    times in 8 s, ~12 Hz; the exact video rect still arrives on its own
//                    136-138 times
//   a scroll         the whole window, once
//
// So the rule is: the video is the LARGEST rect that arrives as a capture's whole damage,
// on its own, many times a second. Other patterns that keep repeating while it does (the
// ambient glow, the theater ring) are learned as companions and are not a reason to stop.
// Anything else is a foreign change: the page may have moved under the overlay, so the
// overlay hides on that very capture and comes back only after the video rect has been
// seen again on its own (I10: fail closed on uncertain geometry).
//
// Pure logic, no Windows calls beyond RECT: tested by tests/rectfind_test.cpp.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace nsp {

// One capture's damage, in WGC SURFACE coordinates (origin = the window's extended frame
// bounds). More than kMaxRects rects is kept as "overflow": its shape is never a companion
// and never the video.
struct DamageSig {
    static constexpr int kMaxRects = 8;
    int n = 0;
    RECT r[kMaxRects]{};
    bool overflow = false;

    bool Same(const DamageSig& o) const;  // the exact same rects in the same order
    RECT Bounds() const;
    std::string Str() const;              // "[x,y wxh] ..." for logs
};

struct RectFinderConfig {
    double windowSec = 1.0;      // the hit count is taken over this much history
    int    minHits = 8;          // standalone arrivals in windowSec (a 24p video gives 12-18)
    double minAgeSec = 0.5;      // and the rect must have been arriving at least this long
    double maxSilenceSec = 0.25; // ... and still be arriving: its last hit no older than this
    // (a 133 ms CSS transition gives 8 hits too, once; review 2026-10-06)
    LONG   minW = 256, minH = 144;
    double minAspect = 0.3, maxAspect = 8.0;  // w/h: vertical shorts to a video half scrolled away
    int    companionHits = 3;    // a repeating non-video shape in companionSec ...
    double companionSec = 2.0;
    double maxCompanionViewportFrac = 0.90;  // ... that does not cover (almost) the whole surface
};

// SCOUTING: fed every capture of the whole window, says when one rect is the video.
class RectFinder {
public:
    explicit RectFinder(RectFinderConfig cfg = {}) : cfg_(cfg) {}
    // `viewport`: the page area in surface coordinates (Chrome's render widget), or empty.
    void Reset(LONG surfaceW, LONG surfaceH, const RECT& viewport = RECT{});
    void Add(double tSec, const DamageSig& d);
    // The video rect (surface coordinates) and the companions learned alongside it.
    bool Found(double tSec, RECT* video, std::vector<DamageSig>* companions) const;
    size_t HistorySize() const { return hist_.size(); }
    // The most frequent whole-capture rects of the last windowSec, for a log line.
    std::string Summary(double tSec) const;

private:
    struct Entry {
        double t;
        DamageSig d;
    };
    bool Plausible(const RECT& r) const;
    RectFinderConfig cfg_;
    LONG surfW_ = 0, surfH_ = 0;
    RECT viewport_{};
    std::vector<Entry> hist_;  // the last companionSec of captures, oldest first
};

enum class GeomVerdict {
    kExact,      // the damage is the video rect itself: the geometry is confirmed
    kInside,     // within the video rect (player UI, part of the picture): neutral
    kCompanion,  // a learned companion shape: neutral
    kElsewhere,  // nowhere near the video (hover, content below it): neutral
    kPending,    // a smaller foreign shape: decided when the video's own rect follows, or not
    kForeign,    // touches the video and is none of the above: the page may have moved
};

struct GeometryGuardConfig {
    LONG   exactTolPx = 2;        // edges within this of the video rect count as the rect
    int    confirmExact = 3;      // exact captures needed to clear a suspicion ...
    double confirmQuietSec = 0.10;  // ... with no foreign capture or input for this long
    // A foreign capture that covers this much of the window is a scroll, a resize or a page
    // change: hide on it. A smaller one (a player's surround repainting, a glow) is tolerated
    // if the video's own rect arrives within foreignGraceSec, and a shape tolerated
    // learnAfter times becomes a companion. MEASURED (2026-10-06): a wheel scroll damaged
    // [1,95 1584x797] of a 1586x893 surface (89 %); YouTube's theater surround [1,157
    // 1568x613] (68 %) every ~330 ms; the ambient glow (74 %) ~12 times a second.
    double immediateSurfaceFrac = 0.85;   // of the surface, when the page area is unknown
    double immediateViewportFrac = 0.95;  // of the page area: a scroll repaints all of it
    double foreignGraceSec = 0.10;
    int    learnAfter = 2;
    double noExactSuspectSec = 0.40;  // captures keep coming but none is the video rect
    double giveUpSec = 0.6;       // suspect this long under foreign damage: scout again
    double idleGiveUpSec = 10.0;  // suspect and no video rect for this long: release the device
};

// ENGAGED: fed every capture while the overlay may show; decides whether it may.
class GeometryGuard {
public:
    explicit GeometryGuard(GeometryGuardConfig cfg = {}) : cfg_(cfg) {}
    // Starts SUSPECT: between the find and this call the page was not watched (an engage
    // takes hundreds of ms), so the overlay shows only after the rect is confirmed again.
    void Begin(double tSec, const RECT& video, const std::vector<DamageSig>& companions,
               LONG surfaceW, LONG surfaceH, const RECT& viewport = RECT{});
    GeomVerdict Add(double tSec, const DamageSig& d);
    // The user did something that can move the page (wheel, click, key on the browser).
    void OnInput(double tSec);
    void Tick(double tSec);  // time-based rules: no exact capture for too long
    bool Suspect() const { return suspect_; }
    bool GiveUp(double tSec) const;
    const RECT& Video() const { return video_; }
    const char* LastReason() const { return reason_; }
    const DamageSig& LastForeign() const { return lastForeign_; }

private:
    void MakeSuspect(double tSec, const char* why);
    GeometryGuardConfig cfg_;
    RECT video_{};
    std::vector<DamageSig> companions_;
    LONG surfW_ = 0, surfH_ = 0;
    RECT viewport_{};
    bool pending_ = false;        // a small foreign capture waiting for the video rect
    double pendingSince_ = 0.0;
    DamageSig pendingSig_;
    std::vector<std::pair<DamageSig, int>> tolerated_;
    bool suspect_ = false;
    double suspectSince_ = 0.0;
    double lastDisturb_ = -1e9;  // last foreign capture or input
    double lastExact_ = 0.0;
    double lastCapture_ = 0.0;
    int exactSinceDisturb_ = 0;
    bool foreignSinceSuspect_ = false;
    DamageSig lastForeign_;
    const char* reason_ = "";
};

}  // namespace nsp
