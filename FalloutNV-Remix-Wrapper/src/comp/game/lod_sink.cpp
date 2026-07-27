#include "std_include.hpp"
#include "lod_sink.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/config.hpp"

namespace comp::game::lod_sink
{
	namespace
	{
		// VS constant registers the distant-terrain shader reads. Verified against
		// SLS2002.vso's constant table (ObjToCubeSpace c8[2], HighDetailRange c12,
		// GeomorphParams c19) -- see lod_sink.hpp for the decoded arithmetic.
		constexpr int REG_OBJ_TO_CUBE = 8;
		constexpr int REG_HIGH_DETAIL = 12;
		constexpr int REG_GEOMORPH    = 19;

		constexpr int CACHE_SIZE = 16;

		struct Entry
		{
			IDirect3DVertexBuffer9* dst = nullptr;
			IDirect3DVertexBuffer9* src = nullptr;
			INT   base_vtx  = 0;
			UINT  count     = 0;   // vertices copied (min_vtx + num_verts)
			UINT  stride    = 0;
			UINT  offset    = 0;
			UINT  capacity  = 0;   // bytes allocated
			// The sink is a pure function of these 16 floats; when any of them
			// move (player crosses a cell, LOD morph advances) the cached
			// geometry is stale and must be rebuilt.
			float consts[16] = {};
			bool  valid = false;
		};

		Entry g_cache[CACHE_SIZE];
		int   g_rebuilds = 0;
		const char* g_reject = "not attempted";

		// Saved stream-0 binding so unbind() can put the game's buffer back.
		IDirect3DVertexBuffer9* g_saved_vb = nullptr;
		UINT g_saved_offset = 0;
		UINT g_saved_stride = 0;
		bool g_bound = false;

		float read_scalar(const unsigned char* p, int type)
		{
			switch (type)
			{
			case D3DDECLTYPE_FLOAT1:
			case D3DDECLTYPE_FLOAT2:
			case D3DDECLTYPE_FLOAT3:
			case D3DDECLTYPE_FLOAT4:
			{
				float v;
				std::memcpy(&v, p, sizeof(v));
				return v;
			}
			case D3DDECLTYPE_FLOAT16_2:
			case D3DDECLTYPE_FLOAT16_4:
			{
				// half -> float. FNV's LOD decls use FLOAT1 in practice; this is
				// here so an unexpected packing degrades to a correct value
				// instead of garbage geometry.
				unsigned short h;
				std::memcpy(&h, p, sizeof(h));
				const unsigned int sign = (h >> 15) & 0x1u;
				unsigned int exp  = (h >> 10) & 0x1Fu;
				unsigned int mant = h & 0x3FFu;
				unsigned int f;
				if (exp == 0)
				{
					if (mant == 0) f = sign << 31;
					else
					{
						while (!(mant & 0x400u)) { mant <<= 1; exp--; }
						exp++; mant &= ~0x400u;
						f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
					}
				}
				else if (exp == 31) f = (sign << 31) | 0x7F800000u | (mant << 13);
				else                f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
				float v;
				std::memcpy(&v, &f, sizeof(v));
				return v;
			}
			default:
				return 0.0f;
			}
		}

		void gather_consts(const float* vs, float* out16)
		{
			std::memcpy(out16 + 0, &vs[REG_OBJ_TO_CUBE * 4], 8 * sizeof(float));   // c8, c9
			std::memcpy(out16 + 8, &vs[REG_HIGH_DETAIL * 4], 4 * sizeof(float));   // c12
			std::memcpy(out16 + 12, &vs[REG_GEOMORPH * 4], 4 * sizeof(float));     // c19
		}

		// Applies SLS2002.vso's Z arithmetic to an already-copied vertex block.
		void sink_vertices(unsigned char* verts, UINT count, UINT stride,
			int pos_off, int pos_type, int tc1_off, int tc1_type, const float* c)
		{
			const float* row0 = c + 0;
			const float* row1 = c + 4;
			const float* hdr  = c + 8;
			const float* geo  = c + 12;

			const bool pos_has_w = (pos_type == D3DDECLTYPE_FLOAT4);

			for (UINT v = 0; v < count; v++)
			{
				unsigned char* vert = verts + v * stride;
				float* pos = reinterpret_cast<float*>(vert + pos_off);

				const float px = pos[0];
				const float py = pos[1];
				const float pz = pos[2];
				const float pw = pos_has_w ? pos[3] : 1.0f;

				// lrp r1.z, c19.x, r0.z, v2.x  ->  lerp(v2.x, pos.z, GeomorphParams.x)
				const float coarse_z = read_scalar(vert + tc1_off, tc1_type);
				const float morphed_z = coarse_z + geo[0] * (pz - coarse_z);

				// The box test runs on the MORPHED position (r1 = x, y, morphedZ, w).
				const float cx = row0[0] * px + row0[1] * py + row0[2] * morphed_z + row0[3] * pw;
				const float cy = row1[0] * px + row1[1] * py + row1[2] * morphed_z + row1[3] * pw;

				const bool inside = (std::fabs(cx - hdr[0]) < hdr[2])
				                 && (std::fabs(cy - hdr[1]) < hdr[3]);

				pos[2] = inside ? (morphed_z - geo[1]) : morphed_z;
			}
		}

