// Nxrth — abstraction the custom title bar uses to drive the OS window
// (implemented over GLFW in main.cpp). Keeps the UI decoupled from the backend.
#pragma once

namespace nxrth::ui {

class IWindowHost {
public:
    virtual ~IWindowHost() = default;

    // LOCKED mode: while true the window position is pinned — drag_by() is a no-op.
    virtual bool is_locked() const = 0;
    virtual void set_locked(bool v) = 0;

    // ALWAYS-ON-TOP: while true the window floats above every other window.
    virtual bool is_always_on_top() const = 0;
    virtual void set_always_on_top(bool v) = 0;

    virtual void minimize() = 0;
    virtual void request_close() = 0;

    // Move the window by (dx,dy) pixels — called each frame while the custom title
    // bar is dragged. No-op when locked.
    virtual void drag_by(float dx, float dy) = 0;
};

}  // namespace nxrth::ui
