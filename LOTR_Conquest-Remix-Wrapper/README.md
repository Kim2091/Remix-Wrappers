# The Lord of the Rings: Conquest — RTX Remix FFP Proxy

A D3D9 proxy DLL that converts LOTR Conquest from its shader-based rendering to the fixed-function pipeline (FFP), enabling full RTX Remix compatibility.

**Status**: Work in Progress 🔧

## What It Does

LOTR Conquest uses the Pandemic Magellan engine with vertex shaders for all geometry, which RTX Remix cannot hook. This proxy intercepts the game's D3D9 device calls and:

- **Recovers world-space transforms** from the game's shader constant writes — the engine uploads a combined ViewProjection matrix at `c239-c242` and a World matrix at `c178-c181` (both row-major HLSL), so the proxy decomposes the ViewProj into separate View and Projection matrices and feeds all three to D3D9 FFP via `SetTransform`
- **Replaces vertex and pixel shaders** with the FFP pipeline on every draw call
- **Handles skinned character meshes** — the Magellan engine uses a 59-bone palette (`c0-c176`, 3 registers per bone) for skeletal animation; the proxy uploads bone matrices to FFP indexed vertex blending slots and expands vertices from the game's packed format (D3DCOLOR blend weights/normals) into FFP-compatible FLOAT3 layouts with cached vertex buffers
- **Expands packed vertex normals** — the engine stores normals as D3DCOLOR (4 bytes); RTX Remix requires FLOAT3, so the proxy decodes normals (`*2-1` remap) and rebuilds vertex buffers on the fly with declaration cloning and VB caching
- **Reconstructs terrain UVs on the CPU** — terrain vertices (FLOAT3 position + D3DCOLOR normal, no texcoords) get expanded to include FLOAT2 UVs computed from pixel shader constants `c212`/`c213` that encode the terrain's UV transform
- **Passes through HUD/UI draws** — screen-space geometry (POSITIONT declarations) and Scaleform UI (color-only vertices) are drawn with original shaders, preserving menus, text, and overlays
- **Drops shader-driven effects** — alpha-blended particles, clouds, and other overlay effects that depend on shader logic are skipped, as Remix handles its own atmospheric and particle rendering
- **Chain-loads RTX Remix** (`d3d9_remix.dll`) so Remix sees the converted FFP geometry

## Installation

1. Install RTX Remix runtime to the game folder
2. Rename Remix's `d3d9.dll` to `d3d9_remix.dll`
3. Download this mod from [Releases](https://github.com/Kim2091/Remix-Wrappers/releases) and extract into your LOTR Conquest game folder
4. Launch the game!

> **Note:** `proxy.ini` __must__ be present alongside `d3d9.dll`.

## Configuration

Edit `proxy.ini` to adjust behaviour:

| Section | Key | Default | Description |
|---------|-----|---------|-------------|
| `[Remix]` | `Enabled` | `1` | Chain-load RTX Remix |
| `[Remix]` | `DLLName` | `d3d9_remix.dll` | Remix DLL filename |
| `[Chain]` | `PreloadDLL` | *(empty)* | Side-effect DLL to load at startup (e.g. SilentPatch) |
| `[FFP]` | `AlbedoStage` | `0` | Texture stage used as the albedo (Do not change!) |
| `[FFP]` | `DepthBias` | `0.0` | Depth bias for FFP draws (small negative fixes Z-fighting) |
| `[FFP]` | `SlopeScaleDepthBias` | `0.0` | Slope-scale depth bias for FFP draws |

## Building from Source

Requires **MSVC x86** (Visual Studio with C++ desktop workload).

```bat
cd proxy
build.bat
```

Run `build.bat` from an **x86 VS Developer Command Prompt**, or just run it directly — it auto-detects Visual Studio via `vswhere`. Outputs `d3d9.dll` in the same folder.

## Known Limitations

- **Particles and atmospheric effects** are dropped — cloud layers, smoke, fire particles, and similar alpha-blended shader effects are suppressed since they depend on vertex/pixel shader logic that FFP cannot replicate. RTX Remix can apply replacement materials and effects.
- **Terrain UV precision** — terrain texcoords are reconstructed from shader constants on the CPU, which may show minor UV seam differences compared to the original GPU shader path. Some terrain is also textureless
- **Billboards** - billboards (animated 2d or 3d "meshes" using shaders) are not visible in-game

## Credits

- Developed using [Vibe-Reverse-Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) — RE tooling framework (disassembly, decompilation, live tracing)
- Based on my generic DX9 FFP wrapper for converting shader games to fixed function pipeline
