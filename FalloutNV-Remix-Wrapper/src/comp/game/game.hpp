#pragma once
#include "structs.hpp"

namespace comp::game
{
	// --- Game addresses (resolved in init_game_addresses) ---

	inline void** renderer_singleton_ptr = nullptr;  // NiDX9Renderer** at 0x11C73B4
	inline void** shadow_scene_node_ptr  = nullptr;  // ShadowSceneNode** at 0x011F91C8
	inline float* camera_position_ptr    = nullptr;  // NiPoint3* at 0x011F8E9C

	// --- NiDX9Renderer helpers ---

	// Read a 4x4 row-major matrix from NiDX9Renderer at the given byte offset.
	float* get_renderer_matrix(unsigned int byte_offset);

	// Orthographic projection (Pip-Boy, menus, UI overlays)
	bool is_2d();

	// NiShadeProperty::m_eShaderType == kProp_Sky (0x0D)
	bool is_sky();

	// Apply World/View/Projection from NiDX9Renderer directly (no VS constant decomposition)
	void apply_transforms(IDirect3DDevice9* dev);

	// --- Render target tracking ---

	// Track whether current RT is the backbuffer (skip FFP for off-screen draws)
	inline UINT  backbuffer_width  = 0;
	inline UINT  backbuffer_height = 0;
	inline bool  rendering_to_backbuffer = true;

	void init_backbuffer_tracking(IDirect3DDevice9* dev);
	void on_set_render_target(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* surface);

	// --- Point light extraction ---

	void update_lights(IDirect3DDevice9* dev);

	// Light config (read from INI)
	inline bool  lights_enabled        = true;
	inline float light_intensity       = 1.0f;
	inline int   light_range_mode      = 0;
	inline bool  lights_updated_frame  = false;
	inline int   last_enabled_lights   = 0;

	// --- Skinning (declaration cloning + game hooks) ---

	// Draw a skinned mesh using FFP indexed vertex blending with declaration cloning
	HRESULT draw_skinned_dip(IDirect3DDevice9* dev,
		D3DPRIMITIVETYPE pt, INT base_vtx, UINT min_vtx, UINT num_verts,
		UINT start_idx, UINT prim_count);

	// Called from d3d9ex SetVertexShaderConstantF for immediate bone upload
	void on_set_vs_const_f(IDirect3DDevice9* dev, UINT start_reg, const float* data, UINT count);

	// Skinning state
	inline int  num_bones          = 0;
	inline int  prev_num_bones     = 0;
	inline bool bones_drawn        = false;
	inline bool skinning_setup     = false;

	void disable_skinning(IDirect3DDevice9* dev);
	void release_skin_cache();
	void on_reset();

	// --- Init ---

	extern void init_game_addresses();
}
