#include "std_include.hpp"

#include "modules/imgui.hpp"
#include "modules/renderer.hpp"
#include "modules/diagnostics.hpp"
#include "modules/tracer.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/common/config.hpp"

namespace comp
{
	bool g_installed_signature_patches = false;
	bool g_install_signature_patches_async = false;

	// ---- Sun cycle driven by GameHour ----

	namespace sun_cycle
	{
		static float* s_game_hour = nullptr;
		static bool   s_initialized = false;
		static int    s_frame_count = 0;

		// Wait this many frames before touching the remix API
		static constexpr int WARMUP_FRAMES = 120;

		static void init()
		{
			s_game_hour = game::find_global_var("GameHour");
			s_initialized = true;

			if (s_game_hour)
				shared::common::log("SunCycle",
					std::format("GameHour at 0x{:X}, value = {:.2f}",
						reinterpret_cast<uintptr_t>(s_game_hour), *s_game_hour),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			else
				shared::common::log("SunCycle", "GameHour TESGlobal not found",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}

		static void update()
		{
			auto& api = shared::common::remix_api::get();
			if (!api.is_initialized()) return;

			// Let the bridge fully initialize before calling SetConfigVariable
			if (s_frame_count < WARMUP_FRAMES) { s_frame_count++; return; }

			if (!s_initialized) init();

			float elevation, rotation;
			if (!game::get_sun_orientation(elevation, rotation)) return;

			char buf[32];
			snprintf(buf, sizeof(buf), "%.2f", elevation);
			api.m_bridge.SetConfigVariable("rtx.atmosphere.sunElevation", buf);

			snprintf(buf, sizeof(buf), "%.2f", rotation);
			api.m_bridge.SetConfigVariable("rtx.atmosphere.sunRotation", buf);
		}

		// Symmetric disable path. sunElevation/sunRotation are RtxOption
		// NoSave in Remix, so they cold-reset to (15, 0) every run on their
		// own — pushing the defaults once is redundant on a clean restart
		// but keeps the toggle responsive if the cycle ever ran earlier in
		// this same session.
		static bool s_disable_pushed = false;
		static int  s_disable_frame_count = 0;

		static void disable()
		{
			if (s_disable_pushed) return;
			auto& api = shared::common::remix_api::get();
			if (!api.is_initialized()) return;
			if (s_disable_frame_count < WARMUP_FRAMES) { s_disable_frame_count++; return; }

			api.m_bridge.SetConfigVariable("rtx.atmosphere.sunElevation", "15.00");
			api.m_bridge.SetConfigVariable("rtx.atmosphere.sunRotation",  "0.00");
			s_disable_pushed = true;
			shared::common::log("SunCycle", "Disabled — reset to Remix defaults (elev=15, rot=0)",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}
	}

	// ---- Moon cycle: drives rtx.atmosphere.moon0.* from engine sky-state ----

	namespace moon_cycle
	{
		static float* s_game_hour = nullptr;  // diagnostic logging only
		static bool   s_initialized = false;
		static int    s_frame_count = 0;
		static std::uint32_t s_log_counter = 0;

		static constexpr int WARMUP_FRAMES = 120;

		static void init()
		{
			s_game_hour = game::find_global_var("GameHour");
			s_initialized = true;
			auto& api = shared::common::remix_api::get();
			if (api.is_initialized()) {
				api.m_bridge.SetConfigVariable("rtx.atmosphere.moon0.enabled0", "1");
			}
		}

		static void update()
		{
			auto& api = shared::common::remix_api::get();
			if (!api.is_initialized()) return;

			if (s_frame_count < WARMUP_FRAMES) { s_frame_count++; return; }

			if (!s_initialized) init();

			float elevation, rotation;
			if (!game::get_masser_orientation(elevation, rotation)) return;

			char buf[32];
			snprintf(buf, sizeof(buf), "%.2f", elevation);
			api.m_bridge.SetConfigVariable("rtx.atmosphere.moon0.elevation0", buf);

			snprintf(buf, sizeof(buf), "%.2f", rotation);
			api.m_bridge.SetConfigVariable("rtx.atmosphere.moon0.rotation0", buf);

			if ((s_log_counter++ % 300) == 0) {
				const float hour = s_game_hour ? *s_game_hour : -1.0f;
				shared::common::log("MoonCycle",
					std::format("hour={:.2f} elev={:.2f} rot={:.2f}",
						hour, elevation, rotation),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			}
		}

		// Symmetric disable path. moon0.enabled0 is a PERSISTENT RtxOption
		// (no NoSave flag) — once init() set it to 1 in a prior session,
		// Remix writes it to rtx.conf and reloads it on the next launch.
		// Without an explicit "set to 0" push here, flipping MoonCycle off
		// in the INI has no visible effect; the moon stays on at default
		// elevation/rotation. This single push fixes that.
		static bool s_disable_pushed = false;
		static int  s_disable_frame_count = 0;

		static void disable()
		{
			if (s_disable_pushed) return;
			auto& api = shared::common::remix_api::get();
			if (!api.is_initialized()) return;
			if (s_disable_frame_count < WARMUP_FRAMES) { s_disable_frame_count++; return; }

			api.m_bridge.SetConfigVariable("rtx.atmosphere.moon0.enabled0", "0");
			s_disable_pushed = true;
			shared::common::log("MoonCycle", "Disabled — moon0.enabled0 = 0",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}
	}

	void on_begin_scene_cb()
	{
		if (!tex_addons::initialized) {
			tex_addons::init_texture_addons();
		}

		auto& cfg = shared::common::config::get();
		if (cfg.sun_cycle.enabled)  sun_cycle::update();
		else                        sun_cycle::disable();
		if (cfg.moon_cycle.enabled) moon_cycle::update();
		else                        moon_cycle::disable();

		// FNV: init backbuffer tracking on first scene (device is available now)
		if (game::backbuffer_width == 0)
			game::init_backbuffer_tracking(shared::globals::d3d_device);

		// FNV: extract point lights from ShadowSceneNode on each BeginScene
		game::update_lights(shared::globals::d3d_device);

		// FNV: apply NiDX9Renderer transforms (world identity for now, view/proj from renderer).
		// Per-draw transforms are applied in renderer.cpp via fnv_engage().
		shared::globals::d3d_device->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);

		// Debug camera (only when explicitly enabled in ImGui F4 overlay)
		const auto& im = imgui::get();
		if (im->m_dbg_use_fake_camera)
		{
			D3DXMATRIX view_matrix
			(
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 0.447f, 0.894f, 0.0f,
				0.0f, -0.894f, 0.447f, 0.0f,
				0.0f, 100.0f, -50.0f, 1.0f
			);

			D3DXMATRIX proj_matrix
			(
				1.359f, 0.0f, 0.0f, 0.0f,
				0.0f, 2.414f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.001f, 1.0f,
				0.0f, 0.0f, -1.0f, 0.0f
			);

			D3DXMATRIX rotation, translation;
			D3DXMatrixRotationYawPitchRoll(&rotation,
				D3DXToRadian(im->m_dbg_camera_yaw),
				D3DXToRadian(im->m_dbg_camera_pitch),
				0.0f);

			D3DXMatrixTranslation(&translation,
				-im->m_dbg_camera_pos[0],
				-im->m_dbg_camera_pos[1],
				-im->m_dbg_camera_pos[2]);

			D3DXMatrixMultiply(&view_matrix, &rotation, &translation);

			D3DXMatrixPerspectiveFovLH(&proj_matrix,
				D3DXToRadian(im->m_dbg_camera_fov),
				im->m_dbg_camera_aspect,
				im->m_dbg_camera_near_plane,
				im->m_dbg_camera_far_plane);

			shared::globals::d3d_device->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);
			shared::globals::d3d_device->SetTransform(D3DTS_VIEW, &view_matrix);
			shared::globals::d3d_device->SetTransform(D3DTS_PROJECTION, &proj_matrix);
		}
	}


	void main()
	{
		shared::common::remix_api::initialize(nullptr, nullptr, nullptr, false);

		// Core modules
		shared::common::loader::module_loader::register_module(std::make_unique<tracer>());
		shared::common::loader::module_loader::register_module(std::make_unique<imgui>());
		shared::common::loader::module_loader::register_module(std::make_unique<renderer>());

		// FFP diagnostics (conditional on config)
		auto& cfg = shared::common::config::get();
		if (cfg.diagnostics.enabled)
			shared::common::loader::module_loader::register_module(std::make_unique<diagnostics>());

		// NOTE: FNV uses game-specific skinning (declaration cloning + game hooks)
		// instead of the generic skinning module. Do NOT register skinning module here.
		// Set [Skinning] Enabled=0 in remix-comp.ini.

		MH_EnableHook(MH_ALL_HOOKS);
	}
}
