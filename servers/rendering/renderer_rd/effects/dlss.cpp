/**************************************************************************/
/*  dlss.cpp                                                              */
/**************************************************************************/
/* PROTOTYPE (fridge fork): see dlss.h.                                    */
/**************************************************************************/

#ifdef DLSS_ENABLED

#include "dlss.h"

#include "core/config/engine.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/safe_refcount.h"
#include "drivers/vulkan/godot_vulkan.h"
#include "servers/rendering/rendering_device.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>

using namespace RendererRD;

namespace {

// Any stable GUID-shaped id is accepted for a custom engine.
const char *NGX_PROJECT_ID = "f41d9e3c-7a52-4c1b-9d0e-6b2a8f5c3e71";

bool ngx_tried = false;
bool ngx_ready = false;
NVSDK_NGX_Parameter *ngx_params = nullptr;

// Written by the main thread, read by the render thread.
SafeNumeric<uint32_t> supported_flag;
SafeNumeric<uint32_t> model_preset;
SafeNumeric<uint32_t> model_preset_generation;

// A released feature may still be referenced by a command buffer the GPU has
// not finished, so releases wait a few frames.
struct PendingRelease {
	NVSDK_NGX_Handle *handle = nullptr;
	uint64_t frame = 0;
};
LocalVector<PendingRelease> pending_releases;
const uint64_t RELEASE_DELAY_FRAMES = 8;

void _flush_releases(bool p_all) {
	uint64_t now = Engine::get_singleton()->get_frames_drawn();
	for (uint32_t i = 0; i < pending_releases.size();) {
		if (p_all || now - pending_releases[i].frame >= RELEASE_DELAY_FRAMES) {
			NVSDK_NGX_VULKAN_ReleaseFeature(pending_releases[i].handle);
			pending_releases.remove_at_unordered(i);
		} else {
			i++;
		}
	}
}

NVSDK_NGX_PerfQuality_Value _quality_for_ratio(float p_ratio) {
	// p_ratio = render height / output height.
	if (p_ratio >= 0.95f) {
		return NVSDK_NGX_PerfQuality_Value_DLAA;
	} else if (p_ratio >= 0.62f) {
		return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	} else if (p_ratio >= 0.54f) {
		return NVSDK_NGX_PerfQuality_Value_Balanced;
	} else if (p_ratio >= 0.42f) {
		return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	}
	return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

NVSDK_NGX_Resource_VK _resource(uint64_t p_image, uint64_t p_view, uint64_t p_format, Size2i p_size, bool p_depth, bool p_writable) {
	VkImageSubresourceRange range = {};
	range.aspectMask = p_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 0;
	range.levelCount = 1;
	range.baseArrayLayer = 0;
	range.layerCount = 1;
	return NVSDK_NGX_Create_ImageView_Resource_VK((VkImageView)p_view, (VkImage)p_image, range, (VkFormat)p_format, p_size.width, p_size.height, p_writable);
}

} // namespace

const char *const *DLSSEffect::get_required_device_extensions(uint32_t *r_count) {
	static const char *const exts[] = {
		"VK_NVX_binary_import",
		"VK_NVX_image_view_handle",
		"VK_KHR_push_descriptor",
	};
	*r_count = 3;
	return exts;
}

bool DLSSEffect::is_available() {
	if (ngx_tried) {
		return ngx_ready;
	}
	ngx_tried = true;

	RD *rd = RD::get_singleton();
	if (rd == nullptr || rd->get_device_api_name() != "Vulkan") {
		print_line("DLSS: not a Vulkan device, unavailable.");
		return false;
	}

	VkInstance instance = (VkInstance)rd->get_driver_resource(RD::DRIVER_RESOURCE_TOPMOST_OBJECT, RID(), 0);
	VkPhysicalDevice physical_device = (VkPhysicalDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE, RID(), 0);
	VkDevice device = (VkDevice)rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);

	// NGX writes its logs here.
	String data_dir = OS::get_singleton()->get_cache_path().path_join("godot_dlss_ngx");
	Char16String data_dir_w = data_dir.replace_char('/', '\\').utf16();

	NVSDK_NGX_Result res = NVSDK_NGX_VULKAN_Init_with_ProjectID(
			NGX_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "4.8-fridge",
			(const wchar_t *)data_dir_w.get_data(),
			instance, physical_device, device,
			vkGetInstanceProcAddr, vkGetDeviceProcAddr);
	if (NVSDK_NGX_FAILED(res)) {
		print_line(vformat("DLSS: NGX init failed (0x%x).", (uint32_t)res));
		return false;
	}

	res = NVSDK_NGX_VULKAN_GetCapabilityParameters(&ngx_params);
	if (NVSDK_NGX_FAILED(res) || ngx_params == nullptr) {
		print_line(vformat("DLSS: no capability parameters (0x%x).", (uint32_t)res));
		NVSDK_NGX_VULKAN_Shutdown1(device);
		return false;
	}

