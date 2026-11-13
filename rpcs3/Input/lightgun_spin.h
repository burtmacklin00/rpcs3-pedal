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
// with T held, an underarm swing forward, then T let go. With a gun: point down (Triangle input) and hold the
// trigger, then let go of the trigger - the swing plays out first (trigger still reported held), then T is released.
struct lightgun_bowl
{
    static constexpr std::uint64_t duration_us = 450'000;
    bool armed = false;
    bool active = false;
    std::uint64_t started = 0;

    // -1 = no swing, else 0..1 = how far through the swing
    float update(std::uint64_t now, bool trigger, bool pointing_down)
    {
        if (active)
        {
            if (now >= started && now - started < duration_us) return static_cast<float>(now - started) / duration_us;
            active = false;
            armed = false;
        }
        if (trigger && pointing_down) armed = true;
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
