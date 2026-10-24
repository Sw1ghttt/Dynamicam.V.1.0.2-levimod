// Dynamicam by Sw1ghttt
//
// Hooks (all resolved by byte signature in libminecraftpe.so):
//   NormalTick                 -> samples the local player 20x/second
//   GetPerspective             -> first person is rendered as "back view" so your body exists,
//                                 then the camera is moved to the face
//   RenderLevel                -> final camera placement every frame
//   GetFov                     -> per-view FOV
//   LocalPlayerApplyTurnDelta  -> free-look (swallows look input while free-look is on)

#include "mod/DynamicamMod.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "mod/Signatures.h"
#include "mod/ViewLogic.h"

#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>

namespace dynamicam {

namespace {

using Clock = std::chrono::steady_clock;

// ---- Game memory layout (Minecraft 1.26.5X, arm64) - from BedrockTools ----
constexpr std::uintptr_t kLevelRendererPlayer = 0x420; // LevelRenderer  -> LevelRendererPlayer*
constexpr std::uintptr_t kCamPos = 0x61C;              // LevelRendererPlayer -> float[3]
constexpr std::uintptr_t kActorLevel = 464;            // Actor -> Level*
constexpr std::uintptr_t kActorStateVector = 0x208;    // Actor -> StateVectorComponent* (Vec3 pos at +0)
constexpr std::uintptr_t kActorRotation = 0x218;       // Actor -> ActorRotationComponent* (x=pitch, y=yaw at +0)
constexpr std::uintptr_t kLevelHitResult = 456;        // Level -> HitResult (inline)
constexpr std::uintptr_t kHitType = 24;                // HitResult::mType
constexpr std::uintptr_t kHitPos = 44;                 // HitResult::mPos (Vec3)

constexpr const char *kMinecraftLib = "libminecraftpe.so";
constexpr const char *kModuleId = "dynamicam";
constexpr const char *kFreeLookButton = "dynamicam.freelook";
constexpr float kTickSeconds = 0.05f;
constexpr float kFreshSeconds = 0.25f;

bool plausible(std::uintptr_t p) { return p > 0x10000 && p < 0x0000FFFFFFFFFFFFull; }

// Reads game memory without ever crashing: the bytes are pushed through a pipe, and the kernel
// reports EFAULT instead of delivering a SIGSEGV if the address is not mapped.
bool safeRead(std::uintptr_t addr, void *out, std::size_t n) {
    if (!plausible(addr) || n == 0 || n > 64) return false;
    thread_local int fds[2] = {-1, -1};
    if (fds[0] < 0) {
        if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
            fds[0] = fds[1] = -1;
            return false;
        }
    }
    const ssize_t w = write(fds[1], reinterpret_cast<const void *>(addr), n);
    if (w != static_cast<ssize_t>(n)) {
        // drain anything partial so the pipe stays empty
        char junk[64];
        while (read(fds[0], junk, sizeof junk) > 0) {
        }
        return false;
    }
    return read(fds[0], out, n) == static_cast<ssize_t>(n);
}

template <class T> bool readMem(std::uintptr_t addr, T &out) {
    T tmp{};
    if (!safeRead(addr, &tmp, sizeof(T))) return false;
    out = tmp;
    return true;
}

// ---- Shared state ----
struct PlayerSnapshot {
    bool valid = false;
    Vec3 prev, cur;
    float yaw = 0, pitch = 0;
    int hitType = 3;
    bool hasHitPos = false;
    Vec3 hitPos;
    std::uintptr_t rotComp = 0;
    Clock::time_point tickTime{};
};

std::mutex gMutex;
ModConfig gConfig;
PlayerSnapshot gSnap;
ViewLogic gLogic;

std::atomic<int> gNativePerspective{0};
std::atomic<bool> gFaked{false};

// eye height calibration
std::atomic<bool> gCalibrated{false};
float gEyeDelta = 0.0f;
float gCalibSum = 0.0f;
int gCalibCount = 0;
Clock::time_point gFirstSnapshot{};
bool gHaveFirstSnapshot = false;

// last camera we wrote, so we never scale our own output twice
Vec3 gLastWritten, gNativeCam;
bool gHaveLast = false;
Clock::time_point gLastRender{};

// diagnostics
std::atomic<bool> gHookOk[5];            // T P R F L
std::atomic<unsigned> gCalls[3];         // tick, perspective, render
std::atomic<unsigned> gFail[3];          // tick: component ptr / value read / not finite
std::atomic<bool> gSnapFresh{false};
Clock::time_point gLastStatus{};

// perspective transition (easing)
int gModeKey = -1;
Vec3 gFrom, gLastOut;
bool gHaveOut = false;
float gTrans = 1.0f;

// free look
std::atomic<bool> gFreeLook{false};
float gFreeYaw = 0, gFreePitch = 0;

// hooks
using TickFn = void (*)(void *);
using PerspectiveFn = int (*)(void *);
using FovFn = float (*)(void *, float, int);
using RenderLevelFn = void (*)(void *, void *, void *);
using TurnDeltaFn = void (*)(void *, Vec2f *);

TickFn gTickOrig = nullptr;
PerspectiveFn gPerspectiveOrig = nullptr;
FovFn gFovOrig = nullptr;
RenderLevelFn gRenderOrig = nullptr;
TurnDeltaFn gTurnOrig = nullptr;

pl::memory::HookHandle gTickHook, gPerspectiveHook, gFovHook, gRenderHook, gTurnHook;

ModConfig configCopy() {
    std::lock_guard lock(gMutex);
    return gConfig;
}

bool snapshotFresh(const PlayerSnapshot &s, Clock::time_point now) {
    return s.valid && std::chrono::duration<float>(now - s.tickTime).count() < kFreshSeconds;
}

Vec3 interpolatedEye(const PlayerSnapshot &s, Clock::time_point now) {
    float t = std::chrono::duration<float>(now - s.tickTime).count() / kTickSeconds;
    t = std::clamp(t, 0.0f, 1.0f);
    return s.prev + (s.cur - s.prev) * t;
}

bool eyeReady(Clock::time_point now) {
    if (gCalibrated.load()) return true;
    std::lock_guard lock(gMutex);
    // never saw native first person: assume the configured fallback after 5 s in a world
    return gHaveFirstSnapshot && std::chrono::duration<float>(now - gFirstSnapshot).count() > 5.0f;
}

float eyeDelta(const ModConfig &c) { return gCalibrated.load() ? gEyeDelta : c.eyeHeightFallback; }

// ---------------------------------------------------------------- hooks

void tickDetour(void *player) {
    if (gTickOrig) gTickOrig(player);
    gCalls[0].fetch_add(1);
    if (!plausible(reinterpret_cast<std::uintptr_t>(player))) return;
    const auto base = reinterpret_cast<std::uintptr_t>(player);

    std::uintptr_t sv = 0, rc = 0, level = 0;
    if (!readMem(base + kActorStateVector, sv) || !readMem(base + kActorRotation, rc)) {
        gFail[0].fetch_add(1);
        return;
    }
    Vec3 pos;
    Vec2f rot;
    if (!readMem(sv, pos) || !readMem(rc, rot)) {
        gFail[1].fetch_add(1);
        return;
    }
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
        gFail[2].fetch_add(1);
        return;
    }

