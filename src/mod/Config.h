#pragma once

// Dynamicam by Sw1ghttt - typed config (saved to config/config.json).

#include <string>
#include <string_view>

#include <pl/Config.hpp>

namespace dynamicam {

struct ModConfig {
    int version = 1;
    bool enabled = true;

    // ---- First person (full body visible, camera at the face) ----
    bool firstPersonBody = true;
    float firstForward = 0.18f;  // blocks the camera sits in front of the eye (hides the head)
    float firstHeight = 0.0f;    // extra blocks up/down
    float firstFov = 0.0f;       // 0 = game default

    // ---- Second person BACK ----
    float backDistance = 4.0f;   // blocks behind the player (max 8)
    float backSide = 0.45f;      // + = camera slides to the right of the screen
    float backHeight = 0.1f;
    float backFov = 0.0f;        // 0 = game default
    bool backAimAssist = true;   // slide back to centre while pointing at a block/mob so the crosshair is exact

    // ---- Second person FRONT ----
    float frontDistance = 4.0f;
    float frontSide = 0.0f;
    float frontHeight = 0.1f;
    float frontFov = 0.0f;

    // ---- Free-look (for emotes) + exact aim. Experimental: rotates the view itself. ----
    bool experimentalRotation = false;
    bool aimToeIn = false;       // needs experimentalRotation: camera turns to look at the target block
    float freeLookSpeed = 1.0f;
    bool freeLookSwapAxes = false;

    // ---- Look & feel ----
    float transitionSeconds = 0.35f; // smooth camera glide when the perspective changes (0 = off)
    bool showStatus = true;          // small on-screen diagnostics (turn off once it works)

    // ---- Advanced ----
    float eyeHeightFallback = 0.0f;  // only used if auto-calibration never ran
    float nativeBackDistance = 4.0f;  // distance the game itself uses (for wall-collision detection)
    float nativeFrontDistance = 4.0f;
    bool debugLog = false;
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace dynamicam

template <> struct pl::config::Schema<dynamicam::ModConfig> {
    static constexpr std::string_view title = "Dynamicam Config";
    static constexpr std::string_view description = "Dynamicam by Sw1ghttt";

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Version", .readOnly = true};
        if (name == "enabled")
            return {.title = "Enabled", .description = "Master switch for Dynamicam."};
        if (name == "firstPersonBody")
            return {.title = "First person: show body",
                    .description = "See your hands, torso and feet in first person. The head stays hidden."};
        if (name == "firstForward")
            return {.title = "First person: forward offset", .minimum = 0.0, .maximum = 0.6};
        if (name == "firstHeight")
            return {.title = "First person: height offset", .minimum = -0.5, .maximum = 0.5};
        if (name == "firstFov")
            return {.title = "First person: FOV (0 = default)", .minimum = 0.0, .maximum = 150.0};
        if (name == "backDistance")
            return {.title = "Back view: distance", .minimum = 1.0, .maximum = 8.0};
        if (name == "backSide")
            return {.title = "Back view: side shift", .minimum = -2.0, .maximum = 2.0};
        if (name == "backHeight")
            return {.title = "Back view: height", .minimum = -1.0, .maximum = 2.0};
        if (name == "backFov")
            return {.title = "Back view: FOV (0 = default)", .minimum = 0.0, .maximum = 150.0};
        if (name == "backAimAssist")
            return {.title = "Back view: aim assist",
                    .description = "Centres the camera while you point at a block or mob so the crosshair is exact."};
        if (name == "frontDistance")
            return {.title = "Front view: distance", .minimum = 1.0, .maximum = 8.0};
        if (name == "frontSide")
            return {.title = "Front view: side shift", .minimum = -2.0, .maximum = 2.0};
        if (name == "frontHeight")
            return {.title = "Front view: height", .minimum = -1.0, .maximum = 2.0};
        if (name == "frontFov")
            return {.title = "Front view: FOV (0 = default)", .minimum = 0.0, .maximum = 150.0};
        if (name == "experimentalRotation")
            return {.title = "Experimental: camera rotation",
                    .description = "Lets Dynamicam rotate the view (free-look for emotes, aim toe-in)."};
        if (name == "aimToeIn")
            return {.title = "Experimental: aim toe-in",
                    .description = "Back view turns the camera to look at the targeted block."};
        if (name == "freeLookSpeed")
            return {.title = "Free-look speed", .minimum = 0.1, .maximum = 5.0};
        if (name == "freeLookSwapAxes")
            return {.title = "Free-look: swap axes",
                    .description = "Turn on if free-look moves up/down when you swipe sideways."};
        if (name == "eyeHeightFallback")
            return {.title = "Eye height fallback", .minimum = -2.0, .maximum = 2.0};
        if (name == "nativeBackDistance")
            return {.title = "Game back-view distance", .minimum = 1.0, .maximum = 8.0};
        if (name == "nativeFrontDistance")
            return {.title = "Game front-view distance", .minimum = 1.0, .maximum = 8.0};
        if (name == "transitionSeconds")
            return {.title = "Perspective easing (seconds)", .minimum = 0.0, .maximum = 1.5};
        if (name == "showStatus")
            return {.title = "Show status overlay",
                    .description = "Shows which hooks are working. Turn off once Dynamicam works."};
        if (name == "debugLog")
            return {.title = "Debug log"};
        return {};
    }
};
