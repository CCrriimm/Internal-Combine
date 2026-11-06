#pragma once
#include <cmath>

namespace radar {
// Output is a unit disk: right is positive X, ahead is negative Y.
inline bool Project(float dx, float dz, float fx, float fz, float range, float& x, float& y) {
    if (!std::isfinite(dx) || !std::isfinite(dz) || !std::isfinite(fx) ||
        !std::isfinite(fz) || !std::isfinite(range) || range <= 0.f) return false;
    float length = std::hypot(fx, fz);
    if (length < .001f || std::hypot(dx, dz) > range) return false;
    fx /= length;
    fz /= length;
    x = (dx * fz - dz * fx) / range;
    y = -(dx * fx + dz * fz) / range;
    return std::isfinite(x) && std::isfinite(y);
}
}
