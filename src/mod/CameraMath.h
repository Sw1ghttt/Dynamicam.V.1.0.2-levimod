// Dynamicam - camera math (pure C++, no game/SDK dependency)
// Created by Sw1ghttt
#pragma once
#include <cmath>

namespace dynamicam {

constexpr float kDeg2Rad = 3.14159265358979f / 180.0f;
constexpr float kRad2Deg = 180.0f / 3.14159265358979f;

struct Vec2f { float x = 0, y = 0; };

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
};

enum class ViewMode { FirstPerson = 0, SecondBack = 1, SecondFront = 2 };

struct CameraPose {
    Vec3 position;
    float yaw = 0;    // degrees, Minecraft convention (0 = +Z)
    float pitch = 0;  // degrees, positive = looking down
    float fov = 70;
};

// Minecraft convention: yaw 0 faces +Z, pitch > 0 looks down.
inline Vec3 forwardVec(float yawDeg, float pitchDeg) {
    float y = yawDeg * kDeg2Rad, p = pitchDeg * kDeg2Rad;
    return {-std::sin(y) * std::cos(p), -std::sin(p), std::cos(y) * std::cos(p)};
}

inline Vec3 rightVec(float yawDeg) {
    float y = yawDeg * kDeg2Rad;
    return {-std::cos(y), 0.0f, -std::sin(y)};
}

// Rotation that makes a camera at `from` look at `to`.
inline void lookAt(const Vec3& from, const Vec3& to, float& yawOut, float& pitchOut) {
    Vec3 d = to - from;
    float len = d.length();
    if (len < 1e-5f) return;
    yawOut = std::atan2(-d.x, d.z) * kRad2Deg;
    pitchOut = -std::asin(d.y / len) * kRad2Deg;
}

} // namespace dynamicam