    int hitType = 3;
    bool hasHitPos = false;
    Vec3 hitPos;
    if (readMem(base + kActorLevel, level) && plausible(level)) {
        const auto hit = level + kLevelHitResult;
        int t = 3;
        if (readMem(hit + kHitType, t)) hitType = t;
        if ((hitType == 0 || hitType == 1) && readMem(hit + kHitPos, hitPos) && std::isfinite(hitPos.x))
            hasHitPos = true;
    }

    const auto now = Clock::now();
    std::lock_guard lock(gMutex);
    PlayerSnapshot &s = gSnap;
    if (!s.valid || (s.cur - pos).length() > 8.0f) {
        s.prev = pos; // first sample or teleport
    } else {
        s.prev = s.cur;
    }
    s.cur = pos;
    s.yaw = rot.y;
    s.pitch = rot.x;
    s.hitType = hitType;
    s.hasHitPos = hasHitPos;
    s.hitPos = hitPos;
    s.rotComp = rc;
    s.tickTime = now;
    s.valid = true;
    if (!gHaveFirstSnapshot) {
        gHaveFirstSnapshot = true;
        gFirstSnapshot = now;
    }
}

int perspectiveDetour(void *self) {
    gCalls[1].fetch_add(1);
    const int native = gPerspectiveOrig ? gPerspectiveOrig(self) : 0;
    gNativePerspective.store(native);

    const ModConfig c = configCopy();
    bool fake = false;
    if (c.enabled && c.firstPersonBody && native == 0) {
        PlayerSnapshot s;
        {
            std::lock_guard lock(gMutex);
            s = gSnap;
        }
        const auto now = Clock::now();
        fake = snapshotFresh(s, now) && eyeReady(now);
    }
    gFaked.store(fake);
    return fake ? 1 : native;
}

