#pragma once

// From the MLGB RPCS3 lightgun fork (armstrongstreener/rpcs3-lightgun): per-game aim scaling
#include <algorithm>

inline float lightgun_scale_axis(float position, float percent)
{
    // Preserve the device's off-screen sentinel coordinates.
    if (position <= 0.f || position >= 1.f || percent == 100.f) return position;
    return std::clamp(.5f + (position - .5f) * (percent / 100.f), 0.f, 1.f);
}