		Entry* find_or_build(IDirect3DDevice9* dev, IDirect3DVertexBuffer9* src,
			INT base_vtx, UINT count, UINT stride, UINT offset,
			int pos_off, int pos_type, int tc1_off, int tc1_type, const float* consts)
		{
			const unsigned int slot =
				((static_cast<unsigned int>(reinterpret_cast<uintptr_t>(src) >> 4))
					^ static_cast<unsigned int>(base_vtx)
					^ (count * 2654435761u)) % CACHE_SIZE;

			Entry& e = g_cache[slot];

			if (e.valid && e.src == src && e.base_vtx == base_vtx && e.count == count &&
				e.stride == stride && e.offset == offset &&
				std::memcmp(e.consts, consts, sizeof(e.consts)) == 0)
			{
				return &e;   // hit: geometry and constants both unchanged
			}

			const UINT bytes = count * stride;

			// Reuse the allocation when it is already big enough; only the vertex
			// contents change as the player moves, and reallocating a D3DPOOL_MANAGED
			// buffer every frame through the Remix bridge is the expensive part.
			if (e.dst && e.capacity < bytes)
			{
				e.dst->Release();
				e.dst = nullptr;
				e.capacity = 0;
			}
			if (!e.dst)
			{
				if (FAILED(dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0,
					D3DPOOL_MANAGED, &e.dst, nullptr)) || !e.dst)
				{
					e.valid = false;
					return nullptr;
				}
				e.capacity = bytes;
			}

			void* src_data = nullptr;
			if (FAILED(src->Lock(offset + static_cast<UINT>(base_vtx) * stride, bytes,
				&src_data, D3DLOCK_READONLY)) || !src_data)
			{
				e.valid = false;
				return nullptr;
			}

			void* dst_data = nullptr;
			if (FAILED(e.dst->Lock(0, bytes, &dst_data, 0)) || !dst_data)
			{
				src->Unlock();
				e.valid = false;
				return nullptr;
			}

			std::memcpy(dst_data, src_data, bytes);
			sink_vertices(static_cast<unsigned char*>(dst_data), count, stride,
				pos_off, pos_type, tc1_off, tc1_type, consts);

			e.dst->Unlock();
			src->Unlock();

			e.src = src; e.base_vtx = base_vtx; e.count = count;
			e.stride = stride; e.offset = offset;
			std::memcpy(e.consts, consts, sizeof(e.consts));
			e.valid = true;
			g_rebuilds++;
			return &e;
		}
	}

	bool bind_sunk_stream(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts)
	{
		PROFILE_ZONE_N("lod_sink::bind_sunk_stream");
		if (!dev || g_bound) return false;

		auto& ffp = shared::common::ffp_state::get();

		const int tc1_off = ffp.cur_decl_texcoord1_off();
		if (tc1_off < 0) { g_reject = "decl has no TEXCOORD1 on stream 0"; return false; }
		const int tc1_type = ffp.cur_decl_texcoord1_type();
		const int pos_off  = ffp.cur_decl_pos_off();
		const int pos_type = ffp.cur_decl_pos_type();

		// Position must be readable as floats; every FNV terrain decl uses FLOAT3.
		if (pos_type != D3DDECLTYPE_FLOAT3 && pos_type != D3DDECLTYPE_FLOAT4)
		{
			g_reject = "POSITION is not FLOAT3/FLOAT4";
			return false;
		}

		auto* src = ffp.stream_vb(0);
		const UINT stride = ffp.stream_stride(0);
		const UINT offset = ffp.stream_offset(0);
		if (!src || stride == 0 || num_verts == 0) { g_reject = "no stream-0 VB / zero stride"; return false; }

		// Indices address the stream at (base_vtx + index) with index in
		// [min_vtx, min_vtx + num_verts). Copy from base_vtx and keep the index
		// range intact so the draw can pass base_vtx = 0 unchanged.
		const UINT count = min_vtx + num_verts;

		// Guard against a decl whose position/texcoord1 would read past the vertex.
		if (static_cast<UINT>(pos_off) + 12u > stride) { g_reject = "POSITION overruns stride"; return false; }
		if (static_cast<UINT>(tc1_off) + 4u > stride) { g_reject = "TEXCOORD1 overruns stride"; return false; }

		float consts[16];
		gather_consts(ffp.vs_const_data(), consts);

		// GeomorphParams.y is the sink distance. Zero means the engine isn't
		// sinking anything right now, so there is nothing to reproduce.
		if (consts[13] == 0.0f) { g_reject = "GeomorphParams.y (c19.y) is 0"; return false; }

		Entry* e = find_or_build(dev, src, base_vtx, count, stride, offset,
			pos_off, pos_type, tc1_off, tc1_type, consts);
		if (!e) { g_reject = "VB create/lock failed"; return false; }
		g_reject = "";

		g_saved_vb = src;
		g_saved_offset = offset;
		g_saved_stride = stride;
		g_bound = true;

		dev->SetStreamSource(0, e->dst, 0, stride);
		return true;
	}

	void unbind(IDirect3DDevice9* dev)
	{
		if (!g_bound || !dev) return;
		dev->SetStreamSource(0, g_saved_vb, g_saved_offset, g_saved_stride);
		g_bound = false;
		g_saved_vb = nullptr;
	}

	void release_cache()
	{
		for (auto& e : g_cache)
		{
			if (e.dst) { e.dst->Release(); e.dst = nullptr; }
			e = Entry{};
		}
		g_bound = false;
		g_saved_vb = nullptr;
	}

	int rebuilds_this_frame() { return g_rebuilds; }
	void on_present() { g_rebuilds = 0; }

	int cached_entries()
	{
		int n = 0;
		for (const auto& e : g_cache) if (e.valid) n++;
		return n;
	}

	const char* last_reject_reason() { return g_reject; }
}

