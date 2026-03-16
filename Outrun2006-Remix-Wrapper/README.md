# OutRun 2006 Coast 2 Coast — RTX Remix FFP Proxy

A D3D9 proxy DLL that converts OutRun 2006 Coast 2 Coast from its shader-based rendering to the fixed-function pipeline (FFP), enabling full RTX Remix compatibility.

**Status**: Finished ✅

## What It Does

OutRun 2006 uses vertex shaders for all geometry, which RTX Remix cannot hook. This proxy intercepts the game's D3D9 device calls and:

- **Recovers world-space transforms** from the game's shader constant writes — the game uploads a WorldView matrix per draw call and separate View/Projection globals, so the proxy back-calculates the World matrix as `WorldView × inv(View)` and feeds it to D3D9 FFP via `SetTransform`
- **Replaces vertex and pixel shaders** with the FFP pipeline on every draw call
- **Handles water geometry** — water uses vertex declarations containing `TANGENT` elements, which D3D9 FFP refuses to render; the proxy swaps in an FFP-compatible declaration (POS/NORMAL/TEX0) around water draws, keeping the same vertex buffer offsets so vertex data is read correctly
- **Chain-loads RTX Remix** (`d3d9_remix.dll`) so Remix sees the converted FFP geometry

## Installation

1. Install this mod: https://github.com/emoose/OutRun2006Tweaks/releases
2. Install Remix runtime to the game folder
3. Rename remix's `d3d9.dll` to `d3d9_remix.dll`
4. Download this mod from [Releases](https://github.com/Kim2091/Outrun2006-Remix-Patch/releases) and extract into your Outrun 2006 game folder
5. Launch the game!

> **Note:** `proxy.ini` __must__ be present alongside `d3d9.dll`.

## Configuration

Edit `proxy.ini` to adjust behaviour:

| Section | Key | Default | Description |
|---------|-----|---------|-------------|
| `[Remix]` | `Enabled` | `1` | Chain-load RTX Remix |
| `[Remix]` | `DLLName` | `d3d9_remix.dll` | Remix DLL filename |
| `[FFP]` | `AlbedoStage` | `0` | Texture stage used as the albedo (Do not change!) |

## Building from Source

Requires **MSVC x86** (Visual Studio with C++ desktop workload).

```bat
cd proxy
build.bat
```

Run `build.bat` from an **x86 VS Developer Command Prompt**. Outputs `d3d9.dll` in the same folder.

## Known Limitations

- **Water animation** is lost — the wave displacement shader is replaced by FFP, so ocean surfaces render as a static plane. RTX Remix can apply an animated water material on top to compensate.
- Any geometry using D3D9 hardware skinning (BLENDWEIGHT + BLENDINDICES vertex elements) is passed through with its original shaders. This does not appear to affect OR2006's visible geometry in practice.

## Credits

- Developed using [Vibe-Reverse-Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) — RE tooling framework (disassembly, decompilation, live tracing)
- Based on **kim2091's** generic DX9 FFP wrapper for converting shader games to fixed function pipeline
