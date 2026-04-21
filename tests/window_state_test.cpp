#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "window_state.h"
#include <cmath>

using namespace window_state;

// ---------------------------------------------------------------------------
// to_physical / to_logical
// ---------------------------------------------------------------------------

TEST_CASE("to_physical scale=1 is identity") {
    auto ps = to_physical(LogicalSize{1280, 720}, 1.0f);
    CHECK(ps.w == 1280);
    CHECK(ps.h == 720);
}

TEST_CASE("to_physical scale=2 doubles dimensions") {
    auto ps = to_physical(LogicalSize{640, 360}, 2.0f);
    CHECK(ps.w == 1280);
    CHECK(ps.h == 720);
}

TEST_CASE("to_logical scale=2 halves dimensions") {
    auto ls = to_logical(PhysicalSize{1280, 720}, 2.0f);
    CHECK(ls.w == 640);
    CHECK(ls.h == 360);
}

TEST_CASE("to_physical scale=0 clamps to 1") {
    auto ps = to_physical(LogicalSize{100, 50}, 0.0f);
    CHECK(ps.w == 100);
    CHECK(ps.h == 50);
}

TEST_CASE("to_logical scale=0 clamps to 1") {
    auto ls = to_logical(PhysicalSize{100, 50}, 0.0f);
    CHECK(ls.w == 100);
    CHECK(ls.h == 50);
}

TEST_CASE("to_logical round-trips through to_physical within 1px") {
    LogicalSize orig{100, 200};
    auto ps = to_physical(orig, 1.5f);
    auto ls = to_logical(ps, 1.5f);
    CHECK(std::abs(ls.w - orig.w) <= 1);
    CHECK(std::abs(ls.h - orig.h) <= 1);
}

// ---------------------------------------------------------------------------
// initial_geometry
// ---------------------------------------------------------------------------

TEST_CASE("initial_geometry no saved data returns physical defaults") {
    Settings::WindowGeometry saved{};
    auto g = initial_geometry(saved, nullptr);
    CHECK(g.size.w == Settings::WindowGeometry::kDefaultPhysicalWidth);
    CHECK(g.size.h == Settings::WindowGeometry::kDefaultPhysicalHeight);
    CHECK(g.has_position == false);
    CHECK(g.maximized == false);
}

TEST_CASE("initial_geometry valid saved size returned as-is") {
    Settings::WindowGeometry saved{};
    saved.width = 1920; saved.height = 1080;
    auto g = initial_geometry(saved, nullptr);
    CHECK(g.size.w == 1920);
    CHECK(g.size.h == 1080);
}

TEST_CASE("initial_geometry negative x sets has_position=false") {
    Settings::WindowGeometry saved{};
    saved.width = 1280; saved.height = 720;
    saved.x = -1; saved.y = 100;
    auto g = initial_geometry(saved, nullptr);
    CHECK(g.has_position == false);
}

TEST_CASE("initial_geometry negative y sets has_position=false") {
    Settings::WindowGeometry saved{};
    saved.width = 1280; saved.height = 720;
    saved.x = 100; saved.y = -1;
    auto g = initial_geometry(saved, nullptr);
    CHECK(g.has_position == false);
}

TEST_CASE("initial_geometry valid position sets has_position=true") {
    Settings::WindowGeometry saved{};
    saved.width = 1280; saved.height = 720;
    saved.x = 50; saved.y = 80;
    auto g = initial_geometry(saved, nullptr);
    CHECK(g.has_position == true);
    CHECK(g.position.x == 50);
    CHECK(g.position.y == 80);
}

TEST_CASE("initial_geometry maximized flag propagated") {
    Settings::WindowGeometry saved{};
    saved.width = 1280; saved.height = 720;
    saved.maximized = true;
    auto g = initial_geometry(saved, nullptr);
    CHECK(g.maximized == true);
}

