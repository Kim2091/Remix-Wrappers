# Fallout: New Vegas — RTX Remix Comp

A `d3d9.dll` proxy that converts Fallout: New Vegas from its shader-based rendering to the fixed-function pipeline (FFP) for RTX Remix compatibility, hooking the rendering pipeline from inside the game process.

**Status**: Work in progress (v0.0.4)

## What It Does

Fallout: New Vegas uses vertex and pixel shaders for all geometry, which RTX Remix cannot hook. This companion plugin — built on the [`xoxor4d/remix-comp-base`](https://github.com/xoxor4d) framework and loaded as a `d3d9.dll` proxy that the game's import resolver picks up automatically — runs *inside* the game process and:

- **Recovers world-space transforms** by reading the game's `NiDX9Renderer` singleton directly for separate World, View, and Projection matrices — no matrix inversion needed
- **Replaces vertex and pixel shaders** with the FFP pipeline on every 3D draw call (via in-process MinHook hooks on the D3D9 device), while passing through 2D/UI content (Pip-Boy, menus) with original shaders
- **Handles skinned meshes** (characters, creatures) via FFP indexed vertex blending — clones vertex declarations to convert BLENDINDICES from D3DCOLOR to UBYTE4, uploads bone matrices per-object with game engine hooks for proper palette reset
- **Extracts point lights** from the game's `ShadowSceneNode` light list and submits them as `D3DLIGHT9` calls so Remix can create path-traced lights
- **Routes sky geometry** through FFP based on `NiShadeProperty` shader type detection
- **Drives the Remix atmosphere sun and moon** from FNV's scene-graph orientation each frame so the renderer's celestial bodies track in-game time
- **Chain-loads RTX Remix** (`d3d9_remix.dll`) by default, with optional preload/postload DLL slots (e.g. SilentPatch) via `remix-comp.ini`
- **Includes a debug UI** (ImGui), diagnostic frame logging, and an integrated D3D9 call tracer for offline analysis

## Installation

1. Install the **latest release of [RTX Remix Plus](https://github.com/RemixProjGroup/dxvk-remix/releases)** — this gives you Remix's `d3d9.dll`. Older Remix builds may lack required bridge entry points.
2. **Rename Remix's `d3d9.dll` to `d3d9_remix.dll`** so it doesn't collide with this wrapper. Place it in the FNV game folder next to `FalloutNV.exe`.
3. Copy this wrapper's `d3d9.dll` into the game folder next to `FalloutNV.exe`. Windows' loader will find our `d3d9.dll` first when the game starts, and we chain-load `d3d9_remix.dll` from there.
4. Copy `remix-comp.ini` into the game folder next to `FalloutNV.exe`.
5. Launch the game.

> **Important — chain order:** The game folder must contain **both** `d3d9.dll` (this wrapper) and `d3d9_remix.dll` (the renamed Remix bridge). With `[Remix] Enabled=1` (default), the wrapper LoadLibrary's `d3d9_remix.dll` and forwards `Direct3DCreate9` through it. Without the renamed Remix bridge present, the wrapper silently falls back to the system `d3d9.dll` and Remix won't engage. Check console output for `[Proxy] Loaded Remix bridge: d3d9_remix.dll` to confirm.

> **Important — ini location:** `remix-comp.ini` is read from the **same directory as the wrapper's `d3d9.dll`** (the game root). Console output and the log file carry diagnostic info if it can't be found.

## Configuration

Edit `remix-comp.ini` to adjust behaviour. Key sections:

| Section | Key | Default | Description |
|---|---|---|---|
| `[Remix]` | `Enabled` | `1` | Chain-load RTX Remix bridge from `DLLName` |
| `[Remix]` | `DLLName` | `d3d9_remix.dll` | Filename of the renamed Remix bridge DLL (placed next to our `d3d9.dll`) |
| `[Chain]` | `Preload` | *(empty)* | Semicolon-separated DLLs/ASIs loaded before the d3d9 chain (e.g. `SilentPatch.asi`) |
| `[Chain]` | `Postload` | *(empty)* | Semicolon-separated DLLs/ASIs loaded after the game window appears |
| `[FFP]` | `Enabled` | `1` | FFP shader replacement |
| `[FFP]` | `AlbedoStage` | `0` | Texture stage used as the albedo |
| `[Lights]` | `Enabled` | `1` | Extract engine point lights for Remix |
| `[Lights]` | `IntensityPercent` | `100` | Light intensity multiplier (100 = 1.0x) |
| `[Lights]` | `RangeMode` | `0` | Point light range: 0=Spec.r, 1=attenuation calc, 2=infinity |
| `[SunCycle]` | `Enabled` | `1` | Drive `rtx.atmosphere.sun*` from scene-graph orientation |
| `[MoonCycle]` | `Enabled` | `1` | Drive `rtx.atmosphere.moon0.*` from scene-graph orientation |
| `[CullingPatch]` | `Enabled` | `0` | Disable `BSCullingProcess` so Remix sees off-screen geometry (Wall_SoGB patch) |
| `[Diagnostics]` | `Enabled` | `1` | Log draw-call data after `DelayMs` |
| `[Tracer]` | `BacktraceDepth` | `8` | D3D9 call tracer call-stack depth |

See `remix-comp.ini` for the full set, including FNV-specific notes.

## Building from Source

Requires **MSVC 2022 x86** (Visual Studio 2022 with C++ desktop workload).

```bat
build.bat                       :: Build release d3d9.dll (default)
build.bat debug                 :: Build debug d3d9.dll
build.bat release --name FNV    :: Build release d3d9.dll with FNV-tagged PDB
```

Output is always `d3d9.dll` (linked via `d3d9.def` so it exports the standard d3d9 entry points). C++20 with PCH, statically-linked CRT (`/MT`). Links MinHook, ImGui, and the DXSDK redistributable libraries vendored under `deps/`. Output goes to `build/bin/<config>/`, along with a copy of `remix-comp.ini`.

## Project Layout

```
src/comp/        FNV-specific code (game hooks, modules, d3d9_proxy chain loader)
src/shared/      Generic remix-comp framework (config, loader, hooking, FFP state, Remix API)
deps/            Vendored dependencies (bridge_api, dxsdk, imgui, minhook)
d3d9.def         Exported d3d9 entry points — linker uses this to make the output a real d3d9.dll
remix-comp.ini   Default configuration (copied to build output)
build.bat        Standalone build script (supports --name and --comp for multi-game variants)
kb.h             Reverse-engineering knowledge base — struct layouts and function addresses for FalloutNV.exe
```

## Known Limitations

- **LOD terrain** lacking vertex normals is passed through with original shaders and won't be path-traced by Remix. Only high-quality terrain (with NORMAL elements) is converted to FFP.
- **Post-process effects** and screen-space overlays using POSITIONT vertices bypass FFP conversion entirely.
- Game engine hooks for skinning, point-light extraction, and scene-graph reads target specific code addresses and are only compatible with the standard Steam/GOG executable.

## Credits

- [xoxor4d](https://github.com/xoxor4d) — `remix-comp-base` framework on which this comp is built
- Wall_SoGB, BlueAmulet, and xoxor4d — [NewVegasRTXHelper](https://github.com/Kim2091/NewVegasRTXHelper), which this comp's light extraction, skinning hooks, and transform logic are ported from
- Developed using [Vibe-Reverse-Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) — RE tooling framework (disassembly, decompilation, live tracing)
