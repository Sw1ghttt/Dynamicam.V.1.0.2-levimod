#pragma once

// Dynamicam by Sw1ghttt - camera placement logic.
// Pure math: no game memory is touched here, so it can be unit tested on a PC.

#include <algorithm>
#include <cmath>

#include "mod/CameraMath.h"
#include "mod/Config.h"

namespace dynamicam {

struct ViewInput {
    int perspective = 0;        // what the GAME is using: 0 first, 1 back, 2 front
    bool faked = false;         // first person body: game was told "back", we place the camera at the face
    Vec3 eye;                   // player's eye position (interpolated, world space)
    Vec3 nativeCam;             // camera position the game computed
    float yaw = 0, pitch = 0;   // where the player looks (degrees)
    int hitType = 3;            // 0 block, 1 entity, anything else = nothing targeted
    bool hasHitPos = false;
    Vec3 hitPos;
    bool freeLook = false;
    float freeYaw = 0, freePitch = 0;
    float dt = 1.0f / 60.0f;
};

struct ViewOutput {
    Vec3 camPos;
    bool modified = false;
    bool overrideRotation = false;
    float yaw = 0, pitch = 0;
};

class ViewLogic {
  public:
    void reset() { mAim = 1.0f; }

    ViewOutput compute(const ModConfig &c, const ViewInput &in) {
        ViewOutput out;
        out.camPos = in.nativeCam;

        const bool rotOk = c.experimentalRotation;
        const bool free = rotOk && in.freeLook;
        const float viewYaw = in.yaw + (free ? in.freeYaw : 0.0f);
        const float viewPitch = std::clamp(in.pitch + (free ? in.freePitch : 0.0f), -89.0f, 89.0f);

        // Aim assist: slide the back camera to the centre while a block/mob is targeted.
        const bool targeting = in.hitType == 0 || in.hitType == 1;
        const float aimTarget = (c.backAimAssist && targeting) ? 0.0f : 1.0f;
        const float k = std::clamp(1.0f - std::exp(-12.0f * std::max(in.dt, 0.0f)), 0.0f, 1.0f);
        mAim += (aimTarget - mAim) * k;

        const Vec3 up{0.0f, 1.0f, 0.0f};

        // ---------------- First person with visible body ----------------
        if (in.faked) {
            Vec3 fwd;
            if (free) {
                fwd = forwardVec(viewYaw, viewPitch);
            } else {
                // The game placed its (fake) back camera behind us, so the eye->camera line gives
                // the exact, per-frame look direction. Fall back to yaw/pitch if a wall squashed it.
                Vec3 v = in.eye - in.nativeCam;
                const float len = v.length();
                fwd = len > 1.0f ? v * (1.0f / len) : forwardVec(in.yaw, in.pitch);
            }
            out.camPos = in.eye + fwd * std::clamp(c.firstForward, 0.0f, 0.6f) + up * c.firstHeight;
            out.modified = true;
            if (free) setRot(out, viewPitch, viewYaw);
            return out;
        }

        // ---------------- Second person ----------------
        if (in.perspective != 1 && in.perspective != 2) return out;

        const Vec3 v = in.nativeCam - in.eye;
        const float len = v.length();
        if (len < 0.05f) return out;
        const Vec3 dir = v * (1.0f / len);

        if (in.perspective == 1) {
            float d = std::clamp(c.backDistance, 1.0f, 8.0f);
            if (len < c.nativeBackDistance - 0.15f) d = std::min(d, len); // wall in the way

            Vec3 pos, right;
            if (free) {
                const Vec3 f = forwardVec(viewYaw, viewPitch);
                pos = in.eye - f * d;
                right = rightVec(viewYaw);
            } else {
                pos = in.eye + dir * d;
                right = rightOf(Vec3{-dir.x, 0.0f, -dir.z}, in.yaw);
            }

            const bool toeIn = rotOk && c.aimToeIn && in.hasHitPos && targeting && !free;
            const float side = c.backSide * (toeIn ? 1.0f : mAim);
            // Height fades with the side shift: only a camera exactly on the look line has an exact crosshair.
            pos = pos + right * side + up * (c.backHeight * (toeIn ? 1.0f : mAim));
            out.camPos = pos;
            out.modified = true;

            if (free) {
                setRot(out, viewPitch, viewYaw);
            } else if (toeIn) {
                float y = in.yaw, p = in.pitch;
                lookAt(pos, in.hitPos, y, p);
                setRot(out, p, y);
            }
            return out;
        }

        // perspective == 2 : front
        float d = std::clamp(c.frontDistance, 1.0f, 8.0f);
        if (len < c.nativeFrontDistance - 0.15f) d = std::min(d, len);

        Vec3 pos;
        if (free) {
            pos = in.eye + forwardVec(viewYaw, viewPitch) * d;
        } else {
            pos = in.eye + dir * d;
        }
        // The front camera looks back at the player, so its "forward" is minus the offset.
        const Vec3 off = pos - in.eye;
        const Vec3 right = rightOf(Vec3{-off.x, 0.0f, -off.z}, in.yaw + 180.0f);
        pos = pos + right * c.frontSide + up * c.frontHeight;
        out.camPos = pos;
        out.modified = true;
        if (free) {
            float y = in.yaw, p = in.pitch;
            lookAt(pos, in.eye, y, p);
            setRot(out, p, y);
        }
        return out;
    }

  private:
    float mAim = 1.0f;

    static void setRot(ViewOutput &o, float pitch, float yaw) {
        o.overrideRotation = true;
        o.pitch = pitch;
        o.yaw = yaw;
    }

    // Camera-right for a camera looking along horizontal vector g (falls back to yaw when looking straight up/down).
    static Vec3 rightOf(Vec3 g, float fallbackYaw) {
        const float hl = std::sqrt(g.x * g.x + g.z * g.z);
        if (hl < 1e-3f) return rightVec(fallbackYaw);
        g.x /= hl;
        g.z /= hl;
        return Vec3{-g.z, 0.0f, g.x};
    }
};

} // namespace dynamicam
