#pragma once

namespace comp::game
{
	// NiDX9Renderer matrix offsets (from NewVegasRTXHelper GameNi.h)
	constexpr unsigned int RENDERER_WORLD_OFF = 0x940;
	constexpr unsigned int RENDERER_VIEW_OFF  = 0x980;
	constexpr unsigned int RENDERER_PROJ_OFF  = 0x9C0;

	// NiShadeProperty shader types
	constexpr unsigned int KPROP_SKY         = 0x0D;

	// ShadowSceneNode light list offsets
	constexpr unsigned int SSN_LIGHTS_START_OFF  = 0xB4;  // NiTList<ShadowSceneLight>.start
	constexpr unsigned int ENTRY_NEXT_OFF        = 0x00;
	constexpr unsigned int ENTRY_DATA_OFF        = 0x08;
	constexpr unsigned int SSL_SOURCE_LIGHT_OFF  = 0xF8;

	// NiLight / NiPointLight offsets
	constexpr unsigned int NI_WORLD_POS_OFF = 0x8C;
	constexpr unsigned int NI_DIMMER_OFF    = 0xC4;
	constexpr unsigned int NI_AMB_OFF       = 0xC8;
	constexpr unsigned int NI_DIFF_OFF      = 0xD4;
	constexpr unsigned int NI_SPEC_OFF      = 0xE0;
	constexpr unsigned int NI_ATTEN0_OFF    = 0xF0;
	constexpr unsigned int NI_ATTEN1_OFF    = 0xF4;
	constexpr unsigned int NI_ATTEN2_OFF    = 0xF8;

	constexpr int MAX_EXTRACTED_LIGHTS   = 128;
	constexpr int MAX_FFP_BONES          = 48;
	constexpr int SKIN_DECL_CACHE_SIZE   = 4;

	// DataHandler singleton pointer (from NVSE source: GameData.cpp)
	inline constexpr uintptr_t DATA_HANDLER_ADDR = 0x011C3F2C;

	// DataHandler::globalList offset (tList<TESGlobal> at +0xE8)
	inline constexpr uint32_t DH_GLOBAL_LIST_OFF = 0xE8;

	// TESGlobal struct offsets (from NVSE source: GameForms.h)
	//   +0x18: String name  (String::m_data is a char* at +0x00 of String)
	//   +0x24: float data   (the actual global variable value)
	inline constexpr uint32_t TESGLOBAL_NAME_OFF = 0x18;
	inline constexpr uint32_t TESGLOBAL_DATA_OFF = 0x24;

	// Walks DataHandler::globalList to find a TESGlobal by editor ID.
	// Returns pointer to the float value field, or nullptr.
	inline float* find_global_var(const char* name)
	{
		auto** dh_pp = reinterpret_cast<void**>(DATA_HANDLER_ADDR);
		if (!dh_pp || !*dh_pp) return nullptr;

		struct Node { void* data; Node* next; };
		auto* node = reinterpret_cast<Node*>(
			reinterpret_cast<uintptr_t>(*dh_pp) + DH_GLOBAL_LIST_OFF);

		while (node)
		{
			if (node->data)
			{
				auto* str = *reinterpret_cast<const char**>(
					reinterpret_cast<uintptr_t>(node->data) + TESGLOBAL_NAME_OFF);
				if (str && _stricmp(str, name) == 0)
					return reinterpret_cast<float*>(
						reinterpret_cast<uintptr_t>(node->data) + TESGLOBAL_DATA_OFF);
			}
			node = node->next;
		}
		return nullptr;
	}

