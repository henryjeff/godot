/**************************************************************************/
/*  world_streamer_native.cpp                                             */
/**************************************************************************/
/*  fridge fork module: world_stream (the WorldStreamer machine, native). */
/**************************************************************************/

#include "world_streamer_native.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/pair.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Mirrors WorldStreamer._KEY_BIAS and _key()'s bit layout — ALL SIDES MUST
// MATCH: depth:4 ix:8 iz:8 (tz+BIAS):20 (tx+BIAS):20, low to high. Three
// codebases now share this layout (GDScript machine, extension StreamCutCore,
// this module).
static const int64_t KEY_BIAS = 1 << 19;

static inline int64_t make_key(int p_tx, int p_tz, int p_depth, int p_ix, int p_iz) {
	return (int64_t)p_depth | ((int64_t)p_ix << 4) | ((int64_t)p_iz << 12) | (((int64_t)p_tz + KEY_BIAS) << 20) | (((int64_t)p_tx + KEY_BIAS) << 40);
}

// WorldStreamer.decode — same bit layout, unpacked.
static inline void decode_key(int64_t p_key, int &r_tx, int &r_tz, int &r_depth, int &r_ix, int &r_iz) {
	r_tx = (int)((p_key >> 40) & 0xFFFFF) - (int)KEY_BIAS;
	r_tz = (int)((p_key >> 20) & 0xFFFFF) - (int)KEY_BIAS;
	r_depth = (int)(p_key & 0xF);
	r_ix = (int)((p_key >> 4) & 0xFF);
	r_iz = (int)((p_key >> 12) & 0xFF);
}

// --- WorldStreamJob -----------------------------------------------------------

// Worker-thread body. Duck-calls the layer's build_leaf exactly as the
// GDScript WorkItem.run does — layers already run this on WorkerThreadPool
// threads FROM GDScript, so the threading contract is unchanged. The raw
// pointer is safe: the jobs map holds a Ref until the task is waited on
// (drain / wait_jobs), so the object outlives the task by construction.
void WorldStreamJob::run_task(void *p_userdata) {
	WorldStreamJob *job = static_cast<WorldStreamJob *>(p_userdata);
	uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	Object *lo = job->layer.get_validated_object();
	if (lo != nullptr) {
		job->payload = lo->call(SNAME("build_leaf"), job->key, job->rect, job->depth, job->ctx);
	}
	job->build_ms = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
}

// --- WorldStreamerNative ------------------------------------------------------

