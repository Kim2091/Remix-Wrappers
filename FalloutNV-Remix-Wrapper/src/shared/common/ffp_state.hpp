#pragma once
#include "config.hpp"

namespace shared::common
{
	struct PsSlotMap;
	/*
	 * FFP state tracker — captures D3D9 state needed for fixed-function pipeline conversion.
	 *
	 * The D3D9Device proxy calls on_xxx() methods when the game sets shaders, constants,
	 * textures, vertex declarations, etc. The renderer reads state via accessors to make
	 * draw routing decisions, then calls engage/disengage to switch between shader and FFP modes.
	 */
	class ffp_state
	{
	public:
		static ffp_state& get();

		void init(IDirect3DDevice9* real_device);

		// --- State mutators (called from D3D9Device interceptions) ---

		void on_set_vs_const_f(UINT start_reg, const float* data, UINT count);
		void on_set_ps_const_f(UINT start_reg, const float* data, UINT count);

		// Returns true if the write would be a no-op (cached constants already
		// match the new data and the range has been written at least once).
		// Used by the d3d9 proxy to skip redundant SetVSConstF / SetPSConstF
		// calls — the game writes identical constants per-draw very heavily.
		bool vs_const_matches(UINT start_reg, const float* data, UINT count) const;
		bool ps_const_matches(UINT start_reg, const float* data, UINT count) const;
		void on_set_vertex_shader(IDirect3DVertexShader9* shader);

		// Returns true if the call should be swallowed (not forwarded to real device)
		bool on_set_pixel_shader(IDirect3DPixelShader9* shader);

		void on_set_texture(UINT stage, IDirect3DBaseTexture9* texture);
		void on_set_stream_source(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride);
		void on_set_vertex_declaration(IDirect3DVertexDeclaration9* decl);
		void on_present();
		void on_begin_scene();
		void on_reset();

		// --- State consumers (called from renderer draw routing) ---

		void engage(IDirect3DDevice9* dev);
		void disengage(IDirect3DDevice9* dev);

		// Bind albedo texture to stage 0, NULL stages 1-7. Call before draw.
		void setup_albedo_texture(IDirect3DDevice9* dev);

		// Classifier-aware variant: skips nulling stages 1-7 for slots the
		// caller will rebind via apply_ps_protocol (normal/glow/height roles).
		// Also tracks the kept-bound mask so restore_textures can skip those
		// slots — their device binding never changed. Pass nullptr to fall
		// back to the no-arg behaviour.
		void setup_albedo_texture(IDirect3DDevice9* dev, const PsSlotMap* psmap);

		// Like setup_albedo_texture, but does NOT null slots 1-7. Used for the
		// multi-layer terrain route in renderer.cpp where dxvk-remix needs ALL 14
		// sampler slots intact to capture the multi-layer texture set. The albedo
		// at slot 0 is still set per the AlbedoStage heuristic for correct FFP
		// rasterisation, but slots 1-7 retain whatever the game bound. The wrapper
		// pairs this with apply_multilayer_terrain_protocol + RS-149 modifier bit
		// so dxvk-remix reads textures[0..6] as albedos and textures[7..13] as normals.
		void setup_albedo_texture_preserve_slots(IDirect3DDevice9* dev);

		// Explicit-stage bind: puts cur_texture_[stage] at slot 0 and leaves
		// slots 1-7 exactly as the game bound them.
		//
		// Used by the water route, where neither the AlbedoStage config nor the
		// decl-shape heuristic can name the right texture -- water's albedo is
		// whichever slot its pixel shader declared NoiseMap at, which the PS
		// classifier reports per shader. Leaving 1-7 bound is safe because
		// setup_texture_stages sets COLOROP=DISABLE on them (so they cannot
		// affect FFP rasterisation) while dxvk-remix still reads them straight
		// out of device state via the RS-149 payload.
		void setup_albedo_texture_stage_preserve(IDirect3DDevice9* dev, int stage);

		// Restore original texture bindings on all 8 stages. Call after draw.
		void restore_textures(IDirect3DDevice9* dev);

		// Tell the tracker that the caller re-pointed stage 0 itself AFTER
		// setup_albedo_texture ran (apply_ps_protocol does this when the PS
		// labelled a non-zero slot as the diffuse). Without this, the common
		// AlbedoStage=0 path leaves stage0_changed_ false and restore_textures
		// skips stage 0 — the device would keep the override bound for every
		// following passthrough draw.
		void note_stage0_override(IDirect3DBaseTexture9* tex);

