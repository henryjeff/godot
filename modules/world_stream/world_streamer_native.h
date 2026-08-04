/**************************************************************************/
/*  world_streamer_native.h                                               */
/**************************************************************************/
/*  fridge fork module: world_stream (the WorldStreamer machine, native). */
/**************************************************************************/

#pragma once

#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

// THE FULL SEND (FORK_PORT_PLAN.md in the game repo): WorldStreamer becomes
// this engine node.
//   M1 — key/dist/cut, lifted verbatim from the extension's StreamCutCore
//        (addons/wasteland_terrain/src/stream_cut.cpp), itself the pinned
//        mirror of WorldStreamer._collect.
//   M2 — the ledgers (desired/jobs/active/cache-LRU) plus pump and drain:
//        worker submission happens here, and the drain ingests height bounds
//        and child error straight from the layer — the GDScript machine's
//        mirror pushes are gone; these maps ARE the state the cut reads.
//
// The GDScript machine remains the AUTHORITY until M5 and keeps the payload
// dict itself (drain returns event-rate deltas for it); layers stay GDScript
// Objects duck-called at leaf events only. tests/test_stream_cut_native.gd
// and tests/test_stream_machine_native.gd in the game repo pin parity.
//
// STATE MIRRORING CONTRACT for the cut inputs (same as the extension):
// per-key height bounds and child error are learned at exactly ONE point (the
// drain, when a payload lands); clear_layer() on flush/rebuild. Arithmetic is
// double throughout with bounds/error stored as float — byte-for-byte the
// extension's layout, so the parity test compares equal work, not close work.

// One worker build in flight: runs the layer's build_leaf against the
// main-thread-snapshotted context. Heap object with a stable address — the
// worker writes payload/build_ms on it while the jobs map may rehash.
// Synchronization is task completion, exactly the GDScript WorkItem contract.
class WorldStreamJob : public RefCounted {
	GDCLASS(WorldStreamJob, RefCounted);

public:
	Variant layer;
	int64_t key = 0;
	Rect2 rect;
	int depth = 0;
	Variant ctx;
	Variant payload;
	double build_ms = 0.0;
	uint64_t started_ms = 0;
	bool stuck_warned = false;
	WorkerThreadPool::TaskID task_id = -1;

	static void run_task(void *p_userdata);

protected:
	static void _bind_methods() {}
};

class WorldStreamerNative : public Node3D {
	GDCLASS(WorldStreamerNative, Node3D);

public:
	void configure(double p_root_tile_size);
	double get_root_tile_size() const { return root_tile; }
	void set_cache_limit(int p_limit);
	int add_layer(const Variant &p_layer = Variant());
	void clear_layer(int p_layer);
	void clear_cache(int p_layer);
	void wait_jobs(int p_layer);
	void push_bounds(int p_layer, int64_t p_key, double p_min_y, double p_max_y);
	void push_child_error(int p_layer, int64_t p_key, double p_err);

	// One recollect: every root tile in `tiles` (tx,tz pairs), recursed to the
	// band, camera-led. Pure — returns the desired keys, touches no ledger
	// (the cut-parity test's surface).
	PackedInt64Array collect(int p_layer, const PackedInt32Array &p_tiles,
			const Vector3 &p_cam, const Vector3 &p_cam_true, double p_px_scale,
			double p_err_threshold, double p_split_factor, double p_radius,
			int p_band_lo, int p_band_hi);
	// The machine's recollect: same cut, but REPLACES the layer's native
	// desired ledger (coords recorded during the recursion, no decode).
	PackedInt64Array collect_apply(int p_layer, const PackedInt32Array &p_tiles,
			const Vector3 &p_cam, const Vector3 &p_cam_true, double p_px_scale,
			double p_err_threshold, double p_split_factor, double p_radius,
			int p_band_lo, int p_band_hi);

	// Submit worker builds for every desired leaf without a payload (not
	// cached, not active, not in flight), nearest-first, into the shared
	// in-flight cap across all layers. `plan_store` (nullable Object) gates
	// leaves behind unresolved plan cells. Returns submissions made.
	int pump(const Vector3 &p_cam, int p_max_in_flight, const Variant &p_plan_store);
	// Move every finished worker job into the cache ledger: duck-calls
	// payload_cached on the layer, ingests bounds/child_error, runs LRU
	// eviction. Returns { "drained": [[layer, key, payload], ...],
	// "evicted": [[layer, key], ...], "worker_ms": float } — the event-rate
	// delta the GDScript payload dict applies.
	Dictionary drain();

	// Active-set ledger, maintained by the GDScript reconcile until M3 moves
	// it in. note_attached also LRU-touches the key (the _attach contract).
	void note_attached(int p_layer, int64_t p_key);
	void note_detached(int p_layer, int64_t p_key);

	int total_jobs() const;
	int job_count(int p_layer) const;
	int last_missing(int p_layer) const;
	int last_plan_wait(int p_layer) const;
	// Keys of jobs older than `age_ms` not yet reported; marks them reported.
	PackedInt64Array stuck_keys(int p_layer, int p_age_ms);
	uint64_t job_started_ms(int p_layer, int64_t p_key) const;
	// Ledger membership snapshots — parity audits and debug panels only.
	PackedInt64Array desired_keys(int p_layer) const;
	PackedInt64Array cache_keys(int p_layer) const;
	PackedInt64Array active_keys(int p_layer) const;

protected:
	static void _bind_methods();

private:
	struct Bounds {
		float min_y = 0.0f;
		float max_y = 0.0f;
	};
	struct DesiredInfo {
		int tx = 0;
		int tz = 0;
		int depth = 0;
		int ix = 0;
		int iz = 0;
	};
	struct LayerMaps {
		Variant layer;
		HashMap<int64_t, Bounds> bounds;
		HashMap<int64_t, float> child_err;
		HashMap<int64_t, DesiredInfo> desired;
		HashMap<int64_t, Ref<WorldStreamJob>> jobs;
		HashSet<int64_t> active;
		HashSet<int64_t> cache;
		LocalVector<int64_t> cache_order;
		int last_missing = 0;
		int last_plan_wait = 0;
	};

	double root_tile = 1024.0;
	int cache_limit = 96;
	LocalVector<LayerMaps> layers;
	PackedInt64Array out_keys;

	struct CollectArgs {
		LayerMaps *maps = nullptr;
		Vector3 cam;
		Vector3 cam_true;
		double px_scale = 0.0;
		double err_threshold = 0.0;
		double split_factor = 2.0;
		double radius = 0.0;
		int band_lo = 0;
		int band_hi = 0;
		bool record_desired = false;
	};
	void collect_node(const CollectArgs &p_args, int p_tx, int p_tz, int p_depth, int p_ix, int p_iz);
	PackedInt64Array collect_impl(int p_layer, const PackedInt32Array &p_tiles,
			const Vector3 &p_cam, const Vector3 &p_cam_true, double p_px_scale,
			double p_err_threshold, double p_split_factor, double p_radius,
			int p_band_lo, int p_band_hi, bool p_record_desired);
	double rect_dist(int p_tx, int p_tz, int p_depth, int p_ix, int p_iz, const Vector3 &p_cam) const;
	void submit(int p_layer, int64_t p_key, const Variant &p_plan_store);
};