void WorldStreamerNative::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure", "root_tile_size"),
			&WorldStreamerNative::configure);
	ClassDB::bind_method(D_METHOD("get_root_tile_size"),
			&WorldStreamerNative::get_root_tile_size);
	ClassDB::bind_method(D_METHOD("set_cache_limit", "limit"),
			&WorldStreamerNative::set_cache_limit);
	ClassDB::bind_method(D_METHOD("add_layer", "layer"),
			&WorldStreamerNative::add_layer, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("clear_layer", "layer"),
			&WorldStreamerNative::clear_layer);
	ClassDB::bind_method(D_METHOD("clear_cache", "layer"),
			&WorldStreamerNative::clear_cache);
	ClassDB::bind_method(D_METHOD("wait_jobs", "layer"),
			&WorldStreamerNative::wait_jobs);
	ClassDB::bind_method(D_METHOD("push_bounds", "layer", "key", "min_y", "max_y"),
			&WorldStreamerNative::push_bounds);
	ClassDB::bind_method(D_METHOD("push_child_error", "layer", "key", "err"),
			&WorldStreamerNative::push_child_error);
	ClassDB::bind_method(D_METHOD("collect", "layer", "tiles", "cam", "cam_true",
								 "px_scale", "err_threshold", "split_factor",
								 "radius", "band_lo", "band_hi"),
			&WorldStreamerNative::collect);
	ClassDB::bind_method(D_METHOD("collect_apply", "layer", "tiles", "cam", "cam_true",
								 "px_scale", "err_threshold", "split_factor",
								 "radius", "band_lo", "band_hi"),
			&WorldStreamerNative::collect_apply);
	ClassDB::bind_method(D_METHOD("pump", "cam", "max_in_flight", "plan_store"),
			&WorldStreamerNative::pump);
	ClassDB::bind_method(D_METHOD("drain"), &WorldStreamerNative::drain);
	ClassDB::bind_method(D_METHOD("note_attached", "layer", "key"),
			&WorldStreamerNative::note_attached);
	ClassDB::bind_method(D_METHOD("note_detached", "layer", "key"),
			&WorldStreamerNative::note_detached);
	ClassDB::bind_method(D_METHOD("reconcile_plan", "cam", "ctx", "ctz",
								 "tile_radius", "max_applies"),
			&WorldStreamerNative::reconcile_plan);
	ClassDB::bind_method(D_METHOD("desired_count", "layer"),
			&WorldStreamerNative::desired_count);
	ClassDB::bind_method(D_METHOD("active_keys_within", "layer", "cam", "radius"),
			&WorldStreamerNative::active_keys_within);
	ClassDB::bind_method(D_METHOD("total_jobs"), &WorldStreamerNative::total_jobs);
	ClassDB::bind_method(D_METHOD("job_count", "layer"),
			&WorldStreamerNative::job_count);
	ClassDB::bind_method(D_METHOD("last_missing", "layer"),
			&WorldStreamerNative::last_missing);
	ClassDB::bind_method(D_METHOD("last_plan_wait", "layer"),
			&WorldStreamerNative::last_plan_wait);
	ClassDB::bind_method(D_METHOD("stuck_keys", "layer", "age_ms"),
			&WorldStreamerNative::stuck_keys);
	ClassDB::bind_method(D_METHOD("job_started_ms", "layer", "key"),
			&WorldStreamerNative::job_started_ms);
	ClassDB::bind_method(D_METHOD("desired_keys", "layer"),
			&WorldStreamerNative::desired_keys);
	ClassDB::bind_method(D_METHOD("cache_keys", "layer"),
			&WorldStreamerNative::cache_keys);
	ClassDB::bind_method(D_METHOD("active_keys", "layer"),
			&WorldStreamerNative::active_keys);
}

void WorldStreamerNative::configure(double p_root_tile_size) {
	root_tile = p_root_tile_size;
}

void WorldStreamerNative::set_cache_limit(int p_limit) {
	cache_limit = MAX(1, p_limit);
}

int WorldStreamerNative::add_layer(const Variant &p_layer) {
	LayerMaps m;
	m.layer = p_layer;
	layers.push_back(m);
	return (int)layers.size() - 1;
}

void WorldStreamerNative::clear_layer(int p_layer) {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return;
	}
	// Never drop a job Ref while its worker task may still be writing into it.
	wait_jobs(p_layer);
	LayerMaps &m = layers[(uint32_t)p_layer];
	m.bounds.clear();
	m.child_err.clear();
	m.desired.clear();
	m.stamped.clear();
	m.active.clear();
	m.cache.clear();
	m.cache_order.clear();
	m.last_missing = 0;
	m.last_plan_wait = 0;
}

void WorldStreamerNative::clear_cache(int p_layer) {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return;
	}
	layers[(uint32_t)p_layer].cache.clear();
	layers[(uint32_t)p_layer].cache_order.clear();
}

void WorldStreamerNative::wait_jobs(int p_layer) {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return;
	}
	LayerMaps &m = layers[(uint32_t)p_layer];
	for (const KeyValue<int64_t, Ref<WorldStreamJob>> &kv : m.jobs) {
		(void)WorkerThreadPool::get_singleton()->wait_for_task_completion(kv.value->task_id);
	}
	m.jobs.clear();
}

void WorldStreamerNative::push_bounds(int p_layer, int64_t p_key, double p_min_y,
		double p_max_y) {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return;
	}
	layers[(uint32_t)p_layer].bounds[p_key] = Bounds{ (float)p_min_y, (float)p_max_y };
}

void WorldStreamerNative::push_child_error(int p_layer, int64_t p_key, double p_err) {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return;
	}
	layers[(uint32_t)p_layer].child_err[p_key] = (float)p_err;
}

