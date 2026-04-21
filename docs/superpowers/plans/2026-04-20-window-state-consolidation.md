# Window State Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract window-state logic (geometry math, transition state machine, physical/logical pixel types) out of `src/main.cpp` and the duplicated Wayland/Windows transition code into a single testable module at `src/window_state.{h,cpp}`.

**Architecture:** New `namespace window_state` of pure free functions (`initial_geometry`, `corrected_size_for_scale`, `save_geometry`, `to_physical`, `to_logical`) plus a standalone `class TransitionGuard` embedded by each platform. Strong `PhysicalSize` / `PhysicalPoint` / `LogicalSize` types replace plain-`int` discipline. `Settings::WindowGeometry` on-disk shape unchanged; mpv property observers in `src/mpv/event.cpp` and the `main.cpp` FULLSCREEN event case stay put (CLAUDE.md: mpv is authoritative).

**Tech Stack:** C++17, CEF (renderer/IPC), mpv (video), CMake + Ninja via `just`, doctest (tests, already vendored at `third_party/doctest`). Platform code: Wayland, X11, Windows (DirectComposition), macOS (Cocoa).

**Branch:** `window-state-consolidation`
**Spec:** `docs/superpowers/specs/2026-04-20-window-state-consolidation-design.md`

---

## Step 1 — Replace sync mpv_get_property calls

### Task 1: Replace blocking mpv reads with atomic accessors

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/main.cpp`

- [ ] Open `src/main.cpp`. Lines 725–731 currently read:

```cpp
double display_hidpi_scale = 0.0;
mpv_get_property(g_mpv.Get(), "display-hidpi-scale",
                 MPV_FORMAT_DOUBLE, &display_hidpi_scale);
int fs_flag = 0;
mpv_get_property(g_mpv.Get(), "fullscreen", MPV_FORMAT_FLAG, &fs_flag);
LOG_INFO(LOG_MAIN, "[FLOW] display-hidpi-scale={} fullscreen={}",
         display_hidpi_scale, fs_flag);
```

Replace those seven lines with:

```cpp
double display_hidpi_scale = mpv::display_scale();
bool fs_flag = mpv::fullscreen();
LOG_INFO(LOG_MAIN, "[FLOW] display-hidpi-scale={} fullscreen={}",
         display_hidpi_scale, fs_flag);
```

`mpv::display_scale()` returns `double` (wraps atomic `s_display_scale`). `mpv::fullscreen()` returns `bool` (wraps atomic `s_fullscreen`). Both atomics are seeded in `observe_properties()` at `src/mpv/event.cpp:55-57`; `display-hidpi-scale` is registered before `osd-dimensions` so its initial value is delivered before the osd-dims wait loop at `main.cpp:651` exits. The `fs_flag` variable changes type from `int` to `bool`; the downstream `if (!fs_flag)` at line 761 compiles correctly with no other changes.

- [ ] Run `just build`. Expect zero errors and zero new warnings.

- [ ] Run `just test`. Expect all tests green.

- [ ] Commit:

```
git add src/main.cpp
git commit -m "Replace sync mpv_get_property calls with atomic accessors

display-hidpi-scale and fullscreen are already tracked by observed
atomics (mpv::display_scale(), mpv::fullscreen()). The sync reads at
startup violate CLAUDE.md's event-driven principle and are the only
remaining sync mpv_get_property calls in the startup path. Both
atomics are populated before the osd-dims wait loop exits."
```

---

## Step 2 — Types, pure functions, and tests

### Task 2a: Skeleton header, stub implementation, test wiring

**Files:**
- Create: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/window_state.h`
- Create: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/window_state.cpp`
- Create: `/home/ar/src/github/jellyfin/jellyfin-desktop/tests/window_state_test.cpp`
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/tests/CMakeLists.txt`

- [ ] Create `src/window_state.h`:

```cpp
#pragma once

#include "settings.h"
#include <functional>
#include <optional>

namespace window_state {

struct PhysicalSize  { int w = 0; int h = 0; };
struct PhysicalPoint { int x = 0; int y = 0; };
struct LogicalSize   { int w = 0; int h = 0; };

// scale <= 0 is clamped to 1.0 inside both helpers.
PhysicalSize to_physical(LogicalSize ls, float scale);
LogicalSize  to_logical (PhysicalSize ps, float scale);

struct MpvInitGeometry {
    PhysicalSize  size;
    PhysicalPoint position;
    bool          has_position = false;
    bool          maximized    = false;
};

// Phase 1 startup geometry. Uses saved.scale as the reference DPI.
// clamp_fn may be null; when provided it is called with the resolved
// w/h/x/y before the result is wrapped into MpvInitGeometry.
MpvInitGeometry initial_geometry(
    const Settings::WindowGeometry& saved,
    std::function<void(int* w, int* h, int* x, int* y)> clamp_fn);

// Phase 2 DPI correction. Returns a corrected physical size only when
// live_scale differs from saved.scale by >= 0.01. Returns nullopt when
// saved data is absent, live_scale == 0, or scales are close enough.
std::optional<PhysicalSize> corrected_size_for_scale(
    const Settings::WindowGeometry& saved,
    double live_scale);

struct SaveInputs {
    bool fullscreen;
    bool maximized;
    bool was_maximized_before_fullscreen;
    PhysicalSize window_size;
    PhysicalSize osd_fallback;
    float scale;
    std::function<std::optional<PhysicalPoint>()> query_position;
};

Settings::WindowGeometry save_geometry(
    const Settings::WindowGeometry& previous,
    const SaveInputs& in);

// Portable transition state machine. Each platform embeds one by value
// in its surface state. All _locked methods require the caller to hold
// the platform's surface mutex — this class carries no mutex of its own.
class TransitionGuard {
public:
    // on_begin_locked fires inside begin_locked() while the caller's
    // surface mutex is held. Must not acquire that mutex.
    explicit TransitionGuard(std::function<void()> on_begin_locked = nullptr);

    void begin_locked(int current_pw, int current_ph);
    void end_locked();
    void set_expected_size_locked(int w, int h);

    bool active() const;
    int  transition_pw() const;
    int  transition_ph() const;

    // Returns true when this frame should be dropped to prevent stretching.
    // Inactive: never drop. Active with no expected size: drop all.
    // Active: drop if frame matches old transition size; pass if matches expected.
    bool should_drop_frame(int frame_pw, int frame_ph) const;

    // If active and frame matches expected size, calls end_locked() and
    // returns true. Otherwise returns false without state change.
    bool maybe_end_on_frame(int frame_pw, int frame_ph);

    int  pending_lw() const;
    int  pending_lh() const;
    void set_pending_logical(int lw, int lh);

private:
    std::function<void()> on_begin_locked_;
    bool transitioning_ = false;
    int  transition_pw_ = 0, transition_ph_ = 0;
    int  expected_w_    = 0, expected_h_    = 0;
    int  pending_lw_    = 0, pending_lh_    = 0;
};

} // namespace window_state
```

