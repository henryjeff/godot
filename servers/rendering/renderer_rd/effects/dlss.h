/**************************************************************************/
/*  dlss.h                                                                */
/**************************************************************************/
/* PROTOTYPE (fridge fork): NVIDIA DLSS Super Resolution through NGX, as a */
/* temporal upscaler in the same slot as FSR2 / MetalFX Temporal.          */
/* Vulkan + Windows only. Built when thirdparty/dlss is present.           */
/**************************************************************************/

#pragma once

#ifdef DLSS_ENABLED

#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/templates/paged_allocator.h"
#include "core/templates/rid.h"
#include "servers/rendering/rendering_device_driver.h"

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace RendererRD {

// One per render-buffer set. The NGX feature is created lazily inside the
// driver callback, because creation records into a live command buffer.
struct DLSSContext {
	NVSDK_NGX_Handle *handle = nullptr;
	Size2i input_size;
	Size2i output_size;
	bool auto_exposure = true;
	bool failed = false;
	uint32_t preset_generation = 0; // The model preset this feature was created under.
	~DLSSContext();
};

class DLSSEffect {
	struct CallbackArgs {
		DLSSEffect *owner = nullptr;
		DLSSContext *ctx = nullptr;
		// Raw Vulkan handles, read on the render thread when the command was recorded.
		uint64_t color_image = 0, color_view = 0, color_format = 0;
		uint64_t depth_image = 0, depth_view = 0, depth_format = 0;
		uint64_t motion_image = 0, motion_view = 0, motion_format = 0;
		uint64_t exposure_image = 0, exposure_view = 0, exposure_format = 0;
		uint64_t output_image = 0, output_view = 0, output_format = 0;
		Size2i internal_size;
		Size2i target_size;
		Vector2 jitter; // Pixels at render resolution, FSR2 convention.
		float sharpness = 0.0f;
		bool reset = false;

		static void free(CallbackArgs **p_args) {
			(*p_args)->owner->args_allocator.free(*p_args);
			*p_args = nullptr;
		}
	};

	PagedAllocator<CallbackArgs, true, 16> args_allocator;

	static void callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata);

public:
	// Initialises NGX on first call (render thread, device must exist).
	static bool is_available();
	static void shutdown();
	// Any thread. False until is_available() has run on the render thread.
	static bool is_supported();
	// NVSDK_NGX_DLSS_Hint_Render_Preset value (0 = NGX default). Any thread;
	// live features are rebuilt on their next frame.
	static void set_preset(int p_preset);
	static int get_preset();
	// Device extensions NGX needs enabled at VkDevice creation.
	static const char *const *get_required_device_extensions(uint32_t *r_count);

	DLSSContext *create_context(Size2i p_input_size, Size2i p_output_size, bool p_auto_exposure) const;

	struct Params {
		RID color;
		RID depth;
		RID velocity; // Must hold camera motion for static pixels too (MotionVectorsStore).
		RID exposure;
		RID output;
		Size2i internal_size;
		Size2i target_size;
		Vector2 jitter;
		float sharpness = 0.0f;
		bool reset = false;
	};

	void process(DLSSContext *p_ctx, const Params &p_params);

	DLSSEffect();
	~DLSSEffect();
};

} //namespace RendererRD

#endif // DLSS_ENABLED
