# Dynamicam

**By Sw1ghttt** · LeviLauncher native mod for Minecraft Bedrock **1.26.5X** (arm64-v8a)

| View | What it does |
| --- | --- |
| **First person** | You see your hands, torso and feet. The camera sits at the face, so the head is never in view. |
| **Second person – back** | Camera sits behind and slightly to the side. While you point at a block or mob it slides to the centre so the crosshair is exact. |
| **Second person – front** | Camera in front, facing your face. Held items animate on the model as the game normally does. |
| **Free-look (emotes)** | Turn on the **Look** button, then swipe: the camera orbits while your character (and emote) stays still. *Experimental.* |

Everything is adjustable in the in-game **Mod Menu → Dynamicam** (distance, side shift, height, FOV per view).

## Install
1. Push this repo to GitHub → **Actions → Build** → download the `.levipack` artifact.
2. Import the `.levipack` in LeviLauncher and enable it.

## Settings that matter
- `Experimental: free-look / rotation` – required for the **Look** button and aim toe-in.
- `Eye height fallback` – only if your first-person camera sits at your feet (try `1.62`).
- If a Minecraft update breaks it, the log says which signature was not found; new patterns go in `src/mod/Signatures.h`.

## Logo
Put your logo at the repo root as `icon.png` and build with `-DMOD_ICON=icon.png`
(or set `MOD_ICON` in `CMakeLists.txt`).

## Layout
```
src/main.cpp              mod registration
src/mod/DynamicamMod.*    hooks, mod menu, lifecycle
src/mod/ViewLogic.h       camera placement math (unit tested)
src/mod/CameraMath.h      vector helpers
src/mod/Config.*          typed config -> config/config.json
src/mod/Signatures.h      game signatures
tests/                    PC unit tests for the math (not part of the Android build)
```
