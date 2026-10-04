/**************************************************************************/
/*  amd_fsr.cpp                                                           */
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

#include "amd_fsr.h"

using namespace RendererRD;

AMDFSRBackend *AMDFSRBackend::singleton = nullptr;

static void _add_resource(LocalVector<RD::CallbackResource> &r_resources, RID p_rid, RD::CallbackResourceUsage p_usage) {
	RD::CallbackResource resource;
	resource.rid = p_rid;
	resource.type = RD::CALLBACK_RESOURCE_TYPE_TEXTURE;
	resource.usage = p_usage;
	r_resources.push_back(resource);
}

AMDFSRContext::~AMDFSRContext() {
	if (handle && AMDFSRBackend::get_singleton()) {
		AMDFSRBackend::get_singleton()->context_free(handle);
	}
}

bool AMDFSREffect::is_available() {
	return AMDFSRBackend::get_singleton() && AMDFSRBackend::get_singleton()->is_available();
}

AMDFSRContext *AMDFSREffect::create_context(Size2i p_render_size, Size2i p_output_size) const {
	if (!is_available()) {
		return nullptr;
	}

	AMDFSRBackend *backend = AMDFSRBackend::get_singleton();
	AMDFSRBackend::ContextParams params;
	params.render_size = p_render_size;
	params.output_size = p_output_size;
	void *handle = backend->context_create(params);
	if (!handle) {
		return nullptr;
	}

	print_line(vformat("AMD FSR: upscaler %s, %dx%d to %dx%d.", backend->context_get_version(handle), p_render_size.x, p_render_size.y, p_output_size.x, p_output_size.y));

	AMDFSRContext *context = memnew(AMDFSRContext);
	context->handle = handle;
	return context;
}

void AMDFSREffect::upscale(AMDFSRContext *p_context, const Params &p_params) {
	ERR_FAIL_NULL(p_context);
	RD *rd = RD::get_singleton();

	CallbackArgs *args = args_allocator.alloc();
	args->owner = this;
	AMDFSRBackend::DispatchParams &d = args->params;
	d.context = p_context->handle;
	d.color = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.color);
	d.depth = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.depth);
	d.motion = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.velocity);
	d.output = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.output);
	d.color_format = (RDD::DataFormat)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.color);
	d.depth_format = (RDD::DataFormat)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.depth);
	d.motion_format = (RDD::DataFormat)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.velocity);
	d.output_format = (RDD::DataFormat)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.output);
	if (p_params.exposure.is_valid()) {
		d.exposure = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.exposure);
		d.exposure_format = (RDD::DataFormat)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_params.exposure);
	}
	d.render_size = p_params.internal_size;
	d.output_size = p_params.target_size;
	d.jitter = p_params.jitter;
	// Godot's velocity is in UV units; FFX wants render pixels (same as FSR2).
	d.motion_vector_scale = Vector2(p_params.internal_size);
	d.sharpness = p_params.sharpness;
	d.delta_time_ms = p_params.delta_time * 1000.0f;
	d.z_near = p_params.z_near;
	d.z_far = p_params.z_far;
	d.fovy = p_params.fovy;
	d.reset = p_params.reset;

	// The FFX runtime returns every resource to the state it received it in, so
	// declaring these usages keeps Godot's barrier tracking correct.
	LocalVector<RD::CallbackResource> resources;
	_add_resource(resources, p_params.color, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	_add_resource(resources, p_params.depth, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	_add_resource(resources, p_params.velocity, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	if (p_params.exposure.is_valid()) {
		_add_resource(resources, p_params.exposure, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	}
	_add_resource(resources, p_params.output, RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);

	rd->driver_callback_add((RDD::DriverCallback)AMDFSREffect::callback, args, VectorView<RD::CallbackResource>(resources.ptr(), resources.size()));
}

void AMDFSREffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	AMDFSRBackend *backend = AMDFSRBackend::get_singleton();
	if (backend) {
		backend->dispatch(p_command_buffer, p_userdata->params);
	}
	p_userdata->owner->args_allocator.free(p_userdata);
}
