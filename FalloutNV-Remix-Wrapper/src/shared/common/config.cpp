#include "std_include.hpp"
#include "config.hpp"

namespace shared::common
{
	config& config::get()
	{
		static config instance;
		return instance;
	}

	void config::load(const std::string& path)
	{
		ini_path_ = path;
		loaded_ = true;
		parse_all();
	}

	int config::get_int(const char* section, const char* key, int default_val) const
	{
		if (!loaded_) return default_val;
		return GetPrivateProfileIntA(section, key, default_val, ini_path_.c_str());
	}

	std::string config::get_string(const char* section, const char* key, const char* default_val) const
	{
		if (!loaded_) return default_val;
		char buf[512];
		GetPrivateProfileStringA(section, key, default_val, buf, sizeof(buf), ini_path_.c_str());
		return buf;
	}

	float config::get_float(const char* section, const char* key, float default_val) const
	{
		auto str = get_string(section, key, "");
		if (str.empty()) return default_val;
		try { return std::stof(str); }
		catch (...) { return default_val; }
	}

	bool config::get_bool(const char* section, const char* key, bool default_val) const
	{
		return get_int(section, key, default_val ? 1 : 0) != 0;
	}

	void config::parse_all()
	{
		// [Remix]
		remix.enabled = get_bool("Remix", "Enabled", true);
		remix.dll_name = get_string("Remix", "DLLName", "d3d9_remix.dll");

		// [Chain]
		chain.preload_dll = get_string("Chain", "PreloadDLL", "");

		// [FFP]
		ffp.enabled = get_bool("FFP", "Enabled", true);
		ffp.albedo_stage = get_int("FFP", "AlbedoStage", 0);
		if (ffp.albedo_stage < 0 || ffp.albedo_stage > 7) ffp.albedo_stage = 0;
		ffp.terrain_albedo_stage = get_int("FFP", "TerrainAlbedoStage", 1);
		if (ffp.terrain_albedo_stage < 0 || ffp.terrain_albedo_stage > 7) ffp.terrain_albedo_stage = 1;
		ffp.bi_albedo_stage = get_int("FFP", "BIAlbedoStage", 1);
		if (ffp.bi_albedo_stage < 0 || ffp.bi_albedo_stage > 7) ffp.bi_albedo_stage = 1;

		// [Skinning]
		skinning.enabled = get_bool("Skinning", "Enabled", false);

		// [Culling]
		culling.enabled = get_bool("Culling", "Enabled", false);

		// [Lights]
		lights.enabled = get_bool("Lights", "Enabled", true);
		lights.intensity_percent = get_int("Lights", "IntensityPercent", 100);
		lights.intensity = static_cast<float>(lights.intensity_percent) / 100.0f;
		lights.range_mode = get_int("Lights", "RangeMode", 0);
		lights.max_lights = get_int("Lights", "MaxLights", 128);

		// [Diagnostics]
		diagnostics.enabled = get_bool("Diagnostics", "Enabled", true);
		diagnostics.delay_ms = get_int("Diagnostics", "DelayMs", 50000);
		diagnostics.log_frames = get_int("Diagnostics", "LogFrames", 3);

		// [Tracer]
		tracer.backtrace_depth = get_int("Tracer", "BacktraceDepth", 8);
		tracer.output_dir = get_string("Tracer", "OutputDir", "captures");

		// [SunCycle] / [MoonCycle] — Remix atmosphere passthrough from engine sky state
		sun_cycle.enabled = get_bool("SunCycle", "Enabled", true);
		moon_cycle.enabled = get_bool("MoonCycle", "Enabled", true);

		log("Config", std::format("Loaded from: {}", ini_path_));
		log("Config", std::format("FFP={} AlbedoStage={} TerrainAlbedoStage={} BIAlbedoStage={}",
			ffp.enabled ? 1 : 0, ffp.albedo_stage, ffp.terrain_albedo_stage, ffp.bi_albedo_stage));
		if (skinning.enabled)
			log("Config", "Skinning ENABLED", LOG_TYPE::LOG_TYPE_WARN);
		if (culling.enabled)
			log("Config", "Culling ENABLED (BSCullingProcess disabled)", LOG_TYPE::LOG_TYPE_WARN);
		log("Config", std::format("Lights={} Intensity={}% RangeMode={}",
			lights.enabled ? 1 : 0, lights.intensity_percent, lights.range_mode));
		log("Config", std::format("SunCycle={} MoonCycle={}",
			sun_cycle.enabled ? 1 : 0, moon_cycle.enabled ? 1 : 0));
	}
}
