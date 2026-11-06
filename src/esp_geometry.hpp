#pragma once
#include <algorithm>
#include <cmath>

namespace esp {
struct Point { float x, y; };
inline Point LineOrigin(float width, float height, unsigned origin) {
    return {width * .5f, origin == 1 ? 0.f : origin == 2 ? height * .5f : height};
}
inline Point LineTarget(float center_x, float top, float bottom, unsigned origin) {
    return {center_x, origin == 1 || origin == 2 ? top : bottom};
}
inline bool Arrow(float width, float height, float dx, float dy, Point& tip, Point& direction) {
    if (!std::isfinite(dx) || !std::isfinite(dy) || width <= 64.f || height <= 64.f) return false;
    float length = std::hypot(dx, dy);
    if (length < .001f) return false;
    direction = {dx / length, dy / length};
    float rx = width * .5f - 28.f, ry = height * .5f - 28.f;
    float scale = std::min(rx / std::max(std::abs(direction.x), .0001f),
                           ry / std::max(std::abs(direction.y), .0001f));
    tip = {width * .5f + direction.x * scale, height * .5f + direction.y * scale};
    return true;
}
}
