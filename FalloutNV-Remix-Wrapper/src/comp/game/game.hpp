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

	// NiShadeProperty::m_eShaderType == kProp_NoLighting (0x15)
	bool is_no_lighting();

	// True if this draw is a fake-shadow overlay (NOLIGHTING + all-grayscale
	// vertex colors with a dark vertex) that should be skipped, unless its PS is
	// on the keep-list. Reads stream-0 vertex colors (cached per VB+base).
	bool should_skip_fake_shadow(IDirect3DDevice9* dev, INT base_vtx, UINT num_verts,
		IDirect3DPixelShader9* ps);

	// Clear the fake-shadow result cache (call on device reset).
	void clear_fake_shadow_cache();

	// Per-frame maintenance; call from Present. Ages out the fake-shadow cache
	// so a vertex buffer whose address gets recycled by a later allocation can't
	// keep inheriting the previous mesh's verdict indefinitely.
	void on_frame_end();

	// Apply World/View/Projection from NiDX9Renderer directly (no VS constant decomposition)
	void apply_transforms(IDirect3DDevice9* dev);

	// Lower the bound D3DTS_WORLD by `sink` world-Z units (FNV is Z-up). Used to
	// approximate the LOD vertex shader's loaded-cell sink for FFP-routed terrain
	// LOD, so the coarse LOD tucks under the real terrain instead of poking
	// through. Reads the world matrix from NiDX9Renderer (same source as
	// apply_transforms) rather than GetTransform, which isn't reliable through
	// the Remix bridge. No-op when sink == 0.
	void apply_world_sink(IDirect3DDevice9* dev, float sink);

	// --- Render target tracking ---

	// Track whether current RT is the backbuffer (skip FFP for off-screen draws)
	inline UINT  backbuffer_width  = 0;
	inline UINT  backbuffer_height = 0;
	inline bool  rendering_to_backbuffer = true;

	// Swap-chain backbuffer surface, kept for POINTER IDENTITY ONLY — never
	// dereferenced, and deliberately not AddRef'd (holding a reference to a
	// D3DPOOL_DEFAULT surface would make IDirect3DDevice9::Reset fail). Cleared
	// on reset and re-acquired by init_backbuffer_tracking.
	//
	// FNV binds several full-resolution offscreen targets (water reflection,
	// post-process chains); the old width/height comparison classified all of
	// them as "the backbuffer" and let them through to FFP. Identity is exact.
	inline IDirect3DSurface9* backbuffer_surface = nullptr;

	// Set once the engine is observed binding `backbuffer_surface` to RT0. Until
	// then the legacy dimension heuristic stays in charge, so an unexpected
	// swap-chain/bridge arrangement degrades to the old behaviour rather than
	// classifying every draw as off-screen and disabling the whole comp.
	inline bool  backbuffer_surface_seen = false;

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

	// Called from d3d9ex SetVertexShaderConstantF for immediate bone upload.
	// Returns true when the upload was consumed as a skinned bone (replayed as
	// a SetTransform) — the caller then skips forwarding the raw constant to the
	// device, since the FFP skinned path nulls the VS and the c[] register is dead.
	bool on_set_vs_const_f(IDirect3DDevice9* dev, UINT start_reg, const float* data, UINT count);

	// Re-upload the cached bone constant range to the real device. Called from
	// the rare draw_skinned_dip fallbacks that draw with the game's real
	// skinning VS, which needs the bones that on_set_vs_const_f swallowed.
	void flush_bones_to_device(IDirect3DDevice9* dev);

	// Skinning state
	inline int  num_bones          = 0;
	inline int  prev_num_bones     = 0;
	inline bool bones_drawn        = false;
	inline bool skinning_setup     = false;

	void disable_skinning(IDirect3DDevice9* dev);
	void release_skin_cache();
	void on_reset();

	// --- Init ---

	// Resolve engine globals and read config. Safe from DllMain — reads and
	// writes nothing in the game's own code.
	extern void init_game_addresses();

	// Apply the fixed-address patches to FalloutNV.exe's .text/.rdata. Must NOT
	// be called from DllMain: the retail Steam executable is still SteamStub-
	// encrypted at that point and patching it corrupts real code once the .bind
	// stub decrypts. Call once the game window exists.
	extern void install_game_patches();
}
