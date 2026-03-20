# Fallout: New Vegas — RTX Remix FFP Proxy

A D3D9 proxy DLL that converts Fallout: New Vegas from its shader-based rendering to the fixed-function pipeline (FFP), enabling full RTX Remix compatibility.

**Status**: Work in progress

## What It Does

Fallout: New Vegas uses vertex and pixel shaders for all geometry, which RTX Remix cannot hook. This proxy intercepts the game's D3D9 device calls and:

- **Recovers world-space transforms** by reading the game's `NiDX9Renderer` singleton directly for separate World, View, and Projection matrices — no matrix inversion needed
- **Replaces vertex and pixel shaders** with the FFP pipeline on every 3D draw call, while passing through 2D/UI content (Pip-Boy, menus) with original shaders
- **Handles skinned meshes** (characters, creatures) via FFP indexed vertex blending — clones vertex declarations to convert BLENDINDICES from D3DCOLOR to UBYTE4, uploads bone matrices per-object with game engine hooks for proper palette reset
- **Extracts point lights** from the game's `ShadowSceneNode` light list and submits them as `D3DLIGHT9` calls so Remix can create path-traced lights
- **Filters fake shadows** — detects and skips baked planar shadow overlays (NOLIGHTING shader + grayscale vertex colors) that conflict with ray-traced shadows
- **Routes sky geometry** through FFP based on `NiShadeProperty` shader type detection
- **Chain-loads RTX Remix** (`d3d9_remix.dll`) and optional side-effect DLLs (e.g. SilentPatch) so Remix sees the converted FFP geometry

## Installation

1. Install RTX Remix runtime to the game folder
2. Rename Remix's `d3d9.dll` to `d3d9_remix.dll`
3. Copy `d3d9.dll` and `proxy.ini` from this mod into the game folder (next to `FalloutNV.exe`)
4. Set `[Remix] Enabled=1` in `proxy.ini`
5. Launch the game

> **Note:** `proxy.ini` __must__ be present alongside `d3d9.dll`. Check `ffp_proxy.log` for diagnostic output.

## Configuration

Edit `proxy.ini` to adjust behaviour:

| Section | Key | Default | Description |
|---------|-----|---------|-------------|
| `[Remix]` | `Enabled` | `0` | Chain-load RTX Remix |
| `[Remix]` | `DLLName` | `d3d9_remix.dll` | Remix DLL filename |
| `[Chain]` | `PreloadDLL` | *(empty)* | Side-effect DLL to load at startup (e.g. SilentPatch) |
| `[FFP]` | `AlbedoStage` | `0` | Texture stage used as the albedo |
| `[FFP]` | `SkipFakeShadows` | `1` | Skip baked shadow overlays (gray vertex color meshes) |
| `[Lights]` | `Enabled` | `1` | Extract engine point lights for Remix |
| `[Lights]` | `IntensityPercent` | `100` | Light intensity multiplier (100 = 1.0x) |
| `[Lights]` | `RangeMode` | `0` | Point light range: 0=Spec.r, 1=attenuation calc, 2=infinity |

## Building from Source

Requires **MSVC x86** (Visual Studio with C++ desktop workload).

```bat
build.bat
```

Run `build.bat` from an **x86 VS Developer Command Prompt**, or just double-click it (auto-detects VS). Outputs `d3d9.dll` in the same folder.

## Known Limitations

- **LOD terrain** lacking vertex normals is passed through with original shaders and won't be path-traced by Remix. Only high-quality terrain (with NORMAL elements) is converted to FFP.
- **Post-process effects** and screen-space overlays using POSITIONT vertices bypass FFP conversion entirely.
- Game engine hooks for skinning target specific code addresses and are only compatible with the standard Steam/GOG executable.

## Credits

- [Wall_SoGB](https://github.com/Wall-SoGB) — [NewVegasRTXHelper](https://github.com/Wall-SoGB/NewVegasRTXHelper), which this proxy's light extraction, skinning hooks, and transform logic are ported from
- Developed using [Vibe-Reverse-Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) — RE tooling framework (disassembly, decompilation, live tracing)