- [ ] Create `src/window_state.cpp` with stub implementations:

```cpp
#include "window_state.h"
#include <cmath>

namespace window_state {

PhysicalSize to_physical(LogicalSize ls, float scale) {
    (void)ls; (void)scale; return {};
}
LogicalSize to_logical(PhysicalSize ps, float scale) {
    (void)ps; (void)scale; return {};
}
MpvInitGeometry initial_geometry(
    const Settings::WindowGeometry& saved,
    std::function<void(int* w, int* h, int* x, int* y)> clamp_fn)
{
    (void)saved; (void)clamp_fn; return {};
}
std::optional<PhysicalSize> corrected_size_for_scale(
    const Settings::WindowGeometry& saved, double live_scale)
{
    (void)saved; (void)live_scale; return std::nullopt;
}
Settings::WindowGeometry save_geometry(
    const Settings::WindowGeometry& previous, const SaveInputs& in)
{
    (void)in; return previous;
}

TransitionGuard::TransitionGuard(std::function<void()> on_begin_locked)
    : on_begin_locked_(std::move(on_begin_locked)) {}
void TransitionGuard::begin_locked(int pw, int ph) { (void)pw; (void)ph; }
void TransitionGuard::end_locked() {}
void TransitionGuard::set_expected_size_locked(int w, int h) { (void)w; (void)h; }
bool TransitionGuard::active() const { return false; }
int  TransitionGuard::transition_pw() const { return transition_pw_; }
int  TransitionGuard::transition_ph() const { return transition_ph_; }
bool TransitionGuard::should_drop_frame(int pw, int ph) const {
    (void)pw; (void)ph; return false;
}
bool TransitionGuard::maybe_end_on_frame(int pw, int ph) {
    (void)pw; (void)ph; return false;
}
int  TransitionGuard::pending_lw() const { return pending_lw_; }
int  TransitionGuard::pending_lh() const { return pending_lh_; }
void TransitionGuard::set_pending_logical(int lw, int lh) { (void)lw; (void)lh; }

} // namespace window_state
```

- [ ] Create `tests/window_state_test.cpp` with the first failing test:

```cpp
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
```

- [ ] Edit `tests/CMakeLists.txt` so the full file is:

```cmake
add_executable(jellyfin_api_test
    jellyfin_api_test.cpp
    ${CMAKE_SOURCE_DIR}/src/jellyfin_api.cpp
    ${CMAKE_SOURCE_DIR}/src/cjson/cJSON.c
)

target_include_directories(jellyfin_api_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest
)

add_test(NAME jellyfin_api_test COMMAND jellyfin_api_test)

add_executable(window_state_test
    window_state_test.cpp
    ${CMAKE_SOURCE_DIR}/src/window_state.cpp
)

target_include_directories(window_state_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest
)

add_test(NAME window_state_test COMMAND window_state_test)
```

- [ ] Run `just build`. Expect compile success (stubs compile cleanly; `Settings::WindowGeometry` is declared inline in `settings.h` so no cJSON or filesystem symbols are pulled in).

- [ ] Run `cd build && ctest -R window_state_test -V`. Expect the `to_physical scale=1 is identity` test to FAIL (stub returns `{0,0}`).

- [ ] Commit:

```
git add src/window_state.h src/window_state.cpp tests/window_state_test.cpp tests/CMakeLists.txt
git commit -m "Add window_state skeleton and test wiring (first test failing)

Creates src/window_state.{h,cpp} with stub implementations of all
declared functions and class TransitionGuard. Creates
tests/window_state_test.cpp with the first failing test for to_physical.
Wires window_state_test into tests/CMakeLists.txt following the
jellyfin_api_test pattern, linking only src/window_state.cpp."
```

---

### Task 2b: Implement to_physical and to_logical

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/window_state.cpp`
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/tests/window_state_test.cpp`

- [ ] Replace the `to_physical` and `to_logical` stubs in `src/window_state.cpp`:

```cpp
PhysicalSize to_physical(LogicalSize ls, float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    return { static_cast<int>(std::lround(ls.w * scale)),
             static_cast<int>(std::lround(ls.h * scale)) };
}

LogicalSize to_logical(PhysicalSize ps, float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    return { static_cast<int>(std::lround(ps.w / scale)),
             static_cast<int>(std::lround(ps.h / scale)) };
}
```

- [ ] Replace `tests/window_state_test.cpp` with the full `to_physical` / `to_logical` block (keep the file header and add the five new cases):

```cpp
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
```

- [ ] Run `cd build && ctest -R window_state_test -V`. Expect all 6 tests PASS.

- [ ] Commit:

```
git add src/window_state.cpp tests/window_state_test.cpp
git commit -m "Implement to_physical / to_logical with scale clamping (6 tests pass)"
```

---

### Task 2c: initial_geometry — first failing test, then implementation

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/tests/window_state_test.cpp`
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/window_state.cpp`

- [ ] Append to `tests/window_state_test.cpp` after the `to_*` block:

```cpp
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
```

- [ ] Run `cd build && ctest -R window_state_test -V`. Expect the new test to FAIL (stub returns `{}`, size is `{0,0}` not `{1280,720}`).

- [ ] Replace the `initial_geometry` stub in `src/window_state.cpp`:

```cpp
MpvInitGeometry initial_geometry(
    const Settings::WindowGeometry& saved,
    std::function<void(int* w, int* h, int* x, int* y)> clamp_fn)
{
    using WG = Settings::WindowGeometry;
    MpvInitGeometry result;

    int w = (saved.width > 0 && saved.height > 0)
                ? saved.width  : WG::kDefaultPhysicalWidth;
    int h = (saved.width > 0 && saved.height > 0)
                ? saved.height : WG::kDefaultPhysicalHeight;
    int x = saved.x;
    int y = saved.y;

    if (clamp_fn) clamp_fn(&w, &h, &x, &y);

    result.size      = { w, h };
    result.maximized = saved.maximized;

    if (x >= 0 && y >= 0) {
        result.position     = { x, y };
        result.has_position = true;
    }
    return result;
}
```

- [ ] Run `cd build && ctest -R window_state_test -V`. Expect PASS.

- [ ] Commit:

```
git add src/window_state.cpp tests/window_state_test.cpp
git commit -m "Implement initial_geometry, add first passing test"
```

---

### Task 2d: Remaining initial_geometry tests

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/tests/window_state_test.cpp`

- [ ] Append after the first `initial_geometry` test:

```cpp
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
```

- [ ] Run `cd build && ctest -R window_state_test -V`. Expect all tests PASS.

- [ ] Commit:

```
git add tests/window_state_test.cpp
git commit -m "Add remaining initial_geometry test cases (all passing)"
```

---

### Task 2e: corrected_size_for_scale — failing test, implementation, remaining tests

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/tests/window_state_test.cpp`
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/window_state.cpp`

- [ ] Append to `tests/window_state_test.cpp`:

```cpp
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
```

- [ ] Run `cd build && ctest -R window_state_test -V`. The `same scale` test passes on the stub; the `above threshold` test FAILS.

- [ ] Replace the `corrected_size_for_scale` stub in `src/window_state.cpp`:

```cpp
std::optional<PhysicalSize> corrected_size_for_scale(
    const Settings::WindowGeometry& saved,
    double live_scale)
{
    using WG = Settings::WindowGeometry;
    if (live_scale <= 0.0) return std::nullopt;

    float saved_scale = saved.scale > 0.f ? saved.scale : WG::kDefaultScale;
    if (std::fabs(live_scale - saved_scale) < 0.01) return std::nullopt;

    int lw = saved.logical_width  > 0 ? saved.logical_width  : WG::kDefaultLogicalWidth;
    int lh = saved.logical_height > 0 ? saved.logical_height : WG::kDefaultLogicalHeight;

    return PhysicalSize{
        static_cast<int>(std::lround(lw * live_scale)),
        static_cast<int>(std::lround(lh * live_scale))
    };
}
```

- [ ] Append remaining tests to `tests/window_state_test.cpp`:

```cpp
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
```

- [ ] Run `cd build && ctest -R window_state_test -V`. Expect all tests PASS.

- [ ] Commit:

```
git add src/window_state.cpp tests/window_state_test.cpp
git commit -m "Implement corrected_size_for_scale with full test coverage (7 tests pass)"
```

---

### Task 2f: save_geometry — fullscreen and maximized branches

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/tests/window_state_test.cpp`
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/window_state.cpp`

- [ ] Append to `tests/window_state_test.cpp`:

```cpp
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
```

- [ ] Run `cd build && ctest -R window_state_test -V`. Expect 3 new tests to FAIL (stub returns `previous` unchanged; the fullscreen test fails because `maximized` stays `false` not `true`).

- [ ] Replace the `save_geometry` stub in `src/window_state.cpp`:

```cpp
Settings::WindowGeometry save_geometry(
    const Settings::WindowGeometry& previous,
    const SaveInputs& in)
{
    if (in.fullscreen) {
        auto geom = previous;
        geom.maximized = in.was_maximized_before_fullscreen;
        return geom;
    }
    if (in.maximized) {
        auto geom = previous;
        geom.maximized = true;
        return geom;
    }

    // Windowed branch: build a fresh geometry from live state.
    int pw = in.window_size.w;
    int ph = in.window_size.h;
    if (pw <= 0 || ph <= 0) {
        pw = in.osd_fallback.w;
        ph = in.osd_fallback.h;
    }
    if (pw <= 0 || ph <= 0) return previous;

    Settings::WindowGeometry geom;
    geom.width  = pw;
    geom.height = ph;

    float scale = in.scale;
    if (scale <= 0.0f) scale = 1.0f;
    geom.scale          = scale;
    geom.logical_width  = static_cast<int>(std::lround(pw / scale));
    geom.logical_height = static_cast<int>(std::lround(ph / scale));
    geom.maximized = false;

    if (in.query_position) {
        if (auto pos = in.query_position()) {
            geom.x = pos->x;
            geom.y = pos->y;
        }
    }
    return geom;
}
```

- [ ] Run `cd build && ctest -R window_state_test -V`. Expect all tests PASS.

- [ ] Commit:

```
git add src/window_state.cpp tests/window_state_test.cpp
git commit -m "Implement save_geometry: fullscreen+maximized branches + windowed branch

