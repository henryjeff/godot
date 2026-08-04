/**************************************************************************/
/*  world_streamer_native.cpp                                             */
/**************************************************************************/
/*  fridge fork module: world_stream (the WorldStreamer machine, native). */
/**************************************************************************/

#include "world_streamer_native.h"

#include "core/object/class_db.h"

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

void WorldStreamerNative::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure", "root_tile_size"),
			&WorldStreamerNative::configure);
	ClassDB::bind_method(D_METHOD("get_root_tile_size"),
			&WorldStreamerNative::get_root_tile_size);
	ClassDB::bind_method(D_METHOD("add_layer"), &WorldStreamerNative::add_layer);
	ClassDB::bind_method(D_METHOD("clear_layer", "layer"),
			&WorldStreamerNative::clear_layer);
	ClassDB::bind_method(D_METHOD("push_bounds", "layer", "key", "min_y", "max_y"),
			&WorldStreamerNative::push_bounds);
	ClassDB::bind_method(D_METHOD("push_child_error", "layer", "key", "err"),
			&WorldStreamerNative::push_child_error);
	ClassDB::bind_method(D_METHOD("collect", "layer", "tiles", "cam", "cam_true",
								 "px_scale", "err_threshold", "split_factor",
								 "radius", "band_lo", "band_hi"),
			&WorldStreamerNative::collect);
}

void WorldStreamerNative::configure(double p_root_tile_size) {
	root_tile = p_root_tile_size;
}

int WorldStreamerNative::add_layer() {
	layers.push_back(LayerMaps());
	return (int)layers.size() - 1;
}

void WorldStreamerNative::clear_layer(int p_layer) {
	if (p_layer < 0 || p_layer >= (int)layers.size()) {
		return;
	}
	layers[(uint32_t)p_layer].bounds.clear();
	layers[(uint32_t)p_layer].child_err.clear();
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
	}
}

PackedInt64Array WorldStreamerNative::collect(int p_layer, const PackedInt32Array &p_tiles,
		const Vector3 &p_cam, const Vector3 &p_cam_true, double p_px_scale,
		double p_err_threshold, double p_split_factor, double p_radius,
		int p_band_lo, int p_band_hi) {
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
	for (int t = 0; t + 1 < p_tiles.size(); t += 2) {
		collect_node(a, p_tiles[t], p_tiles[t + 1], 0, 0, 0);
	}
	return out_keys;
}
