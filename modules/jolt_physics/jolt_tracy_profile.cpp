/**************************************************************************/
/*  jolt_tracy_profile.cpp                                                */
/**************************************************************************/
// fridge perf: implements Jolt's ExternalProfileMeasurement (JPH_EXTERNAL_PROFILE, set in SCsub for
// profiler=tracy) as Tracy zones, so the step's internals (broadphase, narrowphase, solver jobs) show
// up on the main and job threads. Zone names are Jolt's own JPH_PROFILE strings; the file is "jolt".
// Off unless FRIDGE_JOLT_ZONES=1: each zone allocates its name, which inflates what it measures.

#if defined(JPH_EXTERNAL_PROFILE)

#include "core/profiling/profiling.h"

#include <tracy/TracyC.h>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Profiler.h>

#include "core/os/os.h"

#include <cstring>

static bool _jolt_zones_on() {
	static const bool on = OS::get_singleton() != nullptr && OS::get_singleton()->get_environment("FRIDGE_JOLT_ZONES") == "1";
	return on;
}

static_assert(sizeof(TracyCZoneCtx) <= 64, "Tracy zone context must fit ExternalProfileMeasurement::mUserData");

JPH_NAMESPACE_BEGIN

ExternalProfileMeasurement::ExternalProfileMeasurement(const char *inName, uint32 inColor) {
	TracyCZoneCtx ctx = {};
	if (_jolt_zones_on() && TracyCIsConnected) {
		const size_t len = strlen(inName);
		const uint64_t srcloc = ___tracy_alloc_srcloc_name(0, "jolt", 4, inName, len, inName, len, inColor);
		ctx = ___tracy_emit_zone_begin_alloc(srcloc, 1);
	}
	memcpy(mUserData, &ctx, sizeof(ctx));
}

ExternalProfileMeasurement::~ExternalProfileMeasurement() {
	TracyCZoneCtx ctx;
	memcpy(&ctx, mUserData, sizeof(ctx));
	if (ctx.active) {
		___tracy_emit_zone_end(ctx);
	}
}

JPH_NAMESPACE_END

#endif // JPH_EXTERNAL_PROFILE
