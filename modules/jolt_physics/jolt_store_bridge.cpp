/**************************************************************************/
/*  jolt_store_bridge.cpp                                                 */
/*  FRIDGE FORK: batched readback for the nodeless row store.             */
/**************************************************************************/

#include "jolt_store_bridge.h"

#include "jolt_physics_server_3d.h"
#include "objects/jolt_body_3d.h"
#include "objects/jolt_object_3d.h"
#include "spaces/jolt_space_3d.h"

#include "core/variant/typed_array.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>

JoltStoreBridge *JoltStoreBridge::singleton = nullptr;

void JoltStoreBridge::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_active_bodies", "space"), &JoltStoreBridge::get_active_bodies);
}

JoltStoreBridge::JoltStoreBridge() {
	singleton = this;
}

JoltStoreBridge::~JoltStoreBridge() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

Dictionary JoltStoreBridge::get_active_bodies(RID p_space) const {
	Dictionary out;
	JoltPhysicsServer3D *server = JoltPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, out);
	ERR_FAIL_COND_V_MSG(server->is_on_separate_thread(), out, "JoltStoreBridge needs the physics server on the main thread.");
	JoltSpace3D *space = server->get_space(p_space);
	ERR_FAIL_NULL_V(space, out);

	JPH::BodyIDVector ids;
	space->get_physics_system().GetActiveBodies(JPH::EBodyType::RigidBody, ids);
	const JPH::BodyInterface &iface = space->get_body_iface();

	PackedInt64Array rids;
	PackedFloat32Array xf;
	rids.resize((int)ids.size());
	xf.resize((int)ids.size() * 12);
	int64_t *rw = rids.ptrw();
	float *fw = xf.ptrw();
	int n = 0;
	for (const JPH::BodyID &id : ids) {
		JoltObject3D *obj = reinterpret_cast<JoltObject3D *>(iface.GetUserData(id));
		JoltBody3D *body = obj != nullptr ? obj->as_body() : nullptr;
		if (body == nullptr) {
			continue;
		}
		const Transform3D t = body->get_transform_scaled();
		rw[n] = (int64_t)body->get_rid().get_id();
		float *o = fw + (size_t)n * 12;
		for (int r = 0; r < 3; r++) {
			o[r * 4 + 0] = (float)t.basis.rows[r].x;
			o[r * 4 + 1] = (float)t.basis.rows[r].y;
			o[r * 4 + 2] = (float)t.basis.rows[r].z;
			o[r * 4 + 3] = (float)t.origin[r];
		}
		n++;
	}
	rids.resize(n);
	xf.resize(n * 12);
	out["rids"] = rids;
	out["xf"] = xf;
	return out;
}
