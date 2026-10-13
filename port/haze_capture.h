#pragma once

// Window-thread capture intent survives temporary menu/focus ownership. Only
// a deliberate release changes intent; a periodic update cannot undo F1.
struct HazeCapturePolicy {
    bool enabled=true, ready=false, focused=false, menu=false;
    bool keyboard=true, manual_release=false, pause_pending=false;
    bool wants_capture() const {
        return enabled && ready && focused && keyboard && !menu &&
               !manual_release && !pause_pending;
    }
    void toggle(bool captured) {
        if (!enabled || !focused || menu || pause_pending) return;
        manual_release=captured;
        keyboard=true;
    }
    void menu_state(bool active) {
        menu=active;
        if (active) pause_pending=false;
    }
    void use_keyboard() { keyboard=true; }
    void use_controller() { keyboard=false; }
};
