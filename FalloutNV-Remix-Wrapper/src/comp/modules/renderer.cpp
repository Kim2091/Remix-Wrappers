#include "std_include.hpp"
#include "renderer.hpp"

#include "diagnostics.hpp"
#include "imgui.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/remix_protocol.hpp"

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
	static bool apply_ps_protocol(IDirect3DDevice9* dev)
	{
		auto& ffp = shared::common::ffp_state::get();
		const auto* map = shared::common::g_ps_classifier.classify(ffp.last_ps());
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

		// Rebind slot 0 to the actual diffuse texture (might already match
		// what setup_albedo_texture picked; SetTexture is idempotent).
		if (auto* tex = ffp.cur_texture(diffuseSlot)) {
			dev->SetTexture(0, tex);
		}

		// Restore the normal-map binding at its original stage so dxvk can
		// read d3d9State.textures[normalSlot] in setLegacyMaterialState.
		// setup_albedo_texture() already NULLed slots 1-7, so we re-bind here.
		// (If normalSlot is 0 -- shaders that pack normal-only into s0 -- the
		// rebind above already handled it.)
		if (normalSlot != shared::common::PsSlotMap::kNoSlot && normalSlot != 0) {
			if (auto* tex = ffp.cur_texture(normalSlot)) {
				dev->SetTexture(normalSlot, tex);
			}
		}

		// Glow likewise.
		if (glowSlot != shared::common::PsSlotMap::kNoSlot && glowSlot != 0) {
			if (auto* tex = ffp.cur_texture(glowSlot)) {
				dev->SetTexture(glowSlot, tex);
			}
		}

		// Height likewise. Same rebind pattern; dxvk-remix reads
		// d3d9State.textures[heightSlot] and routes it into the opaque
		// material's height channel (driving parallax-occlusion mapping).
		if (heightSlot != shared::common::PsSlotMap::kNoSlot && heightSlot != 0) {
			if (auto* tex = ffp.cur_texture(heightSlot)) {
				dev->SetTexture(heightSlot, tex);
			}
		}

		remix_protocol::set_modifier(dev,
			remix_protocol::encode_slot_roles(diffuseSlot, normalSlot, glowSlot, heightSlot));
		return true;
	}

	// Returns true iff the bound PS has a sampler the classifier maps to the
	// Diffuse role. FX / normal-only / postprocess shaders return false. The
	// FFP-engaged branches use this to decide whether to engage at all -- if
	// there's no albedo to bind at slot 0, engaging FFP would paint whatever
	// the game had at slot 0 (often a normal map) as the surface colour.
	static bool ps_has_diffuse_role(IDirect3DPixelShader9* ps)
	{
		const auto* map = shared::common::g_ps_classifier.classify(ps);
		return map && map->has(shared::common::PsSlotRole::Diffuse);
	}

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
		dev->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);
	}


	// ----

	HRESULT renderer::on_draw_primitive(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE& PrimitiveType, const UINT& StartVertex, const UINT& PrimitiveCount)
	{
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

		if (auto* d = diagnostics::get())
			d->on_draw_primitive(ffp.draw_call_count(), PrimitiveType, StartVertex, PrimitiveCount);

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
			// Non-sky draws whose PS has no diffuse role identified are FX /
			// normal-only / environment-cubemap shaders; engaging FFP would
			// paint slot 0 (often a normal map) as the surface colour, and
			// pure passthrough leaves Remix unable to recover a world
			// transform from the game's programmable VS -- the captured
			// geometry then floats with the camera. Passthrough so the
			// rasterised output is correct, AND tag InstanceCategories::Ignore
			// via RS 42 so Remix skips path-tracing this draw entirely.
			if (!game::is_sky() && !ps_has_diffuse_role(ffp.last_ps()))
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
				ffp.setup_albedo_texture(dev);

				// Sky draws don't have a normal map (cubemap / atmosphere only);
				// skip the PS-classifier override so dxvk sees the legacy path.
				const bool wrote_protocol = !game::is_sky() && apply_ps_protocol(dev);
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
		if (!is_initialized() || shared::globals::imgui_is_rendering) {
			return dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		}

		auto& ctx = setup_context(dev);
		const auto im = imgui::get();
		auto& ffp = shared::common::ffp_state::get();
		ffp.increment_draw_count();

		if (auto* d = diagnostics::get())
			d->on_draw_indexed_prim(ffp.draw_call_count(), dev, PrimitiveType, BaseVertexIndex, NumVertices, primCount);

		im->m_stats._drawcall_indexed_prim_incl_ignored.track_single();

		if (ctx.modifiers.do_not_render)
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
			if (diag) diag->route("PASS_OFFSCREEN_RT");
			fnv_disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (!ffp.is_enabled() || !ffp.view_proj_valid())
		{
			if (diag) diag->route("PASS_NO_VP");
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (game::is_2d())
		{
			if (diag) diag->route("PASS_2D");
			fnv_disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (ffp.cur_decl_is_skinned())
		{
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
			if (diag) diag->route("PASS_POSTT");
			fnv_disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (game::is_sky())
		{
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
		else if (!ffp.cur_decl_has_normal())
		{
			if (diag) diag->route("PASS_NO_NORMAL");
			fnv_disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (!ps_has_diffuse_role(ffp.last_ps()))
		{
			// FX / normal-only / environment-cubemap / postprocess shaders
			// that don't expose an albedo sampler. Engaging FFP would bind
			// slot 0 (often the normal map) as the surface colour, and pure
			// passthrough leaves Remix unable to recover a world transform
			// from the game's programmable VS -- the captured geometry then
			// floats with the camera. Passthrough so the rasterised output is
			// correct, AND tag InstanceCategories::Ignore via RS 42 so Remix
			// skips path-tracing this draw entirely.
			if (diag) diag->route("PASS_NO_DIFFUSE_ROLE");
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
			// All world-geometry-with-normal goes through FFP. Per-decl-shape
			// AlbedoStage logic lives inside ffp.setup_albedo_texture() — for
			// HQ terrain / BI / multi-tile-blend shapes, it samples a non-zero
			// stage to avoid sampling LOD-atlas leftover that gets stuck on
			// stage 0 from prior LOD-passthrough draws.
			const bool is_terrain_shape = (ffp.cur_decl_has_color() && ffp.cur_decl_n_texcoords() >= 2);
			const bool is_bi_shape = (!ffp.cur_decl_is_skinned() && ffp.cur_decl_has_blendindices());
			if (diag) diag->route(is_terrain_shape ? "FFP_TERRAIN" : is_bi_shape ? "FFP_BI" : "FFP_WORLD");
			fnv_engage(dev);
			game::disable_skinning(dev);
			ffp.setup_albedo_texture(dev);

			// PS-classifier drives slot-0 rebind + normal-map slot preservation
			// + RS 149 protocol payload. Always succeeds at this point because
			// the ps_has_diffuse_role gate above already filtered out the
			// no-diffuse cases.
			const bool wrote_protocol = apply_ps_protocol(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			if (wrote_protocol) {
				remix_protocol::reset_all_slots(dev);
			}
			ffp.restore_textures(dev);
			im->m_stats._drawcall_indexed_prim.track_single();
		}

		ctx.restore_all(dev);
		ctx.reset_context();

		return hr;
	}

	// ---

	void renderer::manually_trigger_remix_injection(IDirect3DDevice9* dev)
	{
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
