#include "std_include.hpp"
#include "shared/common/flags.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"

/*
 * Fallout: New Vegas game-specific code.
 *
 * Ported from patches/FNV_proxy/d3d9_device.c (C standalone proxy) to the
 * remix-comp C++ framework. All game addresses and engine structure offsets
 * come from NewVegasRTXHelper.
 */

namespace comp::game
{
	// --- Skinning hook globals ---

	static volatile int g_render_skinned    = 0;  // 1 while inside skinned render batch
	static volatile int g_bone_reset_pending = 0;  // 1 when per-object bone reset needed
	static bool g_hooks_installed = false;

	// Declaration cloning cache (original -> cloned with UBYTE4 BLENDINDICES)
	static IDirect3DVertexDeclaration9* skin_decl_orig[SKIN_DECL_CACHE_SIZE]  = {};
	static IDirect3DVertexDeclaration9* skin_decl_clone[SKIN_DECL_CACHE_SIZE] = {};
	static int skin_decl_count = 0;


	// ================================================================
	// Game memory helpers
	// ================================================================

	static inline float game_float(void* base, unsigned int off) {
		return *reinterpret_cast<float*>(static_cast<unsigned char*>(base) + off);
	}

	static inline float* game_float_ptr(void* base, unsigned int off) {
		return reinterpret_cast<float*>(static_cast<unsigned char*>(base) + off);
	}

	static inline void* game_ptr(void* base, unsigned int off) {
		return *reinterpret_cast<void**>(static_cast<unsigned char*>(base) + off);
	}


	// ================================================================
	// NiDX9Renderer helpers
	// ================================================================

	float* get_renderer_matrix(unsigned int byte_offset)
	{
		if (!renderer_singleton_ptr) return nullptr;
		void* ren = *renderer_singleton_ptr;
		if (!ren) return nullptr;
		return reinterpret_cast<float*>(static_cast<unsigned char*>(ren) + byte_offset);
	}