float fovDetour(void *self, float a, int b) {
    float r = gFovOrig ? gFovOrig(self, a, b) : 70.0f;
    const ModConfig c = configCopy();
    if (!c.enabled) return r;
    float want = 0.0f;
    switch (gNativePerspective.load()) {
    case 0: want = c.firstFov; break;
    case 1: want = c.backFov; break;
    case 2: want = c.frontFov; break;
    }
    return want >= 20.0f ? std::min(want, 150.0f) : r;
}

void turnDeltaDetour(void *self, Vec2f *delta) {
    if (delta && gFreeLook.load()) {
        const ModConfig c = configCopy();
        if (c.enabled && c.experimentalRotation) {
            const float yawIn = c.freeLookSwapAxes ? delta->y : delta->x;
            const float pitchIn = c.freeLookSwapAxes ? delta->x : delta->y;
            gFreeYaw += yawIn * c.freeLookSpeed;
            gFreePitch = std::clamp(gFreePitch + pitchIn * c.freeLookSpeed, -85.0f, 85.0f);
            delta->x = 0.0f; // the player (and the emote) must not turn
            delta->y = 0.0f;
        }
    }
    if (gTurnOrig) gTurnOrig(self, delta);
}

struct RotationGuard {
    std::uintptr_t comp = 0;
    Vec2f wrote{};
    Vec2f old{};
    bool active = false;

    void apply(std::uintptr_t rotComp, float pitch, float yaw) {
        Vec2f cur{};
        if (!readMem(rotComp, cur)) return;
        comp = rotComp;
        old = cur;
        wrote = Vec2f{pitch, yaw};
        *reinterpret_cast<Vec2f *>(comp) = wrote;
        active = true;
    }
    ~RotationGuard() {
        if (!active) return;
        Vec2f cur{};
        // only restore if the game did not change it in the meantime
        if (readMem(comp, cur) && cur.x == wrote.x && cur.y == wrote.y) *reinterpret_cast<Vec2f *>(comp) = old;
    }
};