Fullscreen: preserves previous geometry, sets maximized from
was_maximized_before_fullscreen latch (maintained by FULLSCREEN event
case in main.cpp). Fullscreen wins when both flags are true. Maximized:
preserves previous windowed size, sets maximized=true. Windowed: fresh
geometry from window_size (fallback osd_fallback), scale clamped >= 1.0,
logical dims via lround(pw/scale), position from query_position if set."
```

---

### Task 2g: Remaining save_geometry windowed-branch tests

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/tests/window_state_test.cpp`

- [ ] Append after the three `save_geometry` tests already added:

```cpp
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
```

- [ ] Run `cd build && ctest -R window_state_test -V`. Expect all tests PASS.

- [ ] Commit:

```
git add tests/window_state_test.cpp
git commit -m "Add remaining save_geometry windowed-branch tests (all passing)"
```

---

## Step 3 — Cut main.cpp over to window_state functions

### Task 3a: Replace startup geometry block

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/main.cpp`

- [ ] Add `#include "window_state.h"` to `src/main.cpp` alongside the other `src/` includes (near the existing `#include "settings.h"`).

- [ ] Replace lines 558–588 (the `{ using WG = Settings::WindowGeometry; ... g_mpv.SetOptionString("window-maximized", "yes"); }` block) with:

```cpp
    // Restore saved window geometry. mpv's --geometry is always physical
    // pixels (m_geometry_apply in third_party/mpv/options/m_option.c
    // assigns gm->w/h to widw/widh without applying dpi_scale). If the
    // live display scale differs from the saved scale, the post-CEF-init
    // DPI correction block below fixes the size once display-hidpi-scale
    // is known.
    {
        auto g = window_state::initial_geometry(
            Settings::instance().windowGeometry(),
            g_platform.clamp_window_geometry);
        std::string geom_str = std::to_string(g.size.w) + "x"
                             + std::to_string(g.size.h);
        if (g.has_position) {
            geom_str += "+" + std::to_string(g.position.x)
                      + "+" + std::to_string(g.position.y);
            g_mpv.SetOptionString("force-window-position", "yes");
        }
        g_mpv.SetOptionString("geometry", geom_str);
        if (g.maximized)
            g_mpv.SetOptionString("window-maximized", "yes");
    }
```

- [ ] Run `just build`. Expect zero errors.

- [ ] Run `just test`. Expect all tests green.

- [ ] Manual smoke: `just run`. Verify the window opens at approximately the expected size/position. Check `build/run.log` for `geometry` in the mpv options.

- [ ] Commit:

```
git add src/main.cpp
git commit -m "Replace startup geometry block with window_state::initial_geometry

No behaviour change. Delegates to the unit-tested function.
Removes ~30 lines of inline math from main.cpp."
```

---

