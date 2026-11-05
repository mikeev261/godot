/**************************************************************************/
/*  d3d12_dxc_bridge.cpp                                                  */
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

#include "d3d12_dxc_bridge.h"

#include <spirv_hlsl.hpp>

// clang-format off
// Order matters: dxcapi.h needs the OLE types (IUnknown, BSTR), which the build's WIN32_LEAN_AND_MEAN leaves out of windows.h.
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <dxcapi.h>
#include <wrl/client.h>
// clang-format on

#include <mutex>

using Microsoft::WRL::ComPtr;

namespace {

// SPIR-V capabilities (spirv.hpp) that send a module down this path.
constexpr uint32_t SPV_OP_CAPABILITY = 17;
constexpr uint32_t SPV_CAPABILITY_RAY_QUERY_KHR = 4472;
constexpr uint32_t SPV_CAPABILITY_RAY_QUERY_PROVISIONAL_KHR = 4471;
constexpr uint32_t SPV_CAPABILITY_RAY_TRAVERSAL_PRIMITIVE_CULLING_KHR = 4478;
constexpr uint32_t SPV_OP_FUNCTION = 54; // Capabilities come before any function.

std::once_flag dxc_once;
HMODULE dxc_module = nullptr;
DxcCreateInstanceProc dxc_create_instance = nullptr;
std::string dxc_version;

void load_dxc() {
	std::call_once(dxc_once, []() {
		dxc_module = LoadLibraryW(L"dxcompiler.dll");
		if (!dxc_module) {
			return;
		}
		dxc_create_instance = (DxcCreateInstanceProc)(void *)GetProcAddress(dxc_module, "DxcCreateInstance");
		if (!dxc_create_instance) {
			FreeLibrary(dxc_module);
			dxc_module = nullptr;
			return;
		}
		ComPtr<IDxcCompiler3> compiler;
		if (SUCCEEDED(dxc_create_instance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf())))) {
			ComPtr<IDxcVersionInfo> info;
			if (SUCCEEDED(compiler.As(&info))) {
				UINT32 major = 0, minor = 0;
				info->GetVersion(&major, &minor);
				dxc_version = std::to_string(major) + "." + std::to_string(minor);
			}
			ComPtr<IDxcVersionInfo2> info2;
			if (SUCCEEDED(compiler.As(&info2))) {
				UINT32 commit_count = 0;
				char *commit_hash = nullptr;
				if (SUCCEEDED(info2->GetCommitInfo(&commit_count, &commit_hash)) && commit_hash) {
					dxc_version += " (" + std::to_string(commit_count) + "-" + commit_hash + ")";
					CoTaskMemFree(commit_hash);
				}
			}
		}
	});
}

} // namespace

bool d3d12_dxc_spirv_needs_bridge(const uint32_t *p_spirv, size_t p_word_count) {
	// Header is 5 words; then instructions, each with (word count << 16) | opcode.
	size_t i = 5;
	while (i < p_word_count) {
		const uint32_t word = p_spirv[i];
		const uint32_t opcode = word & 0xffff;
		const uint32_t count = word >> 16;
		if (count == 0) {
			return false;
		}
		if (opcode == SPV_OP_FUNCTION) {
			return false;
		}
		if (opcode == SPV_OP_CAPABILITY && i + 1 < p_word_count) {
			const uint32_t cap = p_spirv[i + 1];
			if (cap == SPV_CAPABILITY_RAY_QUERY_KHR || cap == SPV_CAPABILITY_RAY_QUERY_PROVISIONAL_KHR || cap == SPV_CAPABILITY_RAY_TRAVERSAL_PRIMITIVE_CULLING_KHR) {
				return true;
			}
		}
		i += count;
	}
	return false;
}

bool d3d12_dxc_is_available() {
	load_dxc();
	return dxc_create_instance != nullptr;
}

std::string d3d12_dxc_get_version() {
	load_dxc();
	return dxc_version;
}