	// Read NiShadeProperty::m_eShaderType from renderer property chain.
	// NiDX9Renderer +0x0C -> NiPropertyState +0x0C -> NiShadeProperty +0x1C
	static unsigned int get_shade_property_type()
	{
		if (!renderer_singleton_ptr) return 0;
		void* ren = *renderer_singleton_ptr;
		if (!ren) return 0;

		auto* prop_state = *reinterpret_cast<void**>(static_cast<unsigned char*>(ren) + 0x0C);
		if (!prop_state) return 0;

		auto* shade_prop = *reinterpret_cast<void**>(static_cast<unsigned char*>(prop_state) + 0x0C);
		if (!shade_prop) return 0;

		return *reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(shade_prop) + 0x1C);
	}

	bool is_2d()
	{
		float* proj = get_renderer_matrix(RENDERER_PROJ_OFF);
		if (!proj) return false;
		// Orthographic: m[3][3]==1.0 and m[2][3]==0.0 (no perspective divide)
		return (proj[15] == 1.0f && proj[11] == 0.0f);
	}

	bool is_sky()
	{
		return get_shade_property_type() == KPROP_SKY;
	}

	void apply_transforms(IDirect3DDevice9* dev)
	{
		float* world = get_renderer_matrix(RENDERER_WORLD_OFF);
		float* view  = get_renderer_matrix(RENDERER_VIEW_OFF);
		float* proj  = get_renderer_matrix(RENDERER_PROJ_OFF);

		if (world && view && proj)
		{
			dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(world));
			dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(view));
			dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(proj));
		}
	}


	// ================================================================
	// Render target tracking
	// ================================================================

	void init_backbuffer_tracking(IDirect3DDevice9* dev)
	{
		IDirect3DSurface9* bb = nullptr;
		if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
		{
			D3DSURFACE_DESC desc;
			if (SUCCEEDED(bb->GetDesc(&desc)))
			{
				backbuffer_width = desc.Width;
				backbuffer_height = desc.Height;
				shared::common::log("Game", std::format("Backbuffer: {}x{}", desc.Width, desc.Height));
			}
			bb->Release();
		}
		rendering_to_backbuffer = true;
	}

	void on_set_render_target(IDirect3DDevice9* /*dev*/, DWORD idx, IDirect3DSurface9* surface)
	{
		if (idx != 0 || !surface) return;

		D3DSURFACE_DESC desc;
		if (SUCCEEDED(surface->GetDesc(&desc)))
			rendering_to_backbuffer = (desc.Width == backbuffer_width && desc.Height == backbuffer_height);
		else
			rendering_to_backbuffer = true; // assume backbuffer on failure
	}


	// ================================================================
	// Point light extraction
	// ================================================================

	void update_lights(IDirect3DDevice9* dev)
	{
		if (!lights_enabled || lights_updated_frame) return;
		if (!shadow_scene_node_ptr) return;

		void* scene_node = *shadow_scene_node_ptr;
		if (!scene_node) return;
		if (!camera_position_ptr) return;

		lights_updated_frame = true;
		float strength = light_intensity;

		void* entry = game_ptr(scene_node, SSN_LIGHTS_START_OFF);
		int i = 0;

		while (entry && i < MAX_EXTRACTED_LIGHTS)
		{
			void* ssl = game_ptr(entry, ENTRY_DATA_OFF);
			if (ssl)
			{
				void* ni_light = game_ptr(ssl, SSL_SOURCE_LIGHT_OFF);
				if (ni_light)
				{
					float* pos    = game_float_ptr(ni_light, NI_WORLD_POS_OFF);
					float  dimmer = game_float(ni_light, NI_DIMMER_OFF);
					float* diff   = game_float_ptr(ni_light, NI_DIFF_OFF);
					float* amb    = game_float_ptr(ni_light, NI_AMB_OFF);
					float  specR  = game_float(ni_light, NI_SPEC_OFF);
					float  atten0 = game_float(ni_light, NI_ATTEN0_OFF);
					float  atten1 = game_float(ni_light, NI_ATTEN1_OFF);
					float  atten2 = game_float(ni_light, NI_ATTEN2_OFF);

					D3DLIGHT9 light = {};
					light.Type = D3DLIGHT_POINT;

					light.Diffuse.r = diff[0] * dimmer * strength;
					light.Diffuse.g = diff[1] * dimmer * strength;
					light.Diffuse.b = diff[2] * dimmer * strength;
					light.Diffuse.a = 1.0f;

					light.Ambient.r = amb[0];
					light.Ambient.g = amb[1];
					light.Ambient.b = amb[2];
					light.Ambient.a = 1.0f;

					// Camera-relative position
					light.Position.x = pos[0] - camera_position_ptr[0];
					light.Position.y = pos[1] - camera_position_ptr[1];
					light.Position.z = pos[2] - camera_position_ptr[2];

					if (light_range_mode == 0)
					{
						light.Range = specR;
					}
					else if (light_range_mode == 1)
					{
						if (atten2 > 0.0f)
							light.Range = (-atten1 + sqrtf(atten1 * atten1 + 1020.0f * atten2)) / (2.0f * atten2);
						else if (atten1 > 0.0f)
							light.Range = 255.0f / atten1;
					}
					else
					{
						// Mode 2: INFINITY
						unsigned int inf_bits = 0x7F800000;
						std::memcpy(&light.Range, &inf_bits, sizeof(float));
					}

					light.Attenuation0 = atten0;
					light.Attenuation1 = atten1;
					light.Attenuation2 = atten2;

					dev->SetLight(static_cast<DWORD>(i), &light);
					dev->LightEnable(static_cast<DWORD>(i), TRUE);
					i++;
				}
			}
			entry = game_ptr(entry, ENTRY_NEXT_OFF);
		}

		// Disable stale lights from previous frame
		for (int j = i; j < last_enabled_lights; j++)
			dev->LightEnable(static_cast<DWORD>(j), FALSE);

		last_enabled_lights = i;
	}


	// ================================================================
	// Skinning: Declaration cloning
	// ================================================================

	static IDirect3DVertexDeclaration9* get_cloned_decl(IDirect3DDevice9* dev)
	{
		auto& ffp = shared::common::ffp_state::get();
		auto* orig_decl = ffp.last_decl();
		if (!orig_decl) return nullptr;

		// Check cache
		for (int i = 0; i < skin_decl_count; i++)
		{
			if (skin_decl_orig[i] == orig_decl)
				return skin_decl_clone[i];
		}

		// Clone: get elements from original
		UINT num_elems = 0;
		if (FAILED(orig_decl->GetDeclaration(nullptr, &num_elems))) return nullptr;
		if (num_elems == 0 || num_elems > 32) return nullptr;

		D3DVERTEXELEMENT9 elems[32];
		if (FAILED(orig_decl->GetDeclaration(elems, &num_elems))) return nullptr;

		// Patch BLENDINDICES: D3DCOLOR(4) -> UBYTE4(5)
		for (UINT i = 0; i < num_elems; i++)
		{
			if (elems[i].Stream == 0xFF) break;
			if (elems[i].Usage == D3DDECLUSAGE_BLENDINDICES && elems[i].Type == D3DDECLTYPE_D3DCOLOR)
				elems[i].Type = D3DDECLTYPE_UBYTE4;
		}

		IDirect3DVertexDeclaration9* new_decl = nullptr;
		if (FAILED(dev->CreateVertexDeclaration(elems, &new_decl))) return nullptr;

		// Store in cache (evict slot 0 if full)
		if (skin_decl_count < SKIN_DECL_CACHE_SIZE)
		{
			skin_decl_orig[skin_decl_count] = orig_decl;
			skin_decl_clone[skin_decl_count] = new_decl;
			skin_decl_count++;
		}
		else
		{
			skin_decl_clone[0]->Release();
			for (int i = 1; i < SKIN_DECL_CACHE_SIZE; i++)
			{
				skin_decl_orig[i - 1]  = skin_decl_orig[i];
				skin_decl_clone[i - 1] = skin_decl_clone[i];
			}
			skin_decl_orig[SKIN_DECL_CACHE_SIZE - 1]  = orig_decl;
			skin_decl_clone[SKIN_DECL_CACHE_SIZE - 1] = new_decl;
		}

		return new_decl;
	}


	// ================================================================
	// Skinning: Bone upload + draw
	// ================================================================

	static void upload_bones(IDirect3DDevice9* dev)
	{
		if (num_bones <= 0) return;

		auto& ffp = shared::common::ffp_state::get();
		int nw = ffp.cur_decl_num_weights();

		D3DVERTEXBLENDFLAGS blend_flag;
		if (nw <= 1)      blend_flag = D3DVBF_1WEIGHTS;
		else if (nw == 2) blend_flag = D3DVBF_2WEIGHTS;
		else              blend_flag = D3DVBF_3WEIGHTS;

		dev->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_VERTEXBLEND, blend_flag);
		skinning_setup = true;
	}

	void disable_skinning(IDirect3DDevice9* dev)
	{
		if (skinning_setup)
		{
			dev->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
			dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
			skinning_setup = false;
		}
	}

	HRESULT draw_skinned_dip(IDirect3DDevice9* dev,
		D3DPRIMITIVETYPE pt, INT base_vtx, UINT min_vtx, UINT num_verts,
		UINT start_idx, UINT prim_count)
	{
		auto& ffp = shared::common::ffp_state::get();

		if (num_bones <= 0)
		{
			ffp.disengage(dev);
			return dev->DrawIndexedPrimitive(pt, base_vtx, min_vtx, num_verts, start_idx, prim_count);
		}

		auto* cloned_decl = get_cloned_decl(dev);
		if (!cloned_decl)
		{
			ffp.disengage(dev);
			return dev->DrawIndexedPrimitive(pt, base_vtx, min_vtx, num_verts, start_idx, prim_count);
		}

		// Enter FFP: NULL shaders manually. Do NOT call ffp.engage() which would
		// set D3DTS_WORLD and clobber WORLDMATRIX(0) = bone 0.
		if (!ffp.is_ffp_active())
		{
			dev->SetVertexShader(nullptr);
			dev->SetPixelShader(nullptr);
		}

		// View + Projection from NiDX9Renderer
		float* view = get_renderer_matrix(RENDERER_VIEW_OFF);
		float* proj = get_renderer_matrix(RENDERER_PROJ_OFF);
		if (view && proj)
		{
			dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(view));
			dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(proj));
		}

		// Setup FFP rendering state
		{
			// Texture stages
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
			dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			for (DWORD s = 1; s <= 7; s++)
			{
				dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
				dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
			}

			// Lighting
			dev->SetRenderState(D3DRS_LIGHTING, FALSE);
			D3DMATERIAL9 mat = {};
			mat.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
			mat.Ambient = { 1.0f, 1.0f, 1.0f, 1.0f };
			dev->SetMaterial(&mat);
		}

		upload_bones(dev);

		// Albedo to stage 0, NULL stages 1-7
		auto& cfg = shared::common::config::get().ffp;
		int as = cfg.albedo_stage;
		auto* albedo = (as >= 0 && as < 8) ? ffp.cur_texture(as) : ffp.cur_texture(0);
		dev->SetTexture(0, albedo);
		for (DWORD ts = 1; ts < 8; ts++)
			dev->SetTexture(ts, nullptr);

		// Bind cloned declaration (UBYTE4 BLENDINDICES), keep original VB
		dev->SetVertexDeclaration(cloned_decl);

		auto hr = dev->DrawIndexedPrimitive(pt, base_vtx, min_vtx, num_verts, start_idx, prim_count);

		// Restore original declaration and textures
		dev->SetVertexDeclaration(ffp.last_decl());
		ffp.restore_textures(dev);

		bones_drawn = true;
		disable_skinning(dev);

		return hr;
	}


	// ================================================================
	// Immediate bone upload (called from d3d9ex SetVertexShaderConstantF)
	// ================================================================

	void on_set_vs_const_f(IDirect3DDevice9* dev, [[maybe_unused]] UINT start_reg, const float* data, UINT count)
	{
		constexpr int REGS_PER_BONE = 3;

		if (count != REGS_PER_BONE) return;
		if (!(g_hooks_installed ? g_render_skinned : shared::common::ffp_state::get().cur_decl_is_skinned()))
			return;

		// New bone batch: reset counter on per-object boundary or after draw
		if (g_bone_reset_pending || bones_drawn)
		{
			// Clear stale bone matrices from previous object
			static const D3DXMATRIX ident(
				1, 0, 0, 0,
				0, 1, 0, 0,
				0, 0, 1, 0,
				0, 0, 0, 1);

			for (int slot = 0; slot < num_bones; slot++)
				dev->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLDMATRIX(slot)), &ident);

			prev_num_bones = num_bones;
			num_bones = 0;
			bones_drawn = false;
			g_bone_reset_pending = 0;
		}

		if (num_bones < MAX_FFP_BONES && data)
		{
			// Transpose 4x3 packed -> 4x4 row-major
			D3DXMATRIX bone_mat;
			bone_mat._11 = data[0];  bone_mat._12 = data[4];  bone_mat._13 = data[8];  bone_mat._14 = 0.0f;
			bone_mat._21 = data[1];  bone_mat._22 = data[5];  bone_mat._23 = data[9];  bone_mat._24 = 0.0f;
			bone_mat._31 = data[2];  bone_mat._32 = data[6];  bone_mat._33 = data[10]; bone_mat._34 = 0.0f;
			bone_mat._41 = data[3];  bone_mat._42 = data[7];  bone_mat._43 = data[11]; bone_mat._44 = 1.0f;

			dev->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLDMATRIX(num_bones)), &bone_mat);
			num_bones++;
		}
	}


	// ================================================================
	// Lifecycle
	// ================================================================

	void release_skin_cache()
	{
		for (int i = 0; i < skin_decl_count; i++)
		{
			if (skin_decl_clone[i])
			{
				skin_decl_clone[i]->Release();
				skin_decl_clone[i] = nullptr;
			}
			skin_decl_orig[i] = nullptr;
		}
		skin_decl_count = 0;
	}

	void on_reset()
	{
		release_skin_cache();
		num_bones = 0;
		prev_num_bones = 0;
		bones_drawn = false;
		skinning_setup = false;
		lights_updated_frame = false;
		last_enabled_lights = 0;
	}


	// ================================================================
	// Game engine hooks (skinning)
	// ================================================================

	static void install_skinning_hooks()
	{
		// Gate: only patch game code when the skinning module is actually enabled.
		// Otherwise these patches modify FNV machine code unconditionally and may
		// interact badly with the player's specific FNV build / mod load order.
		if (!shared::common::config::get().skinning.enabled)
		{
			shared::common::log("Game", "Skinning: hooks SKIPPED (config disabled)");
			return;
		}

		// Patch 0xB992F2: conditional jump -> unconditional (fix broken skinning).
		// Matches NewVegasRTXHelper's SafeWrite8(0xB992F2, 0xEB).
		{
			auto* addr = reinterpret_cast<unsigned char*>(0xB992F2);
			DWORD old_prot;
			if (VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &old_prot))
			{
				*addr = 0xEB;
				VirtualProtect(addr, 1, old_prot, &old_prot);
				shared::common::log("Game", "Skinning: patched 0xB992F2 (fix broken skinning)");
			}
			else
			{
				shared::common::log("Game", "Skinning: WARNING - failed to patch 0xB992F2",
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
		}

		// Install code cave hooks for per-object bone reset.
		// Hook 1 (0xB99598): wraps skinned render batch call (0xB99110)
		// Hook 2 (0xB991E7): wraps per-object bone init call (0x43D450)
		auto* cave = static_cast<unsigned char*>(
			VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

		if (!cave)
		{
			shared::common::log("Game", "Skinning: WARNING - VirtualAlloc failed for code cave",
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return;
		}

		auto addr_render_skinned = reinterpret_cast<unsigned int>(&g_render_skinned);
		auto addr_bone_reset     = reinterpret_cast<unsigned int>(&g_bone_reset_pending);

		// Stub 1: on_render_skinned (at cave+0)
		{
			auto* p = cave;
			// mov dword ptr [g_render_skinned], 1
			*p++ = 0xC7; *p++ = 0x05;
			*reinterpret_cast<unsigned int*>(p) = addr_render_skinned; p += 4;
			*reinterpret_cast<unsigned int*>(p) = 1; p += 4;
			// call 0xB99110
			*p++ = 0xE8;
			*reinterpret_cast<int*>(p) = static_cast<int>(0xB99110) - static_cast<int>(reinterpret_cast<uintptr_t>(p + 4));
			p += 4;
			// mov dword ptr [g_render_skinned], 0
			*p++ = 0xC7; *p++ = 0x05;
			*reinterpret_cast<unsigned int*>(p) = addr_render_skinned; p += 4;
			*reinterpret_cast<unsigned int*>(p) = 0; p += 4;
			// mov dword ptr [g_bone_reset_pending], 1
			*p++ = 0xC7; *p++ = 0x05;
			*reinterpret_cast<unsigned int*>(p) = addr_bone_reset; p += 4;
			*reinterpret_cast<unsigned int*>(p) = 1; p += 4;
			// jmp 0xB9959D
			*p++ = 0xE9;
			*reinterpret_cast<int*>(p) = static_cast<int>(0xB9959D) - static_cast<int>(reinterpret_cast<uintptr_t>(p + 4));
		}

		// Stub 2: reset_bones (at cave+64)
		{
			auto* p = cave + 64;
			// call 0x43D450
			*p++ = 0xE8;
			*reinterpret_cast<int*>(p) = static_cast<int>(0x43D450) - static_cast<int>(reinterpret_cast<uintptr_t>(p + 4));
			p += 4;
			// mov dword ptr [g_bone_reset_pending], 1
			*p++ = 0xC7; *p++ = 0x05;
			*reinterpret_cast<unsigned int*>(p) = addr_bone_reset; p += 4;
			*reinterpret_cast<unsigned int*>(p) = 1; p += 4;
			// jmp 0xB991EC
			*p++ = 0xE9;
			*reinterpret_cast<int*>(p) = static_cast<int>(0xB991EC) - static_cast<int>(reinterpret_cast<uintptr_t>(p + 4));
		}

		// Patch game code: overwrite 5-byte CALLs with JMPs to stubs
		bool ok = true;
		auto* hook1 = reinterpret_cast<unsigned char*>(0xB99598);
		auto* hook2 = reinterpret_cast<unsigned char*>(0xB991E7);
		DWORD old1, old2;

		if (VirtualProtect(hook1, 5, PAGE_EXECUTE_READWRITE, &old1))
		{
			hook1[0] = 0xE9;
			*reinterpret_cast<int*>(hook1 + 1) =
				static_cast<int>(reinterpret_cast<uintptr_t>(cave)) - static_cast<int>(reinterpret_cast<uintptr_t>(hook1 + 5));
			VirtualProtect(hook1, 5, old1, &old1);
		}
		else ok = false;

		if (VirtualProtect(hook2, 5, PAGE_EXECUTE_READWRITE, &old2))
		{
			hook2[0] = 0xE9;
			*reinterpret_cast<int*>(hook2 + 1) =
				static_cast<int>(reinterpret_cast<uintptr_t>(cave + 64)) - static_cast<int>(reinterpret_cast<uintptr_t>(hook2 + 5));
			VirtualProtect(hook2, 5, old2, &old2);
		}
		else ok = false;

		if (ok)
		{
			g_hooks_installed = true;
			shared::common::log("Game", "Skinning: hooks installed (render_skinned + reset_bones)");
		}
		else
		{
			shared::common::log("Game", "Skinning: WARNING - failed to install game hooks",
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}
	}


	// ================================================================
	// Game engine hooks (culling)
	//
	// Wall_SoGB's "disable culling" patch ported from NewVegasRTXHelper
	// (TESReloaded/Core/Hooks/NewVegas/Culling.cpp). Forces
	// BSCullingProcess::SetCullMode to CULL_ALLPASS at one call site,
	// and replaces BSCullingProcess::Process so every visited NiAVObject
	// gets OnVisible'd directly without frustum/portal testing. Lets
	// Remix path-trace off-screen geometry that vanilla would reject.
	// ================================================================

	// Partial layout of BSCullingProcess - only the two fields we touch.
	// Upstream: NewVegasRTXHelper/.../GameNi.h (BSCullingProcess + NiCullingProcess).
	struct BSCullingProcess
	{
		unsigned char  pad00[0x90];
		unsigned int   kCullMode;          // +0x90 (static_assert in upstream)
		unsigned int   eTypeStack[10];     // +0x94
		unsigned int   uiStackIndex;       // +0xBC
		void*          pCompoundFrustum;   // +0xC0
	};

	// NiAVObject::OnVisible vtable byte offset.
	// NiRefObject (2 virtuals: 0x00,0x01) + NiObject (0x02-0x22) + NiObjectNET (no new
	// virtuals) + NiAVObject (UpdateControllers..OnVisible spans 0x23-0x35) = slot 0x35.
	// Slot 0x36 PurgeRendererData is annotated "last is 036 verified" upstream.
	static constexpr unsigned int NIAVOBJECT_ONVISIBLE_VTBL_BYTE_OFFSET = 0x35u * 4u;

	static void __fastcall BSCullingProcess_SetCullModeEx(
		BSCullingProcess* apThis, void* /*edx*/, unsigned int /*eType*/)
	{
		apThis->kCullMode = 1; // CULL_ALLPASS
	}

	static void __fastcall BSCullingProcess_ProcessEx(
		BSCullingProcess* apThis, void* /*edx*/, void* apObject)
	{
		apThis->pCompoundFrustum = nullptr;

		// Dispatch apObject->OnVisible(apThis) through the vtable, without needing
		// the full NiAVObject hierarchy in this TU.
		using OnVisible_t = void (__thiscall*)(void*, BSCullingProcess*);
		auto* vtbl = *reinterpret_cast<unsigned char**>(apObject);
		auto fn = *reinterpret_cast<OnVisible_t*>(vtbl + NIAVOBJECT_ONVISIBLE_VTBL_BYTE_OFFSET);
		fn(apObject, apThis);
	}

	static void install_culling_hooks()
	{
		if (!shared::common::config::get().culling.enabled)
		{
			shared::common::log("Game", "Culling: hooks SKIPPED (config disabled)");
			return;
		}

		bool ok = true;

		// Hook 1 (0x8743A6): rewrite a 5-byte relative CALL so this one call site
		// goes to our SetCullMode replacement. Sanity-check the opcode first; if
		// FNV is patched/cracked differently we want to log and skip, not corrupt code.
		{
			auto* addr = reinterpret_cast<unsigned char*>(0x8743A6);
			DWORD old_prot;
			if (addr[0] != 0xE8)
			{
				shared::common::log("Game",
					std::format("Culling: WARNING - 0x8743A6 opcode 0x{:02X}, expected 0xE8; skipping SetCullMode hook", addr[0]),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				ok = false;
			}
			else if (VirtualProtect(addr, 5, PAGE_EXECUTE_READWRITE, &old_prot))
			{
				auto target   = reinterpret_cast<uintptr_t>(&BSCullingProcess_SetCullModeEx);
				auto site_end = reinterpret_cast<uintptr_t>(addr + 5);
				*reinterpret_cast<int*>(addr + 1) = static_cast<int>(target - site_end);
				VirtualProtect(addr, 5, old_prot, &old_prot);
				shared::common::log("Game", "Culling: patched 0x8743A6 (SetCullMode -> CULL_ALLPASS)");
			}
			else
			{
				shared::common::log("Game", "Culling: WARNING - failed to patch 0x8743A6",
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				ok = false;
			}
		}

		// Hook 2 (0x101E330): vtable slot 0x11 of BSCullingProcess_vtable
		// (0x101E2EC + 0x44). Replace Process(NiAVObject*) with our visit-all stub.
		{
			auto* slot = reinterpret_cast<void**>(0x101E330);
			DWORD old_prot;
			if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot))
			{
				*slot = reinterpret_cast<void*>(&BSCullingProcess_ProcessEx);
				VirtualProtect(slot, sizeof(void*), old_prot, &old_prot);
				shared::common::log("Game", "Culling: patched vtable slot 0x101E330 (Process -> visit all)");
			}
			else
			{
				shared::common::log("Game", "Culling: WARNING - failed to patch vtable at 0x101E330",
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				ok = false;
			}
		}

		if (ok)
			shared::common::log("Game", "Culling: hooks installed (BSCullingProcess SetCullMode + Process)");
	}


	// ================================================================
	// Address init
	// ================================================================

#define PATTERN_OFFSET_SIMPLE(var, pattern, byte_offset, static_addr) \
		if (const auto offset = shared::utils::mem::find_pattern(##pattern, byte_offset, #var, use_pattern, static_addr); offset) { \
			(var) = offset; found_pattern_count++; \
		} total_pattern_count++;

#define PATTERN_OFFSET_DWORD_PTR_CAST_TYPE(var, type, pattern, byte_offset, static_addr) \
		if (const auto offset = shared::utils::mem::find_pattern(##pattern, byte_offset, #var, use_pattern, static_addr); offset) { \
			(var) = (type)*(DWORD*)offset; found_pattern_count++; \
		} total_pattern_count++;

	void init_game_addresses()
	{
		const bool use_pattern = !shared::common::flags::has_flag("no_pattern");
		if (use_pattern) {
			shared::common::log("Game", "Getting offsets ...", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}

		std::uint32_t total_pattern_count = 0u;
		std::uint32_t found_pattern_count = 0u;

		// NiDX9Renderer singleton: *(NiDX9Renderer**)0x11C73B4
		renderer_singleton_ptr = reinterpret_cast<void**>(0x11C73B4);

		// ShadowSceneNode: *(ShadowSceneNode**)0x011F91C8
		shadow_scene_node_ptr = reinterpret_cast<void**>(0x011F91C8);

		// Camera position: (NiPoint3*)0x011F8E9C
		camera_position_ptr = reinterpret_cast<float*>(0x011F8E9C);

		// Read light config from INI
		auto& cfg = shared::common::config::get();
		lights_enabled    = cfg.get_bool("Lights", "Enabled", true);
		light_intensity   = static_cast<float>(cfg.get_int("Lights", "IntensityPercent", 100)) / 100.0f;
		light_range_mode  = cfg.get_int("Lights", "RangeMode", 0);

		shared::common::log("Game", std::format("Lights: enabled={} intensity={:.0f}% rangeMode={}",
			lights_enabled, light_intensity * 100.0f, light_range_mode));

		// Install skinning hooks (hardcoded addresses — FNV specific)
		install_skinning_hooks();

		// Install culling-disable hooks (Wall_SoGB patch port)
		install_culling_hooks();

		if (use_pattern)
		{
			if (total_pattern_count == 0)
			{
				shared::common::log("Game", "No patterns needed (FNV uses hardcoded addresses).",
					shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
			}
			else if (found_pattern_count == total_pattern_count)
			{
				shared::common::log("Game", std::format("Found all '{:d}' Patterns.", total_pattern_count),
					shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
			}
			else
			{
				shared::common::log("Game", std::format("Only found '{:d}' out of '{:d}' Patterns.",
					found_pattern_count, total_pattern_count), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
		}
	}

#undef PATTERN_OFFSET_SIMPLE
#undef PATTERN_OFFSET_DWORD_PTR_CAST_TYPE
}
