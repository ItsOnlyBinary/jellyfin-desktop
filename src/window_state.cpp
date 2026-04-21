#include "window_state.h"
#include <cmath>

namespace window_state {

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
