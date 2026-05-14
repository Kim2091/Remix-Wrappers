#include "std_include.hpp"
#include "renderer.hpp"

#include "diagnostics.hpp"
#include "imgui.hpp"
#include "shared/common/ffp_state.hpp"

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
			fnv_engage(dev);
			ffp.setup_albedo_texture(dev);
			hr = dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
			ffp.restore_textures(dev);
			im->m_stats._drawcall_prim.track_single();
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
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
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
