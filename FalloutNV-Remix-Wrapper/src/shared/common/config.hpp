#pragma once

namespace shared::common
{
	class config
	{
	public:
		static config& get();

		void load(const std::string& ini_path);
		bool is_loaded() const { return loaded_; }

		int get_int(const char* section, const char* key, int default_val) const;
		std::string get_string(const char* section, const char* key, const char* default_val) const;
		float get_float(const char* section, const char* key, float default_val) const;
		bool get_bool(const char* section, const char* key, bool default_val) const;

		struct ffp_settings
		{
			bool enabled = true;
			int albedo_stage = 0;
			int terrain_albedo_stage = 1;   // for has_color && n_texcoords >= 2 (HQ terrain / multi-tile blend)
			int bi_albedo_stage = 1;        // for !skinned && has_blendindices (atlas-tile-selector decls)

			// Route distant-terrain LOD shaders (PS with an LOD-prefixed sampler)
			// through the FFP world-geometry path instead of passthrough+Ignore.
			//   ON  -> LOD terrain is path-traced and stops paying the passthrough
			//          tax, but s0 is an atlas the game's PS does UV math on, so the
			//          fixed-function sampler may tile the atlas across each tile.
			//   OFF -> legacy passthrough+Ignore (game's own PS rasterises the LOD).
			// Toggle in [FFP] RouteLodToFfp to A/B without a rebuild.
			bool route_lod_to_ffp = true;

			// Route FNV's WATER shader family through FFP instead of
			// passthrough+Ignore. Water declares no BaseMap/DiffuseMap/TexMap
			// sampler, so without this it fails the Diffuse-role gate and is
			// tagged Ignore -- the path tracer skips it and water is absent
			// from the image.
			//   ON  -> water is FFP-converted and path-traced, using its
			//          NoiseMap as the albedo (its only non-render-target
			//          texture, hence the only stable hash to key a Remix
			//          material replacement on).
			//   OFF -> legacy passthrough+Ignore (water invisible).
			// Toggle in [FFP] RouteWaterToFfp, or live in the ImGui FFP tab.
			bool route_water_to_ffp = true;

			// Force water's FFP surface opaque instead of taking alpha from the
			// NoiseMap. That texture's alpha channel carries nothing meaningful,
			// and reading opacity from it can resolve the whole water surface to
			// ~zero alpha -- indistinguishable from water being missing. Also
			// tags IgnoreAlphaChannel so Remix doesn't re-derive the same
			// near-zero opacity from the albedo on its own side.
			// Turn off only to confirm alpha is (or isn't) the culprit.
			bool water_force_opaque = true;

			// Approximate the engine's per-vertex LOD sink for no-normal terrain
			// LOD routed through FFP. The game's LOD VS lowers vertices that fall
			// inside the loaded-cell range (GeomorphParams.y, c19.y) so the coarse
			// LOD tucks under the real terrain; FFP can't do that per-vertex test,
			// so we sink the whole draw's world-Z uniformly instead.
			//   < 0  -> AUTO: sink by the engine's own GeomorphParams.y each draw.
			//   == 0 -> disabled (no sink; coarse LOD will poke through).
			//   > 0  -> fixed sink in world units (manual override / tuning).
			float lod_sink_z = -1.0f;
			// 0 = no LOD sink, 1 = legacy uniform world-matrix sink (LodSinkZ),
			// 2 = per-vertex sink replicating SLS2002.vso exactly (default).
			// Mode 2 falls back to mode 1 when a draw's decl/constants can't be
			// used, so it is never worse than the legacy path.
			int lod_sink_mode = 2;
			// Hand FNV's multi-layer terrain (BaseMap[2..7] blended by vertex
			// colour) to dxvk-remix instead of collapsing it to layer 0.
			// Requires a runtime that decodes kRemixMultiLayerTerrainBit --
			// dxvk-remix branch fnv-terrain-ffp or fnv-ffp. OFF against any
			// other build, where it would route no textures at all.
			bool multi_layer_terrain = false;

