/**************************************************************************/
/*  d3d12_dxc_bridge.h                                                    */
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

// SPIR-V -> HLSL (SPIRV-Cross) -> DXIL (DXC) for shaders that Mesa's NIR-to-DXIL cannot translate,
// i.e. compute shaders that use inline ray tracing (GL_EXT_ray_query / SPV_KHR_ray_query).
//
// Kept free of Godot headers so that it can be built with C++ exceptions enabled (SPIRV-Cross throws),
// while the rest of the driver keeps them disabled.

#include <cstdint>
#include <string>
#include <vector>

struct D3D12DxcBinding {
	uint32_t set = 0;
	uint32_t binding = 0;
	uint32_t hlsl_register = 0; // Same number for the t, u, b and s register classes; space 0.
};

struct D3D12DxcRequest {
	const uint32_t *spirv = nullptr;
	size_t spirv_word_count = 0;
	// 0 vertex, 1 fragment, 4 compute (RenderingDeviceCommons::ShaderStage). Only compute is supported for now.
	uint32_t stage = 4;
	std::vector<D3D12DxcBinding> bindings;
	uint32_t push_constant_register = 0;
	uint32_t shader_model = 65; // HLSL shader model, e.g. 65 for 6.5 (inline ray tracing needs 6.5).
};

struct D3D12DxcResult {
	bool ok = false;
	std::vector<uint8_t> dxil;
	std::string hlsl; // Kept for diagnostics.
	std::string error;
	bool has_specialization_constants = false;
	bool uses_num_workgroups = false;
};

// True if the SPIR-V module declares a capability that Mesa's SPIR-V to DXIL path does not support
// and that this bridge handles (ray queries).
bool d3d12_dxc_spirv_needs_bridge(const uint32_t *p_spirv, size_t p_word_count);

// Loads dxcompiler.dll (once). Returns false if it is not available next to the executable or on the DLL search path.
bool d3d12_dxc_is_available();
// Version string of the loaded dxcompiler.dll ("" if unavailable).
std::string d3d12_dxc_get_version();

D3D12DxcResult d3d12_dxc_compile(const D3D12DxcRequest &p_request);