	int supported = 0;
	int needs_driver = 0;
	ngx_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
	ngx_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
	if (!supported) {
		int init_result = 0;
		ngx_params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
		print_line(vformat("DLSS: Super Resolution unavailable (init result 0x%x, needs driver update: %d).", (uint32_t)init_result, needs_driver));
		NVSDK_NGX_VULKAN_DestroyParameters(ngx_params);
		ngx_params = nullptr;
		NVSDK_NGX_VULKAN_Shutdown1(device);
		return false;
	}

	ngx_ready = true;
	supported_flag.set(1);
	print_line("DLSS: NGX ready, Super Resolution available.");
	return true;
}

bool DLSSEffect::is_supported() {
	return supported_flag.get() != 0;
}

void DLSSEffect::set_preset(int p_preset) {
	uint32_t preset = (uint32_t)CLAMP(p_preset, 0, 15);
	if (model_preset.get() != preset) {
		model_preset.set(preset);
		model_preset_generation.increment();
	}
}

int DLSSEffect::get_preset() {
	return (int)model_preset.get();
}

void DLSSEffect::shutdown() {
	if (!ngx_ready) {
		return;
	}
	_flush_releases(true);
	if (ngx_params) {
		NVSDK_NGX_VULKAN_DestroyParameters(ngx_params);
		ngx_params = nullptr;
	}
	VkDevice device = (VkDevice)RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
	NVSDK_NGX_VULKAN_Shutdown1(device);
	ngx_ready = false;
	supported_flag.set(0);
}

DLSSContext::~DLSSContext() {
	if (handle) {
		PendingRelease pr;
		pr.handle = handle;
		pr.frame = Engine::get_singleton()->get_frames_drawn();
		pending_releases.push_back(pr);
		handle = nullptr;
	}
}

DLSSEffect::DLSSEffect() {}

DLSSEffect::~DLSSEffect() {
	shutdown();
}

DLSSContext *DLSSEffect::create_context(Size2i p_input_size, Size2i p_output_size, bool p_auto_exposure) const {
	DLSSContext *ctx = memnew(DLSSContext);
	ctx->input_size = p_input_size;
	ctx->output_size = p_output_size;
	ctx->auto_exposure = p_auto_exposure;
	return ctx;
}

void DLSSEffect::process(DLSSContext *p_ctx, const Params &p_params) {
	RD *rd = RD::get_singleton();

	CallbackArgs *a = args_allocator.alloc();
	a->owner = this;
	a->ctx = p_ctx;
	a->color_image = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.color, 0);
	a->color_view = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_params.color, 0);
	a->color_format = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.color, 0);
	a->depth_image = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.depth, 0);
	a->depth_view = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_params.depth, 0);
	a->depth_format = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.depth, 0);
	a->motion_image = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.velocity, 0);
	a->motion_view = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_params.velocity, 0);
	a->motion_format = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.velocity, 0);
	if (p_params.exposure.is_valid()) {
		a->exposure_image = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.exposure, 0);
		a->exposure_view = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_params.exposure, 0);
		a->exposure_format = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.exposure, 0);
	}
	a->output_image = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.output, 0);
	a->output_view = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_params.output, 0);
	a->output_format = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.output, 0);
	a->internal_size = p_params.internal_size;
	a->target_size = p_params.target_size;
	a->jitter = p_params.jitter;
	a->sharpness = p_params.sharpness;
	a->reset = p_params.reset;

	// The graph transitions these before the callback runs: sampled inputs to
	// SHADER_READ_ONLY, the output to GENERAL, which is what NGX expects.
	RD::CallbackResource res[5];
	uint32_t res_count = 0;
	const RID sampled[4] = { p_params.color, p_params.depth, p_params.velocity, p_params.exposure };
	for (int i = 0; i < 4; i++) {
		if (sampled[i].is_valid()) {
			res[res_count].rid = sampled[i];
			res[res_count].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
			res_count++;
		}
	}
	res[res_count].rid = p_params.output;
	res[res_count].usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE;
	res_count++;
	rd->driver_callback_add((RDD::DriverCallback)DLSSEffect::callback, a, VectorView<RD::CallbackResource>(res, res_count));
}