// WorldStreamer._rect_dist — horizontal distance to the node's footprint.
double WorldStreamerNative::rect_dist(int p_tx, int p_tz, int p_depth, int p_ix,
		int p_iz, const Vector3 &p_cam) const {
	double node_size = root_tile / std::pow(2.0, (double)p_depth);
	double min_x = (double)p_tx * root_tile + (double)p_ix * node_size;
	double min_z = (double)p_tz * root_tile + (double)p_iz * node_size;
	double px = std::clamp((double)p_cam.x, min_x, min_x + node_size);
	double pz = std::clamp((double)p_cam.z, min_z, min_z + node_size);
	double dx = (double)p_cam.x - px;
	double dz = (double)p_cam.z - pz;
	return std::sqrt(dx * dx + dz * dz);
}

// WorldStreamer._collect, byte-for-byte in double math. Comments live with the
// AUTHORITY (the GDScript), not the mirror — see world_streamer.gd for the
// render-cut hole rationale and the min-of-two-cameras error rule.
void WorldStreamerNative::collect_node(const CollectArgs &p_args, int p_tx,
		int p_tz, int p_depth, int p_ix, int p_iz) {
	double node_size = root_tile / std::pow(2.0, (double)p_depth);
	int64_t key = make_key(p_tx, p_tz, p_depth, p_ix, p_iz);
	double flat = rect_dist(p_tx, p_tz, p_depth, p_ix, p_iz, p_args.cam);
	double dist = flat;
	const Bounds *b = p_args.maps->bounds.getptr(key);
	if (b != nullptr) {
		double bmin = (double)b->min_y;
		double bmax = (double)b->max_y;
		if (!std::isnan(bmin) && !std::isnan(bmax)) {
			double dy = std::max(std::max(bmin - (double)p_args.cam.y,
										 (double)p_args.cam.y - bmax),
					0.0);
			dist = std::sqrt(flat * flat + dy * dy);
		}
	}
	if (p_args.radius > 0.0 && dist > p_args.radius) {
		return;
	}
	double err = -1.0;
	double seen = dist;
	if (p_args.err_threshold > 0.0 && p_args.px_scale > 0.0) {
		const float *e = p_args.maps->child_err.getptr(key);
		err = e != nullptr ? (double)*e : -1.0;
		if (err >= 0.0) {
			seen = std::min(dist, rect_dist(p_tx, p_tz, p_depth, p_ix, p_iz, p_args.cam_true));
		}
	}
	// LodError.wants_split, inlined.
	bool want_split;
	if (p_args.err_threshold > 0.0 && p_args.px_scale > 0.0 && err >= 0.0) {
		double px = seen <= 0.0 ? std::numeric_limits<double>::infinity()
								: err * p_args.px_scale / seen;
		want_split = px > p_args.err_threshold;
	} else {
		want_split = seen < p_args.split_factor * node_size;
	}
	want_split = want_split && p_depth < p_args.band_hi;
	if (want_split) {
		for (int cz = 0; cz < 2; cz++) {
			for (int cx = 0; cx < 2; cx++) {
				collect_node(p_args, p_tx, p_tz, p_depth + 1, p_ix * 2 + cx, p_iz * 2 + cz);
			}
		}
	} else if (p_depth >= p_args.band_lo) {
		out_keys.push_back(key);
		if (p_args.record_desired) {
			p_args.maps->desired.insert(key, DesiredInfo{ p_tx, p_tz, p_depth, p_ix, p_iz });
		}
	}
}

PackedInt64Array WorldStreamerNative::collect_impl(int p_layer, const PackedInt32Array &p_tiles,
		const Vector3 &p_cam, const Vector3 &p_cam_true, double p_px_scale,
		double p_err_threshold, double p_split_factor, double p_radius,
		int p_band_lo, int p_band_hi, bool p_record_desired) {
	out_keys.clear();
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return out_keys;
	}
	CollectArgs a;
	a.maps = &layers[(uint32_t)p_layer];
	a.cam = p_cam;
	a.cam_true = p_cam_true;
	a.px_scale = p_px_scale;
	a.err_threshold = p_err_threshold;
	a.split_factor = p_split_factor;
	a.radius = p_radius;
	a.band_lo = p_band_lo;
	a.band_hi = p_band_hi;
	a.record_desired = p_record_desired;
	if (p_record_desired) {
		a.maps->desired.clear();
		// The reconcile planner walks with the SAME band and resolved radius
		// this cut ran under.
		a.maps->band_lo = p_band_lo;
		a.maps->band_hi = p_band_hi;
		a.maps->radius = p_radius;
	}
	for (int t = 0; t + 1 < p_tiles.size(); t += 2) {
		collect_node(a, p_tiles[t], p_tiles[t + 1], 0, 0, 0);
	}
	return out_keys;
}

