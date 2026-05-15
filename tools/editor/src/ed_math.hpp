#pragma once

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct Vec3
{
    float x = 0, y = 0, z = 0;

    Vec3 operator+(const Vec3& b) const { return { x + b.x, y + b.y, z + b.z }; }
    Vec3 operator-(const Vec3& b) const { return { x - b.x, y - b.y, z - b.z }; }
    Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
    Vec3 operator-() const { return { -x, -y, -z }; }
    Vec3& operator+=(const Vec3& b) { x += b.x; y += b.y; z += b.z; return *this; }
    Vec3& operator-=(const Vec3& b) { x -= b.x; y -= b.y; z -= b.z; return *this; }
};

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3  Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Length(const Vec3& v) { return sqrtf(Dot(v, v)); }
inline Vec3  Normalize(const Vec3& v) { float l = Length(v); return l > 0.0001f ? v * (1.0f / l) : Vec3{ 0, 0, 0 }; }

// Source engine (X=right, Y=forward, Z=up) <-> OpenGL (X=right, Y=up, Z=-forward)
inline Vec3 SourceToGL(float x, float y, float z) { return { x, z, -y }; }
inline Vec3 SourceToGL(const Vec3& v) { return { v.x, v.z, -v.y }; }
inline Vec3 GLToSource(const Vec3& v) { return { v.x, -v.z, v.y }; }

struct Mat4
{
    float m[16] = {};

    static Mat4 Identity()
    {
        Mat4 r;
        r.m[0] = 1; r.m[5] = 1; r.m[10] = 1; r.m[15] = 1;
        return r;
    }

    static Mat4 Perspective(float fovDeg, float aspect, float nearZ, float farZ)
    {
        float top = nearZ * tanf(fovDeg * (float)(M_PI / 360.0));
        float right = top * aspect;

        Mat4 r;
        r.m[0]  = nearZ / right;
        r.m[5]  = nearZ / top;
        r.m[10] = -(farZ + nearZ) / (farZ - nearZ);
        r.m[11] = -1.0f;
        r.m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
        return r;
    }

    static Mat4 LookAt(const Vec3& eye, const Vec3& center, const Vec3& up)
    {
        Vec3 f = Normalize(center - eye);
        Vec3 r = Normalize(Cross(f, up));
        Vec3 u = Cross(r, f);

        Mat4 result;
        result.m[0]  =  r.x; result.m[4]  =  r.y; result.m[8]  =  r.z;
        result.m[1]  =  u.x; result.m[5]  =  u.y; result.m[9]  =  u.z;
        result.m[2]  = -f.x; result.m[6]  = -f.y; result.m[10] = -f.z;
        result.m[3]  =  0;   result.m[7]  =  0;   result.m[11] =  0;
        result.m[12] = -Dot(r, eye);
        result.m[13] = -Dot(u, eye);
        result.m[14] =  Dot(f, eye);
        result.m[15] = 1.0f;
        return result;
    }

    Mat4 operator*(const Mat4& b) const
    {
        Mat4 r;
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++)
            {
                float sum = 0;
                for (int k = 0; k < 4; k++)
                    sum += m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = sum;
            }
        return r;
    }

    const float* Ptr() const { return m; }

    static Mat4 RotationX(float radians)
    {
        Mat4 r = Identity();
        float c = cosf(radians), s = sinf(radians);
        r.m[5] = c;  r.m[6] = s;
        r.m[9] = -s; r.m[10] = c;
        return r;
    }

    static Mat4 RotationY(float radians)
    {
        Mat4 r = Identity();
        float c = cosf(radians), s = sinf(radians);
        r.m[0] = c;  r.m[2] = -s;
        r.m[8] = s;  r.m[10] = c;
        return r;
    }

    static Mat4 RotationZ(float radians)
    {
        Mat4 r = Identity();
        float c = cosf(radians), s = sinf(radians);
        r.m[0] = c;  r.m[1] = s;
        r.m[4] = -s; r.m[5] = c;
        return r;
    }

    static Mat4 Translation(const Vec3& v)
    {
        Mat4 r = Identity();
        r.m[12] = v.x;
        r.m[13] = v.y;
        r.m[14] = v.z;
        return r;
    }

    static Mat4 Scale(float s)
    {
        Mat4 r = Identity();
        r.m[0] = s; r.m[5] = s; r.m[10] = s;
        return r;
    }

    static Mat4 Scale(float sx, float sy, float sz)
    {
        Mat4 r = Identity();
        r.m[0] = sx; r.m[5] = sy; r.m[10] = sz;
        return r;
    }
};