void adjustCamera(void *levelRenderer, const ModConfig &c, RotationGuard &guard) {
    std::uintptr_t lrp = 0;
    if (!readMem(reinterpret_cast<std::uintptr_t>(levelRenderer) + kLevelRendererPlayer, lrp)) return;
    if (!plausible(lrp)) return;
    const std::uintptr_t camAddr = lrp + kCamPos;

    const auto now = Clock::now();
    PlayerSnapshot s;
    {
        std::lock_guard lock(gMutex);
        s = gSnap;
    }
    gSnapFresh.store(snapshotFresh(s, now));
    if (!gSnapFresh.load()) {
        gHaveLast = false;
        gHaveOut = false;
        gTrans = 1.0f;
        gLogic.reset();
        return;
    }

    float dt = gLastRender.time_since_epoch().count() == 0
                   ? 1.0f / 60.0f
                   : std::chrono::duration<float>(now - gLastRender).count();
    gLastRender = now;
    dt = std::clamp(dt, 0.0f, 0.1f);

    Vec3 seen;
    if (!readMem(camAddr, seen)) return;
    auto *cam = reinterpret_cast<float *>(camAddr);
    if (!std::isfinite(seen.x) || !std::isfinite(seen.y) || !std::isfinite(seen.z)) return;

    // If the game did not rewrite the camera this frame, what we see is our own output.
    const bool ours = gHaveLast && (seen - gLastWritten).length() < 1e-4f;
    if (!ours) gNativeCam = seen;

    const bool faked = gFaked.load();
    const int perspective = gNativePerspective.load();
    Vec3 eye = interpolatedEye(s, now);

    // Auto-calibrate eye height while the game is in REAL first person (we are not faking yet).
    if (!faked && perspective == 0 && !gCalibrated.load()) {
        const Vec3 d = gNativeCam - eye;
        if (std::fabs(d.x) < 0.6f && std::fabs(d.z) < 0.6f && std::fabs(d.y) < 3.0f) {
            gCalibSum += d.y;
            if (++gCalibCount >= 20) {
                gEyeDelta = gCalibSum / static_cast<float>(gCalibCount);
                gCalibrated.store(true);
            }
        }
        return;
    }
    eye.y += eyeDelta(c);

    ViewInput in;
    in.perspective = perspective;
    in.faked = faked;
    in.eye = eye;
    in.nativeCam = gNativeCam;
    in.yaw = s.yaw;
    in.pitch = s.pitch;
    in.hitType = s.hitType;
    in.hasHitPos = s.hasHitPos;
    in.hitPos = s.hitPos;
    in.freeLook = gFreeLook.load();
    in.freeYaw = gFreeYaw;
    in.freePitch = gFreePitch;
    in.dt = dt;

    const ViewOutput out = gLogic.compute(c, in);
    Vec3 target = out.modified ? out.camPos : gNativeCam;

    // Perspective changed -> glide from where the camera was to where it should be.
    if (gModeKey != perspective) {
        if (gModeKey >= 0 && gHaveOut && c.transitionSeconds > 0.01f && (gLastOut - target).length() > 0.02f) {
            gFrom = gLastOut;
            gTrans = 0.0f;
        }
        gModeKey = perspective;
    }

    bool write = out.modified;
    Vec3 finalPos = target;
    if (gTrans < 1.0f) {
        gTrans = std::min(1.0f, gTrans + dt / std::max(c.transitionSeconds, 0.05f));
        const float t = gTrans;
        const float e = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); // smootherstep ease-in-out
        finalPos = gFrom + (target - gFrom) * e;
        write = true;
    }
    gLastOut = finalPos;
    gHaveOut = true;

    if (!write) {
        gHaveLast = false;
        return;
    }
    cam[0] = finalPos.x;
    cam[1] = finalPos.y;
    cam[2] = finalPos.z;
    gLastWritten = finalPos;
    gHaveLast = true;

    if (out.overrideRotation && c.experimentalRotation && gTrans >= 1.0f)
        guard.apply(s.rotComp, out.pitch, out.yaw);
}

// Writes status.txt once a second (no UI calls from the render thread - those are not safe).
std::string gStatusPaths[2];