PackedInt64Array WorldStreamerNative::collect(int p_layer, const PackedInt32Array &p_tiles,
		const Vector3 &p_cam, const Vector3 &p_cam_true, double p_px_scale,
		double p_err_threshold, double p_split_factor, double p_radius,
		int p_band_lo, int p_band_hi) {
	return collect_impl(p_layer, p_tiles, p_cam, p_cam_true, p_px_scale,
			p_err_threshold, p_split_factor, p_radius, p_band_lo, p_band_hi, false);
}

PackedInt64Array WorldStreamerNative::collect_apply(int p_layer, const PackedInt32Array &p_tiles,
		const Vector3 &p_cam, const Vector3 &p_cam_true, double p_px_scale,
		double p_err_threshold, double p_split_factor, double p_radius,
		int p_band_lo, int p_band_hi) {
	(void)collect_impl(p_layer, p_tiles, p_cam, p_cam_true, p_px_scale,
			p_err_threshold, p_split_factor, p_radius, p_band_lo, p_band_hi, true);
	PackedInt64Array fresh;
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return fresh;
	}
	LayerMaps &m = layers[(uint32_t)p_layer];
	for (const KeyValue<int64_t, DesiredInfo> &kv : m.desired) {
		if (!m.stamped.has(kv.key)) {
			m.stamped.insert(kv.key, true);
			fresh.push_back(kv.key);
		}
	}
	return fresh;
}

int WorldStreamerNative::desired_count(int p_layer) const {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return 0;
	}
	return (int)layers[(uint32_t)p_layer].desired.size();
}

PackedInt64Array WorldStreamerNative::active_keys_within(int p_layer,
		const Vector3 &p_cam, double p_radius) const {
	PackedInt64Array out;
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return out;
	}
	const LayerMaps &m = layers[(uint32_t)p_layer];
	LocalVector<Pair<double, int64_t>> hits;
	for (const KeyValue<int64_t, bool> &kv : m.active) {
		int tx, tz, depth, ix, iz;
		decode_key(kv.key, tx, tz, depth, ix, iz);
		double d = rect_dist(tx, tz, depth, ix, iz, p_cam);
		if (d <= p_radius) {
			hits.push_back(Pair<double, int64_t>(d, kv.key));
		}
	}
	std::sort(hits.ptr(), hits.ptr() + hits.size(),
			[](const Pair<double, int64_t> &a, const Pair<double, int64_t> &b) {
				return a.first != b.first ? a.first < b.first : a.second < b.second;
			});
	for (uint32_t i = 0; i < hits.size(); i++) {
		out.push_back(hits[i].second);
	}
	return out;
}

