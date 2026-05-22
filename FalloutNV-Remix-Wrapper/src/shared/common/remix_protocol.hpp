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

	// Set the modifier slot. Pass 0 to clear back to "no modifiers applied";
	// callers that wrote a modifier should follow up with reset_all_slots() at the
	// end of the draw so the marking does not leak forward.
	inline void set_modifier(IDirect3DDevice9* dev, uint32_t modifierMask) {
		dev->SetRenderState(kRsModifier, modifierMask);
	}

	// Reset all 5 protocol slots to the sentinel value, so the next draw the
	// wrapper does not explicitly mark will fall through to Remix's default
	// (non-protocol) behaviour. Call after every protocol-written draw.
	inline void reset_all_slots(IDirect3DDevice9* dev) {
		dev->SetRenderState(kRsCategoryFlags, kSentinel);
		dev->SetRenderState(kRsModifier,      kSentinel);
		dev->SetRenderState(kRsHashOverride,  kSentinel);
		dev->SetRenderState(kRsTempFloat01,   kSentinel);
		dev->SetRenderState(kRsTempFloat02,   kSentinel);
	}

} // namespace remix_protocol
