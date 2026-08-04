/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*  fridge fork module: world_stream (the WorldStreamer machine, native). */
/**************************************************************************/

#include "register_types.h"

#include "core/object/class_db.h"

#include "world_streamer_native.h"

void initialize_world_stream_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(WorldStreamerNative);
}

void uninitialize_world_stream_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}
