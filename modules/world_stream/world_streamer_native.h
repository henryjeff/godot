/**************************************************************************/
/*  world_streamer_native.h                                               */
/**************************************************************************/
/*  fridge fork module: world_stream (the WorldStreamer machine, native). */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

// THE FULL SEND (FORK_PORT_PLAN.md in the game repo): WorldStreamer becomes
// this engine node. M1 = skeleton + key/dist/cut, lifted verbatim from the
// extension's StreamCutCore (addons/wasteland_terrain/src/stream_cut.cpp),
// which is itself the pinned mirror of WorldStreamer._collect. The GDScript
// machine remains the AUTHORITY until M5; tests/test_stream_cut_native.gd in
// the game repo pins all three cut implementations identical.
//
// STATE MIRRORING CONTRACT (same as the extension): per-key height bounds and
// child error are learned at exactly ONE point (the machine's _drain_jobs) and
// pushed here at that moment; clear_layer() on flush/rebuild. Arithmetic is
// double throughout with bounds/error stored as float — byte-for-byte the
// extension's layout, so the parity test compares equal work, not close work.
class WorldStreamerNative : public Node3D {
	GDCLASS(WorldStreamerNative, Node3D);

public:
	void configure(double p_root_tile_size);
	double get_root_tile_size() const { return root_tile; }
	int add_layer();
	void clear_layer(int p_layer);
	void push_bounds(int p_layer, int64_t p_key, double p_min_y, double p_max_y);
	void push_child_error(int p_layer, int64_t p_key, double p_err);
	// One recollect: every root tile in `tiles` (tx,tz pairs), recursed to the
	// band, camera-led. Returns the desired keys (the caller owns dict
	// bookkeeping for genuinely-new ones).
	PackedInt64Array collect(int p_layer, const PackedInt32Array &p_tiles,
			const Vector3 &p_cam, const Vector3 &p_cam_true, double p_px_scale,
			double p_err_threshold, double p_split_factor, double p_radius,
			int p_band_lo, int p_band_hi);

protected:
	static void _bind_methods();

private:
	struct Bounds {
		float min_y = 0.0f;
		float max_y = 0.0f;
	};
	struct LayerMaps {
		HashMap<int64_t, Bounds> bounds;
		HashMap<int64_t, float> child_err;
	};

	double root_tile = 1024.0;
	LocalVector<LayerMaps> layers;
	PackedInt64Array out_keys;

	struct CollectArgs {
		const LayerMaps *maps = nullptr;
		Vector3 cam;
		Vector3 cam_true;
		double px_scale = 0.0;
		double err_threshold = 0.0;
		double split_factor = 2.0;
		double radius = 0.0;
		int band_lo = 0;
		int band_hi = 0;
	};
	void collect_node(const CollectArgs &p_args, int p_tx, int p_tz, int p_depth, int p_ix, int p_iz);
	double rect_dist(int p_tx, int p_tz, int p_depth, int p_ix, int p_iz, const Vector3 &p_cam) const;
};
