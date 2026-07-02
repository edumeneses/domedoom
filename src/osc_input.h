//
// osc_input.h
//
// DomeDoom: parallel game control over OSC/UDP. Runs alongside the normal
// keyboard/mouse/joystick input — it does not replace it. A remote sender
// (e.g. a dome-side touch surface) posts OSC messages under the /domedoom/*
// namespace to drive movement, aim, action buttons, and an absolute
// "rotate to this heading" control tailored to the dome.
//
// The receiver is polled once per tic from G_BuildTiccmd (no threads, no
// locks). Axis/button state is held until refreshed, but auto-releases after
// a short watchdog so a stalled sender never leaves DoomGuy running forever.
//

#pragma once

#include <cstdint>

// Analog axes and action buttons decoded from the /domedoom/* namespace.
// Axes are in [-1, 1]; buttons is a mask of BT_* flags (see d_event.h).
struct OSCInputState
{
    float    forward = 0.f;   // + forward, - back
    float    side    = 0.f;   // + strafe right, - strafe left
    float    turn    = 0.f;   // + turn left, - turn right (matches axis_yaw sign)
    float    pitch   = 0.f;   // + look up, - look down
    float    fly     = 0.f;   // + move up, - move down

    uint32_t buttons = 0;     // BT_ATTACK, BT_USE, ... OR'd together
    bool     run     = false; // speed/run modifier

    bool     haveRotate = false; // absolute-heading request pending
    double   rotateDeg  = 0.0;   // [0, 360) target view yaw on the dome
};

// True when r_... OSC control is enabled by CVAR.
bool OSC_Input_Enabled();

// Open/refresh the UDP socket on the configured port and drain any pending
// packets into the shared state. Call once per tic. Cheap no-op when disabled.
void OSC_Input_Tick();

// Current decoded input. Only meaningful after OSC_Input_Tick() when enabled.
const OSCInputState& OSC_Input_State();

// Consume a pending absolute-heading request, if any. Returns true and writes
// *deg when a /domedoom/rotate message arrived since the last call.
bool OSC_Input_ConsumeRotate(double* deg);