			// What to do with FNV's WITH-NORMAL near land LOD (PS 0x6626FACE,
			// VS SLS2080). Confirmed in-game as the source of the distant-terrain
			// z-fight: it takes passthrough + Ignore, so it is RASTERISED only and
			// composites against the path-traced real terrain.
			//
			// Its vertex shader fades it by horizontal distance from a blend centre:
			//   dist  = length(POSITION.xy - LandBlendParams.zw)      (c19.zw)
			//   alpha = 1 - saturate((9625.59961 - dist) * 0.000375600968)
			// (both magic numbers are `def` immediates baked into the variant)
			// i.e. alpha 0 within ~6964 units of the centre, ramping to 1 beyond
			// ~9626. It is deliberately invisible anywhere near the player -- it
			// exists purely as a raster-era cross-fade into the far LOD, which the
			// path tracer does not need.
			//
			//   0 = passthrough + Ignore (legacy; rasterises and z-fights)
			//   1 = drop the draw entirely (default)
			// A mode 2 that routes it through FFP with the fade evaluated per
			// vertex into a diffuse alpha would need an expanded VB plus a cloned
			// declaration -- the decl carries only POSITION + TEXCOORD0, so there
			// is no COLOR element to write the fade into. Only worth building if
			// mode 1 leaves visible gaps at distance.
			int near_lod_mode = 1;

			// Debug isolation for the far LOD (no-normal, PS 0x36E87D02), which
			// takes FFP + per-vertex sink and IS path-traced. ImGui/hotkey only.
			bool debug_drop_far_lod = false;

			// Skip "fake shadow" overlay draws: NOLIGHTING geometry whose vertex
			// colors are all grayscale with at least one dark vertex (FNV's baked
			// planar shadow overlays). Remix ray-traces real shadows, so these are
			// dropped. Ported from the old standalone proxy's SkipFakeShadows.
			bool skip_fake_shadows = false;

			// VS constant register layout (hardcoded per-game)
			// FNV: combined WorldViewProj at c0-c3, World at c8-c11
			static constexpr int vs_reg_view_start = 0;
			static constexpr int vs_reg_view_end = 4;
			static constexpr int vs_reg_proj_start = 0;
			static constexpr int vs_reg_proj_end = 4;
			static constexpr int vs_reg_world_start = 8;
			static constexpr int vs_reg_world_end = 12;

			static constexpr int vs_reg_bone_threshold = 20;
			static constexpr int vs_regs_per_bone = 3;
			static constexpr int vs_bone_min_regs = 3;
		} ffp;

		struct skinning_settings
		{
			bool enabled = false;
		} skinning;

		struct culling_settings
		{
			bool enabled = false;
		} culling;

		struct lights_settings
		{
			bool enabled = true;
			int intensity_percent = 100;
			float intensity = 1.0f;   // intensity_percent / 100.0f
			int range_mode = 0;       // 0=Spec.r, 1=attenuation calc, 2=infinity
			int max_lights = 128;
		} lights;

		struct diagnostics_settings
		{
			bool enabled = true;
			int delay_ms = 50000;
			int log_frames = 3;
			// When set, log every pixel shader's hash + draw-shape fingerprint
			// on first sight to ps_harvest.log (for identifying UI/keep-diffuse
			// shaders by action sequence). Off in normal builds.
			bool harvest_ps = false;
		} diagnostics;

		struct remix_settings
		{
			bool enabled = true;
			std::string dll_name = "d3d9_remix.dll";
		} remix;

		struct chain_settings
		{
			std::string preload;   // semicolon-separated DLLs/ASIs loaded before d3d9 chain
			std::string postload;  // semicolon-separated DLLs/ASIs loaded after init
		} chain;

		struct tracer_settings
		{
			int backtrace_depth = 8;
			std::string output_dir = "captures";
		} tracer;

		struct sun_cycle_settings
		{
			bool enabled = true;
		} sun_cycle;

		struct moon_cycle_settings
		{
			bool enabled = true;
		} moon_cycle;

	private:
		std::string ini_path_;
		bool loaded_ = false;

		void parse_all();
	};
}
