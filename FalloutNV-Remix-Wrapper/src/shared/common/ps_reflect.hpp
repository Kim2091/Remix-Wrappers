// ps_reflect.hpp -- PS sampler-name classifier for the Remix RS-protocol.
//
// Each unique pixel shader (deduplicated by bytecode hash) is parsed via
// D3DXGetShaderConstantTable to extract the sampler declarations. Each
// declared sampler's name is mapped to a semantic role (Diffuse / Normal /
// Glow / Other), and a (role -> sampler slot) table is cached per PS hash.
//
// The renderer queries this table per draw to drive:
//   - which slot's texture to bind at D3D9 slot 0 for FFP rasterization
//   - which slots to keep bound so dxvk-remix can read them straight from
//     device state (no FFP COLOROP gating loop)
//   - the packed-nibble RS 149 protocol payload that dxvk-remix decodes
//
// See docs/superpowers/specs/2026-05-22-fnv-ffp-protocol-design.md for the
// protocol contract.

#pragma once

#include "shared/common/console.hpp"

namespace shared::common
{
	enum class PsSlotRole : uint8_t {
		Other   = 0,
		Diffuse = 1,
		Normal  = 2,
		Glow    = 3,
		Height  = 4,
		Decal   = 5,
		Count   = 6,
	};

	// Per-PS slot-role map. slot_for_role[role] holds the D3D9 sampler stage
	// (0..7) that the PS declared for that role, or 0xF if the PS does not
	// have a sampler in that role. any_classified is true iff at least one
	// role was identified.
	struct PsSlotMap {
		static constexpr uint8_t kNoSlot = 0xF;
		uint8_t slot_for_role[static_cast<size_t>(PsSlotRole::Count)] = {
			kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot
		};
		bool any_classified = false;
		// True iff the PS declares any sampler whose name starts with "LOD"
		// (e.g. LODLandNoise, LODParentNormals, LODParentTex). FNV's distant-
		// terrain shaders do serious UV math to sample an LOD atlas at s0;
		// routing s0 as raw albedo tiles the atlas across each terrain tile.
		// The renderer uses this to skip RT and let rasterisation handle them.
		bool has_lod_sampler = false;

		uint8_t slot(PsSlotRole role) const {
			return slot_for_role[static_cast<size_t>(role)];
		}
		bool has(PsSlotRole role) const {
			return slot(role) != kNoSlot;
		}
	};

	// Caches PS_hash -> PsSlotMap. classify() returns a stable pointer into
	// the cache; null when the shader is null. Not thread-safe; the wrapper
	// drives all D3D9 traffic on the render thread.
	class PsShaderClassifier {
	public:
		// Returns a stable pointer to the cached PsSlotMap for this shader,
		// classifying it on first sight. Returns nullptr if shader is null.
		const PsSlotMap* classify(IDirect3DPixelShader9* shader) {
			if (!shader) return nullptr;

			// Cache hit by pointer (fast path; PS pointers are reused across draws)
			if (const auto it = ptr_cache_.find(shader); it != ptr_cache_.end()) {
				return &it->second;
			}

			// Fall back to hashing the bytecode
			UINT bytecode_size = 0;
			if (FAILED(shader->GetFunction(nullptr, &bytecode_size)) || bytecode_size == 0) {
				return nullptr;
			}
			std::vector<BYTE> bytecode(bytecode_size);
			if (FAILED(shader->GetFunction(bytecode.data(), &bytecode_size))) {
				return nullptr;
			}
			const uint32_t hash = shared::utils::data_hash32(bytecode.data(), bytecode_size);

			// Hash hit -> store ptr alias for next time and return
			if (const auto it = hash_cache_.find(hash); it != hash_cache_.end()) {
				ptr_cache_[shader] = it->second;
				return &ptr_cache_[shader];
			}

			// First sighting: parse sampler declarations
			PsSlotMap map = parse_samplers(bytecode.data(), hash);
			hash_cache_[hash] = map;
			ptr_cache_[shader] = map;
			return &ptr_cache_[shader];
		}

