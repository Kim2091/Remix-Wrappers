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
				// Null the pointer and drop the initialized flag: D3DXCreateTextureFromFileA
				// leaves *tex untouched when it fails (missing berry.png), so a
				// release that isn't followed by a successful reload would leave a
				// freed pointer here for the next Reset / destructor to release
				// again, and for tab_about() to draw with.
				if (tex_addons::berry)
				{
					tex_addons::berry->Release();
					tex_addons::berry = nullptr;
				}
				tex_addons::initialized = false;
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
				// setup_albedo_texture only marks stage 0 dirty when ITS pick
				// differed from the game's binding. On the default AlbedoStage=0
				// path the pick IS the game's binding, so without this the
				// override below would never be restored and every subsequent
				// passthrough draw would sample this texture at s0.
				ffp.note_stage0_override(tex);
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

	// Returns true iff this draw should take the dedicated water route: the PS
	// is in FNV's WATER family AND it declared a NoiseMap we can use as the
	// albedo. Water declares no Diffuse-role sampler, so without this the draw
	// falls into PASS_NO_DIFFUSE_ROLE, gets tagged Ignore, and never reaches the
	// path tracer -- water simply isn't in the image.
	//
	// A WATER variant with no NoiseMap (none ship that way, but the classifier
	// doesn't assume it) returns false and keeps the old behaviour rather than
	// binding one of water's render targets as an albedo.
	static bool ps_is_water_shader(const shared::common::PsSlotMap* map)
	{
		return map && map->has_water_sampler
			&& map->water_albedo_slot != shared::common::PsSlotMap::kNoSlot
			&& shared::common::config::get().ffp.route_water_to_ffp;
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

		// Bone uploads for a skinned object are swallowed by game::on_set_vs_const_f
		// (replayed as SetTransform) and never reach the device's c[] file. Any
		// route that lands here with a skinned declaration is about to draw with
		// the GAME's real skinning vertex shader, which reads those registers.
		// draw_skinned_dip flushes them on its own fallbacks, but the offscreen-RT,
		// no-view-proj and 2D routes are all tested BEFORE the skinned branch and
		// would otherwise render FaceGen heads / shadow-map passes with whatever
		// bones happened to be left in the register file.
		if (ffp.cur_decl_is_skinned()) {
			game::flush_bones_to_device(dev);
		}

		game::disable_skinning(dev);
		ffp.disengage(dev);
		if (!s_world_is_identity_)
		{
			dev->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);
			s_world_is_identity_ = true;
		}
	}

	/*
	 * One-shot-per-shader report of the gate state a water draw arrives with.
	 *
	 * A water draw that still doesn't reach the screen can be failing at any of
	 * several points: an earlier branch in the routing chain eating it, a null
	 * albedo at the NoiseMap slot, or Remix dropping it after we hand it over.
	 * This logs every gate input the first time each water PS is seen at a draw
	 * call, so the failing stage is identifiable straight from logfile.txt.
	 *
	 * Pair with the WATER-EXEC line in setup_water_draw: a GATE line with no
	 * matching EXEC line means the routing chain consumed the draw before the
	 * water branch, and the flags on the GATE line say which branch did it.
	 *
	 * Cost on the hot path is a single pointer compare -- the classifier lookup
	 * only runs when the bound PS changed since the previous draw, and the set
	 * insert only ever runs for water shaders.
	 */
	static void log_water_gate_once()
	{
		auto& ffp = shared::common::ffp_state::get();
		auto* ps = ffp.last_ps();

		static IDirect3DPixelShader9* s_prev_ps = reinterpret_cast<IDirect3DPixelShader9*>(1);
		if (ps == s_prev_ps) return;
		s_prev_ps = ps;

		const auto* map = shared::common::g_ps_classifier.classify(ps);
		if (!map || !map->has_water_sampler) return;

		static std::unordered_set<uint32_t> s_logged;
		if (!s_logged.insert(map->ps_hash).second) return;

		const uint8_t slot = map->water_albedo_slot;
		const bool have_tex = (slot != shared::common::PsSlotMap::kNoSlot)
			&& (ffp.cur_texture(slot) != nullptr);

		shared::common::log("Water", std::format(
			"GATE PS 0x{:08X} wtex=s{} tex={} | backbuf={} vp={} 2d={} skinned={} posT={} sky={} normal={} cfg={}",
			map->ps_hash, slot, have_tex ? "OK" : "NULL",
			game::rendering_to_backbuffer ? 1 : 0,
			ffp.view_proj_valid() ? 1 : 0,
			game::is_2d() ? 1 : 0,
			ffp.cur_decl_is_skinned() ? 1 : 0,
			ffp.cur_decl_has_pos_t() ? 1 : 0,
			game::is_sky() ? 1 : 0,
			ffp.cur_decl_has_normal() ? 1 : 0,
			shared::common::config::get().ffp.route_water_to_ffp ? 1 : 0),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN);
	}

	/*
	 * Shared water draw setup: FFP-engage, bind the NoiseMap albedo, write the
	 * protocol. Split out so the indexed and non-indexed paths can't drift.
	 *
	 * The RS-149 payload names water's real device slots -- NoiseMap as the
	 * diffuse and, on the variants that declare one, NormalMap. dxvk-remix reads
	 * those slots straight from device state, so slots 1-7 must stay bound;
	 * setup_albedo_texture_stage_preserve is what guarantees that.
	 *
	 * Caller must follow the draw with remix_protocol::reset_all_slots +
	 * ffp.restore_textures.
	 */
	static void setup_water_draw(IDirect3DDevice9* dev, const shared::common::PsSlotMap* psmap)
	{
		auto& ffp = shared::common::ffp_state::get();

		fnv_engage(dev);
		game::disable_skinning(dev);

		const uint8_t albedoSlot = psmap->water_albedo_slot;

		// One-shot confirmation that the route actually ran, with the albedo we
		// resolved. See log_water_gate_once for how to read this against GATE.
		//
		// Also reports the albedo's pixel format and the device's alpha/blend
		// state. If water is still absent with WaterForceOpaque on, these say
		// whether opacity was ever the problem: a format with no alpha channel
		// (e.g. DXT1 / X8R8G8B8) rules the alpha theory out and points at
		// rtx.ignoreTextures instead.
		{
			static std::unordered_set<uint32_t> s_execed;
			if (s_execed.insert(psmap->ps_hash).second) {
				D3DFORMAT fmt = D3DFMT_UNKNOWN;
				UINT tw = 0, th = 0;
				if (auto* base = ffp.cur_texture(albedoSlot)) {
					IDirect3DTexture9* t2d = nullptr;
					if (SUCCEEDED(base->QueryInterface(IID_IDirect3DTexture9,
							reinterpret_cast<void**>(&t2d))) && t2d) {
						D3DSURFACE_DESC sd = {};
						if (SUCCEEDED(t2d->GetLevelDesc(0, &sd))) {
							fmt = sd.Format; tw = sd.Width; th = sd.Height;
						}
						t2d->Release();
					}
				}

				DWORD blend = 0, srcb = 0, dstb = 0, atest = 0, aref = 0;
				dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
				dev->GetRenderState(D3DRS_SRCBLEND, &srcb);
				dev->GetRenderState(D3DRS_DESTBLEND, &dstb);
				dev->GetRenderState(D3DRS_ALPHATESTENABLE, &atest);
				dev->GetRenderState(D3DRS_ALPHAREF, &aref);

				shared::common::log("Water", std::format(
					"EXEC PS 0x{:08X} albedo=s{} tex={} norm=s{} | fmt={} {}x{} | "
					"blend={} src={} dst={} atest={} aref={} forceOpaque={}",
					psmap->ps_hash, albedoSlot,
					ffp.cur_texture(albedoSlot) ? "OK" : "NULL",
					psmap->slot(shared::common::PsSlotRole::Normal),
					static_cast<int>(fmt), tw, th,
					blend, srcb, dstb, atest, aref,
					shared::common::config::get().ffp.water_force_opaque ? 1 : 0),
					shared::common::LOG_TYPE::LOG_TYPE_GREEN);
			}
		}

		ffp.setup_albedo_texture_stage_preserve(dev, static_cast<int>(albedoSlot));

		uint32_t water_categories =
			remix_protocol::category_mask(remix_protocol::CategoryBit::AnimatedWater);

		/*
		 * Force the surface opaque.
		 *
		 * fnv_engage leaves stage 0 at ALPHAOP=SELECTARG1 / ALPHAARG1=TEXTURE,
		 * which is right for foliage cutouts but wrong here: water's albedo is
		 * its NoiseMap, and that texture's alpha channel carries nothing
		 * meaningful. Taking opacity from it can resolve the entire water
		 * surface to ~zero alpha, which is visually identical to the draw never
		 * having been submitted -- the exact symptom this route was added to
		 * fix, reappearing one stage further down the pipe.
		 *
		 * Two halves, because the alpha reaches the image by two routes:
		 *   - rasterisation: take alpha from TFACTOR (white) instead of the
		 *     texture. TFACTOR's D3D9 default is already 0xFFFFFFFF, so setting
		 *     it needs no save/restore, and fnv_engage rewrites ALPHAARG1 on
		 *     every FFP draw, so the override cannot leak to the next one.
		 *   - path tracing: IgnoreAlphaChannel, so Remix doesn't re-derive the
		 *     same ~zero opacity from the albedo texture it samples itself.
		 *
		 * Translucency is not lost by this -- it was never present. It arrives
		 * with a material replacement keyed on the NoiseMap hash, which is why
		 * a stable albedo hash mattered in the first place.
		 */
		if (shared::common::config::get().ffp.water_force_opaque)
		{
			dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, 255, 255, 255));
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
			water_categories |=
				remix_protocol::category_mask(remix_protocol::CategoryBit::IgnoreAlphaChannel);
		}

		remix_protocol::set_category_flags(dev, water_categories);
		remix_protocol::set_modifier(dev,
			remix_protocol::encode_slot_roles(
				albedoSlot,
				psmap->slot(shared::common::PsSlotRole::Normal),
				remix_protocol::kSlotAbsent,
				remix_protocol::kSlotAbsent));
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
		 *   AND (hasNormal OR isSky OR isWater) AND !is2D -> FFP
		 *   Else -> passthrough
		 */

		// One classifier lookup per draw, shared by the gate below and every
		// helper inside it. Hoisted out of the branch because the water test is
		// part of the gate itself -- water's vertex declaration may carry no
		// NORMAL, and without this it would fail the hasNormal term and never
		// reach the FFP side at all.
		const auto* psmap = shared::common::g_ps_classifier.classify(ffp.last_ps());
		const bool is_water = ps_is_water_shader(psmap);

		if (ffp.is_enabled() && ffp.view_proj_valid() && game::rendering_to_backbuffer &&
			ffp.last_decl() && !ffp.cur_decl_has_pos_t() && !ffp.cur_decl_is_skinned() &&
			(ffp.cur_decl_has_normal() || game::is_sky() || is_water) && !game::is_2d())
		{
			// Water gets the same dedicated route as the indexed path -- tested
			// first so it can't be captured by the no-diffuse-role Ignore branch.
			if (is_water)
			{
				setup_water_draw(dev, psmap);
				hr = dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
				remix_protocol::reset_all_slots(dev);
				ffp.restore_textures(dev);
				im->m_stats._drawcall_prim.track_single();
				ctx.restore_all(dev);
				ctx.reset_context();
				return hr;
			}

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

		log_water_gate_once();

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
			// Must disengage like every other passthrough route. FFP stays
			// engaged across draws, and is_enabled() flips to false mid-frame
			// when the tracer starts a capture — without this the draw would run
			// with nulled shaders and the engine WORLD matrix still bound, which
			// is both a visual corruption and the opposite of what the capture is
			// supposed to record.
			fnv_disengage(dev);
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
		else if (ps_is_water_shader(shared::common::g_ps_classifier.classify(ffp.last_ps())))
		{
			// Water must be tested BEFORE the no-normal and no-diffuse-role gates
			// below: FNV's water declares no BaseMap/DiffuseMap/TexMap sampler, so
			// it would otherwise land in PASS_NO_DIFFUSE_ROLE and be tagged Ignore
			// -- the path tracer would skip it and water would be missing from the
			// image. Its albedo is the PS's own NoiseMap slot, which is why this
			// route can't use the AlbedoStage heuristic.
			PROFILE_ZONE_N("route_FFP_WATER");
			if (diag) diag->route("FFP_WATER");
			const auto* psmap = shared::common::g_ps_classifier.classify(ffp.last_ps());
			setup_water_draw(dev, psmap);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			remix_protocol::reset_all_slots(dev);
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
				game::lod_debug::count_near();

				// The with-normal near LOD is rasterise-only (Ignore) and fights
				// the path-traced terrain it overlaps. Its own vertex shader fades
				// it to alpha 0 anywhere within ~6964 units of the blend centre,
				// so near the player it is contributing nothing but the z-fight.
				// Drop it. Gated on has_normal so that RouteLodToFfp=0 -- which
				// sends BOTH LOD classes down this branch -- still draws the far
				// LOD normally. See config.hpp near_lod_mode for the full fade.
				if (shared::common::config::get().ffp.near_lod_mode == 1 &&
					ffp.cur_decl_has_normal())
				{
					ctx.restore_all(dev);
					ctx.reset_context();
					return S_OK;
				}

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
				// FNV terrain blends up to 7 BaseMap layers weighted by vertex
				// COLOR0.rgb + COLOR1.x (proven from the SLS terrain PS
				// disassembly: BaseMap[N] at s0..s(N-1), NormalMap[N] at s7+).
				// The single-albedo path can only pick ONE of those layers, so a
				// blended surface renders as flat layer 0. The multi-layer route
				// keeps all the slots bound and hands dxvk-remix the layer count
				// so it can reconstruct the blend.
				//
				// Default OFF: the payload zeroes the V1 slot-role nibbles, so a
				// runtime that doesn't decode kRemixMultiLayerTerrainBit reads
				// "no diffuse, no normal" and routes nothing at all -- strictly
				// worse than flat layer 0. Only enable against a dxvk-remix build
				// from fnv-terrain-ffp (or fnv-ffp), which has the decode.
				const bool multilayer =
					shared::common::config::get().ffp.multi_layer_terrain &&
					psmap && psmap->multi_layer_count >= 2;

				if (diag) diag->route(multilayer ? "FFP_TERRAIN_MULTILAYER"
					: is_terrain_shape ? "FFP_TERRAIN" : is_bi_shape ? "FFP_BI" : "FFP_WORLD");
				fnv_engage(dev);
				game::disable_skinning(dev);
				if (multilayer) {
					ffp.setup_albedo_texture_preserve_slots(dev);
				} else {
					ffp.setup_albedo_texture(dev, psmap);
				}

				// No-normal terrain LOD is the geomorph/sink-VS class: the distant
				// terrain vertex shader (SLS2002.vso) lowers a vertex ONLY when it
				// lands inside the loaded high-detail rectangle, after geomorphing
				// Z between the coarse and fine meshes:
				//
				//   morphedZ = lerp(TEXCOORD1.x, POSITION.z, GeomorphParams.x)
				//   inside   = |cx - HighDetailRange.x| < HighDetailRange.z
				//           && |cy - HighDetailRange.y| < HighDetailRange.w
				//   finalZ   = morphedZ - inside * GeomorphParams.y
				//
				// The legacy approach lowered the whole draw's WORLD matrix by a
				// constant, which is wrong in both directions at once: vertices
				// inside the rectangle got too little sink (coarse LOD z-fights the
				// real terrain) while vertices outside it got sink they should never
				// receive (distant terrain steps down at the LOD seam). No single
				// LodSinkZ can satisfy both. lod_sink runs the shader's own
				// arithmetic per vertex into a cached rewritten VB instead.
				//
				// LodSinkMode: 2 = per-vertex (default), 1 = legacy uniform world
				// sink, 0 = off. The per-vertex path falls back to the uniform sink
				// if the decl or constants are unusable, so it can never be worse.
				bool sunk_stream = false;
				if (ps_is_lod_shader(psmap) && !ffp.cur_decl_has_normal())
				{
					const auto& fcfg = shared::common::config::get().ffp;
					game::lod_debug::count_far();

					// Debug isolation: drop this LOD class entirely so it can be
					// ruled in or out as the source of a z-fight in one toggle.
					if (fcfg.debug_drop_far_lod)
					{
						ctx.restore_all(dev);
						ctx.reset_context();
						return S_OK;
					}

					if (fcfg.lod_sink_mode >= 2)
					{
						sunk_stream = game::lod_sink::bind_sunk_stream(
							dev, BaseVertexIndex, MinVertexIndex, NumVertices);
					}
					if (!sunk_stream && fcfg.lod_sink_mode >= 1)
					{
						const float cfg_z = fcfg.lod_sink_z;
						const float sink = (cfg_z < 0.0f)
							? ffp.vs_const_data()[19 * 4 + 1]   // AUTO: engine GeomorphParams.y (c19.y)
							: cfg_z;
						game::apply_world_sink(dev, sink);
					}
				}

				// PS-classifier drives slot-0 rebind + normal-map slot preservation
				// + RS 149 protocol payload. Always succeeds at this point because
				// the ps_has_diffuse_role gate above already filtered out the
				// no-diffuse cases.
				const bool wrote_protocol = multilayer
					? apply_multilayer_terrain_protocol(dev, psmap->multi_layer_count)
					: apply_ps_protocol(dev, psmap);
				// bind_sunk_stream rebased the vertex window to 0, so the draw must
				// not re-apply BaseVertexIndex; the index range is unchanged.
				hr = sunk_stream
					? dev->DrawIndexedPrimitive(PrimitiveType, 0, MinVertexIndex, NumVertices, startIndex, primCount)
					: dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
				if (sunk_stream) {
					game::lod_sink::unbind(dev);
				}
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