### Task 3b: Replace DPI correction block

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/main.cpp`

- [ ] Replace the DPI correction block (the `{ using WG = Settings::WindowGeometry; const auto& saved = ...` block at approximately line 741, ending before `// --- Create browsers ---`) with:

```cpp
    {
        if (auto corrected = window_state::corrected_size_for_scale(
                Settings::instance().windowGeometry(),
                display_hidpi_scale)) {
            std::string geom_str = std::to_string(corrected->w) + "x"
                                 + std::to_string(corrected->h);
            LOG_INFO(LOG_MAIN, "[FLOW] scale mismatch, resize to {}",
                     geom_str.c_str());
            g_mpv.SetGeometry(geom_str);
            if (!fs_flag) {
                mw = corrected->w;
                mh = corrected->h;
            }
        }
        mpv::set_window_pixels(static_cast<int>(mw), static_cast<int>(mh));
    }
```

- [ ] Run `just build`. Expect zero errors.

- [ ] Run `just test`. Expect all tests green.

- [ ] Manual smoke: `just run` on the same display that wrote the saved geometry. The correction block should be a no-op (no `scale mismatch` log line). On a display with a different DPI from the saved value, the `[FLOW] scale mismatch` log line should appear and the window should open at the corrected size.

- [ ] Commit:

```
git add src/main.cpp
git commit -m "Replace DPI correction block with window_state::corrected_size_for_scale

No behaviour change. Log message simplified (scale values retrievable
from Settings if needed for debugging)."
```

---

### Task 3c: Replace shutdown save block

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/main.cpp`

- [ ] Replace lines 919–974 (the `// Save window geometry while mpv is still alive.` block through `Settings::instance().save();`) with:

```cpp
    // Save window geometry while mpv is still alive.
    {
        window_state::SaveInputs in;
        in.fullscreen = mpv::fullscreen();
        in.maximized  = mpv::window_maximized();
        in.was_maximized_before_fullscreen = g_was_maximized_before_fullscreen;
        in.window_size  = { mpv::window_pw(), mpv::window_ph() };
        in.osd_fallback = { mpv::osd_pw(),    mpv::osd_ph()    };
        in.scale = g_platform.get_scale ? g_platform.get_scale() : 1.0f;
        if (g_platform.query_window_position) {
            in.query_position = [&]() -> std::optional<window_state::PhysicalPoint> {
                int wx = 0, wy = 0;
                if (g_platform.query_window_position(&wx, &wy))
                    return window_state::PhysicalPoint{wx, wy};
                return std::nullopt;
            };
        }
        auto new_geom = window_state::save_geometry(
            Settings::instance().windowGeometry(), in);
        Settings::instance().setWindowGeometry(new_geom);
        Settings::instance().save();
    }
```

Note: The FULLSCREEN event case at lines 245–252 continues to maintain `g_was_maximized_before_fullscreen` exactly as before. The shutdown code reads that latch into `SaveInputs::was_maximized_before_fullscreen`.

- [ ] Run `just build`. Expect zero errors.

- [ ] Run `just test`. Expect all tests green.

- [ ] Manual smoke: `just run`. Open the app, move and resize the window, close normally. Open the config file (path shown in `build/run.log` or `~/.config/jellyfin-desktop/jellyfin-desktop.json`) and verify `width`, `height`, `x`, `y`, `scale`, `logical_width`, `logical_height`, `maximized` reflect the window state at close time. Repeat with: maximize then close (verify previous windowed size preserved, `maximized: true`); enter fullscreen then close (verify previous windowed size preserved, `maximized` reflects pre-fullscreen state).

- [ ] Commit:

```
git add src/main.cpp
git commit -m "Replace shutdown save block with window_state::save_geometry

No behaviour change. All three branches (fullscreen, maximized, windowed)
are now covered by window_state_test. g_was_maximized_before_fullscreen
latch stays in the FULLSCREEN event case; shutdown reads it via
SaveInputs::was_maximized_before_fullscreen."
```

---

## Step 4 — TransitionGuard implementation and tests

### Task 4a: TransitionGuard — first failing test, full implementation

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/tests/window_state_test.cpp`
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/window_state.cpp`

- [ ] Append to `tests/window_state_test.cpp`:

```cpp
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
```

- [ ] Run `cd build && ctest -R window_state_test -V`. The `initial state not active` test passes on the stub. The `begin_locked` test FAILS.

- [ ] Replace all `TransitionGuard` method stubs in `src/window_state.cpp` with full implementations:

```cpp
TransitionGuard::TransitionGuard(std::function<void()> on_begin_locked)
    : on_begin_locked_(std::move(on_begin_locked)) {}

void TransitionGuard::begin_locked(int current_pw, int current_ph) {
    transitioning_  = true;
    transition_pw_  = current_pw;
    transition_ph_  = current_ph;
    expected_w_     = 0;
    expected_h_     = 0;
    pending_lw_     = 0;
    pending_lh_     = 0;
    if (on_begin_locked_) on_begin_locked_();
}

void TransitionGuard::end_locked() {
    transitioning_ = false;
    expected_w_    = 0;
    expected_h_    = 0;
}

void TransitionGuard::set_expected_size_locked(int w, int h) {
    if (transitioning_ && w == transition_pw_ && h == transition_ph_)
        return;
    expected_w_ = w;
    expected_h_ = h;
}

bool TransitionGuard::active() const { return transitioning_; }
int  TransitionGuard::transition_pw() const { return transition_pw_; }
int  TransitionGuard::transition_ph() const { return transition_ph_; }

bool TransitionGuard::should_drop_frame(int frame_pw, int frame_ph) const {
    if (!transitioning_) return false;
    if (expected_w_ <= 0) return true;
    if (frame_pw == expected_w_ && frame_ph == expected_h_) return false;
    return true;
}

bool TransitionGuard::maybe_end_on_frame(int frame_pw, int frame_ph) {
    if (!transitioning_) return false;
    if (expected_w_ <= 0) return false;
    if (frame_pw == expected_w_ && frame_ph == expected_h_) {
        end_locked();
        return true;
    }
    return false;
}

int  TransitionGuard::pending_lw() const { return pending_lw_; }
int  TransitionGuard::pending_lh() const { return pending_lh_; }

void TransitionGuard::set_pending_logical(int lw, int lh) {
    pending_lw_ = lw;
    pending_lh_ = lh;
}
```

- [ ] Run `cd build && ctest -R window_state_test -V`. Expect all tests PASS.

- [ ] Commit:

```
git add src/window_state.cpp tests/window_state_test.cpp
git commit -m "Implement TransitionGuard; add begin_locked/initial-state tests"
```

---

### Task 4b: Remaining TransitionGuard tests

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/tests/window_state_test.cpp`

- [ ] Append after the two `TransitionGuard` tests already added:

```cpp
TEST_CASE("TransitionGuard end_locked clears active and expected size") {
    TransitionGuard g;
    g.begin_locked(1280, 720);
    g.set_expected_size_locked(1920, 1080);
    g.end_locked();
    CHECK(g.active() == false);
    CHECK(g.should_drop_frame(1280, 720) == false);  // inactive never drops
}

TEST_CASE("TransitionGuard on_begin_locked callback invoked on begin") {
    bool called = false;
    TransitionGuard g([&]{ called = true; });
    g.begin_locked(100, 100);
    CHECK(called == true);
}

TEST_CASE("TransitionGuard null on_begin_locked is safe") {
    TransitionGuard g(nullptr);
    g.begin_locked(100, 100);
    CHECK(g.active() == true);
}

TEST_CASE("TransitionGuard double begin_locked resets state cleanly") {
    int call_count = 0;
    TransitionGuard g([&]{ ++call_count; });
    g.begin_locked(1280, 720);
    g.set_expected_size_locked(1920, 1080);
    g.set_pending_logical(960, 540);
    // Second begin without prior end
    g.begin_locked(1920, 1080);
    CHECK(g.active() == true);
    CHECK(g.transition_pw() == 1920);
    CHECK(g.transition_ph() == 1080);
    // expected and pending cleared on second begin
    CHECK(g.should_drop_frame(1920, 1080) == true);  // no expected set
    CHECK(g.pending_lw() == 0);
    CHECK(g.pending_lh() == 0);
    CHECK(call_count == 2);
}

TEST_CASE("TransitionGuard out-of-order end_locked when inactive is a no-op") {
    TransitionGuard g;
    g.end_locked();  // must not crash
    CHECK(g.active() == false);
}

TEST_CASE("TransitionGuard should_drop_frame inactive never drops") {
    TransitionGuard g;
    CHECK(g.should_drop_frame(1920, 1080) == false);
    CHECK(g.should_drop_frame(0, 0)       == false);
}

TEST_CASE("TransitionGuard should_drop_frame active no expected size drops all") {
    TransitionGuard g;
    g.begin_locked(1280, 720);
    // expected_w_ == 0 after begin
    CHECK(g.should_drop_frame(1280, 720)  == true);
    CHECK(g.should_drop_frame(1920, 1080) == true);
    CHECK(g.should_drop_frame(0, 0)       == true);
}

TEST_CASE("TransitionGuard should_drop_frame active frame matches transition size drops") {
    TransitionGuard g;
    g.begin_locked(1280, 720);
    g.set_expected_size_locked(1920, 1080);
    CHECK(g.should_drop_frame(1280, 720) == true);
}

TEST_CASE("TransitionGuard should_drop_frame active frame matches expected size does not drop") {
    TransitionGuard g;
    g.begin_locked(1280, 720);
    g.set_expected_size_locked(1920, 1080);
    CHECK(g.should_drop_frame(1920, 1080) == false);
}

TEST_CASE("TransitionGuard maybe_end_on_frame matching expected ends transition and returns true") {
    TransitionGuard g;
    g.begin_locked(1280, 720);
    g.set_expected_size_locked(1920, 1080);
    bool ended = g.maybe_end_on_frame(1920, 1080);
    CHECK(ended == true);
    CHECK(g.active() == false);
}

TEST_CASE("TransitionGuard maybe_end_on_frame non-matching returns false") {
    TransitionGuard g;
    g.begin_locked(1280, 720);
    g.set_expected_size_locked(1920, 1080);
    bool ended = g.maybe_end_on_frame(1280, 720);
    CHECK(ended == false);
    CHECK(g.active() == true);
}

TEST_CASE("TransitionGuard maybe_end_on_frame inactive returns false") {
    TransitionGuard g;
    CHECK(g.maybe_end_on_frame(1920, 1080) == false);
}

TEST_CASE("TransitionGuard set_expected_size_locked updates drop predicate") {
    TransitionGuard g;
    g.begin_locked(1280, 720);
    g.set_expected_size_locked(1920, 1080);
    CHECK(g.should_drop_frame(1920, 1080) == false);
    g.set_expected_size_locked(2560, 1440);
    CHECK(g.should_drop_frame(1920, 1080) == true);
    CHECK(g.should_drop_frame(2560, 1440) == false);
}

TEST_CASE("TransitionGuard set_expected_size_locked matching transition size is ignored") {
    TransitionGuard g;
    g.begin_locked(1280, 720);
    // Setting expected to the same as transition pw/ph must be a no-op
    // (would otherwise create an always-allow state that defeats the guard)
    g.set_expected_size_locked(1280, 720);
    // expected_w_ stays 0, so all frames drop
    CHECK(g.should_drop_frame(1280, 720) == true);
}

TEST_CASE("TransitionGuard set_pending_logical preserved through end_locked") {
    TransitionGuard g;
    g.begin_locked(1280, 720);
    g.set_pending_logical(640, 360);
    g.end_locked();
    // pending is not cleared by end_locked; platform reads it after end
    CHECK(g.pending_lw() == 640);
    CHECK(g.pending_lh() == 360);
}
```

- [ ] Run `cd build && ctest -R window_state_test -V`. Expect all tests PASS.

- [ ] Commit:

```
git add tests/window_state_test.cpp
git commit -m "Add full TransitionGuard test suite (all passing)

Covers: initial state, begin/end lifecycle, on_begin_locked callback,
null callback safety, double-begin reset, out-of-order end no-op,
should_drop_frame (4 cases), maybe_end_on_frame (3 cases),
set_expected_size_locked semantics (2 cases), set_pending_logical
preserved through end. Total: ~45 cases across all tasks."
```

---

## Step 5 — Wire platforms to TransitionGuard

### Task 5a: Wayland — replace duplicated transition fields with TransitionGuard

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/platform/wayland.cpp`

- [ ] Add `#include "window_state.h"` near the top of `src/platform/wayland.cpp`, after the existing `#include` block.

- [ ] In `WlState` (lines 91–95), remove the five duplicated fields and replace with a `TransitionGuard` member. The struct section changes from:

```cpp
    int transition_pw = 0, transition_ph = 0;
    int pending_lw = 0, pending_lh = 0;
    int expected_w = 0, expected_h = 0;
    bool transitioning = false;
    bool was_fullscreen = false;
```

to:

```cpp
    window_state::TransitionGuard guard;
    bool was_fullscreen = false;
```

`guard` is default-constructed (no-op callback) at field declaration. The real Wayland-protocol callback is injected during init after surfaces exist.

- [ ] Inside the Wayland init function (where `g_wl.was_fullscreen` is seeded at approximately line 989), add immediately before that seed line:

```cpp
    g_wl.guard = window_state::TransitionGuard([]{
        if (g_wl.cef_surface) {
            wl_surface_attach(g_wl.cef_surface, nullptr, 0, 0);
            if (g_wl.cef_viewport)
                wp_viewport_set_destination(g_wl.cef_viewport, -1, -1);
            wl_surface_commit(g_wl.cef_surface);
            wl_display_flush(g_wl.display);
        }
    });
```

- [ ] Rewrite `wl_begin_transition_locked` (lines 1221–1234):

```cpp
static void wl_begin_transition_locked() {
    g_wl.guard.begin_locked(g_wl.mpv_pw, g_wl.mpv_ph);
}
```

- [ ] Rewrite `wl_end_transition_locked` (lines 1236–1248). The Wayland-specific viewport update with pending dims is preserved here since it requires direct Wayland object access:

```cpp
static void wl_end_transition_locked() {
    int plw = g_wl.guard.pending_lw();
    int plh = g_wl.guard.pending_lh();
    g_wl.guard.end_locked();
    if (g_wl.cef_viewport && plw > 0) {
        wp_viewport_set_source(g_wl.cef_viewport,
            wl_fixed_from_int(0), wl_fixed_from_int(0),
            wl_fixed_from_int(g_wl.mpv_pw), wl_fixed_from_int(g_wl.mpv_ph));
        wp_viewport_set_destination(g_wl.cef_viewport, plw, plh);
    }
}
```

- [ ] Rewrite `wl_set_expected_size_locked` (lines 1290–1294):

```cpp
static void wl_set_expected_size_locked(int w, int h) {
    g_wl.guard.set_expected_size_locked(w, h);
}
```

- [ ] Rewrite `wl_in_transition` (line 1287):

```cpp
static bool wl_in_transition() {
    return g_wl.guard.active();
}
```

- [ ] Rewrite `update_surface_size_locked` (lines 1190–1214) — replace `g_wl.transitioning`, `g_wl.pending_lw/lh` field accesses with guard API:

```cpp
static void update_surface_size_locked(int lw, int lh, int pw, int ph) {
    if (g_wl.guard.active()) {
        g_wl.guard.set_pending_logical(lw, lh);
        if (g_wl.cef_surface && g_wl.cef_viewport) {
            wp_viewport_set_destination(g_wl.cef_viewport, lw, lh);
            wl_surface_commit(g_wl.cef_surface);
            wl_display_flush(g_wl.display);
        }
    } else if (g_wl.cef_surface) {
        bool growing = pw > g_wl.mpv_pw || ph > g_wl.mpv_ph;
        if (growing)
            wl_surface_attach(g_wl.cef_surface, nullptr, 0, 0);
        if (g_wl.cef_viewport) {
            wp_viewport_set_source(g_wl.cef_viewport,
                wl_fixed_from_int(0), wl_fixed_from_int(0),
                wl_fixed_from_int(pw), wl_fixed_from_int(ph));
            wp_viewport_set_destination(g_wl.cef_viewport, lw, lh);
        }
        wl_surface_commit(g_wl.cef_surface);
        wl_display_flush(g_wl.display);
    }
    g_wl.mpv_pw = pw;
    g_wl.mpv_ph = ph;
}
```

- [ ] Update the Phase 1 frame-drop check in `wl_present` (approximately line 165):

```cpp
    {
        std::lock_guard<std::mutex> lock(g_wl.surface_mtx);
        if (!g_wl.cef_surface || !g_wl.dmabuf) return;
        if (g_wl.guard.should_drop_frame(w, h)) return;
    }
```

- [ ] Update the Phase 3 in-lock transition check (approximately lines 184–189):

```cpp
        if (g_wl.guard.active()) {
            if (g_wl.guard.should_drop_frame(w, h)) {
                wl_buffer_destroy(buf);
                return;
            }
            wl_end_transition_locked();
        }
```

- [ ] Update the `was_fullscreen` comparisons in the configure callback (approximately lines 686–696) — replace `g_wl.transitioning` with `g_wl.guard.active()`:

```cpp
    if (fs != g_wl.was_fullscreen) {
        if (!g_wl.guard.active()) {
            wl_begin_transition_locked();
            wl_set_expected_size_locked(pw, ph);
        } else {
            wl_end_transition_locked();
        }
        g_wl.was_fullscreen = fs;
    }
```

- [ ] Update the fullscreen-state-mismatch cancel in `wl_set_fullscreen` (approximately line 1260):

```cpp
        if (g_wl.guard.active() && fullscreen == g_wl.was_fullscreen)
            wl_end_transition_locked();
```

- [ ] Run `just build`. Resolve any remaining `g_wl.transitioning`, `g_wl.expected_w`, `g_wl.expected_h`, `g_wl.pending_lw`, `g_wl.pending_lh`, `g_wl.transition_pw`, `g_wl.transition_ph` references by replacing with the corresponding guard calls. Run `just build` again until clean.

- [ ] Run `just test`. Expect all tests green.

- [ ] Manual smoke on Wayland: `just run`. Toggle fullscreen with F11. Verify: (a) no stretched CEF content during the transition, (b) fullscreen completes cleanly, (c) toggling back to windowed works, (d) dragging to resize does not stretch. Check `build/run.log` for any unexpected errors.

- [ ] Commit:

```
git add src/platform/wayland.cpp
git commit -m "Wayland: replace duplicated transition fields with TransitionGuard

Removes transitioning/transition_pw/ph/expected_w/h/pending_lw/lh from
WlState. TransitionGuard owns the portable state machine. Wayland-specific
protocol calls (wl_surface_attach null, wp_viewport_set_destination -1,
wl_display_flush) move into the on_begin_locked lambda assigned in wl_init
after surfaces are created. wl_end_transition_locked reads pending dims
from the guard before calling end_locked, then applies them to the viewport."
```

---

### Task 5b: Windows — replace duplicated transition fields with TransitionGuard

**Files:**
- Modify: `/home/ar/src/github/jellyfin/jellyfin-desktop/src/platform/windows.cpp`

This task cannot be smoke-tested from this Linux host. Build verification only; mark for Windows CI.

- [ ] Add `#include "window_state.h"` near the top of `src/platform/windows.cpp`, after the existing includes.

- [ ] In `WinState` (lines 77–81), remove the five duplicated fields and replace:

```cpp
    window_state::TransitionGuard guard;
    bool was_fullscreen = false;
```

- [ ] Inside the Windows init function, after `dcomp_device->Commit()` in the DComp setup block (before the `was_fullscreen` seed at approximately line 662), add:

```cpp
    g_win.guard = window_state::TransitionGuard([]{
        if (g_win.dcomp_main_visual) {
            g_win.dcomp_main_visual->SetContent(nullptr);
            if (g_win.main_swap_chain) {
                g_win.main_swap_chain->Release();
                g_win.main_swap_chain = nullptr;
                g_win.main_sw = 0;
                g_win.main_sh = 0;
            }
            g_win.dcomp_device->Commit();
        }
    });
```

- [ ] Rewrite `win_begin_transition_locked` (lines 479–497):

```cpp
static void win_begin_transition_locked() {
    g_win.guard.begin_locked(g_win.mpv_pw, g_win.mpv_ph);
}
```

- [ ] Rewrite `win_end_transition_locked` (lines 499–505). Windows has no viewport to update; pending dims are preserved in the guard for any caller that reads them after end:

```cpp
static void win_end_transition_locked() {
    g_win.guard.end_locked();
}
```

- [ ] Rewrite `win_in_transition` (line 518):

```cpp
static bool win_in_transition() {
    return g_win.guard.active();
}
```

- [ ] Rewrite `win_set_expected_size` (lines 521–526):

```cpp
static void win_set_expected_size(int w, int h) {
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);
    g_win.guard.set_expected_size_locked(w, h);
}
```

- [ ] Rewrite `update_surface_size_locked` (lines 463–472):

```cpp
static void update_surface_size_locked(int lw, int lh, int pw, int ph) {
    if (g_win.guard.active())
        g_win.guard.set_pending_logical(lw, lh);
    g_win.mpv_pw = pw;
    g_win.mpv_ph = ph;
}
```

- [ ] Update the frame-drop check in `win_present` (approximately lines 227–234):

```cpp
    std::lock_guard<std::mutex> lock(g_win.surface_mtx);

    if (g_win.guard.active()) {
        if (g_win.guard.should_drop_frame(w, h)) {
            src->Release();
            return;
        }
        win_end_transition_locked();
    }
```

- [ ] Update the fullscreen-change detection in the `WM_SIZE` hook (approximately lines 612–620):

```cpp
                    if (fs != g_win.was_fullscreen) {
                        if (!g_win.guard.active())
                            win_begin_transition_locked();
                        else
                            win_end_transition_locked();
                        g_win.was_fullscreen = fs;
                    } else if (g_win.guard.active()) {
                        win_end_transition_locked();
                    }
```

- [ ] Update `win_set_fullscreen` and `win_toggle_fullscreen` — replace `g_win.transitioning` (which no longer exists) with `g_win.guard.active()` if referenced. Grep the file for any remaining direct field references and replace with guard API.

- [ ] Run `just build` (Linux build; the `windows.cpp` translation unit is skipped on Linux). The important check is that `window_state.h` compiles cleanly and the rest of the Linux build is unaffected.

- [ ] Run `just test`. Expect all tests green.

- [ ] Commit:

```
git add src/platform/windows.cpp
git commit -m "Windows: replace duplicated transition fields with TransitionGuard

Removes transitioning/transition_pw/ph/expected_w/h/pending_lw/lh from
WinState. DComp visual detachment and swap-chain teardown move into the
on_begin_locked lambda assigned in win_init after DComp is set up.
win_end_transition_locked delegates to guard.end_locked(); DComp swap
chain is recreated lazily when the next correctly-sized frame arrives.

NEEDS-WINDOWS-CI: smoke test fullscreen toggle and window resize on
Windows to verify no stretched content and no frozen transitions."
```
