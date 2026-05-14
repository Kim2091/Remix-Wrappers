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

	// Sun version. Same idea as get_masser_orientation but the chain is one
	// level deeper because the sun billboard's m_kLocal.translate is (0,0,0).
	// The orbital arm offset lives in the BILLBOARD's PARENT's m_kLocal —
	// so we walk billboard → parent → grandparent and take the world-space
	// translate difference between parent and grandparent. Magnitude ~595.
	inline bool get_sun_orientation(float& elev_deg_out, float& rot_deg_out)
	{
		auto* mgr = *reinterpret_cast<void**>(WORLD_MANAGER_ADDR);
		if (!mgr) return false;
		auto* sky = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(mgr) + WM_SKY_OFF);
		if (!sky) return false;
		auto* sun = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(sky) + SKY_SUN_OFF);
		if (!sun) return false;
		auto* billboard = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(sun) + SUN_BILLBOARD_OFF);
		if (!billboard) return false;
		auto* parent = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(billboard) + NIAV_PARENT_OFF);
		if (!parent) return false;
		auto* grandparent = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(parent) + NIAV_PARENT_OFF);
		if (!grandparent) return false;

		auto* parent_T = reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(parent) + NIAV_KWORLD_T_OFF);
		auto* gp_T = reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(grandparent) + NIAV_KWORLD_T_OFF);

		float dx = parent_T[0] - gp_T[0];
		float dy = parent_T[1] - gp_T[1];
		float dz = parent_T[2] - gp_T[2];
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
}
