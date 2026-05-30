#include "std_include.hpp"
#include "renderer.hpp"

#include "diagnostics.hpp"
#include "imgui.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/remix_protocol.hpp"
#include "shared/common/config.hpp"

namespace comp
{
	bool g_rendered_first_primitive = false;
	int g_is_rendering_something = 0;

	namespace tex_addons
	{
		bool initialized = false;
		LPDIRECT3DTEXTURE9 berry = nullptr;

		void init_texture_addons(bool release)
		{
			if (release)
			{
				if (tex_addons::berry) tex_addons::berry->Release();
				return;
			}

			shared::common::log("Renderer", "Loading CompMod Textures ...", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);

			auto load_texture = [](IDirect3DDevice9* dev, const char* path, LPDIRECT3DTEXTURE9* tex)
				{
					HRESULT hr;
					hr = D3DXCreateTextureFromFileA(dev, path, tex);
					if (FAILED(hr)) shared::common::log("Renderer", std::format("Failed to load {}", path), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				};

			const auto dev = shared::globals::d3d_device;
			load_texture(dev, "rtx_comp\\textures\\berry.png", &tex_addons::berry);
			tex_addons::initialized = true;
		}
	}


	// ----

	drawcall_mod_context& setup_context(IDirect3DDevice9* dev)
	{
		auto& ctx = renderer::dc_ctx;
		ctx.info.device_ptr = dev;
		return ctx;
	}


	// ----

	/*
	 * PS-classifier-driven RS-protocol setup for an FFP-engaged draw.
	 *
	 * setup_albedo_texture() picks an albedo by per-decl-shape heuristic and
	 * NULLs slots 1-7. That's only correct when slot 0 happens to be the
	 * actual diffuse texture. FNV's PS shaders preserve sampler names, so we
	 * can do better: look up the bound PS's (sampler name -> slot) map and
	 *   1. Rebind slot 0 with the slot that the PS labelled "BaseMap"
	 *      (or DiffuseMap / TexMap). This corrects FFP rasterisation for the
	 *      shaders where slot 0 was a NormalMap.
	 *   2. Restore the slot the PS labelled "NormalMap" at its original D3D9
	 *      stage, so dxvk-remix can read it straight from device state. Read
	 *      directly from d3d9State.textures[slot] -- this bypasses the
	 *      COLOROP-DISABLE binding-loop in d3d9_rtx.cpp that was eating the
	 *      slot-1 rebind in the previous attempt.
	 *   3. Write RS 149 with packed slot indices so the dxvk side knows
	 *      which device-state slots to read.
	 *
	 * Returns true if the protocol was written (caller must follow up with
	 * remix_protocol::reset_all_slots after the draw).
	 */
	static bool apply_ps_protocol(IDirect3DDevice9* dev, const shared::common::PsSlotMap* map)
	{
		auto& ffp = shared::common::ffp_state::get();
		if (!map || !map->any_classified) return false;

		const uint8_t diffuseSlot = map->slot(shared::common::PsSlotRole::Diffuse);
		const uint8_t normalSlot  = map->slot(shared::common::PsSlotRole::Normal);
		const uint8_t glowSlot    = map->slot(shared::common::PsSlotRole::Glow);
		const uint8_t heightSlot  = map->slot(shared::common::PsSlotRole::Height);

		// Without a diffuse role we have nothing to override at slot 0 -- fall
		// back to the legacy AlbedoStage heuristic and the unwritten sentinel.
		// (A purely-normal-map PS like 0x387C3875 hits this branch; the
		// wrapper-side fallback still binds setup_albedo_texture's pick, so
		// the rasterised colour is unchanged, but dxvk-remix doesn't get a
		// normal-channel routing for it either. V1 limitation.)
		if (diffuseSlot == shared::common::PsSlotMap::kNoSlot) return false;

		// Rebind slot 0 to the actual diffuse texture. setup_albedo_texture
		// may already have bound this exact pointer via the AlbedoStage
		// heuristic (e.g. when diffuseSlot matches the picked stage, or the
		// same texture is bound at two slots) -- skip the redundant write.
		if (auto* tex = ffp.cur_texture(diffuseSlot)) {
			if (tex != ffp.last_albedo_stage0()) {
				dev->SetTexture(0, tex);
			}
		}

		// setup_albedo_texture(dev, psmap) skipped the null-write for these
		// slots (they're in the kept-bound mask), so the device already holds
		// the correct texture. No rebind needed. The (slot != 0) guard remains
		// to skip shaders that pack normal-only into s0 -- those got rebound
		// above via the diffuse path.

		// PSes declaring DecalMap / Decal2Map samplers (e.g. blood splatters,
		// dirt overlays, bullet holes) get tagged DecalStatic via RS 42 so the
		// path tracer applies decal placement / blending. The decal-slot
		// textures themselves aren't routed -- FFP rasterises the BaseMap only
		// and the protocol contract has no decal nibble.
		uint32_t category_bits = 0;
		if (map->has(shared::common::PsSlotRole::Decal)) {
			category_bits |= remix_protocol::category_mask(
				remix_protocol::CategoryBit::DecalStatic);
		}
		if (category_bits != 0) {
			remix_protocol::set_category_flags(dev, category_bits);
		}

		remix_protocol::set_modifier(dev,
			remix_protocol::encode_slot_roles(diffuseSlot, normalSlot, glowSlot, heightSlot));
		return true;
	}

