#pragma once

// Dynamicam by Sw1ghttt

#include <optional>

#include "mod/Config.h"

#include <pl/Mod.hpp>

namespace dynamicam {

class DynamicamMod {
  public:
    static DynamicamMod &instance();

    DynamicamMod();

    [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

  private:
    void registerMenu();
    void installHooks();
    void removeHooks();

    ll::mod::NativeMod &mSelf;
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile;
};

} // namespace dynamicam
