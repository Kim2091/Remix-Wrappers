#pragma once

namespace comp::game::lod_sink
{
	// Exact per-vertex replication of FNV's distant-terrain sink.
	//
	// The distant-terrain vertex shader (SLS2002.vso, paired with the no-normal
	// far-LOD pixel shader 0x36E87D02) lowers a vertex ONLY when it falls inside
	// the currently-loaded high-detail rectangle, so the coarse LOD tucks under
	// the real terrain there and stays put everywhere else:
	//
	//   morphedZ = lerp(TEXCOORD1.x, POSITION.z, GeomorphParams.x)
	//   cx       = dot(ObjToCubeSpace[0], (x, y, morphedZ, w))
	//   cy       = dot(ObjToCubeSpace[1], (x, y, morphedZ, w))
	//   inside   = |cx - HighDetailRange.x| < HighDetailRange.z
	//           && |cy - HighDetailRange.y| < HighDetailRange.w
	//   finalZ   = morphedZ - inside * GeomorphParams.y
	//
	// FFP has no programmable vertex stage, so the wrapper used to approximate
	// this by lowering the whole draw's WORLD matrix by a constant. That is wrong
	// in both directions at once: vertices inside the rectangle get too little
	// sink (the coarse LOD z-fights the real terrain) while vertices outside it
	// get sink they should never receive (distant terrain steps down at the LOD
	// seam). No single constant fixes both, which is why LodSinkZ never converged.
	//
	// Instead we run the shader's arithmetic on the CPU and hand the draw a
	// rewritten vertex buffer with the corrected Z. Only the position Z changes;
	// every other byte of the vertex is copied verbatim.
	//
	// Constants come from ffp_state's mirror of the game's VS constant writes:
	//   c8,c9 = ObjToCubeSpace rows 0-1   c12 = HighDetailRange   c19 = GeomorphParams

	// Builds (or reuses) a sunk copy of the current stream-0 vertex range and
	// binds it to stream 0. Returns true if the swap happened, in which case the
	// caller must draw with base_vtx = 0 and then call unbind().
	//
	// Returns false when the decl or constants aren't usable (no TEXCOORD1, zero
	// stride, lock failure, ...); the caller should then fall back to the legacy
	// uniform world sink so behaviour never gets worse than before.
	bool bind_sunk_stream(IDirect3DDevice9* dev, INT base_vtx, UINT min_vtx, UINT num_verts);

	// Restores the game's original stream-0 binding after the draw.
	void unbind(IDirect3DDevice9* dev);

	// Release cached buffers (device reset / shutdown).
	void release_cache();

	// Diagnostics: how many rebuilds happened this frame, and cache occupancy.
	int rebuilds_this_frame();
	void on_present();
	int cached_entries();

	// Why the last bind_sunk_stream call declined, for the ImGui readout.
	// "" once it has succeeded at least once.
	const char* last_reject_reason();
}

// Per-frame counts of each distant-terrain LOD class, so it is possible to tell
// which one is actually on screen (and therefore which one is z-fighting)
// without guessing from shader hashes.
namespace comp::game::lod_debug
{
	void count_far();
	void count_near();
	int far_draws();
	int near_draws();
	void on_present();

	// Global hotkeys, polled once per frame from Present. These deliberately do
	// NOT go through ImGui: the F4 overlay can be read but not clicked in this
	// setup (game input conflict), so the isolation toggles need to work without
	// it. Every press logs its new state, and a status line is written to
	// rtx_comp\logfile.txt periodically, so the whole diagnostic is usable with
	// the overlay closed.
	//   F6 = drop FAR LOD (path-traced)   F7 = drop NEAR LOD (rasterised)
	//   F8 = LOD sink mode per-vertex <-> uniform
	// F5/F9 are avoided (FNV quicksave/quickload).
	void poll_hotkeys();
}