void DLSSEffect::callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	CallbackArgs *a = p_userdata;
	DLSSContext *ctx = a->ctx;

	// LAYOUT DEPENDENCY: RenderingDeviceDriverVulkan::CommandBufferInfo starts
	// with its VkCommandBuffer.
	VkCommandBuffer cmd = *(VkCommandBuffer *)p_command_buffer.id;

	_flush_releases(false);

	if (!ngx_ready || ctx == nullptr || ctx->failed) {
		CallbackArgs::free(&a);
		return;
	}

	// A new model preset: retire the feature, the block below builds its successor.
	const uint32_t preset_generation = model_preset_generation.get();
	if (ctx->handle != nullptr && ctx->preset_generation != preset_generation) {
		PendingRelease pr;
		pr.handle = ctx->handle;
		pr.frame = Engine::get_singleton()->get_frames_drawn();
		pending_releases.push_back(pr);
		ctx->handle = nullptr;
	}

	if (ctx->handle == nullptr) {
		const uint32_t preset = model_preset.get();
		ctx->preset_generation = preset_generation;
		// One hint per quality tier; the same model is asked for in all of them.
		NVSDK_NGX_Parameter_SetUI(ngx_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
		NVSDK_NGX_Parameter_SetUI(ngx_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
		NVSDK_NGX_Parameter_SetUI(ngx_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
		NVSDK_NGX_Parameter_SetUI(ngx_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
		NVSDK_NGX_Parameter_SetUI(ngx_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset);

		NVSDK_NGX_DLSS_Create_Params create = {};
		create.Feature.InWidth = ctx->input_size.width;
		create.Feature.InHeight = ctx->input_size.height;
		create.Feature.InTargetWidth = ctx->output_size.width;
		create.Feature.InTargetHeight = ctx->output_size.height;
		create.Feature.InPerfQualityValue = _quality_for_ratio(float(ctx->input_size.height) / float(MAX(ctx->output_size.height, 1)));
		create.InFeatureCreateFlags =
				NVSDK_NGX_DLSS_Feature_Flags_IsHDR | // Linear HDR, before the tonemapper.
				NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | // Velocity is at render resolution.
				NVSDK_NGX_DLSS_Feature_Flags_DepthInverted; // Godot is reverse-Z.
		if (ctx->auto_exposure) {
			create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
		}

		NVSDK_NGX_Result res = NGX_VULKAN_CREATE_DLSS_EXT(cmd, 1, 1, &ctx->handle, ngx_params, &create);
		if (NVSDK_NGX_FAILED(res)) {
			ctx->failed = true;
			ctx->handle = nullptr;
			print_line(vformat("DLSS: feature creation failed (0x%x) for %dx%d -> %dx%d.", (uint32_t)res,
					ctx->input_size.width, ctx->input_size.height, ctx->output_size.width, ctx->output_size.height));
			CallbackArgs::free(&a);
			return;
		}
		print_line(vformat("DLSS: feature created %dx%d -> %dx%d (quality %d, preset %d).",
				ctx->input_size.width, ctx->input_size.height, ctx->output_size.width, ctx->output_size.height,
				(int)create.Feature.InPerfQualityValue, (int)preset));
		a->reset = true;
	}

	NVSDK_NGX_Resource_VK color = _resource(a->color_image, a->color_view, a->color_format, a->internal_size, false, false);
	NVSDK_NGX_Resource_VK depth = _resource(a->depth_image, a->depth_view, a->depth_format, a->internal_size, true, false);
	NVSDK_NGX_Resource_VK motion = _resource(a->motion_image, a->motion_view, a->motion_format, a->internal_size, false, false);
	NVSDK_NGX_Resource_VK output = _resource(a->output_image, a->output_view, a->output_format, a->target_size, false, true);
	NVSDK_NGX_Resource_VK exposure = {};
	if (a->exposure_image != 0) {
		exposure = _resource(a->exposure_image, a->exposure_view, a->exposure_format, Size2i(1, 1), false, false);
	}

	NVSDK_NGX_VK_DLSS_Eval_Params eval = {};
	eval.Feature.pInColor = &color;
	eval.Feature.pInOutput = &output;
	eval.Feature.InSharpness = a->sharpness;
	eval.pInDepth = &depth;
	eval.pInMotionVectors = &motion;
	eval.pInExposureTexture = (a->exposure_image != 0) ? &exposure : nullptr;
	eval.InJitterOffsetX = a->jitter.x;
	eval.InJitterOffsetY = a->jitter.y;
	eval.InRenderSubrectDimensions.Width = a->internal_size.width;
	eval.InRenderSubrectDimensions.Height = a->internal_size.height;
	eval.InReset = a->reset ? 1 : 0;
	// Godot velocity is a UV delta; DLSS wants render-resolution pixels.
	eval.InMVScaleX = float(a->internal_size.width);
	eval.InMVScaleY = float(a->internal_size.height);
	eval.InPreExposure = 1.0f;

	NVSDK_NGX_Result res = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, ctx->handle, ngx_params, &eval);
	if (NVSDK_NGX_FAILED(res)) {
		ctx->failed = true;
		print_line(vformat("DLSS: evaluate failed (0x%x).", (uint32_t)res));
	}

	CallbackArgs::free(&a);
}

#endif // DLSS_ENABLED
