# ForceBumpTerrain

ASI patch for **Star Wars: Empire at War – Forces of Corruption** (Steam, 64-bit) that forces high-quality bump-mapped terrain at all zoom levels.

## What it does

During zoom-out, the game's `TerrainCameraState_Update` function calls `SetUseBakedTerrain(1)` to switch to a low-quality pre-baked terrain renderer. This patch NOPs that 5-byte `CALL` instruction (RVA `0x2CF7D2`), keeping the high-quality bump terrain active regardless of camera distance.

| Detail | Value |
|---|---|
| Target executable | `StarWarsG.exe` (x64, ASLR-enabled) |
| Patch site | RVA `0x002CF7D2` |
| Original bytes | `E8 F9 80 E6 FF` |
| Replacement | `90 90 90 90 90` (NOP × 5) |

## Files

| File | Description |
|---|---|
| `ForceBumpTerrain.json` | Declarative patch spec consumed by `asi_patcher.py` |
| `ForceBumpTerrain.c` | Hand-written standalone C source (alternative to the auto-generated `patch.c`) |
| `patch.c` | Auto-generated C source produced by `asi_patcher.py build` |
| `build.bat` | One-click build script — compiles `patch.c` into `ForceBumpTerrain.asi` |

## Building

### Prerequisites

- **Microsoft Visual Studio** (2019 or later) with the **C++ Desktop** workload installed.

### Option 1 — build.bat (recommended)

```bat
build.bat
```

The script auto-detects `vcvarsall.bat` via `vswhere`. If detection fails, you can pass the path manually:

```bat
build.bat "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
```

### Option 2 — asi_patcher.py

From the repository root:

```bat
python retools/asi_patcher.py build patches/SWOFC/git/ForceBumpTerrain.json ^
    --vcvarsall "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
```

Both methods produce `ForceBumpTerrain.asi` in this directory.

## Installation

1. Copy `ForceBumpTerrain.asi` into the game's executable directory (next to `StarWarsG.exe`).
2. An ASI loader must already be present (e.g. `dinput8.dll` / Ultimate ASI Loader).
3. Launch the game. Check `ForceBumpTerrain.log` in the same directory to verify the patch applied successfully.

## Log output

On success the log will contain:

```
ForceBumpTerrain loaded
Base: 0x00007FF6XXXXXXXX
verify OK
[OK]   NopSetBakedTerrainCall @ 0x00007FF6XXXXXXXX
All patches applied
```

If you see `verify FAILED`, the executable version does not match the expected bytes — the patch is **not** applied and the game runs unmodified.