	// FNV sky/atmosphere chain (reversed via livetools 2026-05-09).
	//
	// Pointer chain to the rendered moon body:
	//   0x011DEA10 → manager (singleton)
	//   +0x68      → sky_state
	//   +0x30      → masser_config (0x7c-byte struct, vtable 0x0104ED28)
	//   +0x10      → moon billboard NiAVObject (vtable 0x0109D454)
	//   +0x18 of billboard → orbit-root parent NiNode (vtable 0x0109B5AC)
	//
	// NiAVObject layout (FNV-specific, verified live):
	//   +0x18:        m_pkParent (NiNode*)
	//   +0x34..+0x67: m_kLocal  (NiTransform: 9 rot + 3 translate + 1 scale)
	//   +0x68..+0x9B: m_kWorld  (composed)
	//     +0x68..+0x8B: m_kWorld.m_Rotate    (3x3 row-major)
	//     +0x8C..+0x97: m_kWorld.m_Translate (NiPoint3, X/Y/Z world units)
	//     +0x98:        m_kWorld.m_fScale
	//
	// The orbit root sits at the camera/sky-anchor world position; the
	// billboard sits 512 game units away along its local +Y axis (the
	// orbital arm direction). The vector from parent.T to billboard.T,
	// normalized, IS the world-space direction from camera to the moon —
	// exactly what we need for elevation/rotation. No calibration needed.
	//
	// Bethesda axis convention: Z = up, X = east, Y = north. Azimuth is
	// measured clockwise from north (atan2(x, y)).
	inline constexpr uintptr_t WORLD_MANAGER_ADDR  = 0x011DEA10;
	inline constexpr uint32_t  WM_SKY_OFF          = 0x68;
	inline constexpr uint32_t  SKY_SUN_OFF         = 0x28;
	inline constexpr uint32_t  SKY_MASSER_OFF      = 0x30;
	inline constexpr uint32_t  MASSER_BILLBOARD_OFF = 0x10;
	// The sun has TWO billboards (glare + disc). Billboard B at sun+0x14 has
	// a parent with a non-zero local translate ~595 game units (the orbital
	// arm) — that's the chain whose translate-difference encodes the sun's
	// rendered direction. Billboard A at sun+0x10 has identity all the way
	// down (used for screen-space glare, not orbital position).
	inline constexpr uint32_t  SUN_BILLBOARD_OFF   = 0x14;
	inline constexpr uint32_t  NIAV_PARENT_OFF     = 0x18;
	inline constexpr uint32_t  NIAV_KWORLD_T_OFF   = 0x8C;

	// ---- Sun day/night cycle (reversed from FalloutNV.exe 2026-07-30) ----
	//
	// Sun::Update @0x641830 (its asserts carry "...\Fallout Shared\Sky\Sun.cpp",
	// line 0x12C) computes the sun's arc position analytically and writes it as the
	// LOCAL TRANSLATE of the sun root node:
	//
	//   H = *(float*)(sky + 0xEC)                        Sky::GetGameHour @0x966A20
	//   A = (climT[1]+climT[0])/2 - fSunAlphaTransTime*0.5    sun at horizon, east
	//   B = (climT[3]+climT[2])/2 + fSunAlphaTransTime*0.5    sun at horizon, west
	//
	//   day    A<H<B :  t = (H-A)/(B-A)           s = 1 - 2t      (+1 -> -1)
	//   night  H>=B  :  t = (H-B)/(24-(B-A))      s = 2t - 1      (-1 -> +1)
	//   night  H<=A  :  t = (24-B+H)/(24-(B-A))   s = 2t - 1
	//
	//   X = fSunXExtreme * s        (fSunXExtreme = -400)
	//   Y = fSunYExtreme            (25, constant)
	//   Z = |fSunXExtreme| - |X|
	//
	// The "f" applied to X for the height term is fabs: the helper at 0x408860 tail-
	// calls 0x00EC6CDE, whose normal path (0x00EC6D77) just masks the sign bit off
	// the high dword. The position is then handed to NiAVObject::SetLocalTranslate
	// @0x440460 (which writes 3 floats at node+0x58) on *(void**)(sun+0x04) -- the
	// disc root -- and again on the glare root at sun+0x0C.
	//
	// THE CATCH, and why a verbatim passthrough breaks: Z is a function of |X| ONLY.
	// X sweeps -400 -> +400 across the day and +400 -> -400 across the night, so Z
	// traces the IDENTICAL above-horizon tent on both halves. The engine never moves
	// the sun below the horizon; it just app-culls the disc root at night
	// (SetAppCulled on sun+0x04 @0x641C41). Mirroring the node verbatim therefore
	// yields a sun that bounces off the horizon at A and B and stays up all night --
	// the "sets at hour 6 then immediately rises again" symptom. We keep the engine
	// arc exactly for the lit half and MIRROR Z for the dark half, which is the
	// faithful continuation of the engine's own single-plane model and is exactly
	// right at dawn/dusk, where the twilight direction actually matters.
	//
	// NOTE: sun+0x10 / sun+0x14 are NOT the arc nodes. They are child NiGeometry the
	// engine only queries for material fades (GetProperty(3) compared against
	// KPROP_SKY = 0x0D, @0x641EC8 / @0x641FC2). The previous
	// billboard->parent->grandparent walk read those, which is why the pushed
	// position never matched the rendered sun.
	inline constexpr uint32_t  SKY_GAME_HOUR_OFF   = 0xEC;  // float, Sky::GetGameHour
	inline constexpr uint32_t  SUN_ROOT_OFF        = 0x04;  // NiPointer<NiNode>, positioned + culled
	inline constexpr uint32_t  NIAV_KLOCAL_T_OFF   = 0x58;  // m_kLocal.m_Translate