void drawStatus(const ModConfig &c) {
    if (!c.showStatus) return;
    const auto now = Clock::now();
    if (std::chrono::duration<float>(now - gLastStatus).count() < 1.0f) return;
    gLastStatus = now;

    const auto ok = [](int i) { return gHookOk[i].load() ? "ok" : "MISSING"; };
    char buf[900];
    std::snprintf(buf, sizeof buf,
                  "Dynamicam status\n"
                  "hooks: tick=%s perspective=%s render=%s fov=%s turn=%s\n"
                  "calls: tick=%u perspective=%u render=%u\n"
                  "tick read failures: ptr=%u value=%u notfinite=%u\n"
                  "player data fresh: %s\n"
                  "game view: %d (0 first, 1 back, 2 front)\n"
                  "body mode on: %s\n"
                  "eye calibrated: %s (delta %.3f)\n"
                  "easing: %s\n",
                  ok(0), ok(1), ok(2), ok(3), ok(4), gCalls[0].load(), gCalls[1].load(), gCalls[2].load(),
                  gFail[0].load(), gFail[1].load(), gFail[2].load(), gSnapFresh.load() ? "yes" : "NO",
                  gNativePerspective.load(), gFaked.load() ? "yes" : "no", gCalibrated.load() ? "yes" : "no",
                  eyeDelta(c), gTrans < 1.0f ? "playing" : "idle");
    for (const auto &path : gStatusPaths) {
        if (path.empty()) continue;
        std::ofstream f(path, std::ios::trunc);
        if (f) f << buf;
    }
}

void renderLevelDetour(void *self, void *screenContext, void *a3) {
    RotationGuard guard;
    gCalls[2].fetch_add(1);
    const ModConfig c = configCopy();
    if (c.enabled && self) adjustCamera(self, c, guard);
    drawStatus(c);
    if (gRenderOrig) gRenderOrig(self, screenContext, a3);
}

// ---------------------------------------------------------------- menu

enum class Kind { Toggle, Slider };

struct MenuItem {
    const char *key;
    const char *label;
    Kind kind;
    bool ModConfig::*b;
    float ModConfig::*f;
    const char *min;
    const char *max;
};

const std::vector<MenuItem> &menuItems() {
    static const std::vector<MenuItem> items = {
        {"firstPersonBody", "First person: show body", Kind::Toggle, &ModConfig::firstPersonBody, nullptr, "", ""},
        {"firstForward", "First person: forward offset", Kind::Slider, nullptr, &ModConfig::firstForward, "0", "0.6"},
        {"firstHeight", "First person: height", Kind::Slider, nullptr, &ModConfig::firstHeight, "-0.5", "0.5"},
        {"firstFov", "First person: FOV (0 = default)", Kind::Slider, nullptr, &ModConfig::firstFov, "0", "150"},

        {"backDistance", "Back view: distance", Kind::Slider, nullptr, &ModConfig::backDistance, "1", "8"},
        {"backSide", "Back view: side shift", Kind::Slider, nullptr, &ModConfig::backSide, "-2", "2"},
        {"backHeight", "Back view: height", Kind::Slider, nullptr, &ModConfig::backHeight, "-1", "2"},
        {"backFov", "Back view: FOV (0 = default)", Kind::Slider, nullptr, &ModConfig::backFov, "0", "150"},
        {"backAimAssist", "Back view: aim assist", Kind::Toggle, &ModConfig::backAimAssist, nullptr, "", ""},

        {"frontDistance", "Front view: distance", Kind::Slider, nullptr, &ModConfig::frontDistance, "1", "8"},
        {"frontSide", "Front view: side shift", Kind::Slider, nullptr, &ModConfig::frontSide, "-2", "2"},
        {"frontHeight", "Front view: height", Kind::Slider, nullptr, &ModConfig::frontHeight, "-1", "2"},
        {"frontFov", "Front view: FOV (0 = default)", Kind::Slider, nullptr, &ModConfig::frontFov, "0", "150"},

        {"experimentalRotation", "Experimental: free-look / rotation", Kind::Toggle,
         &ModConfig::experimentalRotation, nullptr, "", ""},
        {"aimToeIn", "Experimental: aim toe-in", Kind::Toggle, &ModConfig::aimToeIn, nullptr, "", ""},
        {"freeLookSpeed", "Free-look speed", Kind::Slider, nullptr, &ModConfig::freeLookSpeed, "0.1", "5"},
        {"freeLookSwapAxes", "Free-look: swap axes", Kind::Toggle, &ModConfig::freeLookSwapAxes, nullptr, "", ""},
        {"transitionSeconds", "Perspective easing (seconds)", Kind::Slider, nullptr, &ModConfig::transitionSeconds, "0", "1.5"},
        {"showStatus", "Show status overlay", Kind::Toggle, &ModConfig::showStatus, nullptr, "", ""},
        {"eyeHeightFallback", "Eye height fallback", Kind::Slider, nullptr, &ModConfig::eyeHeightFallback, "-2", "2"},
    };
    return items;
}