// WorldStreamer._pump_jobs. The missing-set walk, the plan backpressure, the
// nearest-first sort, and the shared in-flight cap across layers — the whole
// loop the GDScript machine ran over Variant dicts.
int WorldStreamerNative::pump(const Vector3 &p_cam, int p_max_in_flight,
		const Variant &p_plan_store) {
	int free_slots = p_max_in_flight - total_jobs();
	int submitted = 0;
	Object *ps = p_plan_store.get_validated_object();
	for (uint32_t li = 0; li < layers.size(); li++) {
		LayerMaps &m = layers[li];
		LocalVector<Pair<double, int64_t>> missing;
		int plan_wait = 0;
		for (const KeyValue<int64_t, DesiredInfo> &kv : m.desired) {
			if (m.active.has(kv.key) || m.jobs.has(kv.key) || m.cache.has(kv.key)) {
				continue;
			}
			// Plan backpressure: a leaf whose plan cell isn't published yet
			// queues behind the prefetch — never build against a missing plan.
			if (ps != nullptr) {
				bool resolved = ps->call(SNAME("is_resolved"), Vector2i(kv.value.tx, kv.value.tz));
				if (!resolved) {
					plan_wait++;
					continue;
				}
			}
			const DesiredInfo &d = kv.value;
			missing.push_back(Pair<double, int64_t>(
					rect_dist(d.tx, d.tz, d.depth, d.ix, d.iz, p_cam), kv.key));
		}
		m.last_missing = (int)missing.size() + plan_wait;
		m.last_plan_wait = plan_wait;
		if (missing.is_empty() || free_slots <= 0) {
			continue;
		}
		// Nearest-first; key breaks distance ties so submission order is
		// deterministic (the GDScript sort left ties unspecified).
		std::sort(missing.ptr(), missing.ptr() + missing.size(),
				[](const Pair<double, int64_t> &a, const Pair<double, int64_t> &b) {
					return a.first != b.first ? a.first < b.first : a.second < b.second;
				});
		int n = MIN(free_slots, (int)missing.size());
		for (int i = 0; i < n; i++) {
			submit((int)li, missing[i].second, p_plan_store);
		}
		submitted += n;
		free_slots -= n;
	}
	return submitted;
}

// WorldStreamer._submit. The layer snapshots its live knobs into the context
// HERE, on the main thread — the worker then reads only the context (the
// threading contract). Low priority is CORRECT (measured in the GDScript
// machine): high_priority oversubscribed the pool with zero settle gain.
void WorldStreamerNative::submit(int p_layer, int64_t p_key, const Variant &p_plan_store) {
	LayerMaps &m = layers[(uint32_t)p_layer];
	const DesiredInfo *d = m.desired.getptr(p_key);
	Object *lo = m.layer.get_validated_object();
	if (d == nullptr || lo == nullptr) {
		return;
	}
	double node_size = root_tile / std::pow(2.0, (double)d->depth);
	Rect2 rect(
			(double)d->tx * root_tile + (double)d->ix * node_size,
			(double)d->tz * root_tile + (double)d->iz * node_size,
			node_size, node_size);
	Ref<WorldStreamJob> job;
	job.instantiate();
	job->layer = m.layer;
	job->key = p_key;
	job->rect = rect;
	job->depth = d->depth;
	job->ctx = lo->call(SNAME("make_context"), p_key, rect, d->depth);
	// Inject the published plan cell (pump guarantees it exists before this
	// leaf was submittable). Frozen snapshot, shared by reference.
	Object *ps = p_plan_store.get_validated_object();
	if (ps != nullptr) {
		Variant cell = ps->call(SNAME("cell"), Vector2i(d->tx, d->tz));
		Object *ctxo = job->ctx.get_validated_object();
		if (ctxo != nullptr) {
			ctxo->set(SNAME("plan"), cell);
		}
	}
	job->started_ms = OS::get_singleton()->get_ticks_msec();
	m.jobs.insert(p_key, job);
	job->task_id = WorkerThreadPool::get_singleton()->add_native_task(
			&WorldStreamJob::run_task, job.ptr(), false, "world_leaf");
}

