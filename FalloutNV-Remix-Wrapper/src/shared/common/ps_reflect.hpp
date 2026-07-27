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

#include <fstream>

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
		// Bytecode hash of the PS this map was parsed from (0 if unset). Lets
		// consumers log/identify the shader without re-hashing.
		uint32_t ps_hash = 0;
		// True iff the PS declares any sampler whose name starts with "LOD"
		// (e.g. LODLandNoise, LODParentNormals, LODParentTex). FNV's distant-
		// terrain shaders do serious UV math to sample an LOD atlas at s0;
		// routing s0 as raw albedo tiles the atlas across each terrain tile.
		// The renderer uses this to skip RT and let rasterisation handle them.
		bool has_lod_sampler = false;

		// True iff the PS belongs to FNV's WATER shader family.
		//
		// No WATER variant declares BaseMap / DiffuseMap / TexMap, so without
		// this signal every water draw fails the Diffuse-role gate in the
		// renderer, gets tagged InstanceCategories::Ignore, and disappears from
		// the path-traced image entirely.
		//
		// Signature (verified against all 16 shipped shaderpackage*.sdp files):
		// ReflectionMap / RefractionMap / DepthMap / DisplacementMap are each
		// declared by the WATER family and by nothing else. NoiseMap is shared
		// with two ImageSpace post-process shaders (ISNOISENORMALMAP,
		// ISNOISESCROLLANDBLEND), but those bind it at s0 while WATER always
		// binds it at s2 -- so the slot disambiguates.
		bool has_water_sampler = false;

		// The slot WATER declared NoiseMap at (s2 in every shipped variant).
		// Water's other samplers are render targets whose contents change every
		// frame -- ReflectionMap, RefractionMap and DepthMap all hash differently
		// per frame, which would defeat any Remix material replacement. NoiseMap
		// is water's only *static* texture, so it is both the FFP albedo and the
		// stable hash a translucent water material can be keyed on.
		// kNoSlot when the variant declares no NoiseMap; the renderer then
		// declines the water route and leaves legacy behaviour untouched.
		uint8_t water_albedo_slot = kNoSlot;

		// Multi-layer terrain (PS declares BaseMap[N] for N >= 2 alongside a
		// matching NormalMap[N] at samplers s7..s(7+N-1)). 0 means "not multi-
		// layer terrain"; values 2-7 mean "N layers detected." See the multi-
		// layer plan for the corresponding dxvk-remix wire format.
		uint8_t multi_layer_count = 0;

		// Set to true when BaseMap[N>=2] is declared but the matching NormalMap
		// layout doesn't line up (different N, or NormalMap doesn't start at s7).
		// The renderer routes ambiguous draws to Ignore + logs the mismatch.
		bool multi_layer_ambiguous = false;

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
		// NOTE: deliberately no destructor-driven clear(). The global instance
		// is destroyed during static teardown, which can run after the D3D9
		// device and the Remix bridge DLL are already gone -- releasing the
		// cached shaders there would be a crash on exit. Letting the process
		// reclaim them is the correct trade.

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
				return &insert_ptr_alias(shader, it->second);
			}

			// First sighting: parse sampler declarations
			PsSlotMap map = parse_samplers(bytecode.data(), hash);
			hash_cache_[hash] = map;
			return &insert_ptr_alias(shader, map);
		}

		// Drops every reference the pointer cache owns. Not called anywhere by
		// default (see the note above); only safe while the D3D9 device is still
		// alive, and invalidates every map handed out by classify().
		void clear() {
			for (auto& [shader, map] : ptr_cache_) {
				if (shader) shader->Release();
			}
			ptr_cache_.clear();
			hash_cache_.clear();
		}

	private:
		// Adds the (shader pointer -> map) alias, taking a reference on the
		// shader. The pointer IS the cache key, so if the game released a shader
		// and the allocator handed the same address to a different one, every
		// later lookup would return the previous shader's slot map -- textures
		// routed to the wrong material channel, draws tagged Ignore that
		// shouldn't be. Owning a reference keeps the address unique for the
		// process lifetime. FNV creates a few hundred pixel shaders, so holding
		// them all alive costs nothing worth measuring.
		PsSlotMap& insert_ptr_alias(IDirect3DPixelShader9* shader, const PsSlotMap& map) {
			const auto [it, inserted] = ptr_cache_.try_emplace(shader, map);
			if (inserted) {
				shader->AddRef();
			}
			return it->second;
		}

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
			map.ps_hash = hash;

			// Dump disassembly for the terrain-shape PSes seen in the FFPRoute
			// diagnostic so the maintainer can read what FNV's terrain actually
			// computes and build an FFP-equivalent stage setup. Fires once per
			// first sighting of a target hash. Output lands next to the FNV exe.
			// Remove the target list when no longer needed.
			constexpr uint32_t kPsDumpTargets[] = {
				0x47EC69D6u, // 1 sampler DiffuseMap, blend OFF, shape=TERRAIN
				0xB1753588u, // BaseMap + s7=NormalMap, blend OFF
				0xD9F171A4u, // BaseMap + s7=NormalMap, blend OFF
				0x28D33296u, // BaseMap + s7=NormalMap, blend OFF
				0x8F4BB6C0u, // BaseMap + s7=NormalMap, blend OFF
				0xED8C1984u, // BaseMap + s7=NormalMap, blend OFF
				0x7BE50954u, // BaseMap + s7=NormalMap, blend OFF
				0x815B7F80u, // 1 sampler DiffuseMap, blend ON, shape=TERRAIN
			};
			for (uint32_t target : kPsDumpTargets) {
				if (hash != target) continue;
				ID3DXBuffer* disasm = nullptr;
				HRESULT dhr = D3DXDisassembleShader(
					reinterpret_cast<const DWORD*>(bytecode), FALSE, nullptr, &disasm);
				if (SUCCEEDED(dhr) && disasm) {
					const std::string path = std::format("ps_dump_{:08X}.asm", hash);
					std::ofstream f(path, std::ios::binary);
					if (f) {
						f.write(static_cast<const char*>(disasm->GetBufferPointer()),
							static_cast<std::streamsize>(disasm->GetBufferSize()));
						log("PSReflect",
							std::format("Dumped PS 0x{:08X} disassembly to {}", hash, path),
							LOG_TYPE::LOG_TYPE_GREEN);
					}
					disasm->Release();
				}
				break;
			}

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

			// WATER binds NoiseMap here in every shipped variant; the two
			// ImageSpace shaders that also declare a NoiseMap bind it at s0.
			constexpr uint32_t kWaterNoiseSlot = 2;
			uint8_t noise_slot = PsSlotMap::kNoSlot;

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

				// WATER-family detection (see PsSlotMap::has_water_sampler).
				// Also independent of the role check -- water declares no
				// Diffuse-role sampler at all, which is precisely why it needs
				// its own signal to avoid being tagged Ignore.
				if (cdesc.Name) {
					if (std::strcmp(cdesc.Name, "ReflectionMap") == 0 ||
						std::strcmp(cdesc.Name, "RefractionMap") == 0 ||
						std::strcmp(cdesc.Name, "DepthMap") == 0 ||
						std::strcmp(cdesc.Name, "DisplacementMap") == 0) {
						map.has_water_sampler = true;
					}
					else if (std::strcmp(cdesc.Name, "NoiseMap") == 0) {
						noise_slot = static_cast<uint8_t>(slot);
						if (slot == kWaterNoiseSlot) map.has_water_sampler = true;
					}
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

				// Multi-layer terrain detection: BaseMap is declared as a sampler
				// ARRAY (RegisterCount > 1). FNV terrain convention is BaseMap[N]
				// at s0 + NormalMap[N] at s7 -- the NormalMap alignment is validated
				// below in a follow-up scan after the loop.
				if (role == PsSlotRole::Diffuse && cdesc.RegisterCount > 1 && map.multi_layer_count == 0) {
					map.multi_layer_count = static_cast<uint8_t>(cdesc.RegisterCount);
				}
			}

			// Water's albedo is its NoiseMap -- the only sampler in the family
			// that isn't a per-frame render target. A WATER variant with no
			// NoiseMap leaves this at kNoSlot and the renderer declines the
			// water route rather than guessing at a render target.
			if (map.has_water_sampler) {
				map.water_albedo_slot = noise_slot;
			}

			if (map.multi_layer_count > 0) {
				// Expect NormalMap declared as ARRAY of the same count at s7.
				// Re-scan the constant table for the NormalMap declaration.
				bool normalAligns = false;
				for (UINT i = 0; i < tdesc.Constants; i++) {
					D3DXHANDLE h2 = table->GetConstant(nullptr, i);
					if (!h2) continue;
					D3DXCONSTANT_DESC cd = {};
					UINT cnt = 1;
					if (FAILED(table->GetConstantDesc(h2, &cd, &cnt))) continue;
					if (cd.RegisterSet != D3DXRS_SAMPLER) continue;
					if (cd.Name && std::strcmp(cd.Name, "NormalMap") == 0
							&& cd.RegisterIndex == 7
							&& cd.RegisterCount == map.multi_layer_count) {
						normalAligns = true;
						break;
					}
				}
				if (!normalAligns) {
					log("PSReflect",
						std::format("PS 0x{:08X}: BaseMap[{}] declared but NormalMap[N] at s7 doesn't match -- multi-layer disabled, draw will be Ignored.",
							hash, map.multi_layer_count),
						LOG_TYPE::LOG_TYPE_WARN);
					map.multi_layer_ambiguous = true;
					map.multi_layer_count = 0;
				}
			}

			table->Release();

			if (sampler_count == 0) {
				log("PSReflect",
					std::format("PS 0x{:08X}: no samplers declared", hash),
					LOG_TYPE::LOG_TYPE_DEFAULT);
			} else {
				log("PSReflect",
					std::format("PS 0x{:08X} ({} samplers): {} [diff=s{} norm=s{} glow=s{} height=s{} decal=s{} lod={} water={} wtex=s{} mlc={}]",
						hash, sampler_count, summary,
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Diffuse)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Normal)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Glow)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Height)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Decal)],
						map.has_lod_sampler ? '1' : '0',
						map.has_water_sampler ? '1' : '0',
						map.water_albedo_slot,
						map.multi_layer_count),
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