std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}


} // namespace

// ---------------------------------------------------------------- lifecycle

DynamicamMod &DynamicamMod::instance() {
    static DynamicamMod instance;
    return instance;
}

DynamicamMod::DynamicamMod() : mSelf(*ll::mod::NativeMod::current()) {}

bool DynamicamMod::load() {
    auto &self = getSelf();
    std::error_code ec;
    std::filesystem::create_directories(self.getDataDir(), ec);
    std::filesystem::create_directories(self.getConfigDir(), ec);
    if (ec) {
        self.getLogger().error("Failed to create directories: {}", ec.message());
        return false;
    }

    mConfigFile.emplace();
    if (!mConfigFile->load()) {
        self.getLogger().warn("Failed to load config, using defaults");
    }
    {
        std::lock_guard lock(gMutex);
        gConfig = mConfigFile->value();
    }
    self.getLogger().info("Dynamicam {} by {} loaded", self.getVersion(), self.getAuthor());
    return true;
}

bool DynamicamMod::enable() {
    auto &log = getSelf().getLogger();
    gStatusPaths[0] = "/storage/emulated/0/Android/media/org.levimc.launcher/Dynamicam_status.txt";
    gStatusPaths[1] = (getSelf().getDataDir() / "status.txt").string();
    installHooks();
    registerMenu();
    log.info("Dynamicam enabled");
    return true;
}

bool DynamicamMod::disable() {
    pl::modmenu::unregisterButton(kFreeLookButton);
    pl::modmenu::unregisterModule(kModuleId);
    removeHooks();
    return true;
}

bool DynamicamMod::unload() {
    mConfigFile.reset();
    return true;
}

void DynamicamMod::installHooks() {
    auto &log = getSelf().getLogger();
    removeHooks();

    auto install = [&](const char *name, const char *signature, void *detour, void **original,
                       pl::memory::HookHandle &handle) {
        const std::uintptr_t addr = pl::memory::resolveSignature(signature, kMinecraftLib);
        if (!addr) {
            log.error("Signature for {} not found (this Minecraft build is not supported yet)", name);
            return false;
        }
        handle = pl::memory::HookHandle(reinterpret_cast<void *>(addr), detour, original);
        if (!handle.installed()) {
            log.error("Failed to hook {} at 0x{:x}", name, addr);
            return false;
        }
        log.info("Hooked {} at 0x{:x}", name, addr);
        return true;
    };

    for (auto &f : gHookOk) f.store(false);
    gHookOk[0] = install("NormalTick", sig::kNormalTick, reinterpret_cast<void *>(&tickDetour),
            reinterpret_cast<void **>(&gTickOrig), gTickHook);
    gHookOk[1] = install("GetPerspective", sig::kGetPerspective, reinterpret_cast<void *>(&perspectiveDetour),
            reinterpret_cast<void **>(&gPerspectiveOrig), gPerspectiveHook);
    gHookOk[2] = install("RenderLevel", sig::kRenderLevel, reinterpret_cast<void *>(&renderLevelDetour),
            reinterpret_cast<void **>(&gRenderOrig), gRenderHook);
    gHookOk[3] = install("GetFov", sig::kGetFov, reinterpret_cast<void *>(&fovDetour),
            reinterpret_cast<void **>(&gFovOrig), gFovHook);
    gHookOk[4] = install("LocalPlayerApplyTurnDelta", sig::kLocalPlayerApplyTurnDelta,
            reinterpret_cast<void *>(&turnDeltaDetour), reinterpret_cast<void **>(&gTurnOrig), gTurnHook);
}