	// Cached climate transition hours, 4 contiguous floats: sunrise begin, sunrise
	// end, sunset begin, sunset end. Written by the Sky::GetClimateTime* accessors
	// @0x595EA0 / 0x595F50 / 0x595FC0 / 0x596030, which pull a raw byte off the
	// climate record and divide by 6.0 (Bethesda stores these in 10-minute units).
	// Sun::Update refreshes all four every frame, so these are never stale in-world.
	inline constexpr uintptr_t CLIMATE_HOURS_ADDR  = 0x011CA9E8;

	// Setting fSunAlphaTransTime: Setting object at 0x011CCEA4, float value at +4
	// (Setting::GetValuePtr @0x403E20 returns this+4). Default 2.0.
	inline constexpr uintptr_t SUN_ALPHA_TRANS_TIME_ADDR = 0x011CCEA8;

	// The two hours at which the engine's sun arc crosses the horizon, matching
	// Sun::Update's own A/B bounds. False when the climate cache looks unpopulated
	// (main menu, or before the first sky tick).
	inline bool get_sun_horizon_hours(float& a_out, float& b_out)
	{
		const float* t = reinterpret_cast<const float*>(CLIMATE_HOURS_ADDR);
		for (int i = 0; i < 4; ++i)
			if (!(t[i] >= 0.0f) || t[i] > 24.0f) return false;

		const float sunrise = (t[1] + t[0]) * 0.5f;
		const float sunset  = (t[3] + t[2]) * 0.5f;
		if (!(sunset > sunrise)) return false;

		const float half = *reinterpret_cast<const float*>(SUN_ALPHA_TRANS_TIME_ADDR) * 0.5f;
		a_out = sunrise - half;
		b_out = sunset  + half;
		return b_out > a_out;
	}

