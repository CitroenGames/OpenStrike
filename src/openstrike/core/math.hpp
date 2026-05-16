#pragma once

#include <algorithm>
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

    [[nodiscard]] float length() const
    {
        return std::sqrt((x * x) + (y * y) + (z * z));
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

inline Vec3 operator/(Vec3 value, float scalar)
{
    return {value.x / scalar, value.y / scalar, value.z / scalar};
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

inline float dot(Vec3 lhs, Vec3 rhs)
{
    return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

inline Vec3 cross(Vec3 lhs, Vec3 rhs)
{
    return {
        (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.z * rhs.x) - (lhs.x * rhs.z),
        (lhs.x * rhs.y) - (lhs.y * rhs.x),
    };
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

inline Vec3 normalize(Vec3 value)
{
    const float length = value.length();
    if (length <= 0.00001F)
    {
        return {};
    }

    return value / length;
}

inline Quat normalize(Quat value)
{
    const float length = std::sqrt((value.x * value.x) + (value.y * value.y) + (value.z * value.z) + (value.w * value.w));
    if (length <= 0.00001F)
    {
        return {};
    }

    return {value.x / length, value.y / length, value.z / length, value.w / length};
}

inline Quat operator*(Quat lhs, Quat rhs)
{
    return {
        (lhs.w * rhs.x) + (lhs.x * rhs.w) + (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.w * rhs.y) - (lhs.x * rhs.z) + (lhs.y * rhs.w) + (lhs.z * rhs.x),
        (lhs.w * rhs.z) + (lhs.x * rhs.y) - (lhs.y * rhs.x) + (lhs.z * rhs.w),
        (lhs.w * rhs.w) - (lhs.x * rhs.x) - (lhs.y * rhs.y) - (lhs.z * rhs.z),
    };
}

inline Quat slerp(Quat a, Quat b, float t)
{
    t = std::clamp(t, 0.0F, 1.0F);
    float cos_theta = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
    if (cos_theta < 0.0F)
    {
        b = {-b.x, -b.y, -b.z, -b.w};
        cos_theta = -cos_theta;
    }

    if (cos_theta > 0.9995F)
    {
        return normalize(Quat{
            a.x + ((b.x - a.x) * t),
            a.y + ((b.y - a.y) * t),
            a.z + ((b.z - a.z) * t),
            a.w + ((b.w - a.w) * t),
        });
    }

    const float angle = std::acos(std::clamp(cos_theta, -1.0F, 1.0F));
    const float inv_sin = 1.0F / std::sin(angle);
    const float scale_a = std::sin((1.0F - t) * angle) * inv_sin;
    const float scale_b = std::sin(t * angle) * inv_sin;
    return {
        (a.x * scale_a) + (b.x * scale_b),
        (a.y * scale_a) + (b.y * scale_b),
        (a.z * scale_a) + (b.z * scale_b),
        (a.w * scale_a) + (b.w * scale_b),
    };
}

inline Vec3 lerp(Vec3 a, Vec3 b, float t)
{
    t = std::clamp(t, 0.0F, 1.0F);
    return a + ((b - a) * t);
}

inline Mat3x4 matrix_from_quat_position(Quat rotation, Vec3 position)
{
    rotation = normalize(rotation);
    const float xx = rotation.x * rotation.x;
    const float yy = rotation.y * rotation.y;
    const float zz = rotation.z * rotation.z;
    const float xy = rotation.x * rotation.y;
    const float xz = rotation.x * rotation.z;
    const float yz = rotation.y * rotation.z;
    const float wx = rotation.w * rotation.x;
    const float wy = rotation.w * rotation.y;
    const float wz = rotation.w * rotation.z;

    return {
        {1.0F - (2.0F * (yy + zz)), 2.0F * (xy + wz), 2.0F * (xz - wy)},
        {2.0F * (xy - wz), 1.0F - (2.0F * (xx + zz)), 2.0F * (yz + wx)},
        {2.0F * (xz + wy), 2.0F * (yz - wx), 1.0F - (2.0F * (xx + yy))},
        position,
    };
}

inline Vec3 transform_point(const Mat3x4& matrix, Vec3 point)
{
    return {
        (point.x * matrix.axis_x.x) + (point.y * matrix.axis_y.x) + (point.z * matrix.axis_z.x) + matrix.origin.x,
        (point.x * matrix.axis_x.y) + (point.y * matrix.axis_y.y) + (point.z * matrix.axis_z.y) + matrix.origin.y,
        (point.x * matrix.axis_x.z) + (point.y * matrix.axis_y.z) + (point.z * matrix.axis_z.z) + matrix.origin.z,
    };
}

inline Mat3x4 concat_transforms(const Mat3x4& parent, const Mat3x4& local)
{
    auto transform_axis = [&parent](Vec3 axis) {
        return Vec3{
            (axis.x * parent.axis_x.x) + (axis.y * parent.axis_y.x) + (axis.z * parent.axis_z.x),
            (axis.x * parent.axis_x.y) + (axis.y * parent.axis_y.y) + (axis.z * parent.axis_z.y),
            (axis.x * parent.axis_x.z) + (axis.y * parent.axis_y.z) + (axis.z * parent.axis_z.z),
        };
    };

    return {
        transform_axis(local.axis_x),
        transform_axis(local.axis_y),
        transform_axis(local.axis_z),
        transform_point(parent, local.origin),
    };
}
}
