#pragma once

// From the MLGB RPCS3 lightgun fork (armstrongstreener/rpcs3-lightgun): The Shoot gesture helpers
#include <cstdint>
#include <cmath>
#include <array>
#include <atomic>

// Pedal build: last bound key each player pressed (raw mouse Key Buttons) - the bat reads the stick's direction from it
extern std::array<std::atomic<std::uint32_t>, 4> g_lightgun_last_key;

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
    // the game wants the ball held low first, then T pressed ("hold like this, then hold T"): when the bowling
    // button arms the throw, T is reported let go for regrab_us, then held again (a fresh press while held low)
    static constexpr std::uint64_t regrab_us = 200'000;
    std::uint64_t armed_since = 0;
    bool t_released_for_regrab(std::uint64_t now) const { return armed && !active && now >= armed_since && now - armed_since < regrab_us; }
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

    // follow-through after the release (the arm keeps going, so the ball leaves at full speed)
    std::uint64_t follow_us = 150'000; // "The Shoot Bowl Follow"

    // -1 = no swing, else 0..1 = how far through the swing, above 1 = follow-through (T already let go)
    float update(std::uint64_t now, bool trigger, bool bowl_button)
    {
        if (active)
        {
            if (now >= started && now - started < duration_us + follow_us) return static_cast<float>(now - started) / duration_us;
            active = false;
            armed = false;
        }
        if (trigger && bowl_button) { if (!armed) armed_since = now; armed = true; }
        else if (!trigger && armed) { armed = false; active = true; started = now; return 0.f; }
        else if (!trigger) armed = false;
        return -1.f;
    }

    // still swinging (T held); false during the follow-through
    bool swinging(std::uint64_t now) const { return active && now >= started && now - started < duration_us; }

    // speeding up all the way to the release (fastest when the ball is let go), then going on at that speed
    static float ease(float p) { return p <= 1.f ? p * p : 1.f + 2.f * (p - 1.f); }
};

// Pedal build: The Shoot's Deep Strike bat. The gun's stick (arrow keys -> Select) swings the bat.
// The bat is cocked to one side (+1) while batting and swings across to the other side (-1) fast.
// Away from batting (start screen, pause menu) it rests where the gun aims (0), so the menus can be pointed at:
// the first flick only gets the bat ready (cocks it, no swing), and some seconds after the last swing it drifts back to the aim.
struct lightgun_bat
{
    bool was_down = false;
    bool swinging = false;
    bool winding = false;
    float cock = 0.f;                  // 0 = at the aim, 1 = cocked
    int dir = 1;                       // which way the stick was flicked (1 = left, -1 = right): the bat swings that way
    float wind_from = 0.f;
    std::uint64_t started = 0;
    std::uint64_t last = 0;
    std::uint64_t cocked_until = 0;
    std::uint64_t swing_us = 120'000;  // "The Shoot Bat Time"
    static constexpr std::uint64_t wind_us = 300'000;   // wind-up from the aim (first swing only)
    static constexpr std::uint64_t hold_us = 400'000;   // stays swung through
    static constexpr std::uint64_t back_us = 700'000;   // slow return, so it does not count as a swing
    std::uint64_t stay_us = 10'000'000; // stays cocked this long after a swing ("The Shoot Bat Stay")

    // 0..1: how ready (cocked) the bat is - the height boost only applies while batting
    float ready() const { return swinging ? 1.f : (cock < 0.f ? -cock : cock); }

    static float smooth(float p) { return p * p * (3.f - 2.f * p); }

    // -1..1 (times half the sweep, times dir): where the bat is now
    float update(std::uint64_t now, bool down, int new_dir)
    {
        const std::uint64_t dt = last && now > last ? now - last : 0;
        last = now;
        if (down && !was_down && !swinging && !winding)
        {
            if (new_dir != dir) { cock = -cock; dir = new_dir; } // other side: wind up across from where it is
            if (cock >= 0.99f) { swinging = true; started = now; }
            else { winding = true; started = now; wind_from = cock; }
        }
        was_down = down;

        if (winding)
        {
            const std::uint64_t len = static_cast<std::uint64_t>(wind_us * (1.f - wind_from)) + 1; // from -1 (other side) takes twice as long
            const std::uint64_t t = now >= started ? now - started : 0;
            if (t < len) { cock = wind_from + (1.f - wind_from) * smooth(static_cast<float>(t) / len); return cock; }
            winding = false; cock = 1.f; cocked_until = now + stay_us; // ready - the next flick swings
        }
        if (swinging)
        {
            const std::uint64_t t = now >= started ? now - started : 0;
            if (t < swing_us) return 1.f - 2.f * smooth(static_cast<float>(t) / swing_us);
            if (t < swing_us + hold_us) return -1.f;
            if (t < swing_us + hold_us + back_us) return -1.f + 2.f * smooth(static_cast<float>(t - swing_us - hold_us) / back_us);
            swinging = false; cock = 1.f; cocked_until = now + stay_us;
        }
        if (now >= cocked_until && cock != 0.f) // drift back to the aim
        {
            const float step = dt / 1'500'000.f;
            cock = cock > 0.f ? (cock - step > 0.f ? cock - step : 0.f) : (cock + step < 0.f ? cock + step : 0.f);
        }
        return cock;
    }
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
