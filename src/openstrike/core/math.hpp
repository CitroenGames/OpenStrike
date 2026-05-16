#pragma once

#include <cmath>

namespace openstrike
{
struct Vec2
{
    float x = 0.0F;
    float y = 0.0F;
};

struct Vec4
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;
};

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

struct Quat
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;
};

struct EulerAngles
{
    float pitch_degrees = 0.0F;
    float yaw_degrees = 0.0F;
    float roll_degrees = 0.0F;
};

struct Mat3x4
{
    Vec3 axis_x{1.0F, 0.0F, 0.0F};
    Vec3 axis_y{0.0F, 1.0F, 0.0F};
    Vec3 axis_z{0.0F, 0.0F, 1.0F};
    Vec3 origin;
};

struct Aabb
{
    Vec3 mins;
    Vec3 maxs;
};

struct ColorRgba8
{
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
    unsigned char a = 255;
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

inline Vec3 operator*(float scalar, Vec3 value)
{
    return value * scalar;
}

inline Vec3& operator+=(Vec3& lhs, Vec3 rhs)
{
    lhs = lhs + rhs;
    return lhs;
}

inline Vec3& operator-=(Vec3& lhs, Vec3 rhs)
{
    lhs = lhs - rhs;
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
