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
