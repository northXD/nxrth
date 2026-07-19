// Nxrth — abstraction the custom title bar uses to drive the OS window
// (implemented over GLFW in main.cpp). Keeps the UI decoupled from the backend.
#pragma once

namespace nxrth::ui {

class IWindowHost {
public:
    virtual ~IWindowHost() = default;

    // LOCKED mode: while true the window position is pinned — dragging is a no-op.
    virtual bool is_locked() const = 0;
    virtual void set_locked(bool v) = 0;

    // ALWAYS-ON-TOP: while true the window floats above every other window.
    virtual bool is_always_on_top() const = 0;
    virtual void set_always_on_top(bool v) = 0;

    virtual void minimize() = 0;
    virtual void request_close() = 0;

    // Window drag, anchored to the ABSOLUTE screen cursor so it never jitters:
    // begin_drag() records the window + screen-cursor position at grab time, then
    // drag_update() each frame sets the window to anchor + (cursor_now - cursor_at_grab).
    // (Moving by per-frame client-relative deltas oscillates: the window slides out
    // from under the cursor and the next delta reverses it.) No-op when locked.
    virtual void begin_drag() = 0;
    virtual void drag_update() = 0;
};

}  // namespace nxrth::ui
