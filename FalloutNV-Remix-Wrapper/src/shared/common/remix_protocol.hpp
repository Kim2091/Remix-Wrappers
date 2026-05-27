// remix_protocol.hpp -- D3D9 RS-protocol writers for the Remix Plus FNV port.
//
// The Remix Plus dxvk-remix fork ("fnv-ffp" branch and beyond) reads per-draw
// classification metadata from unused D3D9 render-state slots. The wrapper
// writes those slots before each draw call so Remix can route textures to the
// correct material channel without manual rtx.conf hash lists.
//
// Protocol contract: docs/superpowers/specs/2026-05-22-fnv-ffp-protocol-design.md
// in the dxvk-remix repo (kim/fnv-ffp branch).

#pragma once

#include <d3d9.h>
#include <cstdint>

namespace remix_protocol {

	constexpr uint32_t kSentinel = 0xfefefefeu;

	// RS slot numbers as defined by the protocol. Do not change without coordinating
	// with the dxvk-remix-side capture code in setLegacyMaterialState.
	constexpr D3DRENDERSTATETYPE kRsCategoryFlags = static_cast<D3DRENDERSTATETYPE>(42);
	constexpr D3DRENDERSTATETYPE kRsModifier      = static_cast<D3DRENDERSTATETYPE>(149);
	constexpr D3DRENDERSTATETYPE kRsHashOverride  = static_cast<D3DRENDERSTATETYPE>(150);
	constexpr D3DRENDERSTATETYPE kRsTempFloat01   = static_cast<D3DRENDERSTATETYPE>(169);
	constexpr D3DRENDERSTATETYPE kRsTempFloat02   = static_cast<D3DRENDERSTATETYPE>(177);

	// RS 42 encoding: raw bitmask of InstanceCategories bit positions, mirroring
	// the enum in dxvk-remix/src/dxvk/rtx_render/rtx_types.h. Each enum value is
	// a bit POSITION; the wire value is (1u << position). Captured into
	// materialData.remixTextureCategoryFlagsFromD3D and OR'd into the draw's
	// CategoryFlags via getCategoryFlags(). Setting Ignore short-circuits RT
	// capture at d3d9_rtx.cpp:1106 (rasterization still happens).
	//
	// Only the bits the wrapper actually drives are listed here; extend as
	// needed. Keep the enum-value-to-bit-position math identical to the
	// upstream enum order or the routing breaks.
	enum class CategoryBit : uint32_t {
		WorldUI = 0,
		WorldMatte = 1,
		Sky = 2,
		Ignore = 3,
		DecalStatic = 12,
	};

	inline constexpr uint32_t category_mask(CategoryBit b) {
		return 1u << static_cast<uint32_t>(b);
	}

	// Tag the next draw with a CategoryFlags bitmask (raw `Flags<T>::raw()`
	// encoding -- (1u << bit_position) per category). Pass 0 to clear back to
	// "no protocol categories"; pair with reset_all_slots() after the draw.
	inline void set_category_flags(IDirect3DDevice9* dev, uint32_t flagsMask) {
		dev->SetRenderState(kRsCategoryFlags, flagsMask);
	}

	// RS 149 encoding: packed PS slot-role nibbles.
	//   bits 0-3:   diffuse slot (0..7) or 0xF if PS has no diffuse role
	//   bits 4-7:   normal slot  (0..7) or 0xF if PS has no normal role
	//   bits 8-11:  glow slot    (0..7) or 0xF if PS has no glow role
	//   bits 12-15: height slot  (0..7) or 0xF if PS has no height role
	//   bits 16-31: reserved (always 0)
	//
	// Example: PS has s0=BaseMap, s1=NormalMap          -> 0x0000_FF10
	//          PS has s0=NormalMap only                 -> 0x0000_FFF0
	//          PS has s0=BaseMap, s7=NormalMap          -> 0x0000_FF70
	//          PS has s0=BaseMap, s1=NormalMap,
	//                  s3=HeightMap                     -> 0x0000_3F10
	//
	// Decoded on the dxvk-remix side by setLegacyMaterialState (in
	// d3d9_rtx_utils.cpp). A nibble of 0xF means "this role is absent for
	// this PS"; the dxvk side then leaves that channel alone.
	//
	// Backwards compat note: bits 12-15 were previously hard-coded to 0xF
	// ("reserved"). Old wrappers therefore encode kSlotAbsent in the new
	// height nibble, which the dxvk-remix decoder interprets as "no height
	// slot" -- no spurious height routing for pre-Height callers.
	constexpr uint8_t kSlotAbsent = 0xF;

