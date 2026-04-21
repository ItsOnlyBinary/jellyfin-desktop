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
    bool fullscreen                       = false;
    bool maximized                        = false;
    bool was_maximized_before_fullscreen  = false;
    PhysicalSize window_size              = {};
    PhysicalSize osd_fallback             = {};
    float scale                           = 0.0f;
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