		// Skinned variant of setup_texture_stages (uses ALPHAOP=SELECTARG1
		// instead of MODULATE+DIFFUSE — the skinned vertex blend doesn't have
		// a meaningful per-vertex alpha to combine with). Shares the tss_mode_
		// shadow with setup_texture_stages; cross-mode entry triggers a
		// re-program. Call from skinned draw paths.
		void setup_texture_stages_skinned(IDirect3DDevice9* dev);

		// Lighting setup (D3DRS_LIGHTING off + white material). Idempotent
		// across consecutive draws via the ffp_setup_ flag, which already
		// gets reset on BeginScene / device reset / shader change. Call from
		// any FFP draw path that needs lighting state.
		void ensure_ffp_lighting(IDirect3DDevice9* dev);

		// --- Read-only accessors for renderer ---

		bool is_enabled() const { return enabled_; }
		void set_enabled(bool e) { enabled_ = e; }

		bool view_proj_valid() const { return view_proj_valid_; }
		bool is_ffp_active() const { return ffp_active_; }
		bool cur_decl_is_skinned() const { return cur_decl_is_skinned_; }
		bool cur_decl_has_normal() const { return cur_decl_has_normal_; }
		bool cur_decl_has_pos_t() const { return cur_decl_has_pos_t_; }
		bool cur_decl_has_texcoord() const { return cur_decl_has_texcoord_; }
		bool cur_decl_has_color() const { return cur_decl_has_color_; }
		int cur_decl_color_off() const { return cur_decl_color_off_; }
		int cur_decl_color_type() const { return cur_decl_color_type_; }
		bool cur_decl_has_blendindices() const { return cur_decl_has_blendindices_; }
		bool cur_decl_has_tangent() const { return cur_decl_has_tangent_; }
		int cur_decl_texcoord_type() const { return cur_decl_texcoord_type_; }
		int cur_decl_n_texcoords() const { return cur_decl_n_texcoords_; }

		IDirect3DVertexShader9* last_vs() const { return last_vs_; }
		IDirect3DPixelShader9* last_ps() const { return last_ps_; }
		IDirect3DVertexDeclaration9* last_decl() const { return last_decl_; }

		// --- Diagnostic data access ---

		const float* vs_const_data() const { return vs_const_; }
		const float* ps_const_data() const { return ps_const_; }
		const int* vs_const_write_log() const { return vs_const_write_log_; }

		// The per-register write log is only consumed by the diagnostics
		// module (frame log) and the imgui debug overlay (heatmap). The
		// per-write fill loop in on_set_vs_const_f is ~256 writes per draw
		// in the worst case; gate it on this flag so the hot path only pays
		// for the two FSM sentinels (proj_start, view_start). Consumers
		// (diagnostics::is_active / imgui overlay open) flip it on demand.
		static inline bool vs_write_log_enabled_ = false;
		UINT draw_call_count() const { return draw_call_count_; }
		UINT frame_count() const { return frame_count_; }
		UINT scene_count() const { return scene_count_; }
		DWORD create_tick() const { return create_tick_; }

		// Texture tracking (stages 0-7)
		IDirect3DBaseTexture9* cur_texture(UINT stage) const { return stage < 8 ? cur_texture_[stage] : nullptr; }

		// Stream source tracking
		IDirect3DVertexBuffer9* stream_vb(UINT stream) const { return stream < 4 ? stream_vb_[stream] : nullptr; }
		UINT stream_offset(UINT stream) const { return stream < 4 ? stream_offset_[stream] : 0; }
		UINT stream_stride(UINT stream) const { return stream < 4 ? stream_stride_[stream] : 0; }

		// Skinning data (populated by skinning module via on_set_vs_const_f bone detection)
		int bone_start_reg() const { return bone_start_reg_; }
		int num_bones() const { return num_bones_; }
		int cur_decl_num_weights() const { return cur_decl_num_weights_; }
		int cur_decl_blend_weight_off() const { return cur_decl_blend_weight_off_; }
		int cur_decl_blend_weight_type() const { return cur_decl_blend_weight_type_; }
		int cur_decl_blend_indices_off() const { return cur_decl_blend_indices_off_; }
		int cur_decl_pos_off() const { return cur_decl_pos_off_; }
		int cur_decl_pos_type() const { return cur_decl_pos_type_; }
		int cur_decl_normal_off() const { return cur_decl_normal_off_; }
		int cur_decl_normal_type() const { return cur_decl_normal_type_; }
		int cur_decl_texcoord_off() const { return cur_decl_texcoord_off_; }
		int cur_decl_texcoord1_off() const { return cur_decl_texcoord1_off_; }
		int cur_decl_texcoord1_type() const { return cur_decl_texcoord1_type_; }

		void increment_draw_count() { draw_call_count_++; }

		// --- Utility ---

