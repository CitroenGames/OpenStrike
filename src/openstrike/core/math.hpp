#pragma once

#include <cmath>

namespace openstrike
{
struct Vec3
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    [[nodiscard]] float length_2d() const
    {
        return std::sqrt((x * x) + (y * y));
    }
};

inline Vec3 operator+(Vec3 lhs, Vec3 rhs)
{
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

inline Vec3 operator-(Vec3 lhs, Vec3 rhs)
{
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

inline Vec3 operator*(Vec3 value, float scalar)
{
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

inline Vec3& operator+=(Vec3& lhs, Vec3 rhs)
{
    lhs = lhs + rhs;
    return lhs;
}

inline float dot_2d(Vec3 lhs, Vec3 rhs)
{
    return (lhs.x * rhs.x) + (lhs.y * rhs.y);
}

inline Vec3 normalize_2d(Vec3 value)
{
    const float length = value.length_2d();
    if (length <= 0.00001F)
    {
        return {};
    }

    return {value.x / length, value.y / length, 0.0F};
}
}