namespace comp::game::lod_debug
{
	namespace { int g_far = 0, g_near = 0, g_far_shown = 0, g_near_shown = 0; }

	void count_far()  { g_far++; }
	void count_near() { g_near++; }
	int  far_draws()  { return g_far_shown; }
	int  near_draws() { return g_near_shown; }

	void on_present()
	{
		// Latch last frame's totals so the ImGui readout isn't whatever partial
		// count the overlay happened to be drawn at.
		g_far_shown = g_far;  g_far = 0;
		g_near_shown = g_near; g_near = 0;

		// Periodic status to the log so the readout is available with the
		// overlay closed. Only while LOD terrain is actually being drawn, so an
		// interior cell doesn't fill the log with zeroes.
		static UINT s_tick = 0;
		if ((g_far_shown || g_near_shown) && (++s_tick % 300u) == 0u)
		{
			const auto& cfg = shared::common::config::get().ffp;
			const char* rej = lod_sink::last_reject_reason();
			shared::common::log("LOD", std::format(
				"far={} near={} sink={} dropFar={} nearMode={} rebuilds={} {}",
				g_far_shown, g_near_shown,
				cfg.lod_sink_mode == 2 ? "per-vertex" : cfg.lod_sink_mode == 1 ? "uniform" : "off",
				cfg.debug_drop_far_lod ? 1 : 0, cfg.near_lod_mode,
				lod_sink::rebuilds_this_frame(),
				(rej && rej[0]) ? std::format("[sink INACTIVE: {}]", rej) : std::string("[sink active]")),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
		}
	}

	void poll_hotkeys()
	{
		auto& cfg = shared::common::config::get().ffp;

		struct Key { int vk; bool was; };
		static Key k_far{ VK_F6, false }, k_near{ VK_F7, false }, k_sink{ VK_F8, false };

		auto edge = [](Key& k)
			{
				const bool now = (GetAsyncKeyState(k.vk) & 0x8000) != 0;
				const bool hit = now && !k.was;
				k.was = now;
				return hit;
			};

		if (edge(k_far))
		{
			cfg.debug_drop_far_lod = !cfg.debug_drop_far_lod;
			shared::common::log("LOD", std::format("F6: drop FAR LOD (path-traced) = {}",
				cfg.debug_drop_far_lod ? "ON" : "off"),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		if (edge(k_near))
		{
			cfg.near_lod_mode = (cfg.near_lod_mode == 1) ? 0 : 1;
			shared::common::log("LOD", std::format("F7: near LOD = {}",
				cfg.near_lod_mode == 1 ? "DROPPED (default)" : "passthrough (legacy, z-fights)"),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		if (edge(k_sink))
		{
			cfg.lod_sink_mode = (cfg.lod_sink_mode == 2) ? 1 : 2;
			shared::common::log("LOD", std::format("F8: LOD sink mode = {}",
				cfg.lod_sink_mode == 2 ? "per-vertex (exact)" : "uniform (legacy LodSinkZ)"),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
	}
}