// WorldStreamer._drain_jobs + _store_cache. Bounds/child-error ingestion
// happens HERE, directly off the layer — the drain is the ONE place they are
// learned, and these maps are the state the cut reads (no GDScript mirror).
Dictionary WorldStreamerNative::drain() {
	Array drained;
	Array evicted;
	double worker_ms = 0.0;
	for (uint32_t li = 0; li < layers.size(); li++) {
		LayerMaps &m = layers[li];
		LocalVector<int64_t> done;
		for (const KeyValue<int64_t, Ref<WorldStreamJob>> &kv : m.jobs) {
			if (WorkerThreadPool::get_singleton()->is_task_completed(kv.value->task_id)) {
				done.push_back(kv.key);
			}
		}
		Object *lo = m.layer.get_validated_object();
		for (uint32_t i = 0; i < done.size(); i++) {
			int64_t key = done[i];
			Ref<WorldStreamJob> job = m.jobs[key];
			(void)WorkerThreadPool::get_singleton()->wait_for_task_completion(job->task_id);
			m.jobs.erase(key);
			worker_ms += job->build_ms;
			if (lo != nullptr) {
				lo->call(SNAME("payload_cached"), key, job->payload);
				Vector2 hb = lo->call(SNAME("height_bounds"), key);
				if (!(std::isnan((double)hb.x) || std::isnan((double)hb.y))) {
					m.bounds[key] = Bounds{ (float)hb.x, (float)hb.y };
				}
				double ce = lo->call(SNAME("child_error"), key);
				if (ce >= 0.0) {
					m.child_err[key] = (float)ce;
				}
			}
			if (!m.cache.has(key)) {
				m.cache_order.push_back(key);
			}
			m.cache.insert(key, true);
			Array row;
			row.push_back((int)li);
			row.push_back(key);
			row.push_back(job->payload);
			drained.push_back(row);
			// LRU eviction — don't evict something currently attached or
			// rebuilding. The rotation guard only changes the would-have-hung
			// case (everything resident pinned active with the cache over
			// limit); the GDScript loop would spin forever there.
			int rotations = 0;
			while ((int)m.cache_order.size() > cache_limit && rotations <= (int)m.cache_order.size()) {
				int64_t old = m.cache_order[0];
				m.cache_order.remove_at(0);
				if (m.active.has(old) || m.jobs.has(old)) {
					m.cache_order.push_back(old);
					rotations++;
					continue;
				}
				m.cache.erase(old);
				Array ev;
				ev.push_back((int)li);
				ev.push_back(old);
				evicted.push_back(ev);
				rotations = 0;
			}
		}
	}
	Dictionary out;
	out["drained"] = drained;
	out["evicted"] = evicted;
	out["worker_ms"] = worker_ms;
	return out;
}

void WorldStreamerNative::note_attached(int p_layer, int64_t p_key) {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return;
	}
	LayerMaps &m = layers[(uint32_t)p_layer];
	// The _attach contract: attaching touches the payload's LRU slot.
	int64_t at = m.cache_order.find(p_key);
	if (at >= 0) {
		m.cache_order.remove_at(at);
		m.cache_order.push_back(p_key);
	}
	m.active.insert(p_key, true);
}

void WorldStreamerNative::note_detached(int p_layer, int64_t p_key) {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return;
	}
	layers[(uint32_t)p_layer].active.erase(p_key);
}

int WorldStreamerNative::total_jobs() const {
	int n = 0;
	for (uint32_t li = 0; li < layers.size(); li++) {
		n += (int)layers[li].jobs.size();
	}
	return n;
}

int WorldStreamerNative::job_count(int p_layer) const {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return 0;
	}
	return (int)layers[(uint32_t)p_layer].jobs.size();
}

int WorldStreamerNative::last_missing(int p_layer) const {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return 0;
	}
	return layers[(uint32_t)p_layer].last_missing;
}

int WorldStreamerNative::last_plan_wait(int p_layer) const {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return 0;
	}
	return layers[(uint32_t)p_layer].last_plan_wait;
}

PackedInt64Array WorldStreamerNative::stuck_keys(int p_layer, int p_age_ms) {
	PackedInt64Array out;
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return out;
	}
	LayerMaps &m = layers[(uint32_t)p_layer];
	uint64_t now = OS::get_singleton()->get_ticks_msec();
	for (KeyValue<int64_t, Ref<WorldStreamJob>> &kv : m.jobs) {
		if (kv.value->stuck_warned) {
			continue;
		}
		if (now - kv.value->started_ms > (uint64_t)p_age_ms) {
			kv.value->stuck_warned = true;
			out.push_back(kv.key);
		}
	}
	return out;
}

uint64_t WorldStreamerNative::job_started_ms(int p_layer, int64_t p_key) const {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return 0;
	}
	const Ref<WorldStreamJob> *job = layers[(uint32_t)p_layer].jobs.getptr(p_key);
	return job != nullptr ? (*job)->started_ms : 0;
}