	// Multi-layer terrain protocol writer. Writes RS-149 with the MULTI_LAYER_TERRAIN
	// modifier bit + layer count in bits 16-19, so dxvk-remix's setLegacyMaterialState
	// captures all 14 sampler slots (s0..s(N-1) as albedos, s7..s(7+N-1) as normals).
	// The caller is responsible for ensuring slots 1-13 still hold FNV's bindings —
	// use ffp_state::setup_albedo_texture_preserve_slots, NOT setup_albedo_texture.
	//
	// Returns true (consistent with apply_ps_protocol's interface) so the caller pairs
	// it with remix_protocol::reset_all_slots after the draw.
	static bool apply_multilayer_terrain_protocol(IDirect3DDevice9* dev, uint8_t layerCount)
	{
		const uint32_t modifier = remix_protocol::encode_multilayer_terrain(layerCount);
		remix_protocol::set_modifier(dev, modifier);
		return true;
	}

	// Returns true iff the bound PS has a sampler the classifier maps to the
	// Diffuse role. FX / normal-only / postprocess shaders return false. The
	// FFP-engaged branches use this to decide whether to engage at all -- if
	// there's no albedo to bind at slot 0, engaging FFP would paint whatever
	// the game had at slot 0 (often a normal map) as the surface colour.
	static bool ps_has_diffuse_role(const shared::common::PsSlotMap* map)
	{
		return map && map->has(shared::common::PsSlotRole::Diffuse);
	}

	// Returns true iff the bound PS declares any LOD-prefixed sampler
	// (LODLandNoise, LODParentNormals, LODParentTex, ...). FNV's distant-
	// terrain shaders do serious UV math to sample an LOD atlas at s0;
	// engaging FFP and routing s0 as raw albedo paints the entire atlas
	// tiled across each tile. The renderer falls back to passthrough +
	// Ignore for these so the game's actual PS produces the rasterised
	// blend and the path tracer skips them entirely.
	static bool ps_is_lod_shader(const shared::common::PsSlotMap* map)
	{
		return map && map->has_lod_sampler;
	}

	// Track whether the device's WORLD matrix is currently identity, so
	// back-to-back passthrough draws skip the redundant SetTransform.
	// fnv_engage clears it (game::apply_transforms writes a non-identity
	// WORLD); device reset re-asserts identity (D3D9 default).
	static bool s_world_is_identity_ = true;

	void renderer_on_reset() { s_world_is_identity_ = true; }

	/*
	 * FNV-specific FFP engage: uses NiDX9Renderer matrices instead of VS constants.
	 * Calls ffp.engage() for shader nulling + texture stages + lighting,
	 * then overwrites transforms with NiDX9Renderer direct reads.
	 */
	static void fnv_engage(IDirect3DDevice9* dev)
	{
		auto& ffp = shared::common::ffp_state::get();
		ffp.engage(dev);
		game::apply_transforms(dev);
		s_world_is_identity_ = false;

		// FNV: alpha straight from texture (SELECTARG1), not modulated with vertex diffuse.
		// Preserves alpha channel for foliage/cutout transparency.
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	}

