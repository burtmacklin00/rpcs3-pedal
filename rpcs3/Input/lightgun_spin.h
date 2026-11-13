#pragma once

// From the MLGB RPCS3 lightgun fork (armstrongstreener/rpcs3-lightgun): The Shoot gesture helpers
#include <cstdint>
#include <cmath>
#include <array>

// One complete turn per press; holding the input does not repeat it.
struct lightgun_spin
{
    static constexpr std::uint64_t duration_us = 600'000;
    // Horizontal arm orbit, in millimetres, relative to the starting sphere.
    static constexpr float orbit_radius_mm = 600.f;
    static std::array<float, 2> orbit(float angle)
    {
        return {orbit_radius_mm * std::sin(angle), orbit_radius_mm * (std::cos(angle) - 1.f)};
    }
    bool held = false;
    bool active = false;
    std::uint64_t started = 0;
    std::uint64_t last_time = 0;

    float update(std::uint64_t now, bool pressed)
    {
        if (now < last_time) { active = false; held = pressed; }
        last_time = now;
        if (pressed && !held && !active) { active = true; started = now; }
        held = pressed;
        if (!active) return 0.f;
        const auto elapsed = now - started;
        if (elapsed >= duration_us) { active = false; return 0.f; }
        return 6.283185307179586f * static_cast<float>(elapsed) / duration_us;
    }
};

// Pedal build: The Shoot's bowling challenge (Graveyard Alley) wants the Move held low and pointing down
// with T held, an underarm swing forward, then T let go. With a gun: pull and hold the trigger (half a second or
// more), then press and hold right click (the spin button - after holding the trigger that long it does not spin) - the Move is held low and
// pointing down - then let go of the trigger: the swing plays out first (trigger still reported held), then T is
// released. Circle works as the bowling button too. Right click without the trigger held still spins, so no other
// mode changes.
struct lightgun_bowl
{
    std::uint64_t duration_us = 450'000; // "The Shoot Bowl Time"
    // what the swing is doing to the Move right now (also used for the camera image state)
    bool shaping = false;  // holding the ball or swinging
    float dy = 0.f, dz = 0.f;
    bool armed = false;
    bool active = false;
    std::uint64_t started = 0;
    // right click (the spin button) pressed after the trigger has been held for a while (hold_us) = bowling
    // button, not a spin, until right click is let go. A normal shot is a short pull, so right click during
    // shooting (even with the trigger still down from a shot) stays the Showtime spin.
    static constexpr std::uint64_t hold_us = 500'000;
    bool spin_button_latched = false;
    bool spin_button_was_down = false;
    bool trigger_was_down = false;
    std::uint64_t trigger_down_since = 0;
    void update_spin_button(std::uint64_t now, bool spin_button, bool trigger)
    {
        if (trigger && !trigger_was_down) trigger_down_since = now;
        trigger_was_down = trigger;
        const bool held_long = trigger && now >= trigger_down_since && now - trigger_down_since >= hold_us;
        if (!spin_button) spin_button_latched = false;
        else if (!spin_button_was_down && held_long) spin_button_latched = true;
        spin_button_was_down = spin_button;
    }

    // -1 = no swing, else 0..1 = how far through the swing
    float update(std::uint64_t now, bool trigger, bool bowl_button)
    {
        if (active)
        {
            if (now >= started && now - started < duration_us) return static_cast<float>(now - started) / duration_us;
            active = false;
            armed = false;
        }
        if (trigger && bowl_button) armed = true;
        else if (!trigger && armed) { armed = false; active = true; started = now; return 0.f; }
        else if (!trigger) armed = false;
        return -1.f;
    }

    // smooth start and stop, fastest in the middle (like an arm)
    static float ease(float p) { return 0.5f - 0.5f * std::cos(3.14159265358979f * p); }
};

// Decide on the press edge, so crossing the screen border cannot change actions.
struct lightgun_middle_gesture
{
    bool held = false;
    bool point_down = false;
    void update(bool pressed, bool offscreen, bool allow_point_down)
    {
        if (!pressed) point_down = false;
        else if (!held) point_down = allow_point_down && offscreen;
        held = pressed;
    }
};

// Explicit floor input takes priority over the off-screen air-shot pose.
inline int lightgun_vertical_direction(bool spinning, bool point_down, bool offscreen, bool allow_point_up)
{
    if (spinning) return 0;
    if (point_down) return -1;
    return offscreen && allow_point_up ? 1 : 0;
}