PackedInt64Array WorldStreamerNative::desired_keys(int p_layer) const {
	PackedInt64Array out;
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return out;
	}
	for (const KeyValue<int64_t, DesiredInfo> &kv : layers[(uint32_t)p_layer].desired) {
		out.push_back(kv.key);
	}
	return out;
}

PackedInt64Array WorldStreamerNative::cache_keys(int p_layer) const {
	PackedInt64Array out;
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return out;
	}
	for (const KeyValue<int64_t, bool> &kv : layers[(uint32_t)p_layer].cache) {
		out.push_back(kv.key);
	}
	return out;
}

PackedInt64Array WorldStreamerNative::active_keys(int p_layer) const {
	PackedInt64Array out;
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return out;
	}
	for (const KeyValue<int64_t, bool> &kv : layers[(uint32_t)p_layer].active) {
		out.push_back(kv.key);
	}
	return out;
}

// --- M3: the reconcile planner ------------------------------------------------

// WorldStreamer._all_cached / _desired_ancestor / _has_active_ancestor /
// _collect_desired_under / _collect_active_under — semantics ported exactly;
// see the GDScript for the archaeology (the past-the-render-cut-counts-as-
// covered rule and the pin-the-parent-forever bug it fixes).

bool WorldStreamerNative::collect_desired_under(const LayerMaps &p_m, int p_tx,
		int p_tz, int p_depth, int p_ix, int p_iz, LocalVector<int64_t> &r_out,
		const Vector3 &p_cam) const {
	if (p_depth >= p_m.band_hi) {
		return false;
	}
	double cut = p_m.radius;
	bool full = true;
	for (int cz = 0; cz < 2; cz++) {
		for (int cx = 0; cx < 2; cx++) {
			int cd = p_depth + 1;
			int cix = p_ix * 2 + cx;
			int ciz = p_iz * 2 + cz;
			int64_t ck = make_key(p_tx, p_tz, cd, cix, ciz);
			if (p_m.desired.has(ck)) {
				r_out.push_back(ck);
			} else if (cut > 0.0 && rect_dist(p_tx, p_tz, cd, cix, ciz, p_cam) > cut) {
				continue; // legitimately empty, not missing
			} else if (!collect_desired_under(p_m, p_tx, p_tz, cd, cix, ciz, r_out, p_cam)) {
				full = false;
			}
		}
	}
	return full;
}

void WorldStreamerNative::collect_active_under(const HashMap<int64_t, bool> &p_act,
		int p_band_hi, int p_tx, int p_tz, int p_depth, int p_ix, int p_iz,
		LocalVector<int64_t> &r_out) const {
	if (p_depth >= p_band_hi) {
		return;
	}
	for (int cz = 0; cz < 2; cz++) {
		for (int cx = 0; cx < 2; cx++) {
			int cd = p_depth + 1;
			int cix = p_ix * 2 + cx;
			int ciz = p_iz * 2 + cz;
			int64_t ck = make_key(p_tx, p_tz, cd, cix, ciz);
			if (p_act.has(ck)) {
				r_out.push_back(ck);
			} else {
				collect_active_under(p_act, p_band_hi, p_tx, p_tz, cd, cix, ciz, r_out);
			}
		}
	}
}