	/*
	 * FNV-specific FFP disengage: restore shaders and reset world to identity.
	 */
	static void fnv_disengage(IDirect3DDevice9* dev)
	{
		auto& ffp = shared::common::ffp_state::get();
		game::disable_skinning(dev);
		ffp.disengage(dev);
		if (!s_world_is_identity_)
		{
			dev->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);
			s_world_is_identity_ = true;
		}
	}


	// ----

	HRESULT renderer::on_draw_primitive(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE& PrimitiveType, const UINT& StartVertex, const UINT& PrimitiveCount)
	{
		PROFILE_ZONE();
		if (!g_rendered_first_primitive) {
			g_rendered_first_primitive = true;
		}

		if (!is_initialized() || shared::globals::imgui_is_rendering) {
			return dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		}

		static auto im = imgui::get();
		im->m_stats._drawcall_prim_incl_ignored.track_single();

		auto& ctx = setup_context(dev);
		auto& ffp = shared::common::ffp_state::get();
		ffp.increment_draw_count();

		if (auto* d = diagnostics::get()) {
			d->harvest_draw();
			d->on_draw_primitive(ffp.draw_call_count(), PrimitiveType, StartVertex, PrimitiveCount);
		}

		// Drop FNV fake-shadow overlays (see DrawIndexedPrimitive note).
		if (game::should_skip_fake_shadow(dev, static_cast<INT>(StartVertex), PrimitiveCount * 3, ffp.last_ps()))
		{
			ctx.restore_all(dev);
			ctx.reset_context();
			return S_OK;
		}

		auto hr = S_OK;

		/*
		 * FNV DrawPrimitive routing (ported from WD_DrawPrimitive):
		 *   viewProjValid AND has decl AND !POSITIONT AND !skinned
		 *   AND (hasNormal OR isSky) AND !is2D -> FFP
		 *   Else -> passthrough
		 */
		if (ffp.is_enabled() && ffp.view_proj_valid() && game::rendering_to_backbuffer &&
			ffp.last_decl() && !ffp.cur_decl_has_pos_t() && !ffp.cur_decl_is_skinned() &&
			(ffp.cur_decl_has_normal() || game::is_sky()) && !game::is_2d())
		{
			// One classifier lookup per draw, shared across all helpers below.
			const auto* psmap = shared::common::g_ps_classifier.classify(ffp.last_ps());

			// Non-sky draws whose PS has no diffuse role identified are FX /
			// normal-only / environment-cubemap shaders; engaging FFP would
			// paint slot 0 (often a normal map) as the surface colour, and
			// pure passthrough leaves Remix unable to recover a world
			// transform from the game's programmable VS -- the captured
			// geometry then floats with the camera. Passthrough so the
			// rasterised output is correct, AND tag InstanceCategories::Ignore
			// via RS 42 so Remix skips path-tracing this draw entirely.
			// LOD shaders stay passthrough when RouteLodToFfp is off, OR when the
			// LOD has a vertex normal -- the with-normal near land LOD fades via a
			// VS-computed alpha (oT4.x) that FFP can't reproduce, so it must keep its
			// own PS. Only the no-normal far LOD goes to FFP. See the indexed path.
			if (!game::is_sky() && (!ps_has_diffuse_role(psmap) ||
				(ps_is_lod_shader(psmap) &&
				 (!shared::common::config::get().ffp.route_lod_to_ffp || ffp.cur_decl_has_normal()))))
			{
				fnv_disengage(dev);
				remix_protocol::set_category_flags(dev,
					remix_protocol::category_mask(remix_protocol::CategoryBit::Ignore));
				hr = dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
				remix_protocol::reset_all_slots(dev);
				im->m_stats._drawcall_prim.track_single();
				im->m_stats._drawcall_using_vs.track_single();
			}
			else
			{
				fnv_engage(dev);
				// Sky draws skip the PS-classifier protocol; passing null to
				// setup_albedo_texture nulls all slots 1-7 like legacy behaviour,
				// matching apply_ps_protocol being a no-op for the sky branch.
				ffp.setup_albedo_texture(dev, game::is_sky() ? nullptr : psmap);

				// Sky draws don't have a normal map (cubemap / atmosphere only);
				// skip the PS-classifier override so dxvk sees the legacy path.
				const bool wrote_protocol = !game::is_sky() && apply_ps_protocol(dev, psmap);
				hr = dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
				if (wrote_protocol) {
					remix_protocol::reset_all_slots(dev);
				}
				ffp.restore_textures(dev);
				im->m_stats._drawcall_prim.track_single();
			}
		}
		else
		{
			fnv_disengage(dev);
			hr = dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
			im->m_stats._drawcall_prim.track_single();
			im->m_stats._drawcall_using_vs.track_single();
		}

		ctx.restore_all(dev);
		ctx.reset_context();

		return hr;
	}