D3D12DxcResult d3d12_dxc_compile(const D3D12DxcRequest &p_request) {
	D3D12DxcResult result;

	if (p_request.stage != 4) {
		result.error = "Only compute shaders can use inline ray tracing on Direct3D 12 for now.";
		return result;
	}

	// 1. SPIR-V -> HLSL.
	try {
		spirv_cross::CompilerHLSL compiler(p_request.spirv, p_request.spirv_word_count);

		spirv_cross::CompilerHLSL::Options hlsl_options;
		hlsl_options.shader_model = p_request.shader_model;
		hlsl_options.enable_16bit_types = false;
		compiler.set_hlsl_options(hlsl_options);

		const spv::ExecutionModel model = spv::ExecutionModelGLCompute;
		for (const D3D12DxcBinding &b : p_request.bindings) {
			spirv_cross::HLSLResourceBinding rb;
			rb.stage = model;
			rb.desc_set = b.set;
			rb.binding = b.binding;
			rb.cbv.register_binding = b.hlsl_register;
			rb.uav.register_binding = b.hlsl_register;
			rb.srv.register_binding = b.hlsl_register;
			rb.sampler.register_binding = b.hlsl_register;
			compiler.add_hlsl_resource_binding(rb);
		}
		{
			spirv_cross::HLSLResourceBinding rb;
			rb.stage = model;
			rb.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
			rb.binding = spirv_cross::ResourceBindingPushConstantBinding;
			rb.cbv.register_binding = p_request.push_constant_register;
			compiler.add_hlsl_resource_binding(rb);
		}

		result.has_specialization_constants = !compiler.get_specialization_constants().empty();
		result.uses_num_workgroups = compiler.remap_num_workgroups_builtin() != 0;
		if (result.uses_num_workgroups) {
			result.error = "gl_NumWorkGroups is not supported in ray-query compute shaders on Direct3D 12; pass the size in a push constant.";
			return result;
		}

		result.hlsl = compiler.compile();
	} catch (const std::exception &e) {
		result.error = std::string("SPIRV-Cross HLSL translation failed: ") + e.what();
		return result;
	}

	// 2. HLSL -> DXIL.
	load_dxc();
	if (!dxc_create_instance) {
		result.error = "dxcompiler.dll was not found next to the executable; it is needed to compile ray-query shaders for Direct3D 12.";
		return result;
	}

	ComPtr<IDxcCompiler3> compiler;
	if (FAILED(dxc_create_instance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.GetAddressOf())))) {
		result.error = "DxcCreateInstance(CLSID_DxcCompiler) failed.";
		return result;
	}

	const std::wstring target = L"cs_" + std::to_wstring(p_request.shader_model / 10) + L"_" + std::to_wstring(p_request.shader_model % 10);
	std::vector<LPCWSTR> args = {
		L"-E",
		L"main",
		L"-T",
		target.c_str(),
		L"-HV",
		L"2021",
		L"-O3",
		L"-Qstrip_debug",
		L"-Qstrip_reflect",
		L"-Zpc", // SPIRV-Cross states matrix majorness explicitly; keep DXC's default consistent with SPIR-V.
	};

	DxcBuffer source = {};
	source.Ptr = result.hlsl.data();
	source.Size = result.hlsl.size();
	source.Encoding = DXC_CP_UTF8;

	ComPtr<IDxcResult> dxc_result;
	HRESULT hr = compiler->Compile(&source, args.data(), (UINT32)args.size(), nullptr, IID_PPV_ARGS(dxc_result.GetAddressOf()));
	if (FAILED(hr) || !dxc_result) {
		result.error = "IDxcCompiler3::Compile failed.";
		return result;
	}

	HRESULT status = E_FAIL;
	dxc_result->GetStatus(&status);

	ComPtr<IDxcBlobUtf8> errors;
	dxc_result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(errors.GetAddressOf()), nullptr);
	if (FAILED(status)) {
		result.error = "DXC failed to compile the translated HLSL";
		if (errors && errors->GetStringLength()) {
			result.error += ":\n" + std::string(errors->GetStringPointer(), errors->GetStringLength());
		}
		return result;
	}

	ComPtr<IDxcBlob> object;
	dxc_result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(object.GetAddressOf()), nullptr);
	if (!object || object->GetBufferSize() == 0) {
		result.error = "DXC returned no DXIL.";
		return result;
	}

	const uint8_t *bytes = (const uint8_t *)object->GetBufferPointer();
	result.dxil.assign(bytes, bytes + object->GetBufferSize());
	result.ok = true;
	return result;
}