TEST_CASE("initial_geometry zero-width saved falls back to defaults") {
    Settings::WindowGeometry saved{};
    saved.width = 0; saved.height = 720;
    auto g = initial_geometry(saved, nullptr);
    CHECK(g.size.w == Settings::WindowGeometry::kDefaultPhysicalWidth);
    CHECK(g.size.h == Settings::WindowGeometry::kDefaultPhysicalHeight);
}

TEST_CASE("initial_geometry zero-height saved falls back to defaults") {
    Settings::WindowGeometry saved{};
    saved.width = 1280; saved.height = 0;
    auto g = initial_geometry(saved, nullptr);
    CHECK(g.size.w == Settings::WindowGeometry::kDefaultPhysicalWidth);
    CHECK(g.size.h == Settings::WindowGeometry::kDefaultPhysicalHeight);
}

TEST_CASE("initial_geometry clamp_fn is called when provided") {
    Settings::WindowGeometry saved{};
    saved.width = 1280; saved.height = 720;
    bool called = false;
    auto clamp = [&](int* w, int* h, int* x, int* y) {
        called = true;
        *w = 800; *h = 600; *x = -1; *y = -1;
    };
    auto g = initial_geometry(saved, clamp);
    CHECK(called == true);
    CHECK(g.size.w == 800);
    CHECK(g.size.h == 600);
    CHECK(g.has_position == false);
}

TEST_CASE("initial_geometry clamp_fn nullptr is safe") {
    Settings::WindowGeometry saved{};
    saved.width = 1280; saved.height = 720;
    auto g = initial_geometry(saved, nullptr);
    CHECK(g.size.w == 1280);
}

// ---------------------------------------------------------------------------
// corrected_size_for_scale
// ---------------------------------------------------------------------------

TEST_CASE("corrected_size_for_scale same scale returns nullopt") {
    Settings::WindowGeometry saved{};
    saved.scale = 1.0f;
    saved.logical_width = 1280; saved.logical_height = 720;
    CHECK(corrected_size_for_scale(saved, 1.0).has_value() == false);
}

TEST_CASE("corrected_size_for_scale scale change above threshold returns resized size") {
    Settings::WindowGeometry saved{};
    saved.scale = 1.0f;
    saved.logical_width = 1280; saved.logical_height = 720;
    auto r = corrected_size_for_scale(saved, 2.0);
    REQUIRE(r.has_value());
    CHECK(r->w == 2560);
    CHECK(r->h == 1440);
}

TEST_CASE("corrected_size_for_scale scale change below 0.01 threshold returns nullopt") {
    Settings::WindowGeometry saved{};
    saved.scale = 1.0f;
    saved.logical_width = 1280; saved.logical_height = 720;
    CHECK(corrected_size_for_scale(saved, 1.005).has_value() == false);
}

TEST_CASE("corrected_size_for_scale live_scale=0 returns nullopt") {
    Settings::WindowGeometry saved{};
    saved.scale = 1.0f;
    saved.logical_width = 1280; saved.logical_height = 720;
    CHECK(corrected_size_for_scale(saved, 0.0).has_value() == false);
}

TEST_CASE("corrected_size_for_scale saved.scale=0 uses kDefaultScale as reference") {
    Settings::WindowGeometry saved{};
    saved.scale = 0.0f;
    saved.logical_width = 1280; saved.logical_height = 720;
    auto r = corrected_size_for_scale(saved, 2.0);
    REQUIRE(r.has_value());
    CHECK(r->w == 2560);
    CHECK(r->h == 1440);
}

TEST_CASE("corrected_size_for_scale absent saved logical dims uses defaults") {
    Settings::WindowGeometry saved{};
    saved.scale = 1.0f;
    saved.logical_width = 0; saved.logical_height = 0;
    auto r = corrected_size_for_scale(saved, 2.0);
    REQUIRE(r.has_value());
    CHECK(r->w == Settings::WindowGeometry::kDefaultLogicalWidth  * 2);
    CHECK(r->h == Settings::WindowGeometry::kDefaultLogicalHeight * 2);
}