	// Reads the moon's rendered direction from the live scene graph and
	// converts to (elevation, rotation) in degrees. Returns false when any
	// pointer in the chain is null (main menu, interior cell without sky,
	// or sky tick hasn't run yet).
	inline bool get_masser_orientation(float& elev_deg_out, float& rot_deg_out)
	{
		auto* mgr = *reinterpret_cast<void**>(WORLD_MANAGER_ADDR);
		if (!mgr) return false;
		auto* sky = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(mgr) + WM_SKY_OFF);
		if (!sky) return false;
		auto* masser = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(sky) + SKY_MASSER_OFF);
		if (!masser) return false;
		auto* billboard = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(masser) + MASSER_BILLBOARD_OFF);
		if (!billboard) return false;
		auto* parent = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(billboard) + NIAV_PARENT_OFF);
		if (!parent) return false;

		auto* moon_T   = reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(billboard) + NIAV_KWORLD_T_OFF);
		auto* parent_T = reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(parent) + NIAV_KWORLD_T_OFF);

		float dx = moon_T[0] - parent_T[0];
		float dy = moon_T[1] - parent_T[1];
		float dz = moon_T[2] - parent_T[2];
		float len = sqrtf(dx*dx + dy*dy + dz*dz);
		if (len < 1.0f) return false;
		dx /= len; dy /= len; dz /= len;

		constexpr float RAD_TO_DEG = 57.2957795f;
		elev_deg_out = asinf(dz) * RAD_TO_DEG;
		float rot = atan2f(dx, dy) * RAD_TO_DEG;
		if (rot < 0.0f) rot += 360.0f;
		rot_deg_out = rot;
		return true;
	}

	// Reads the sun's rendered direction and converts it to (elevation, rotation) in
	// degrees, mirroring the arc below the horizon across the dark half of the cycle.
	// See the block comment above SKY_GAME_HOUR_OFF for the full derivation.
	//
	// hour_out / night_out are diagnostics for the caller's log line — the sun path
	// never had periodic logging, which is a large part of why this went unnoticed.
	inline bool get_sun_orientation(float& elev_deg_out, float& rot_deg_out,
	                                float* hour_out = nullptr, bool* night_out = nullptr)
	{
		auto* mgr = *reinterpret_cast<void**>(WORLD_MANAGER_ADDR);
		if (!mgr) return false;
		auto* sky = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(mgr) + WM_SKY_OFF);
		if (!sky) return false;
		auto* sun = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(sky) + SKY_SUN_OFF);
		if (!sun) return false;

		// sun+0x04 is a NiPointer<NiNode>; NiPointer::operator* @0x559450 is a plain
		// single deref. This is the node Sun::Update actually positions.
		auto* root = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(sun) + SUN_ROOT_OFF);
		if (!root) return false;

		// Prefer the world-space arm (root vs its parent) so any rotation carried by
		// the sky root is folded in — that is what "matches the rendered sun" means.
		// Fall back to the local translate Sun::Update wrote, which stays correct
		// even if the scene-graph update hasn't run yet this frame.
		float x, y, z;
		auto* parent = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(root) + NIAV_PARENT_OFF);
		if (parent)
		{
			auto* root_T = reinterpret_cast<const float*>(
				reinterpret_cast<uintptr_t>(root) + NIAV_KWORLD_T_OFF);
			auto* par_T = reinterpret_cast<const float*>(
				reinterpret_cast<uintptr_t>(parent) + NIAV_KWORLD_T_OFF);
			x = root_T[0] - par_T[0];
			y = root_T[1] - par_T[1];
			z = root_T[2] - par_T[2];
		}
		else
		{
			auto* local_T = reinterpret_cast<const float*>(
				reinterpret_cast<uintptr_t>(root) + NIAV_KLOCAL_T_OFF);
			x = local_T[0]; y = local_T[1]; z = local_T[2];
		}

		const float hour = *reinterpret_cast<const float*>(
			reinterpret_cast<uintptr_t>(sky) + SKY_GAME_HOUR_OFF);
		if (hour_out) *hour_out = hour;

		// Dark half: the engine retraces the daytime arc, so drive the sun under the
		// horizon instead of letting it climb back up.
		float a, b;
		const bool night = get_sun_horizon_hours(a, b) && (hour <= a || hour >= b);
		if (night) z = -z;
		if (night_out) *night_out = night;

		float len = sqrtf(x*x + y*y + z*z);
		if (len < 1.0f) return false;
		x /= len; y /= len; z /= len;

		constexpr float RAD_TO_DEG = 57.2957795f;
		elev_deg_out = asinf(z) * RAD_TO_DEG;
		float rot = atan2f(x, y) * RAD_TO_DEG;
		if (rot < 0.0f) rot += 360.0f;
		rot_deg_out = rot;
		return true;
	}
}