void DynamicamMod::removeHooks() {
    gFaked.store(false);
    gFreeLook.store(false);
    gTickHook.reset();
    gPerspectiveHook.reset();
    gRenderHook.reset();
    gFovHook.reset();
    gTurnHook.reset();
    gTickOrig = nullptr;
    gPerspectiveOrig = nullptr;
    gRenderOrig = nullptr;
    gFovOrig = nullptr;
    gTurnOrig = nullptr;
    gHaveLast = false;
    gHaveOut = false;
    gModeKey = -1;
    gTrans = 1.0f;
}

void DynamicamMod::registerMenu() {
    const ModConfig cfg = configCopy();

    pl::modmenu::ModuleBuilder builder(kModuleId, "Dynamicam");
    builder.description("Full-body first person, side back view, front view and free-look. By Sw1ghttt.")
        .modId(getSelf().getId())
        .defaultEnabled(cfg.enabled);

    for (const auto &item : menuItems()) {
        if (item.kind == Kind::Toggle) {
            builder.config(item.key, item.label, pl::modmenu::ConfigType::Toggle,
                           (cfg.*(item.b)) ? "true" : "false");
        } else {
            builder.config(item.key, item.label, pl::modmenu::ConfigType::SliderFloat, num(cfg.*(item.f)),
                           item.min, item.max);
        }
    }

    builder
        .onToggle([this](std::string_view, bool enabled) {
            {
                std::lock_guard lock(gMutex);
                gConfig.enabled = enabled;
            }
            if (mConfigFile) {
                mConfigFile->value() = configCopy();
                mConfigFile->save();
            }
        })
        .onConfigChanged([this](std::string_view, std::string_view key, std::string_view value) {
            const std::string v(value);
            {
                std::lock_guard lock(gMutex);
                for (const auto &item : menuItems()) {
                    if (key != item.key) continue;
                    if (item.kind == Kind::Toggle) {
                        gConfig.*(item.b) = (v == "true" || v == "1");
                    } else {
                        char *end = nullptr;
                        const float f = std::strtof(v.c_str(), &end);
                        if (end != v.c_str() && std::isfinite(f)) gConfig.*(item.f) = f;
                    }
                    break;
                }
            }
            if (mConfigFile) {
                mConfigFile->value() = configCopy();
                mConfigFile->save();
            }
        });

    if (!builder.registerModule()) {
        getSelf().getLogger().warn("Could not register the Dynamicam menu module");
    }

    const bool buttonOk = pl::modmenu::ButtonBuilder(kFreeLookButton, "Free-look")
        .moduleId(kModuleId)
        .modId(getSelf().getId())
        .label("Look")
        .behavior(pl::modmenu::ButtonBehavior::Toggle)
        .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
        .onEvent([this](std::string_view, pl::modmenu::ButtonEvent ev, float value) {
            if (ev != pl::modmenu::ButtonEvent::StateChanged) return;
            const bool on = value > 0.5f;
            if (on && !configCopy().experimentalRotation) {
                getSelf().getLogger().warn(
                    "Free-look needs 'Experimental: free-look / rotation' turned on in the Dynamicam menu");
                return;
            }
            if (!on) {
                gFreeYaw = 0.0f;
                gFreePitch = 0.0f;
            }
            gFreeLook.store(on);
        })
        .registerButton();
    if (!buttonOk) getSelf().getLogger().warn("Could not register the Free-look button");
}

} // namespace dynamicam