	// ----

	HRESULT renderer::on_draw_indexed_prim(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE& PrimitiveType, const INT& BaseVertexIndex, const UINT& MinVertexIndex, const UINT& NumVertices, const UINT& startIndex, const UINT& primCount)
	{
		PROFILE_ZONE();
		if (!is_initialized() || shared::globals::imgui_is_rendering) {
			return dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		}

		auto& ctx = setup_context(dev);
		static auto im = imgui::get();
		auto& ffp = shared::common::ffp_state::get();
		ffp.increment_draw_count();

		if (auto* d = diagnostics::get()) {
			d->harvest_draw();
			d->on_draw_indexed_prim(ffp.draw_call_count(), dev, PrimitiveType, BaseVertexIndex, NumVertices, primCount);
		}

		im->m_stats._drawcall_indexed_prim_incl_ignored.track_single();

		if (ctx.modifiers.do_not_render)
		{
			ctx.restore_all(dev);
			ctx.reset_context();
			return S_OK;
		}

		// Drop FNV fake-shadow overlays (NOLIGHTING + all-gray vertex colors);
		// Remix ray-traces real shadows. Keep-listed PSes (Pip-Boy) are spared.
		if (game::should_skip_fake_shadow(dev, BaseVertexIndex, NumVertices, ffp.last_ps()))
		{
			ctx.restore_all(dev);
			ctx.reset_context();
			return S_OK;
		}

		auto hr = S_OK;

		/*
		 * FNV DrawIndexedPrimitive decision tree (ported from WD_DrawIndexedPrimitive):
		 *
		 *   !renderingToBackbuffer -> passthrough (FaceGen, shadow maps)
		 *   viewProjValid?
		 *   +-- NO  -> passthrough
		 *   +-- YES
		 *       +-- is2D (orthographic) -> passthrough (Pip-Boy, menus)
		 *       +-- isSkinned           -> game::draw_skinned_dip (decl cloning + bones)
		 *       +-- hasPosT             -> passthrough (screen-space pre-transformed)
		 *       +-- isSky               -> FFP (Remix handles as path-traced geometry)
		 *       +-- !hasNormal          -> passthrough (LOD terrain, effects, post-process)
		 *       +-- else                -> FFP (world geometry with NORMAL)
		 */
		auto* diag = diagnostics::get();

		if (!game::rendering_to_backbuffer)
		{
			PROFILE_ZONE_N("route_PASS_OFFSCREEN_RT");
			if (diag) diag->route("PASS_OFFSCREEN_RT");
			fnv_disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (!ffp.is_enabled() || !ffp.view_proj_valid())
		{
			PROFILE_ZONE_N("route_PASS_NO_VP");
			if (diag) diag->route("PASS_NO_VP");
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (game::is_2d())
		{
			PROFILE_ZONE_N("route_PASS_2D");
			if (diag) diag->route("PASS_2D");
			fnv_disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (ffp.cur_decl_is_skinned())
		{
			PROFILE_ZONE_N("route_SKINNED");
			if (diag) diag->route("SKINNED");
			// Skinned draws use a separate dispatch path with its own texture
			// setup; the slot-1 binding is unclear here, so don't claim normal-
			// map semantics. Revisit per-shader if skinned characters need it.
			hr = game::draw_skinned_dip(dev, PrimitiveType, BaseVertexIndex, MinVertexIndex,
				NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
		}
		else if (ffp.cur_decl_has_pos_t())
		{
			PROFILE_ZONE_N("route_PASS_POSTT");
			if (diag) diag->route("PASS_POSTT");
			fnv_disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (game::is_sky())
		{
			PROFILE_ZONE_N("route_FFP_SKY");
			if (diag) diag->route("FFP_SKY");
			fnv_engage(dev);
			game::disable_skinning(dev);
			ffp.setup_albedo_texture(dev);
			// Sky draws don't have a normal map at slot 1 (cubemap / atmosphere
			// only); leave the protocol untagged so Remix doesn't misroute.
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			ffp.restore_textures(dev);
			im->m_stats._drawcall_indexed_prim.track_single();
		}
		else if (!ffp.cur_decl_has_normal() &&
			!(shared::common::config::get().ffp.route_lod_to_ffp &&
			  ps_is_lod_shader(shared::common::g_ps_classifier.classify(ffp.last_ps()))))
		{
			// No-normal draws are HUD / effects / post-process / distant-terrain LOD.
			// EXCEPTION: when RouteLodToFfp is on, LOD-sampler shaders skip this gate
			// and fall through to FFP_GEO below so they get path-traced instead of
			// paying the passthrough tax. ps_harvest confirmed FNV's distant-terrain
			// LOD has normal=0, so it would otherwise be trapped here -- three branches
			// before the LOD branch. Remix synthesises geometric normals for FFP geo,
			// so the missing vertex normal is fine; the open risk is the s0 LOD atlas
			// tiling if the game does its sub-tile UV math in the pixel shader.
			PROFILE_ZONE_N("route_PASS_NO_NORMAL");
			if (diag) diag->route("PASS_NO_NORMAL");
			fnv_disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else
		{
			// One classifier lookup per FFP-candidate draw, shared across the
			// no-diffuse / LOD / FFP_GEO branches below (was 2-3 lookups before).
			const auto* psmap = shared::common::g_ps_classifier.classify(ffp.last_ps());

			if (!ps_has_diffuse_role(psmap))
			{
				// FX / normal-only / environment-cubemap / postprocess shaders
				// that don't expose an albedo sampler. Engaging FFP would bind
				// slot 0 (often the normal map) as the surface colour, and pure
				// passthrough leaves Remix unable to recover a world transform
				// from the game's programmable VS -- the captured geometry then
				// floats with the camera. Passthrough so the rasterised output is
				// correct, AND tag InstanceCategories::Ignore via RS 42 so Remix
				// skips path-tracing this draw entirely.
				PROFILE_ZONE_N("route_PASS_NO_DIFFUSE_ROLE");
				if (diag) diag->route("PASS_NO_DIFFUSE_ROLE");
				fnv_disengage(dev);
				remix_protocol::set_category_flags(dev,
					remix_protocol::category_mask(remix_protocol::CategoryBit::Ignore));
				hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
				remix_protocol::reset_all_slots(dev);
				im->m_stats._drawcall_indexed_prim.track_single();
				im->m_stats._drawcall_indexed_prim_using_vs.track_single();
			}
			else if (ps_is_lod_shader(psmap) &&
				(!shared::common::config::get().ffp.route_lod_to_ffp || ffp.cur_decl_has_normal()))
			{
				// LOD land shaders keep passthrough in two cases:
				//   * RouteLodToFfp off -> legacy behaviour (everything passthrough).
				//   * The WITH-NORMAL near land LOD (e.g. 0x6626FACE) -> its vertex
				//     shader computes a distance ALPHA FADE (oT4.x) that the PS emits
				//     as output alpha, so the LOD dissolves as you approach. FFP takes
				//     alpha from the texture (SELECTARG1), not that computed fade, so
				//     under FFP the near LOD stays fully opaque and covers the real
				//     terrain until it stops drawing. Passthrough preserves the fade.
				// Only the no-normal FAR LOD (0x36E87D02) goes to FFP (+ world sink).
				// Tag Ignore so the path tracer skips RT -- rasterisation is output.
				PROFILE_ZONE_N("route_PASS_LOD_SHADER");
				if (diag) diag->route("PASS_LOD_SHADER");
				fnv_disengage(dev);
				remix_protocol::set_category_flags(dev,
					remix_protocol::category_mask(remix_protocol::CategoryBit::Ignore));
				hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
				remix_protocol::reset_all_slots(dev);
				im->m_stats._drawcall_indexed_prim.track_single();
				im->m_stats._drawcall_indexed_prim_using_vs.track_single();
			}
			else
			{
				PROFILE_ZONE_N("route_FFP_GEO");
				const bool is_terrain_shape = (ffp.cur_decl_has_color() && ffp.cur_decl_n_texcoords() >= 2);
				const bool is_bi_shape = (!ffp.cur_decl_is_skinned() && ffp.cur_decl_has_blendindices());

				// All world-geometry-with-normal goes through FFP. Per-decl-shape
				// AlbedoStage logic lives inside ffp.setup_albedo_texture() — for
				// HQ terrain / BI / multi-tile-blend shapes, it samples a non-zero
				// stage to avoid sampling LOD-atlas leftover that gets stuck on
				// stage 0 from prior LOD-passthrough draws. Multi-layer terrain
				// flows through this same path -- one of its layer textures becomes
				// the surface albedo and the path tracer treats it as single-layer.
				if (diag) diag->route(is_terrain_shape ? "FFP_TERRAIN" : is_bi_shape ? "FFP_BI" : "FFP_WORLD");
				fnv_engage(dev);
				game::disable_skinning(dev);
				ffp.setup_albedo_texture(dev, psmap);

				// No-normal terrain LOD is the geomorph/sink-VS class (F36CCF49): the
				// VS sinks vertices inside HighDetailRange by GeomorphParams.y so the
				// coarse LOD tucks under real terrain. FFP can't run that per-vertex
				// box test, and a per-DRAW approximation can't reproduce chunks that
				// straddle the loaded-cell boundary (verified empirically -- per-draw
				// under-sinks straddlers and seams). So we sink the whole draw
				// uniformly by a small amount: AUTO (LodSinkZ < 0) uses the engine's
				// own GeomorphParams.y; tune LodSinkZ down to trade a little steady-
				// state poke-through for less terrain drop during cell streaming.
				if (ps_is_lod_shader(psmap) && !ffp.cur_decl_has_normal())
				{
					const float cfg_z = shared::common::config::get().ffp.lod_sink_z;
					const float sink = (cfg_z < 0.0f)
						? ffp.vs_const_data()[19 * 4 + 1]   // AUTO: engine GeomorphParams.y (c19.y)
						: cfg_z;
					game::apply_world_sink(dev, sink);
				}

				// PS-classifier drives slot-0 rebind + normal-map slot preservation
				// + RS 149 protocol payload. Always succeeds at this point because
				// the ps_has_diffuse_role gate above already filtered out the
				// no-diffuse cases.
				const bool wrote_protocol = apply_ps_protocol(dev, psmap);
				hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
				if (wrote_protocol) {
					remix_protocol::reset_all_slots(dev);
				}
				ffp.restore_textures(dev);
				im->m_stats._drawcall_indexed_prim.track_single();
			}
		}

		ctx.restore_all(dev);
		ctx.reset_context();

		return hr;
	}

	// ---

	void renderer::manually_trigger_remix_injection(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE();
		if (!m_triggered_remix_injection)
		{
			auto& ctx = dc_ctx;

			dev->SetRenderState(D3DRS_FOGENABLE, FALSE);

			ctx.save_vs(dev);
			dev->SetVertexShader(nullptr);
			ctx.save_ps(dev);
			dev->SetPixelShader(nullptr);

			ctx.save_rs(dev, D3DRS_ZWRITEENABLE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

			struct CUSTOMVERTEX
			{
				float x, y, z, rhw;
				D3DCOLOR color;
			};

			const auto color = D3DCOLOR_COLORVALUE(0, 0, 0, 0);
			const auto w = -0.49f;
			const auto h = -0.495f;

			CUSTOMVERTEX vertices[] =
			{
				{ -0.5f, -0.5f, 0.0f, 1.0f, color },
				{     w, -0.5f, 0.0f, 1.0f, color },
				{ -0.5f,     h, 0.0f, 1.0f, color },
				{     w,     h, 0.0f, 1.0f, color }
			};

			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(CUSTOMVERTEX));

			ctx.restore_vs(dev);
			ctx.restore_ps(dev);
			ctx.restore_render_state(dev, D3DRS_ZWRITEENABLE);
			m_triggered_remix_injection = true;
		}
	}


	// ---

	renderer::renderer()
	{
		p_this = this;

		shared::common::ffp_state::get().init(shared::globals::d3d_device);

		m_initialized = true;
		shared::common::log("Renderer", "FNV renderer module initialized.", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	renderer::~renderer()
	{
		tex_addons::init_texture_addons(true);
		game::release_skin_cache();
	}
}
