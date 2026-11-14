#include "nsp_rectfind.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

namespace nsp {
namespace {

LONG W(const RECT& r) { return r.right - r.left; }
LONG H(const RECT& r) { return r.bottom - r.top; }

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool Near(const RECT& a, const RECT& b, LONG tol) {
    return std::abs(a.left - b.left) <= tol && std::abs(a.top - b.top) <= tol &&
           std::abs(a.right - b.right) <= tol && std::abs(a.bottom - b.bottom) <= tol;
}

bool Inside(const RECT& in, const RECT& out, LONG tol) {
    return in.left >= out.left - tol && in.top >= out.top - tol && in.right <= out.right + tol &&
           in.bottom <= out.bottom + tol;
}

bool Touches(const RECT& a, const RECT& b, LONG margin) {
    return a.left < b.right + margin && b.left - margin < a.right && a.top < b.bottom + margin &&
           b.top - margin < a.bottom;
}

using RectKey = std::tuple<LONG, LONG, LONG, LONG>;
RectKey Key(const RECT& r) { return {r.left, r.top, r.right, r.bottom}; }

std::vector<std::pair<int, LONG>> SplitLines(const DamageSig& raw, const RECT& bounds) {
    std::vector<std::pair<int, LONG>> lines;
    for (int axis = 0; axis < 2; ++axis) {
        const LONG low = axis ? bounds.left : bounds.top;
        const LONG high = axis ? bounds.right : bounds.bottom;
        for (int i = 0; i < raw.n; ++i) {
            if (!Inside(raw.r[i], bounds, 0)) continue;
            const LONG edges[2] = {axis ? raw.r[i].left : raw.r[i].top,
                                   axis ? raw.r[i].right : raw.r[i].bottom};
            for (const LONG edge : edges) {
                if (edge <= low || edge >= high) continue;
                bool crossed = false;
                for (int j = 0; j < raw.n; ++j) {
                    const RECT& r = raw.r[j];
                    if (!Inside(r, bounds, 0)) continue;
                    crossed = crossed || (axis ? r.left < edge && edge < r.right
                                              : r.top < edge && edge < r.bottom);
                }
                const auto line = std::make_pair(axis, edge);
                if (!crossed && std::find(lines.begin(), lines.end(), line) == lines.end())
                    lines.push_back(line);
            }
        }
    }
    return lines;
}

}  // namespace

bool DamageSig::Same(const DamageSig& o) const {
    if (overflow || o.overflow || n != o.n) return false;
    for (int i = 0; i < n; ++i)
        if (!SameRect(r[i], o.r[i])) return false;
    return true;
}

DamageSig DamageSig::Normalized() const {
    DamageSig out = *this;
    if (overflow) return out;
    // Chrome can split one video repaint into horizontal/vertical strips beside an
    // unrelated animation. Counting a strip as the video leaves a seam and the guard
    // never sees it again. Join only exact rectangular unions, with no added pixels.
    bool merged;
    do {
        merged = false;
        for (int i = 0; i < out.n && !merged; ++i) {
            for (int j = i + 1; j < out.n; ++j) {
                const RECT a = out.r[i], b = out.r[j];
                const bool vertical = a.left == b.left && a.right == b.right &&
                                      a.top <= b.bottom && b.top <= a.bottom;
                const bool horizontal = a.top == b.top && a.bottom == b.bottom &&
                                        a.left <= b.right && b.left <= a.right;
                if (!vertical && !horizontal) continue;
                out.r[i] = RECT{(std::min)(a.left, b.left), (std::min)(a.top, b.top),
                                (std::max)(a.right, b.right), (std::max)(a.bottom, b.bottom)};
                for (int k = j + 1; k < out.n; ++k) out.r[k - 1] = out.r[k];
                --out.n;
                merged = true;
                break;
            }
        }
    } while (merged);
    return out;
}

RECT DamageSig::Bounds() const {
    if (n <= 0) return RECT{};
    RECT b = r[0];
    for (int i = 1; i < n; ++i) {
        b.left = (std::min)(b.left, r[i].left);
        b.top = (std::min)(b.top, r[i].top);
        b.right = (std::max)(b.right, r[i].right);
        b.bottom = (std::max)(b.bottom, r[i].bottom);
    }
    return b;
}

std::string DamageSig::Str() const {
    std::string out;
    char buf[64];
    for (int i = 0; i < n; ++i) {
        snprintf(buf, sizeof(buf), "%s[%ld,%ld %ldx%ld]", i ? " " : "", r[i].left, r[i].top, W(r[i]),
                 H(r[i]));
        out += buf;
    }
    if (overflow) out += " +more";
    return out.empty() ? std::string("(none)") : out;
}

// ------------------------------------------------------------------ RectFinder

std::string RectFinder::Summary(double tSec) const {
    std::map<std::string, int> counts;
    for (const auto& e : hist_)
        if (e.t >= tSec - cfg_.windowSec) ++counts[e.d.Str()];
    std::vector<std::pair<int, std::string>> v;
    for (const auto& kv : counts) v.push_back({kv.second, kv.first});
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string out;
    for (size_t i = 0; i < v.size() && i < 4; ++i)
        out += (i ? ", " : "") + std::to_string(v[i].first) + "x " + v[i].second;
    return out.empty() ? std::string("no damage") : out;
}

void RectFinder::Reset(LONG surfaceW, LONG surfaceH, const RECT& viewport, double expectedAspect,
                       const RECT& reference) {
    surfW_ = surfaceW;
    surfH_ = surfaceH;
    viewport_ = viewport;
    expectedAspect_ = expectedAspect;
    reference_ = reference;
    hist_.clear();
}

void RectFinder::Add(double tSec, const DamageSig& raw) {
    const DamageSig d = raw.Normalized();
    if (d.n <= 0 && !d.overflow) return;  // a capture with no damage says nothing
    hist_.push_back({tSec, d, raw});
    const double keep = (std::max)(cfg_.companionSec, cfg_.windowSec + cfg_.minAgeSec);
    size_t drop = 0;
    while (drop < hist_.size() && hist_[drop].t < tSec - keep) ++drop;
    if (drop) hist_.erase(hist_.begin(), hist_.begin() + static_cast<std::ptrdiff_t>(drop));
}

bool RectFinder::Plausible(const RECT& r) const {
    const LONG w = W(r), h = H(r);
    if (w < cfg_.minW || h < cfg_.minH) return false;
    const double aspect = static_cast<double>(w) / h;
    if (aspect < cfg_.minAspect || aspect > cfg_.maxAspect) return false;
    // A fixed page header can leave a plausible inner rect after scrolling.
    if (expectedAspect_ > 0.0) {
        const double expectedH = static_cast<double>(w) / expectedAspect_;
        const bool centredFormat = W(reference_) > 0 && std::abs(w - W(reference_)) <= 2 &&
            std::abs((r.left + r.right) - (reference_.left + reference_.right)) <= 4 &&
            std::abs((r.top + r.bottom) - (reference_.top + reference_.bottom)) <= 4;
        if (!centredFormat && std::abs(static_cast<double>(h) - expectedH) > (std::max)(3.0, 0.005 * expectedH))
            return false;
    }
    // The whole window repainting is a scroll, a resize or a page load, never a video in a
    // window; a video that fills the window is the fullscreen path's business.
    if (surfW_ > 0 && surfH_ > 0 && w >= surfW_ - 8 && h >= surfH_ - 8) return false;
    // Nor is the whole PAGE repainting: a layout settling after a load or a move repaints the
    // viewport many times a second. MEASURED: [1,95 1584x797], the render widget exactly, was
    // once taken for the video while YouTube laid itself out (2026-10-06).
    const LONG vw = W(viewport_), vh = H(viewport_);
    if (vw > 0 && vh > 0 &&
        static_cast<double>(w) * h >= 0.9 * static_cast<double>(vw) * vh)
        return false;
    // A vertically clipped repaint does not establish the video's geometry. Alloha can
    // report an enlarged player/surround union ending at the page's bottom after a failed
    // fullscreen transition (1132x676 on a 2560x1392 window). Accepting it puts the readout
    // outside the picture. Wait until both vertical edges are in view (I10); horizontal
    // edge-to-edge theater layouts remain eligible.
    if (vw > 0 && vh > 0) {
        const bool cutV = r.top <= viewport_.top + 1 || r.bottom >= viewport_.bottom - 1;
        if (cutV) return false;
    }
    return true;
}

bool RectFinder::Found(double tSec, RECT* video, std::vector<DamageSig>* companions) const {
    struct Hit {
        int recent = 0;      // hits in the last windowSec
        int lastHalf = 0;    // hits in the last half of it
        double first = 0.0;  // the earliest hit kept
        double last = 0.0;   // the latest
        RECT r{};
        std::map<std::pair<int, LONG>, int> splits;
        bool SplitEvidence(int witnesses) const {
            for (const auto& split : splits)
                if (recent - split.second < witnesses) return false;
            return true;
        }
    };
    std::map<RectKey, Hit> hits;
    for (const auto& e : hist_) {
        if (e.d.overflow) continue;
        // A rect counts when it is a capture's damage ON ITS OWN: alone, or beside rects that
        // do not touch it (a tab strip repaint in the same frame does not make the picture any
        // less the picture). A rect overlapping another is part of a union and counts for none.
        for (int i = 0; i < e.d.n; ++i) {
            bool apart = true;
            for (int j = 0; j < e.d.n; ++j)
                apart = apart && (j == i || !Touches(e.d.r[i], e.d.r[j], 0));
            if (!apart) continue;
            auto& h = hits[Key(e.d.r[i])];
            if (h.recent == 0 && h.first == 0.0) {
                h.first = e.t;
                h.r = e.d.r[i];
            }
            if (e.t >= tSec - cfg_.windowSec) {
                ++h.recent;
                // Stable full-edge subdivisions can be independent animations,
                // not one video. Require repeated captures crossing each split.
                for (const auto& split : SplitLines(e.raw, e.d.r[i])) ++h.splits[split];
            }
            if (e.t >= tSec - cfg_.windowSec * 0.5) ++h.lastHalf;
            h.last = (std::max)(h.last, e.t);
        }
    }
    std::vector<const Hit*> cands;
    for (const auto& kv : hits) {
        const Hit& h = kv.second;
        // Steady, not a burst: still arriving, and in both halves of the window.
        if (h.recent >= cfg_.minHits && h.first <= tSec - cfg_.minAgeSec &&
            h.last >= tSec - cfg_.maxSilenceSec && h.lastHalf * 4 >= cfg_.minHits &&
            (h.recent - h.lastHalf) * 4 >= cfg_.minHits && Plausible(h.r) &&
            h.SplitEvidence(cfg_.minSplitWitnesses))
            cands.push_back(&h);
    }
    const Hit* best = nullptr;
    std::vector<const Hit*> inner;  // pictures found inside a surround, at half the hits
    for (const Hit* hp : cands) {
        const Hit& h = *hp;
        // A frequent rect that CONTAINS another frequent one is the video's surround, not the
        // video: YouTube's ambient glow arrives on its own ~12 times a second in the dark
        // theme, is bigger than the picture and would otherwise win on area.
        // The inner rect need not be a full candidate itself: when the glow repaints in the same
        // compositor frame as the picture, Chrome reports their union, and the picture's own
        // rect gets through on its own only now and then. Half the hits are enough - but only
        // for a rect at least a third the area, so a panel inside a video (an end screen) can
        // never make the video look like a surround: a seam inside the picture is far worse
        // than generating over a static glow (D22).
        bool surround = false;
        for (const auto& kv : hits) {
            const Hit& o = kv.second;
            if (&o == hp || SameRect(o.r, h.r) || !Inside(o.r, h.r, 0)) continue;
            if (o.recent * 2 < cfg_.minHits || !Plausible(o.r) ||
                !o.SplitEvidence(cfg_.minSplitWitnesses)) continue;
            if (static_cast<double>(W(o.r)) * H(o.r) < 0.35 * static_cast<double>(W(h.r)) * H(h.r))
                continue;
            surround = true;
            if (o.first <= tSec - cfg_.minAgeSec && o.last >= tSec - 1.5 * cfg_.maxSilenceSec)
                inner.push_back(&o);
        }
        if (surround) continue;
        if (!best || static_cast<long long>(W(h.r)) * H(h.r) >
                         static_cast<long long>(W(best->r)) * H(best->r))
            best = &h;
    }
    for (const Hit* hp : inner)
        if (!best || static_cast<long long>(W(hp->r)) * H(hp->r) >
                         static_cast<long long>(W(best->r)) * H(best->r))
            best = hp;
    if (!best) return false;
    if (video) *video = best->r;

    if (companions) {
        companions->clear();
        const double surfArea = static_cast<double>(surfW_) * surfH_;
        for (size_t i = 0; i < hist_.size(); ++i) {
            const DamageSig& d = hist_[i].d;
            if (hist_[i].t < tSec - cfg_.companionSec || d.overflow || d.n <= 0) continue;
            if (d.n == 1 && SameRect(d.r[0], best->r)) continue;
            // Within the video, or nowhere near it: never a reason to hide, nothing to learn.
            bool inside = true, touches = false;
            for (int k = 0; k < d.n; ++k) {
                inside = inside && Inside(d.r[k], best->r, 2);
                touches = touches || Touches(d.r[k], best->r, 2);
            }
            if (inside || !touches) continue;
            const RECT b = d.Bounds();
            // A shape covering (almost) the whole surface is what a scroll looks like. Learning
            // it would blind the guard to exactly the change it exists for.
            if (surfArea > 0.0 &&
                static_cast<double>(W(b)) * H(b) > cfg_.maxCompanionViewportFrac * surfArea)
                continue;
            bool known = false;
            for (const auto& c : *companions) known = known || c.Same(d);
            if (known) continue;
            int count = 0;
            for (size_t j = i; j < hist_.size(); ++j)
                if (hist_[j].t >= tSec - cfg_.companionSec && hist_[j].d.Same(d)) ++count;
            if (count >= cfg_.companionHits) companions->push_back(d);
        }
    }
    return true;
}

// --------------------------------------------------------------- GeometryGuard

void GeometryGuard::Begin(double tSec, const RECT& video, const std::vector<DamageSig>& companions,
                          LONG surfaceW, LONG surfaceH, const RECT& viewport) {
    video_ = video;
    companions_ = companions;
    surfW_ = surfaceW;
    surfH_ = surfaceH;
    viewport_ = viewport;
    pending_ = false;
    tolerated_.clear();
    suspect_ = true;
    suspectSince_ = tSec;
    lastDisturb_ = tSec;
    lastExact_ = tSec;
    lastCapture_ = tSec;
    exactSinceDisturb_ = 0;
    foreignSinceSuspect_ = false;
    reason_ = "confirming the video rect after engaging";
}

void GeometryGuard::MakeSuspect(double tSec, const char* why) {
    lastDisturb_ = tSec;
    exactSinceDisturb_ = 0;
    if (!suspect_) {
        suspect_ = true;
        suspectSince_ = tSec;
        foreignSinceSuspect_ = false;
        reason_ = why;
    }
}

GeomVerdict GeometryGuard::Add(double tSec, const DamageSig& raw) {
    const DamageSig d = raw.Normalized();
    if (d.n <= 0 && !d.overflow) return GeomVerdict::kInside;  // no damage: nothing moved
    lastCapture_ = tSec;
    GeomVerdict v = GeomVerdict::kForeign;
    DamageSig rel;  // only the rects that reach the video
    if (!d.overflow) {
        // PER RECT. Chrome hands a frame's damage over as a few rects, and a rect nowhere
        // near the video says nothing about it: on the owner's Chrome a 358x40 repaint in the
        // tab strip (the title's unread count) arrived in the same frame as the picture every
        // few seconds, and judged as one shape it hid the overlay each time (2026-10-07).
        bool inside = true, exact = false;
        for (int k = 0; k < d.n; ++k) {
            // Dirty rects are half-open: sharing an edge repaints no video pixels.
            if (!Touches(d.r[k], video_, 0)) continue;
            rel.r[rel.n++] = d.r[k];
            inside = inside && Inside(d.r[k], video_, cfg_.exactTolPx);
            exact = exact || Near(d.r[k], video_, cfg_.exactTolPx);
        }
        const bool touches = rel.n > 0;
        if (touches && inside && exact) {
            v = GeomVerdict::kExact;
        } else if (touches && inside) {
            v = GeomVerdict::kInside;
        } else if (!touches) {
            // Damage that does not reach the video cannot mean the video moved: anything that
            // moves it repaints where it was. Hover effects, a description unfolding below,
            // comments loading - none of them is a reason to hide.
            v = GeomVerdict::kElsewhere;
        } else {
            for (const auto& c : companions_)
                if (c.Same(d) || c.Same(rel)) v = GeomVerdict::kCompanion;
        }
    }
    if (v == GeomVerdict::kForeign && !d.overflow) {
        const RECT b = rel.Bounds();
        const double area = static_cast<double>(W(b)) * H(b);
        const LONG vw = W(viewport_), vh = H(viewport_);
        // Against the PAGE area when it is known: a scroll repaints all of it, and the share of
        // the whole window that is depends on the toolbars and the window's height (review).
        const bool minor =
            (vw > 0 && vh > 0) ? area < cfg_.immediateViewportFrac * static_cast<double>(vw) * vh
            : (surfW_ > 0 && surfH_ > 0)
                ? area < cfg_.immediateSurfaceFrac * static_cast<double>(surfW_) * surfH_
                : false;
        if (minor) {
            // Small enough to be a surround repainting: wait for the video's own rect.
            if (!pending_) {
                pending_ = true;
                pendingSince_ = tSec;
                pendingSig_ = d;
            }
            lastCapture_ = tSec;
            return GeomVerdict::kPending;
        }
    }
    switch (v) {
        case GeomVerdict::kExact:
            if (pending_) {
                pending_ = false;
                if (tSec - pendingSince_ <= cfg_.foreignGraceSec) {
                    bool known = false;
                    for (auto& t : tolerated_) {
                        if (t.first.Same(pendingSig_)) {
                            known = true;
                            if (++t.second == cfg_.learnAfter && companions_.size() < 16)
                                companions_.push_back(pendingSig_);
                        }
                    }
                    if (!known) tolerated_.push_back({pendingSig_, 1});
                    if (cfg_.learnAfter <= 1 && !known && companions_.size() < 16)
                        companions_.push_back(pendingSig_);
                } else {
                    lastForeign_ = pendingSig_;
                    MakeSuspect(pendingSince_, "the page changed around the video");
                    foreignSinceSuspect_ = true;
                }
            }
            lastExact_ = tSec;
            ++exactSinceDisturb_;
            if (suspect_ && exactSinceDisturb_ >= cfg_.confirmExact &&
                tSec - lastDisturb_ >= cfg_.confirmQuietSec)
                suspect_ = false;
            break;
        case GeomVerdict::kForeign:
            lastForeign_ = d;
            MakeSuspect(tSec, "the page changed around the video");
            foreignSinceSuspect_ = true;
            break;
        default:
            break;
    }
    return v;
}

void GeometryGuard::OnInput(double tSec) { MakeSuspect(tSec, "input that can scroll the page"); }

void GeometryGuard::Tick(double tSec) {
    if (pending_ && tSec - pendingSince_ > cfg_.foreignGraceSec) {
        // The video's own rect did not follow: whatever repainted around it, it may have moved.
        pending_ = false;
        lastForeign_ = pendingSig_;
        MakeSuspect(pendingSince_, "the page changed around the video");
        foreignSinceSuspect_ = true;
    }
    if (suspect_) {
        if (exactSinceDisturb_ >= cfg_.confirmExact && tSec - lastDisturb_ >= cfg_.confirmQuietSec)
            suspect_ = false;
        return;
    }
    // Captures keep coming but none of them is the video's own rect: whatever is changing,
    // it is no longer the picture we think we cover.
    if (tSec - lastCapture_ < 0.1 && tSec - lastExact_ > cfg_.noExactSuspectSec) {
        MakeSuspect(tSec, "the video rect stopped arriving on its own");
        foreignSinceSuspect_ = true;
    }
}

bool GeometryGuard::GiveUp(double tSec) const {
    if (!suspect_ || tSec - suspectSince_ <= cfg_.giveUpSec) return false;
    // Foreign damage keeps arriving and the rect never came back: the layout changed.
    if (foreignSinceSuspect_ && tSec - lastExact_ > cfg_.giveUpSec) return true;
    // Nothing at all for a long time (scrolled away, paused): release the device.
    return tSec - lastExact_ > cfg_.idleGiveUpSec;
}

}  // namespace nsp
