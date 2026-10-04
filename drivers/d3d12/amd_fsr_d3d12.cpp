/**************************************************************************/
/*  amd_fsr_d3d12.cpp                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "amd_fsr_d3d12.h"

#include "thirdparty/amd-ffx/api/include/ffx_api_types.h"
#include "thirdparty/amd-ffx/upscalers/include/ffx_upscale.h"

#include "drivers/d3d12/rendering_device_driver_d3d12.h"

// From ffx_api_dx12.h, whose inline helpers do not build with MinGW's D3D12 headers.
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 FFX_API_MAKE_BACKEND_SUB_ID(FFX_API_BACKEND_ID_DX12, 0x02)
struct ffxCreateBackendDX12Desc {
	ffxCreateContextDescHeader header;
	ID3D12Device *device;
};

// AMD's loader picks the upscaler DLL that sits next to it.
static const wchar_t *FFX_LOADER_DLL = L"amd_fidelityfx_loader_dx12.dll";

static void _ffx_message(uint32_t p_type, const wchar_t *p_message) {
	if (p_type == FFX_API_MESSAGE_TYPE_ERROR) {
		ERR_PRINT("AMD FSR: " + String(p_message));
	} else {
		WARN_PRINT("AMD FSR: " + String(p_message));
	}
}

static uint32_t _ffx_surface_format(RDD::DataFormat p_format) {
	switch (p_format) {
		case RDD::DATA_FORMAT_R16G16B16A16_SFLOAT:
			return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
		case RDD::DATA_FORMAT_R32G32B32A32_SFLOAT:
			return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
		case RDD::DATA_FORMAT_R16G16_SFLOAT:
			return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
		case RDD::DATA_FORMAT_R32G32_SFLOAT:
			return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
		case RDD::DATA_FORMAT_R32_SFLOAT:
		case RDD::DATA_FORMAT_D32_SFLOAT:
		case RDD::DATA_FORMAT_D32_SFLOAT_S8_UINT:
			return FFX_API_SURFACE_FORMAT_R32_FLOAT;
		case RDD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32:
			return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
		case RDD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32:
			return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
		case RDD::DATA_FORMAT_R8G8B8A8_UNORM:
			return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
		case RDD::DATA_FORMAT_R8G8B8A8_SRGB:
			return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
		case RDD::DATA_FORMAT_D24_UNORM_S8_UINT:
			return FFX_API_SURFACE_FORMAT_R32_UINT; // What AMD's own DX12 helper reports for D24S8.
		default:
			return FFX_API_SURFACE_FORMAT_UNKNOWN;
	}
}

// Godot creates most textures with typeless formats, so pass the real one;
// the runtime derives depth views from the resource itself.
static FfxApiResource _ffx_resource(uint64_t p_native, RDD::DataFormat p_format, uint32_t p_state) {
	FfxApiResource resource = {};
	resource.state = p_state;
	ID3D12Resource *d3d_resource = (ID3D12Resource *)p_native;
	if (!d3d_resource) {
		return resource;
	}

#if defined(_MSC_VER) || !defined(_WIN32)
	const D3D12_RESOURCE_DESC desc = d3d_resource->GetDesc();
#else
	D3D12_RESOURCE_DESC desc;
	d3d_resource->GetDesc(&desc);
#endif

	resource.resource = d3d_resource;
	resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
	resource.description.format = _ffx_surface_format(p_format);
	resource.description.width = (uint32_t)desc.Width;
	resource.description.height = desc.Height;
	resource.description.depth = desc.DepthOrArraySize;
	resource.description.mipCount = desc.MipLevels;
	resource.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
	resource.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
	if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
		resource.description.usage |= FFX_API_RESOURCE_USAGE_UAV;
	}
	return resource;
}

bool AMDFSRBackendD3D12::_load() {
	load_attempted = true;

	if (!driver->is_enhanced_barriers_supported()) {
		// Godot only tracks legacy resource states inside its own commands, which
		// the runtime cannot see. Enhanced barriers keep both sides consistent.
		print_verbose("AMD FSR: unavailable, the D3D12 device does not support enhanced barriers.");
		return false;
	}

	HMODULE lib = LoadLibraryExW(FFX_LOADER_DLL, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!lib) {
		print_verbose("AMD FSR: unavailable, amd_fidelityfx_loader_dx12.dll was not found next to the executable.");
		return false;
	}

	fn_create_context = (PfnFfxCreateContext)(void *)GetProcAddress(lib, "ffxCreateContext");
	fn_destroy_context = (PfnFfxDestroyContext)(void *)GetProcAddress(lib, "ffxDestroyContext");
	fn_query = (PfnFfxQuery)(void *)GetProcAddress(lib, "ffxQuery");
	fn_dispatch = (PfnFfxDispatch)(void *)GetProcAddress(lib, "ffxDispatch");
	if (!fn_create_context || !fn_destroy_context || !fn_query || !fn_dispatch) {
		WARN_PRINT("AMD FSR: amd_fidelityfx_loader_dx12.dll is missing FFX API entry points.");
		FreeLibrary(lib);
		return false;
	}

	module = lib;
	return true;
}

bool AMDFSRBackendD3D12::is_available() {
	if (!load_attempted) {
		loaded = _load();
	}
	return loaded;
}

void *AMDFSRBackendD3D12::context_create(const ContextParams &p_params) {
	ERR_FAIL_COND_V(!is_available(), nullptr);

	ffxCreateContextDescUpscaleVersion version_desc = {};
	version_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
	version_desc.version = FFX_UPSCALER_VERSION;

	ffxCreateBackendDX12Desc backend_desc = {};
	backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backend_desc.header.pNext = &version_desc.header;
	backend_desc.device = driver->get_d3d12_device();

	ffxCreateContextDescUpscale desc = {};
	desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	desc.header.pNext = &backend_desc.header;
	// Same inputs as Godot's FSR 2.2: HDR linear color, reverse Z.
	desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
#ifdef DEV_ENABLED
	desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
#endif
	desc.maxRenderSize.width = p_params.render_size.width;
	desc.maxRenderSize.height = p_params.render_size.height;
	desc.maxUpscaleSize.width = p_params.output_size.width;
	desc.maxUpscaleSize.height = p_params.output_size.height;
	desc.fpMessage = _ffx_message;

	ffxContext context = nullptr;
	ffxReturnCode_t result = fn_create_context(&context, &desc.header, nullptr);
	if (result != FFX_API_RETURN_OK || context == nullptr) {
		WARN_PRINT(vformat("AMD FSR: could not create an upscaler context (FFX error %d).", result));
		return nullptr;
	}
	return context;
}

void AMDFSRBackendD3D12::context_free(void *p_context) {
	if (p_context) {
		PendingFree pending;
		pending.context = (ffxContext)p_context;
		pending.frame = frames_drawn;
		pending_free.push_back(pending);
	}
}

String AMDFSRBackendD3D12::context_get_version(void *p_context) {
	ffxContext context = (ffxContext)p_context;
	ffxQueryGetProviderVersion query = {};
	query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
	if (fn_query(&context, &query.header) != FFX_API_RETURN_OK || query.versionName == nullptr) {
		return "unknown";
	}
	return vformat("%s (provider 0x%s)", String::utf8(query.versionName), String::num_uint64(query.versionId, 16));
}

void AMDFSRBackendD3D12::dispatch(RDD::CommandBufferID p_command_buffer, const DispatchParams &p_params) {
	ffxContext context = (ffxContext)p_params.context;

	ffxDispatchDescUpscale desc = {};
	desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	desc.commandList = driver->command_buffer_get_d3d12_list(p_command_buffer);
	// States match the layouts Godot's render graph put these textures in.
	desc.color = _ffx_resource(p_params.color, p_params.color_format, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	desc.depth = _ffx_resource(p_params.depth, p_params.depth_format, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	desc.motionVectors = _ffx_resource(p_params.motion, p_params.motion_format, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	if (p_params.exposure) {
		desc.exposure = _ffx_resource(p_params.exposure, p_params.exposure_format, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	}
	desc.output = _ffx_resource(p_params.output, p_params.output_format, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
	desc.jitterOffset.x = p_params.jitter.x;
	desc.jitterOffset.y = p_params.jitter.y;
	desc.motionVectorScale.x = p_params.motion_vector_scale.x;
	desc.motionVectorScale.y = p_params.motion_vector_scale.y;
	desc.renderSize.width = p_params.render_size.width;
	desc.renderSize.height = p_params.render_size.height;
	desc.upscaleSize.width = p_params.output_size.width;
	desc.upscaleSize.height = p_params.output_size.height;
	desc.enableSharpening = p_params.sharpness > 1e-6f;
	desc.sharpness = p_params.sharpness;
	desc.frameTimeDelta = p_params.delta_time_ms;
	desc.preExposure = 1.0f;
	desc.reset = p_params.reset;
	desc.cameraNear = p_params.z_near;
	desc.cameraFar = p_params.z_far;
	desc.cameraFovAngleVertical = p_params.fovy;
	desc.viewSpaceToMetersFactor = 1.0f;
	desc.flags = 0;

	ffxReturnCode_t result = fn_dispatch(&context, &desc.header);

	// The runtime bound its own descriptor heaps, root signature and pipeline.
	driver->command_buffer_invalidate_bound_state(p_command_buffer);

	ERR_FAIL_COND_MSG(result != FFX_API_RETURN_OK, vformat("AMD FSR: dispatch failed (FFX error %d).", result));
}

void AMDFSRBackendD3D12::begin_frame(uint64_t p_frames_drawn, uint32_t p_frame_count) {
	frames_drawn = p_frames_drawn;
	for (uint32_t i = 0; i < pending_free.size();) {
		if (frames_drawn >= pending_free[i].frame + p_frame_count + 1) {
			fn_destroy_context(&pending_free[i].context, nullptr);
			pending_free.remove_at_unordered(i);
		} else {
			i++;
		}
	}
}

AMDFSRBackendD3D12::AMDFSRBackendD3D12(RenderingDeviceDriverD3D12 *p_driver) {
	driver = p_driver;
}

AMDFSRBackendD3D12::~AMDFSRBackendD3D12() {
	// The device is idle by the time the driver is destroyed.
	for (PendingFree &pending : pending_free) {
		fn_destroy_context(&pending.context, nullptr);
	}
	pending_free.clear();
	if (module) {
		FreeLibrary((HMODULE)module);
	}
}
