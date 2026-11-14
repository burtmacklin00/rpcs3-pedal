#pragma once

// From the MLGB RPCS3 lightgun fork (armstrongstreener/rpcs3-lightgun): per-game aim scaling
#include <algorithm>

inline float lightgun_scale_axis(float position, float percent)
{
    // Preserve the device's off-screen sentinel coordinates.
    if (position <= 0.f || position >= 1.f || percent == 100.f) return position;
    return std::clamp(.5f + (position - .5f) * (percent / 100.f), 0.f, 1.f);
}

// Pedal build: bend only the top part of the screen. y: 0 = top, 1 = bottom.
inline float lightgun_top_bend(float y, float knee_percent, float scale_percent)
{
    if (y <= 0.f || y >= 1.f || knee_percent >= 100.f || scale_percent == 100.f) return y;
    const float up = (.5f - y) * 2.f; // 0 = centre, 1 = top edge
    const float knee = knee_percent / 100.f;
    if (up <= knee) return y;
    const float bent = knee + (up - knee) * (scale_percent / 100.f);
    return std::clamp(.5f - bent / 2.f, 0.f, 1.f);
}
