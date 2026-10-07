// rectfind_test.cpp - nsp_rectfind on the damage patterns measured on windowed YouTube
// (tools/yt-check/windowed_probe.py, 1080p secondary, 2026-10-06). Surface 1586x893.

#include <cstdio>
#include <vector>

#include "nsp_rectfind.h"

using namespace nsp;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("  FAIL line %d: %s\n", __LINE__, #c);                \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

static DamageSig Sig(std::initializer_list<RECT> rs) {
    DamageSig d;
    for (const RECT& r : rs) {
        if (d.n == DamageSig::kMaxRects) {
            d.overflow = true;
            break;
        }
        d.r[d.n++] = r;
    }
    return d;
}
static RECT R(LONG x, LONG y, LONG w, LONG h) { return RECT{x, y, x + w, y + h}; }
static bool Eq(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

static const RECT kVideo = R(19, 171, 1087, 455);   // default layout (surface coords)
static const RECT kGlow = R(1, 95, 1380, 762);      // ambient mode, dark theme
static const RECT kWhole = R(0, 0, 1586, 893);      // a scroll
static const LONG kSW = 1586, kSH = 893;

// 24p video at 1/24 s, ambient glow interleaved at ~12 Hz.
static void FeedPlayback(RectFinder* f, double t0, double sec, bool glow) {
    int i = 0;
    for (double t = t0; t < t0 + sec; t += 1.0 / 24.0, ++i) {
        if (glow && (i % 2) == 1)
            f->Add(t, Sig({kGlow}));
        else
            f->Add(t, Sig({kVideo}));
    }
}

static void TestFindsVideo() {
    printf("test1_finds_video_default_layout\n");
    RectFinder f;
    f.Reset(kSW, kSH);
    FeedPlayback(&f, 0.0, 0.4, false);
    RECT v{};
    CHECK(!f.Found(0.4, &v, nullptr));  // too young
    FeedPlayback(&f, 0.4, 1.0, false);
    std::vector<DamageSig> comp;
    CHECK(f.Found(1.4, &v, &comp));
    CHECK(Eq(v, kVideo));
    CHECK(comp.empty());
}

static void TestAmbientCompanion() {
    printf("test2_ambient_glow_is_learned_as_companion\n");
    RectFinder f;
    f.Reset(kSW, kSH);
    FeedPlayback(&f, 0.0, 1.6, true);
    RECT v{};
    std::vector<DamageSig> comp;
    CHECK(f.Found(1.6, &v, &comp));
    CHECK(Eq(v, kVideo));  // the glow arrives alone 12 times a second and is bigger: still not it
    CHECK(comp.size() == 1 && comp[0].n == 1 && Eq(comp[0].r[0], kGlow));
}

static void TestTheaterRing() {
    printf("test3_theater_ring\n");
    const RECT tv = R(55, 158, 1460, 611);
    const DamageSig ring = Sig({R(1, 157, 1568, 1), R(1, 158, 54, 611), R(1515, 158, 54, 611),
                                R(1, 769, 1568, 1)});
    RectFinder f;
    f.Reset(kSW, kSH);
    int i = 0;
    for (double t = 0; t < 1.6; t += 1.0 / 24.0, ++i) f.Add(t, (i % 3 == 2) ? ring : Sig({tv}));
    RECT v{};
    std::vector<DamageSig> comp;
    CHECK(f.Found(1.6, &v, &comp));
    CHECK(Eq(v, tv));
    CHECK(comp.size() == 1 && comp[0].Same(ring));
}

static void TestNoVideo() {
    printf("test4_no_video_static_page_and_small_animation\n");
    RectFinder f;
    f.Reset(kSW, kSH);
    // A 48x48 spinner at 60 Hz, and a scroll now and then.
    for (double t = 0; t < 2.0; t += 1.0 / 60.0) f.Add(t, Sig({R(700, 400, 48, 48)}));
    f.Add(1.0, Sig({kWhole}));
    RECT v{};
    CHECK(!f.Found(2.0, &v, nullptr));
    // Whole-window repaints at a video's rate are not a video in a window either.
    RectFinder g;
    g.Reset(kSW, kSH);
    for (double t = 0; t < 2.0; t += 1.0 / 24.0) g.Add(t, Sig({kWhole}));
    CHECK(!g.Found(2.0, &v, nullptr));
}

static void TestLargestWins() {
    printf("test5_two_videos_largest_wins\n");
    RectFinder f;
    f.Reset(kSW, kSH);
    const RECT minor = R(1200, 200, 320, 180);
    int i = 0;
    for (double t = 0; t < 1.6; t += 1.0 / 48.0, ++i) f.Add(t, Sig({(i % 2) ? minor : kVideo}));
    RECT v{};
    CHECK(f.Found(1.6, &v, nullptr));
    CHECK(Eq(v, kVideo));
}

static void TestScrollNotCompanion() {
    printf("test6_whole_window_never_a_companion\n");
    RectFinder f;
    f.Reset(kSW, kSH);
    int i = 0;
    for (double t = 0; t < 1.6; t += 1.0 / 24.0, ++i) f.Add(t, Sig({(i % 4 == 3) ? kWhole : kVideo}));
    RECT v{};
    std::vector<DamageSig> comp;
    CHECK(f.Found(1.6, &v, &comp));
    CHECK(comp.empty());
}

static void TestGuard() {
    printf("test7_guard_scroll_hides_and_reconfirms\n");
    GeometryGuard g;
    std::vector<DamageSig> comp{Sig({kGlow})};
    g.Begin(0.0, kVideo, comp, kSW, kSH);
    double t = 0.0;
    for (int i = 0; i < 12; ++i, t += 1.0 / 24.0) {
        CHECK(g.Add(t, Sig({i % 2 ? kGlow : kVideo})) != GeomVerdict::kForeign);
        g.Tick(t);
    }
    CHECK(!g.Suspect());
    // The controls fading in: inside the video. A hover far away: elsewhere.
    CHECK(g.Add(t, Sig({R(19, 560, 1087, 66)})) == GeomVerdict::kInside);
    CHECK(g.Add(t, Sig({R(1200, 300, 300, 80)})) == GeomVerdict::kElsewhere);
    CHECK(!g.Suspect());
    // A scroll: the whole window. Suspect at once.
    t += 0.01;
    CHECK(g.Add(t, Sig({kWhole})) == GeomVerdict::kForeign);
    CHECK(g.Suspect());
    // The video scrolled 75 px: its new rect is foreign too, again and again -> give up.
    const RECT moved = R(19, 96, 1087, 455);
    for (int i = 0; i < 60; ++i) {
        t += 1.0 / 24.0;
        CHECK(g.Add(t, Sig({moved})) != GeomVerdict::kExact);
        g.Tick(t);
    }
    CHECK(g.Suspect());
    CHECK(g.GiveUp(t));
}

static void TestGuardInputThenSame() {
    printf("test8_guard_input_preempts_then_clears_on_three_exact\n");
    GeometryGuard g;
    g.Begin(0.0, kVideo, {}, kSW, kSH);
    g.OnInput(0.5);
    CHECK(g.Suspect());
    g.Add(0.52, Sig({kVideo}));
    g.Add(0.56, Sig({kVideo}));
    g.Tick(0.57);
    CHECK(g.Suspect());  // two is not three
    g.Add(0.61, Sig({kVideo}));
    g.Tick(0.61);
    CHECK(!g.Suspect());  // three, and 110 ms quiet
    CHECK(!g.GiveUp(0.61));
}

static void TestGuardNoExact() {
    printf("test9_guard_companions_alone_for_too_long\n");
    GeometryGuard g;
    std::vector<DamageSig> comp{Sig({kGlow})};
    g.Begin(0.0, kVideo, comp, kSW, kSH);
    double t = 0.0;
    for (int i = 0; i < 20; ++i, t += 1.0 / 24.0) {
        g.Add(t, Sig({kGlow}));
        g.Tick(t);
    }
    CHECK(g.Suspect());  // 0.8 s of glow and never the video itself
}

static void TestGuardIdle() {
    printf("test10_guard_idle_give_up_only_after_long\n");
    GeometryGuard g;
    g.Begin(0.0, kVideo, {}, kSW, kSH);
    g.OnInput(1.0);
    g.Tick(5.0);
    CHECK(g.Suspect() && !g.GiveUp(5.0));  // paused after a key: hold the engagement
    CHECK(g.GiveUp(11.5));
}

static void TestTheaterSurroundTolerated() {
    printf("test12_theater_surround_tolerated_then_learned\n");
    const RECT tv = R(55, 158, 1460, 611);
    const DamageSig surround = Sig({R(1, 157, 1568, 613)});  // 68 % of the surface, ~3 Hz
    GeometryGuard g;
    g.Begin(0.0, tv, {}, kSW, kSH);
    double t = 0.0;
    int pending = 0;
    for (int i = 0; i < 72; ++i, t += 1.0 / 24.0) {
        const GeomVerdict v = g.Add(t, (i % 8 == 4) ? surround : Sig({tv}));
        if (v == GeomVerdict::kPending) ++pending;
        g.Tick(t + 0.001);
        if (i >= 4) CHECK(!g.Suspect());  // the first three exact captures confirm the rect
    }
    CHECK(pending == 2);  // the first two waited for the video rect; then it was a companion
}

static void TestScrollImmediate() {
    printf("test13_viewport_scroll_is_immediate\n");
    GeometryGuard g;
    g.Begin(0.0, kVideo, {}, kSW, kSH);
    CHECK(g.Add(0.5, Sig({R(1, 95, 1584, 797)})) == GeomVerdict::kForeign);  // 89 %
    CHECK(g.Suspect());
}

static void TestSmallForeignNoVideoFollows() {
    printf("test14_small_foreign_without_video_rect_suspects_after_grace\n");
    GeometryGuard g;
    g.Begin(0.3, kVideo, {}, kSW, kSH);
    CHECK(g.Suspect());  // starts unconfirmed: the page was not watched during the engage
    g.Add(0.32, Sig({kVideo}));
    g.Add(0.36, Sig({kVideo}));
    g.Add(0.41, Sig({kVideo}));
    g.Tick(0.42);
    CHECK(!g.Suspect());
    CHECK(g.Add(0.5, Sig({R(10, 150, 1200, 520)})) == GeomVerdict::kPending);
    g.Tick(0.55);
    CHECK(!g.Suspect());
    g.Tick(0.62);
    CHECK(g.Suspect());
}

static void TestViewportRejected() {
    printf("test15_page_relayout_is_not_a_video\n");
    const RECT vp = R(1, 95, 1584, 797);
    RectFinder f;
    f.Reset(kSW, kSH, vp);
    for (double t = 0; t < 1.6; t += 1.0 / 30.0) f.Add(t, Sig({vp}));
    RECT v{};
    CHECK(!f.Found(1.6, &v, nullptr));
}

static void TestGlowMostlyUnioned() {
    printf("test16_glow_unioned_with_the_picture_most_frames\n");
    // The glow repaints with three picture frames in four: the picture alone only 6 times a
    // second, below minHits, yet the glow must not be taken for the video.
    RectFinder f;
    f.Reset(kSW, kSH);
    int i = 0;
    for (double t = 0; t < 1.6; t += 1.0 / 24.0, ++i) f.Add(t, Sig({(i % 4 == 0) ? kVideo : kGlow}));
    RECT v{};
    CHECK(f.Found(1.6, &v, nullptr));
    CHECK(Eq(v, kVideo));
}

static void TestEndScreenPanel() {
    printf("test17_panel_inside_the_video_does_not_unseat_it\n");
    const RECT panel = R(200, 250, 500, 250);  // 0.25 of the video
    RectFinder f;
    f.Reset(kSW, kSH);
    int i = 0;
    for (double t = 0; t < 1.6; t += 1.0 / 48.0, ++i) f.Add(t, Sig({(i % 2) ? panel : kVideo}));
    RECT v{};
    CHECK(f.Found(1.6, &v, nullptr));
    CHECK(Eq(v, kVideo));
}

static void TestCornerClippedUnion() {
    printf("test18_union_clipped_at_the_viewport_corner_is_refused\n");
    const RECT vp = R(1, 95, 1584, 797);
    const RECT unionGlow = R(1, 95, 1321, 778);  // measured on the local glow page
    RectFinder f;
    f.Reset(kSW, kSH, vp);
    for (double t = 0; t < 1.6; t += 1.0 / 19.0) f.Add(t, Sig({unionGlow}));
    RECT v{};
    CHECK(!f.Found(1.6, &v, nullptr));
    // A video scrolled half out at the TOP only is still a video.
    RectFinder g;
    g.Reset(kSW, kSH, vp);
    const RECT half = R(44, 95, 1058, 285);
    for (double t = 0; t < 1.6; t += 1.0 / 24.0) g.Add(t, Sig({half}));
    CHECK(g.Found(1.6, &v, nullptr));
    CHECK(Eq(v, half));
}

static void TestBurstIsNotAVideo() {
    printf("test19_a_short_transition_is_not_a_video\n");
    // A 133 ms CSS transition at 60 Hz: 8 hits, then nothing.
    RectFinder f;
    f.Reset(kSW, kSH);
    for (int i = 0; i < 8; ++i) f.Add(0.2 + i / 60.0, Sig({kVideo}));
    RECT v{};
    CHECK(!f.Found(0.75, &v, nullptr));
    CHECK(!f.Found(1.1, &v, nullptr));
}

static void TestViewportImmediate() {
    printf("test20_scroll_judged_against_the_page_area\n");
    // A short window with bookmarks: the page area is 70 % of the surface, and a scroll
    // repaints all of it - still immediate.
    const RECT vp = R(1, 140, 1584, 625);
    GeometryGuard g;
    g.Begin(0.0, R(19, 200, 900, 380), {}, kSW, kSH, vp);
    CHECK(g.Add(0.5, Sig({vp})) == GeomVerdict::kForeign);
}

static void TestTabStripBeside() {
    printf("test21_tab_strip_repaint_beside_the_picture\n");
    const DamageSig both = Sig({R(1842, 50, 358, 40), kVideo});
    GeometryGuard g;
    g.Begin(0.0, kVideo, {}, kSW, kSH);
    CHECK(g.Add(0.1, both) == GeomVerdict::kExact);
    RectFinder f;
    f.Reset(kSW, kSH);
    int i = 0;
    for (double t = 0; t < 1.6; t += 1.0 / 24.0, ++i) f.Add(t, (i % 2) ? both : Sig({kVideo}));
    RECT v{};
    CHECK(f.Found(1.6, &v, nullptr));
    CHECK(Eq(v, kVideo));
}

static void TestOverflow() {
    printf("test11_overflow_is_foreign\n");
    GeometryGuard g;
    g.Begin(0.0, kVideo, {}, kSW, kSH);
    DamageSig d = Sig({R(0, 0, 10, 10), R(20, 20, 10, 10), R(40, 40, 10, 10), R(60, 60, 10, 10),
                       R(80, 80, 10, 10), R(100, 100, 10, 10), R(120, 120, 10, 10),
                       R(140, 140, 10, 10), R(160, 160, 10, 10)});
    CHECK(d.overflow);
    CHECK(g.Add(0.1, d) == GeomVerdict::kForeign);
}

int main() {
    TestFindsVideo();
    TestAmbientCompanion();
    TestTheaterRing();
    TestNoVideo();
    TestLargestWins();
    TestScrollNotCompanion();
    TestGuard();
    TestGuardInputThenSame();
    TestGuardNoExact();
    TestGuardIdle();
    TestOverflow();
    TestTabStripBeside();
    TestBurstIsNotAVideo();
    TestViewportImmediate();
    TestCornerClippedUnion();
    TestGlowMostlyUnioned();
    TestEndScreenPanel();
    TestViewportRejected();
    TestTheaterSurroundTolerated();
    TestScrollImmediate();
    TestSmallForeignNoVideoFollows();
    printf("\nResults: %s (%d failed checks)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
