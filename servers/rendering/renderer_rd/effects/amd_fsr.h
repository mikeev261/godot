/**************************************************************************/
/*  amd_fsr.h                                                             */
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

#pragma once

#include "core/templates/paged_allocator.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Temporal upscaling through the AMD FidelityFX API (AMD FSR SDK 2.x). The FFX
// runtime picks FSR 4 on supported AMD GPUs and FSR 3.1 everywhere else.
// The runtime is a closed DLL that records into the driver's own command
// buffers, so a rendering driver must provide the backend. Only D3D12 does.
class AMDFSRBackend {
	static AMDFSRBackend *singleton;

public:
	static AMDFSRBackend *get_singleton() { return singleton; }
	static void set_singleton(AMDFSRBackend *p_backend) { singleton = p_backend; }

	struct ContextParams {
		Size2i render_size;
		Size2i output_size;
	};

	struct DispatchParams {
		void *context = nullptr;

		// Native resources (RD::get_driver_resource(DRIVER_RESOURCE_TEXTURE)) and their formats.
		uint64_t color = 0;
		uint64_t depth = 0;
		uint64_t motion = 0;
		uint64_t exposure = 0; // Optional.
		uint64_t output = 0;
		RDD::DataFormat color_format = RDD::DATA_FORMAT_MAX;
		RDD::DataFormat depth_format = RDD::DATA_FORMAT_MAX;
		RDD::DataFormat motion_format = RDD::DATA_FORMAT_MAX;
		RDD::DataFormat exposure_format = RDD::DATA_FORMAT_MAX;
		RDD::DataFormat output_format = RDD::DATA_FORMAT_MAX;

		Size2i render_size;
		Size2i output_size;
		Vector2 jitter; // In render pixels.
		Vector2 motion_vector_scale;
		float sharpness = 0.0f;
		float delta_time_ms = 0.0f;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f; // Radians.
		bool reset = false;
	};

	// Loads the FFX runtime on first call. False if it is missing or unusable.
	virtual bool is_available() = 0;
	virtual void *context_create(const ContextParams &p_params) = 0;
	// Freed once the GPU can no longer be using the context.
	virtual void context_free(void *p_context) = 0;
	// Name of the upscaler the runtime picked for this context, e.g. "4.1.1".
	virtual String context_get_version(void *p_context) = 0;
	// Called from a driver callback while the command buffer is recording.
	virtual void dispatch(RDD::CommandBufferID p_command_buffer, const DispatchParams &p_params) = 0;

	virtual ~AMDFSRBackend() {}
};

struct AMDFSRContext {
	void *handle = nullptr;
	~AMDFSRContext();
};

class AMDFSREffect {
	struct CallbackArgs {
		AMDFSREffect *owner = nullptr;
		AMDFSRBackend::DispatchParams params;
	};

	PagedAllocator<CallbackArgs, true, 16> args_allocator;
	static void callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata);

public:
	static bool is_available();

	// Null if the runtime refused to create a context.
	AMDFSRContext *create_context(Size2i p_render_size, Size2i p_output_size) const;

	struct Params {
		RID color;
		RID depth;
		RID velocity;
		RID exposure;
		RID output;
		Size2i internal_size;
		Size2i target_size;
		Vector2 jitter;
		float sharpness = 0.0f;
		float delta_time = 0.0f; // Seconds.
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f;
		bool reset = false;
	};

	void upscale(AMDFSRContext *p_context, const Params &p_params);
};

} // namespace RendererRD