TEST_CASE("corrected_size_for_scale result is lround(logical * live_scale)") {
    Settings::WindowGeometry saved{};
    saved.scale = 1.0f;
    saved.logical_width = 100; saved.logical_height = 75;
    // 75 * 1.5 = 112.5 -> lround = 113
    auto r = corrected_size_for_scale(saved, 1.5);
    REQUIRE(r.has_value());
    CHECK(r->w == 150);
    CHECK(r->h == 113);
}

// ---------------------------------------------------------------------------
// save_geometry
// ---------------------------------------------------------------------------

TEST_CASE("save_geometry fullscreen preserves saved size and sets maximized from latch") {
    Settings::WindowGeometry prev{};
    prev.width = 1280; prev.height = 720;
    prev.logical_width = 1280; prev.logical_height = 720;
    prev.scale = 1.0f; prev.maximized = false;
    prev.x = 50; prev.y = 60;

    SaveInputs in{};
    in.fullscreen = true; in.maximized = false;
    in.was_maximized_before_fullscreen = true;
    in.window_size = {1920, 1080}; in.osd_fallback = {1920, 1080};
    in.scale = 1.0f; in.query_position = nullptr;

    auto r = save_geometry(prev, in);
    CHECK(r.width  == 1280);
    CHECK(r.height == 720);
    CHECK(r.x == 50);
    CHECK(r.y == 60);
    CHECK(r.scale == doctest::Approx(1.0f));
    CHECK(r.maximized == true);  // from was_maximized_before_fullscreen latch
}

TEST_CASE("save_geometry fullscreen wins when both fullscreen and maximized true") {
    Settings::WindowGeometry prev{};
    prev.width = 1280; prev.height = 720; prev.maximized = false;

    SaveInputs in{};
    in.fullscreen = true; in.maximized = true;
    in.was_maximized_before_fullscreen = false;
    in.window_size = {1920, 1080}; in.osd_fallback = {1920, 1080};
    in.scale = 1.0f; in.query_position = nullptr;

    auto r = save_geometry(prev, in);
    CHECK(r.width  == 1280);
    CHECK(r.height == 720);
    CHECK(r.maximized == false);  // fullscreen branch wins; latch = false
}

TEST_CASE("save_geometry maximized preserves saved windowed size and sets maximized=true") {
    Settings::WindowGeometry prev{};
    prev.width = 800; prev.height = 600;
    prev.logical_width = 800; prev.logical_height = 600;
    prev.scale = 1.0f; prev.maximized = false;

    SaveInputs in{};
    in.fullscreen = false; in.maximized = true;
    in.was_maximized_before_fullscreen = false;
    in.window_size = {2560, 1440}; in.osd_fallback = {2560, 1440};
    in.scale = 1.0f; in.query_position = nullptr;

    auto r = save_geometry(prev, in);
    CHECK(r.width  == 800);
    CHECK(r.height == 600);
    CHECK(r.maximized == true);
}

TEST_CASE("save_geometry windowed saves window_size pw/ph") {
    Settings::WindowGeometry prev{};
    SaveInputs in{};
    in.fullscreen = false; in.maximized = false;
    in.window_size = {1920, 1080}; in.osd_fallback = {0, 0};
    in.scale = 1.0f; in.query_position = nullptr;
    auto r = save_geometry(prev, in);
    CHECK(r.width  == 1920);
    CHECK(r.height == 1080);
    CHECK(r.maximized == false);
}

TEST_CASE("save_geometry windowed falls back to osd_fallback when window_size w is zero") {
    Settings::WindowGeometry prev{};
    SaveInputs in{};
    in.fullscreen = false; in.maximized = false;
    in.window_size = {0, 1080}; in.osd_fallback = {1280, 720};
    in.scale = 1.0f; in.query_position = nullptr;
    auto r = save_geometry(prev, in);
    CHECK(r.width  == 1280);
    CHECK(r.height == 720);
}