	inline uint32_t encode_slot_roles(uint8_t diffuseSlot,
	                                  uint8_t normalSlot,
	                                  uint8_t glowSlot   = kSlotAbsent,
	                                  uint8_t heightSlot = kSlotAbsent) {
		const uint32_t diff   = static_cast<uint32_t>(diffuseSlot & 0xF);
		const uint32_t norm   = static_cast<uint32_t>(normalSlot  & 0xF);
		const uint32_t glow   = static_cast<uint32_t>(glowSlot    & 0xF);
		const uint32_t height = static_cast<uint32_t>(heightSlot  & 0xF);
		return diff | (norm << 4) | (glow << 8) | (height << 12);
	}

	// Multi-layer terrain extension to RS-149 modifier:
	//   bit 16        : MULTI_LAYER_TERRAIN flag (1 = this draw is FNV multi-layer terrain)
	//   bits 17-19    : layer count (1..7), only meaningful when bit 16 is set
	//   bits 20-31    : reserved (must be 0)
	//
	// Bits 0-15 (V1 slot-role nibbles) should be set to all-kSlotAbsent (0xFFFF) when
	// emitting multi-layer terrain, so the dxvk-remix V1 capture path doesn't pick up
	// stale slot routing. The multi-layer capture branch reads d3d9State.textures[0..6]
	// for albedos and [7..13] for normals directly, bypassing V1's 4-slot capture.
	//
	// Keep these constants in lockstep with dxvk-remix/src/dxvk/rtx_render/rtx_materials.h:
	// kRemixMultiLayerTerrainBit / kRemixMultiLayerCountShift / kRemixMultiLayerCountMask.
	constexpr uint32_t kRemixMultiLayerTerrainBit  = 1u << 16;
	constexpr uint32_t kRemixMultiLayerCountShift  = 17;
	constexpr uint32_t kRemixMultiLayerCountMask   = 0x7u;  // 3 bits, valid values 1..7

	// Builds an RS-149 modifier value for a multi-layer terrain draw with `layerCount`
	// layers (1..7). Combines: bits 0-15 = all-absent V1 nibbles (0xFFFF), bit 16 =
	// kRemixMultiLayerTerrainBit, bits 17-19 = layerCount. Bits 20-31 stay zero.
	// Caller responsibility to validate layerCount is in range 1..7; out-of-range
	// values are masked to 3 bits and may produce a count of 0 which the decoder
	// will reject.
	inline uint32_t encode_multilayer_terrain(uint8_t layerCount) {
		const uint32_t v1Nibbles = encode_slot_roles(kSlotAbsent, kSlotAbsent, kSlotAbsent, kSlotAbsent);
		const uint32_t countField = (static_cast<uint32_t>(layerCount) & kRemixMultiLayerCountMask) << kRemixMultiLayerCountShift;
		return v1Nibbles | kRemixMultiLayerTerrainBit | countField;
	}

	// Set the modifier slot. Pass 0 to clear back to "no modifiers applied";
	// callers that wrote a modifier should follow up with reset_all_slots() at the
	// end of the draw so the marking does not leak forward.
	inline void set_modifier(IDirect3DDevice9* dev, uint32_t modifierMask) {
		dev->SetRenderState(kRsModifier, modifierMask);
	}

	// Reset the protocol slots the wrapper actually writes (CategoryFlags +
	// Modifier) back to the sentinel so the next un-marked draw falls through
	// to Remix's default (non-protocol) behaviour. kRsHashOverride /
	// kRsTempFloat01 / kRsTempFloat02 have no writers in this wrapper; they
	// never leave the device default and don't need to be re-sentinelled per
	// draw. Call after every protocol-written draw.
	inline void reset_all_slots(IDirect3DDevice9* dev) {
		dev->SetRenderState(kRsCategoryFlags, kSentinel);
		dev->SetRenderState(kRsModifier,      kSentinel);
	}

} // namespace remix_protocol