		static void mat4_transpose(float* dst, const float* src);
		static bool mat4_is_interesting(const float* m);

	private:
		bool enabled_ = true;

		// VS/PS constant capture
		float vs_const_[256 * 4] = {};
		float ps_const_[32 * 4] = {};

		// Dirty tracking
		bool world_dirty_ = false;
		bool view_proj_dirty_ = false;
		bool view_proj_valid_ = false;
		bool ffp_active_ = false;
		bool ffp_setup_ = false;
		// TSS values differ between FFP_GEO route (ALPHAOP=MODULATE+DIFFUSE)
		// and skinned route (ALPHAOP=SELECTARG1). Track which variant is
		// currently programmed so consecutive same-mode draws skip the 22
		// SetTextureStageState calls. Mode changes trigger a re-program.
		// Reset on device loss.
		enum class tss_mode_t : uint8_t { none, ffp_geo, skinned };
		tss_mode_t tss_mode_ = tss_mode_t::none;

		// Shader tracking
		IDirect3DVertexShader9* last_vs_ = nullptr;
		IDirect3DPixelShader9* last_ps_ = nullptr;

		// Vertex declaration tracking
		IDirect3DVertexDeclaration9* last_decl_ = nullptr;
		bool cur_decl_is_skinned_ = false;
		bool cur_decl_has_texcoord_ = false;
		bool cur_decl_has_normal_ = false;
		bool cur_decl_has_color_ = false;
		int cur_decl_color_off_ = -1;   // byte offset of COLOR0 (usage idx 0, stream 0), or -1
		int cur_decl_color_type_ = -1;  // D3DDECLTYPE of COLOR0
		bool cur_decl_has_blendindices_ = false;
		bool cur_decl_has_tangent_ = false;
		bool cur_decl_has_pos_t_ = false;
		int cur_decl_texcoord_type_ = -1;
		int cur_decl_texcoord_off_ = 0;
		// TEXCOORD1 (stream 0). -1 offset means the decl has no TEXCOORD1.
		int cur_decl_texcoord1_type_ = -1;
		int cur_decl_texcoord1_off_ = -1;
		int cur_decl_n_texcoords_ = 0;

		// Skinning-related declaration data
		int cur_decl_num_weights_ = 0;
		int cur_decl_blend_weight_off_ = 0;
		int cur_decl_blend_weight_type_ = 0;
		int cur_decl_blend_indices_off_ = 0;
		int cur_decl_pos_off_ = 0;
		int cur_decl_pos_type_ = -1;
		int cur_decl_normal_off_ = 0;
		int cur_decl_normal_type_ = -1;

		// Bone detection
		int bone_start_reg_ = 0;
		int num_bones_ = 0;

		// Texture tracking
		IDirect3DBaseTexture9* cur_texture_[8] = {};

		// Bitmask of slots 1-7 that setup_albedo_texture left bound (because
		// apply_ps_protocol will rebind them). restore_textures reads this to
		// skip restoring those slots — their device binding is unchanged.
		// Bit 0 unused (stage 0 always handled separately).
		uint8_t kept_bound_mask_ = 0;

		// True when setup_albedo_texture rebound stage 0 to a different
		// texture than cur_texture_[0]; restore_textures only writes stage 0
		// when this is true. The common HQ-terrain/BI case picks a non-zero
		// AlbedoStage so cur_texture_[0] never gets touched and the restore
		// is wasted.
		bool stage0_changed_ = false;

		// The pointer most recently bound at stage 0 by setup_albedo_texture
		// (the albedo pick). apply_ps_protocol reads this to dedup its
		// slot-0 rebind: if the picked albedo already equals the diffuse
		// texture, the rebind is a no-op.
		IDirect3DBaseTexture9* last_albedo_stage0_ = nullptr;

	public:
		IDirect3DBaseTexture9* last_albedo_stage0() const { return last_albedo_stage0_; }
	private:

		// Stream source tracking
		IDirect3DVertexBuffer9* stream_vb_[4] = {};
		UINT stream_offset_[4] = {};
		UINT stream_stride_[4] = {};

		// Frame/draw counters
		UINT frame_count_ = 0;
		UINT draw_call_count_ = 0;
		UINT scene_count_ = 0;
		DWORD create_tick_ = 0;

		// VS constant write log (for diagnostics: which registers have been written)
		int vs_const_write_log_[256] = {};

		// Cached config
		const config::ffp_settings* cfg_ = nullptr;

		// Internal helpers
		void apply_transforms(IDirect3DDevice9* dev);
		void setup_lighting(IDirect3DDevice9* dev);
		void setup_texture_stages(IDirect3DDevice9* dev);
	};
}