TEST_CASE("save_geometry windowed falls back to osd_fallback when window_size h is zero") {
    Settings::WindowGeometry prev{};
    SaveInputs in{};
    in.fullscreen = false; in.maximized = false;
    in.window_size = {1920, 0}; in.osd_fallback = {1280, 720};
    in.scale = 1.0f; in.query_position = nullptr;
    auto r = save_geometry(prev, in);
    CHECK(r.width  == 1280);
    CHECK(r.height == 720);
}

TEST_CASE("save_geometry windowed both zero does not overwrite previous geometry") {
    Settings::WindowGeometry prev{};
    prev.width = 800; prev.height = 600; prev.scale = 1.0f;
    SaveInputs in{};
    in.fullscreen = false; in.maximized = false;
    in.window_size = {0, 0}; in.osd_fallback = {0, 0};
    in.scale = 1.0f; in.query_position = nullptr;
    auto r = save_geometry(prev, in);
    CHECK(r.width  == 800);
    CHECK(r.height == 600);
}

TEST_CASE("save_geometry windowed stores position when query_position returns a value") {
    Settings::WindowGeometry prev{};
    SaveInputs in{};
    in.fullscreen = false; in.maximized = false;
    in.window_size = {1280, 720}; in.scale = 1.0f;
    in.query_position = []() -> std::optional<PhysicalPoint> {
        return PhysicalPoint{100, 200};
    };
    auto r = save_geometry(prev, in);
    CHECK(r.x == 100);
    CHECK(r.y == 200);
}

TEST_CASE("save_geometry windowed does not store position when query_position returns nullopt") {
    Settings::WindowGeometry prev{};
    prev.x = 50; prev.y = 60;
    SaveInputs in{};
    in.fullscreen = false; in.maximized = false;
    in.window_size = {1280, 720}; in.scale = 1.0f;
    in.query_position = []() -> std::optional<PhysicalPoint> {
        return std::nullopt;
    };
    auto r = save_geometry(prev, in);
    // freshly constructed geom defaults x=-1, y=-1
    CHECK(r.x == Settings::WindowGeometry{}.x);
    CHECK(r.y == Settings::WindowGeometry{}.y);
}

TEST_CASE("save_geometry windowed query_position nullptr is safe") {
    Settings::WindowGeometry prev{};
    SaveInputs in{};
    in.fullscreen = false; in.maximized = false;
    in.window_size = {1280, 720}; in.scale = 1.0f;
    in.query_position = nullptr;
    auto r = save_geometry(prev, in);
    CHECK(r.width == 1280);
}

TEST_CASE("save_geometry windowed scale<=0 clamped to 1.0") {
    Settings::WindowGeometry prev{};
    SaveInputs in{};
    in.fullscreen = false; in.maximized = false;
    in.window_size = {1280, 720}; in.scale = 0.0f;
    in.query_position = nullptr;
    auto r = save_geometry(prev, in);
    CHECK(r.scale == doctest::Approx(1.0f));
    CHECK(r.logical_width  == 1280);
    CHECK(r.logical_height == 720);
}

TEST_CASE("save_geometry windowed logical dims computed as lround(pw/scale)") {
    Settings::WindowGeometry prev{};
    SaveInputs in{};
    in.fullscreen = false; in.maximized = false;
    in.window_size = {1920, 1080}; in.scale = 2.0f;
    in.query_position = nullptr;
    auto r = save_geometry(prev, in);
    CHECK(r.logical_width  == 960);
    CHECK(r.logical_height == 540);
}

// ---------------------------------------------------------------------------
// TransitionGuard
// ---------------------------------------------------------------------------

TEST_CASE("TransitionGuard initial state not active") {
    TransitionGuard g;
    CHECK(g.active() == false);
}

TEST_CASE("TransitionGuard begin_locked sets active and captures transition size") {
    TransitionGuard g;
    g.begin_locked(1920, 1080);
    CHECK(g.active() == true);
    CHECK(g.transition_pw() == 1920);
    CHECK(g.transition_ph() == 1080);
}
