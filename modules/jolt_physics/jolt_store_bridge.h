/**************************************************************************/
/*  jolt_store_bridge.h                                                   */
/*  FRIDGE FORK: batched readback for the nodeless row store.             */
/*  Engine singleton "JoltStoreBridge" (reachable from a GDExtension via    */
/*  Engine.get_singleton by name; the physics server itself hides behind   */
/*  PhysicsServer3DWrapMT, so its own bound methods are not callable).     */
/**************************************************************************/

#pragma once

#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"

class JoltStoreBridge : public Object {
	GDCLASS(JoltStoreBridge, Object)

	static JoltStoreBridge *singleton;

protected:
	static void _bind_methods();

public:
	static JoltStoreBridge *get_singleton() { return singleton; }

	// Every awake rigid body in the space, ONE call: {"rids": PackedInt64Array
	// of RID ids, "xf": PackedFloat32Array 12 floats per body (basis rows +
	// origin), "sleeping_count": int}. Main thread only.
	Dictionary get_active_bodies(RID p_space) const;

	JoltStoreBridge();
	~JoltStoreBridge();
};
