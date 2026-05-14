# Fallout: New Vegas — RTX Remix Comp

An ASI plugin that converts Fallout: New Vegas from its shader-based rendering to the fixed-function pipeline (FFP) for RTX Remix compatibility, hooking the rendering pipeline from inside the game process.

**Status**: Work in progress (v0.0.4)

## What It Does

Fallout: New Vegas uses vertex and pixel shaders for all geometry, which RTX Remix cannot hook. This companion plugin — built on the [`xoxor4d/remix-comp-base`](https://github.com/xoxor4d) framework and loaded into `FalloutNV.exe` via the standard `dinput8.dll` ASI mechanism — runs *inside* the game process and:

- **Recovers world-space transforms** by reading the game's `NiDX9Renderer` singleton directly for separate World, View, and Projection matrices — no matrix inversion needed
- **Replaces vertex and pixel shaders** with the FFP pipeline on every 3D draw call (via in-process MinHook hooks on the D3D9 device), while passing through 2D/UI content (Pip-Boy, menus) with original shaders
- **Handles skinned meshes** (characters, creatures) via FFP indexed vertex blending — clones vertex declarations to convert BLENDINDICES from D3DCOLOR to UBYTE4, uploads bone matrices per-object with game engine hooks for proper palette reset
- **Extracts point lights** from the game's `ShadowSceneNode` light list and submits them as `D3DLIGHT9` calls so Remix can create path-traced lights
- **Routes sky geometry** through FFP based on `NiShadeProperty` shader type detection
- **Drives the Remix atmosphere sun and moon** from FNV's scene-graph orientation each frame so the renderer's celestial bodies track in-game time
- **Optionally chain-loads RTX Remix** (`d3d9_remix.dll`) and a preload side-effect DLL (e.g. SilentPatch) via `remix-comp.ini`
- **Includes a debug UI** (ImGui), diagnostic frame logging, and an integrated D3D9 call tracer for offline analysis

## Installation

1. Install RTX Remix runtime to the game folder
2. (Optional) Rename Remix's `d3d9.dll` to `d3d9_remix.dll` and set `[Remix] Enabled=1` in `remix-comp.ini` if you want the comp to chain-load it
3. Copy `dinput8.dll`, the built `*-comp.asi` (e.g. `FNV-comp.asi` or `remix-comp.asi`), and `remix-comp.ini` into the game folder (next to `FalloutNV.exe`)
4. Launch the game

> **Note:** `remix-comp.ini` must be present alongside the comp `.asi`. Console output and the log file carry diagnostic info.

## Configuration

Edit `remix-comp.ini` to adjust behaviour. Key sections:

| Section | Key | Default | Description |
|---|---|---|---|
| `[Remix]` | `Enabled` | `0` | Chain-load RTX Remix bridge |
| `[Remix]` | `DLLName` | `d3d9_remix.dll` | Remix bridge DLL filename |
| `[Chain]` | `PreloadDLL` | *(empty)* | Side-effect DLL to load at startup (e.g. SilentPatch) |
| `[FFP]` | `Enabled` | `1` | FFP shader replacement |
| `[FFP]` | `AlbedoStage` | `0` | Texture stage used as the albedo |
| `[Lights]` | `Enabled` | `1` | Extract engine point lights for Remix |
| `[Lights]` | `IntensityPercent` | `100` | Light intensity multiplier (100 = 1.0x) |
| `[Lights]` | `RangeMode` | `0` | Point light range: 0=Spec.r, 1=attenuation calc, 2=infinity |
| `[SunCycle]` | `Enabled` | `1` | Drive `rtx.atmosphere.sun*` from scene-graph orientation |
| `[MoonCycle]` | `Enabled` | `1` | Drive `rtx.atmosphere.moon0.*` from scene-graph orientation |
| `[Diagnostics]` | `Enabled` | `1` | Log draw-call data after `DelayMs` |
| `[Tracer]` | `BacktraceDepth` | `8` | D3D9 call tracer call-stack depth |

See `remix-comp.ini` for the full set, including FNV-specific notes.

## Building from Source

Requires **MSVC 2022 x86** (Visual Studio 2022 with C++ desktop workload).

```bat
build.bat                       :: Build release remix-comp.asi (default)
build.bat debug                 :: Build debug
build.bat release --name FNV    :: Build as FNV-comp.asi
```

C++20 with PCH, statically-linked CRT (`/MT`). Links MinHook, ImGui, and the DXSDK redistributable libraries vendored under `deps/`. Output goes to `build/bin/<config>/`.

## Project Layout

```
src/comp/      FNV-specific code (game hooks, modules)
src/shared/    Generic remix-comp framework (config, loader, hooking, FFP state, Remix API)
deps/          Vendored dependencies (bridge_api, dxsdk, imgui, minhook)
assets/        Deploy payload (dinput8.dll ASI loader, remix-comp.ini, rtx_comp textures)
build.bat      Standalone build script (supports --name and --comp for multi-game variants)
kb.h           Reverse-engineering knowledge base — struct layouts and function addresses for FalloutNV.exe
```

## Known Limitations

- **LOD terrain** lacking vertex normals is passed through with original shaders and won't be path-traced by Remix. Only high-quality terrain (with NORMAL elements) is converted to FFP.
- **Post-process effects** and screen-space overlays using POSITIONT vertices bypass FFP conversion entirely.
- Game engine hooks for skinning, point-light extraction, and scene-graph reads target specific code addresses and are only compatible with the standard Steam/GOG executable.

## Credits

- [xoxor4d](https://github.com/xoxor4d) — `remix-comp-base` framework on which this comp is built
- Wall_SoGB, BlueAmulet, and xoxor4d — [NewVegasRTXHelper](https://github.com/Kim2091/NewVegasRTXHelper), which this comp's light extraction, skinning hooks, and transform logic are ported from
- Developed using [Vibe-Reverse-Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) — RE tooling framework (disassembly, decompilation, live tracing)