	private:
		static PsSlotRole role_for_name(const char* name) {
			if (!name) return PsSlotRole::Other;
			// Diffuse aliases first (BaseMap and TexMap are the most common)
			if (std::strcmp(name, "BaseMap") == 0)    return PsSlotRole::Diffuse;
			if (std::strcmp(name, "DiffuseMap") == 0) return PsSlotRole::Diffuse;
			if (std::strcmp(name, "TexMap") == 0)     return PsSlotRole::Diffuse;
			if (std::strcmp(name, "NormalMap") == 0)  return PsSlotRole::Normal;
			if (std::strcmp(name, "GlowMap") == 0)    return PsSlotRole::Glow;
			if (std::strcmp(name, "HeightMap") == 0)  return PsSlotRole::Height;
			// Decal samplers tag the draw via RS 42 InstanceCategories::DecalStatic;
			// the slot itself isn't routed (FFP rasterises BaseMap only), but the
			// path tracer needs the category to apply decal placement / blending.
			if (std::strcmp(name, "DecalMap") == 0)   return PsSlotRole::Decal;
			if (std::strcmp(name, "Decal2Map") == 0)  return PsSlotRole::Decal;
			return PsSlotRole::Other;
		}

		static PsSlotMap parse_samplers(const BYTE* bytecode, uint32_t hash) {
			PsSlotMap map;

			ID3DXConstantTable* table = nullptr;
			HRESULT hr = D3DXGetShaderConstantTable(
				reinterpret_cast<const DWORD*>(bytecode), &table);
			if (FAILED(hr) || !table) {
				log("PSReflect",
					std::format("PS 0x{:08X}: D3DXGetShaderConstantTable failed (hr=0x{:08X})",
						hash, static_cast<unsigned>(hr)),
					LOG_TYPE::LOG_TYPE_WARN);
				return map;
			}

			D3DXCONSTANTTABLE_DESC tdesc = {};
			table->GetDesc(&tdesc);

			std::string summary;
			UINT sampler_count = 0;

			for (UINT i = 0; i < tdesc.Constants; i++) {
				D3DXHANDLE h = table->GetConstant(nullptr, i);
				if (!h) continue;

				D3DXCONSTANT_DESC cdesc = {};
				UINT count = 1;
				if (FAILED(table->GetConstantDesc(h, &cdesc, &count))) continue;

				if (cdesc.RegisterSet != D3DXRS_SAMPLER) continue;

				const uint32_t slot = cdesc.RegisterIndex;
				if (slot >= 8) continue;  // D3D9 has 8 PS sampler stages

				const PsSlotRole role = role_for_name(cdesc.Name);

				if (!summary.empty()) summary += " ";
				summary += std::format("s{}={}", slot, cdesc.Name ? cdesc.Name : "<null>");
				sampler_count++;

				// Detect LOD-flavoured samplers by name prefix. Independent of
				// the semantic-role check below since LOD samplers don't map
				// to a routing role -- they're a "this draw is distant LOD"
				// signal for the renderer to skip RT entirely.
				if (cdesc.Name && std::strncmp(cdesc.Name, "LOD", 3) == 0) {
					map.has_lod_sampler = true;
				}

				if (role == PsSlotRole::Other) continue;

				// First-claim wins per role. With FNV's content this is unique
				// in practice; if it ever isn't, the log line will show all
				// declarations and we can tighten the policy.
				const size_t idx = static_cast<size_t>(role);
				if (map.slot_for_role[idx] == PsSlotMap::kNoSlot) {
					map.slot_for_role[idx] = static_cast<uint8_t>(slot);
					map.any_classified = true;
				}
			}

			table->Release();

			if (sampler_count == 0) {
				log("PSReflect",
					std::format("PS 0x{:08X}: no samplers declared", hash),
					LOG_TYPE::LOG_TYPE_DEFAULT);
			} else {
				log("PSReflect",
					std::format("PS 0x{:08X} ({} samplers): {} [diff=s{} norm=s{} glow=s{} height=s{} decal=s{} lod={}]",
						hash, sampler_count, summary,
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Diffuse)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Normal)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Glow)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Height)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Decal)],
						map.has_lod_sampler ? '1' : '0'),
					LOG_TYPE::LOG_TYPE_GREEN);
			}

			return map;
		}

		std::unordered_map<uint32_t, PsSlotMap> hash_cache_;
		std::unordered_map<IDirect3DPixelShader9*, PsSlotMap> ptr_cache_;
	};

	// Global instance; the renderer queries this every draw on the FFP-engaged
	// branches. SetPixelShader pre-populates the cache on first sighting.
	inline PsShaderClassifier g_ps_classifier;
}