Dictionary WorldStreamerNative::reconcile_plan(const Vector3 &p_cam, int p_ctx,
		int p_ctz, int p_tile_radius, int p_max_applies) {
	Array actions;
	bool hungry = false;
	int applied = 0;
	for (uint32_t li = 0; li < layers.size(); li++) {
		LayerMaps &m = layers[li];
		// TEMP overlay — planning never touches the real ledgers. Built by
		// insertion (HashMap is not copy-constructible), which also carries
		// the insertion order the pass-1 walk depends on.
		HashMap<int64_t, bool> act;
		act.reserve(MAX(1u, (uint32_t)m.active.size()));
		for (const KeyValue<int64_t, bool> &kv : m.active) {
			act.insert(kv.key, true);
		}
		// Pass 1 — SPLITs (active leaf replaced by a finer desired subtree) +
		// drop leaves whose tile scrolled out of range. Snapshot the keys the
		// way keys() did: culls/detaches must not disturb the walk.
		LocalVector<int64_t> akeys;
		for (const KeyValue<int64_t, bool> &kv : act) {
			akeys.push_back(kv.key);
		}
		for (uint32_t i = 0; i < akeys.size(); i++) {
			int64_t key = akeys[i];
			int tx, tz, depth, ix, iz;
			decode_key(key, tx, tz, depth, ix, iz);
			if (Math::abs(tx - p_ctx) > p_tile_radius || Math::abs(tz - p_ctz) > p_tile_radius) {
				Array row;
				row.push_back(0); // cull
				row.push_back((int)li);
				row.push_back(key);
				actions.push_back(row);
				act.erase(key);
				continue;
			}
			if (m.desired.has(key)) {
				continue;
			}
			// Walk up for a desired ancestor — that is a MERGE (pass 2's job).
			{
				bool has_anc = false;
				int d = depth - 1;
				int ax = ix >> 1;
				int az = iz >> 1;
				while (d >= 0) {
					if (m.desired.has(make_key(tx, tz, d, ax, az))) {
						has_anc = true;
						break;
					}
					ax >>= 1;
					az >>= 1;
					d -= 1;
				}
				if (has_anc) {
					continue;
				}
			}
			if (applied >= p_max_applies) {
				hungry = true;
				continue;
			}
			LocalVector<int64_t> subs;
			bool full = collect_desired_under(m, tx, tz, depth, ix, iz, subs, p_cam);
			if (full && subs.size() > 0) {
				bool all_cached = true;
				for (uint32_t s = 0; s < subs.size(); s++) {
					if (!m.cache.has(subs[s])) {
						all_cached = false;
						break;
					}
				}
				if (all_cached) {
					PackedInt64Array ps;
					for (uint32_t s = 0; s < subs.size(); s++) {
						ps.push_back(subs[s]);
						act.insert(subs[s], true);
					}
					act.erase(key);
					Array row;
					row.push_back(1); // split
					row.push_back((int)li);
					row.push_back(key);
					row.push_back(ps);
					actions.push_back(row);
					applied += 1;
				}
			}
		}
		// Pass 2 — MERGEs (desired ancestor replaces its active descendants) +
		// FRESH desired leaves attaching into empty area. Desired iterates in
		// insertion (= recursion) order, exactly the authority's dict walk.
		for (const KeyValue<int64_t, DesiredInfo> &kv : m.desired) {
			int64_t dkey = kv.key;
			if (applied >= p_max_applies) {
				hungry = true;
				break;
			}
			if (act.has(dkey) || !m.cache.has(dkey)) {
				continue;
			}
			const DesiredInfo &d = kv.value;
			LocalVector<int64_t> descs;
			collect_active_under(act, m.band_hi, d.tx, d.tz, d.depth, d.ix, d.iz, descs);
			if (descs.size() > 0) {
				PackedInt64Array ps;
				for (uint32_t s = 0; s < descs.size(); s++) {
					ps.push_back(descs[s]);
					act.erase(descs[s]);
				}
				act.insert(dkey, true);
				Array row;
				row.push_back(2); // merge
				row.push_back((int)li);
				row.push_back(dkey);
				row.push_back(ps);
				actions.push_back(row);
				applied += 1;
			} else {
				bool anc_active = false;
				int ad = d.depth - 1;
				int ax = d.ix >> 1;
				int az = d.iz >> 1;
				while (ad >= 0) {
					if (act.has(make_key(d.tx, d.tz, ad, ax, az))) {
						anc_active = true;
						break;
					}
					ax >>= 1;
					az >>= 1;
					ad -= 1;
				}
				if (!anc_active) {
					act.insert(dkey, true);
					Array row;
					row.push_back(3); // fill
					row.push_back((int)li);
					row.push_back(dkey);
					actions.push_back(row);
					applied += 1;
				}
				// else: an active ancestor still covers this leaf — mid-split,
				// waiting on its siblings; attaching now would overlap.
			}
		}
	}
	Dictionary out;
	out["actions"] = actions;
	out["hungry"] = hungry;
	return out;
}
