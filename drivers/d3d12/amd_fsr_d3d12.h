/**************************************************************************/
/*  amd_fsr_d3d12.h                                                       */
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

#include "thirdparty/amd-ffx/api/include/ffx_api.h"

#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/effects/amd_fsr.h"

class RenderingDeviceDriverD3D12;

// AMD FSR backend on D3D12: loads AMD's signed FidelityFX API DLLs at runtime
// and records the upscaler into Godot's own command list. Godot's barrier
// tracking stays valid because the runtime restores every resource state.
class AMDFSRBackendD3D12 : public RendererRD::AMDFSRBackend {
	RenderingDeviceDriverD3D12 *driver = nullptr;

	void *module = nullptr;
	bool load_attempted = false;
	bool loaded = false;

	PfnFfxCreateContext fn_create_context = nullptr;
	PfnFfxDestroyContext fn_destroy_context = nullptr;
	PfnFfxQuery fn_query = nullptr;
	PfnFfxDispatch fn_dispatch = nullptr;

	struct PendingFree {
		ffxContext context = nullptr;
		uint64_t frame = 0;
	};
	LocalVector<PendingFree> pending_free;
	uint64_t frames_drawn = 0;

	bool _load();

public:
	virtual bool is_available() override;
	virtual void *context_create(const ContextParams &p_params) override;
	virtual void context_free(void *p_context) override;
	virtual String context_get_version(void *p_context) override;
	virtual void dispatch(RDD::CommandBufferID p_command_buffer, const DispatchParams &p_params) override;

	// Called by the driver once the GPU has finished with the oldest frame in flight.
	void begin_frame(uint64_t p_frames_drawn, uint32_t p_frame_count);

	AMDFSRBackendD3D12(RenderingDeviceDriverD3D12 *p_driver);
	virtual ~AMDFSRBackendD3D12();
};
