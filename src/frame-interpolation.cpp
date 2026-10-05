#include "frame-interpolation.hpp"
#include <stdio.h>
#include <obs-module.h>

FrameInterpolation::FrameInterpolation()
{
}

FrameInterpolation::~FrameInterpolation()
{
    Release();
}

bool FrameInterpolation::LoadDLL()
{
    m_fruc_dll = LoadLibraryA("NvOFFRUC.dll");
    if (!m_fruc_dll) {
        blog(LOG_WARNING, "[RTX-VSR] NvOFFRUC.dll not found. Frame interpolation will be disabled.");
        return false;
    }
    blog(LOG_INFO, "[RTX-VSR] NvOFFRUC.dll loaded successfully");

    m_create = (PtrToFuncNvOFFRUCCreate)GetProcAddress(m_fruc_dll, "NvOFFRUCCreate");
    m_register = (PtrToFuncNvOFFRUCRegisterResource)GetProcAddress(m_fruc_dll, "NvOFFRUCRegisterResource");
    m_unregister = (PtrToFuncNvOFFRUCUnregisterResource)GetProcAddress(m_fruc_dll, "NvOFFRUCUnregisterResource");
    m_process = (PtrToFuncNvOFFRUCProcess)GetProcAddress(m_fruc_dll, "NvOFFRUCProcess");
    m_destroy = (PtrToFuncNvOFFRUCDestroy)GetProcAddress(m_fruc_dll, "NvOFFRUCDestroy");

    if (!m_create || !m_register || !m_unregister || !m_process || !m_destroy) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to get NvOFFRUC function pointers");
        FreeLibrary(m_fruc_dll);
        m_fruc_dll = nullptr;
        return false;
    }
    return true;
}

void FrameInterpolation::PushCudaContext() {
    if (m_cuCtxSetCurrent && m_fruc_ctx) {
        typedef int (__stdcall *PFN_cuCtxSetCurrent)(void*);
        ((PFN_cuCtxSetCurrent)m_cuCtxSetCurrent)(m_fruc_ctx);
    }
}

void FrameInterpolation::PopCudaContext() {
    if (m_cuCtxSetCurrent) {
        typedef int (__stdcall *PFN_cuCtxSetCurrent)(void*);
        ((PFN_cuCtxSetCurrent)m_cuCtxSetCurrent)(nullptr);
    }
}

bool FrameInterpolation::Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device, uint32_t width, uint32_t height, ID3D11Texture2D** d3d11_textures, void* cu_ctx)
{
    m_device = d3d11_device;
    m_width = width;
    m_height = height;
    m_cu_ctx = cu_ctx;

    if (!m_nvcuda_dll) {
        m_nvcuda_dll = LoadLibraryA("nvcuda.dll");
        if (m_nvcuda_dll) {
            m_cuCtxGetCurrent = (void*)GetProcAddress(m_nvcuda_dll, "cuCtxGetCurrent");
            m_cuCtxSetCurrent = (void*)GetProcAddress(m_nvcuda_dll, "cuCtxSetCurrent");
            m_cuCtxSynchronize = (void*)GetProcAddress(m_nvcuda_dll, "cuCtxSynchronize");
        }
    }

    if (!LoadDLL()) return false;

    // Initialize D3D11 fence for synchronization (required by NvOFFRUC for DirectX11Resource)
    HRESULT hr = d3d11_device.As(&m_device5);
    if (SUCCEEDED(hr) && m_device5) {
        hr = m_device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence));
        if (SUCCEEDED(hr)) {
            m_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        } else {
            blog(LOG_WARNING, "[RTX-VSR] FRUC: Failed to create D3D11 Fence (hr: 0x%X)", hr);
        }
    } else {
        blog(LOG_WARNING, "[RTX-VSR] FRUC: Failed to get ID3D11Device5 for Fence (hr: 0x%X)", hr);
    }

    int count = 3;
    {
        if (m_fruc_handle) {
            m_destroy(m_fruc_handle);
            m_fruc_handle = nullptr;
        }
        
        blog(LOG_INFO, "[RTX-VSR] FRUC: Trying config with %d resources (DirectX11Resource, ARGBSurface)", count);
        
        NvOFFRUC_CREATE_PARAM params = {};
        params.uiWidth = width;
        params.uiHeight = height;
        params.pDevice = d3d11_device.Get();
        params.eResourceType = DirectX11Resource;
        params.eCUDAResourceType = CudaResourceCuDevicePtr; // Ignored for DX11
        params.eSurfaceFormat = ARGBSurface; // BGRA textures

        // DO NOT push any context here. NvOFFRUC creates its own context during m_create.
        NvOFFRUC_STATUS status = m_create(&params, &m_fruc_handle);
        
        // Capture the context that NvOFFRUC just created and left on the thread!
        if (status == NvOFFRUC_SUCCESS && m_cuCtxGetCurrent) {
            typedef int (__stdcall *PFN_cuCtxGetCurrent)(void**);
            ((PFN_cuCtxGetCurrent)m_cuCtxGetCurrent)(&m_fruc_ctx);
        }
        
        if (status != NvOFFRUC_SUCCESS) {
            blog(LOG_WARNING, "[RTX-VSR] FRUC: NvOFFRUCCreate failed: status=%d", status);
            return false;
        }
        
        NvOFFRUC_REGISTER_RESOURCE_PARAM reg_param = {};
        if (m_fence) {
            reg_param.pD3D11FenceObj = m_fence.Get();
        }
        
        for (int t = 0; t < count; t++) {
            reg_param.pArrResource[t] = d3d11_textures[t];
            m_cuda_ptrs[t] = d3d11_textures[t];
        }
        
        reg_param.uiCount = count;
        
        status = m_register(m_fruc_handle, &reg_param);
        
        // Pop NvOFFRUC's context off the thread so it doesn't break VSR's NvCVImage_Transfer!
        PopCudaContext();
        
        if (status == NvOFFRUC_SUCCESS) {
            m_resources_registered = true;
            m_resource_count = count;
            m_tex_format = DXGI_FORMAT_R8G8B8A8_UNORM;
            blog(LOG_INFO, "[RTX-VSR] FRUC: RegisterResource SUCCEEDED with DirectX11Resource (ARGB), %d resources", count);
            blog(LOG_INFO, "[RTX-VSR] NVIDIA Frame Interpolation initialized (%ux%u, D3D11 BGRA, %d res)", 
                 width, height, count);
            return true;
        }
        
        blog(LOG_WARNING, "[RTX-VSR] FRUC: RegisterResource FAILED: status=%d for %d res", status, count);
    }
    
    blog(LOG_ERROR, "[RTX-VSR] FRUC: All texture configurations failed for RegisterResource");
    Release();
    return false;
}

void FrameInterpolation::Release()
{
    if (m_resources_registered && m_unregister && m_fruc_handle) {
        PushCudaContext();
        NvOFFRUC_UNREGISTER_RESOURCE_PARAM unreg = {};
        for (uint32_t t = 0; t < m_resource_count; t++) {
            unreg.pArrResource[t] = m_cuda_ptrs[t];
        }
        unreg.uiCount = m_resource_count;
        m_unregister(m_fruc_handle, &unreg);
        m_resources_registered = false;
        PopCudaContext();
    }

    for (int t = 0; t < 3; t++) {
        if (m_cu_arrays[t] && m_cuArrayDestroy) {
            typedef int (__stdcall *PFN_cuArrayDestroy)(void*);
            ((PFN_cuArrayDestroy)m_cuArrayDestroy)(m_cu_arrays[t]);
            m_cu_arrays[t] = nullptr;
        }
    }

    if (m_fruc_handle && m_destroy) {
        m_destroy(m_fruc_handle);
        m_fruc_handle = nullptr;
    }

    if (m_nvcuda_dll) {
        FreeLibrary(m_nvcuda_dll);
        m_nvcuda_dll = nullptr;
    }
    m_cuCtxGetCurrent = nullptr;
    m_cuCtxSetCurrent = nullptr;
    m_fruc_ctx = nullptr;

    if (m_fruc_dll) {
        FreeLibrary(m_fruc_dll);
        m_fruc_dll = nullptr;
    }
    
    if (m_fence_event) {
        CloseHandle(m_fence_event);
        m_fence_event = nullptr;
    }
    m_fence.Reset();
    m_diag_stage.Reset();
    m_context4.Reset();
    m_device5.Reset();
    
    m_device.Reset();
}

int FrameInterpolation::GetNextInputIndex() {
    if (!m_resources_registered || m_resource_count < 3) return 1;
    // Input indices: 1 and 2
    return 1 + (m_process_count % 2);
}

int FrameInterpolation::GetNextOutputIndex() {
    // Output index: always 0
    return 0;
}

void* FrameInterpolation::GetNextInputPointer() {
    return m_cuda_ptrs[GetNextInputIndex()];
}

void* FrameInterpolation::GetNextOutputPointer() {
    return m_cuda_ptrs[GetNextOutputIndex()];
}

// Reads back a 16x16 block from the center of a FRUC texture and returns a checksum.
// Used only for a limited number of frames to verify that FRUC receives distinct inputs.
uint64_t FrameInterpolation::DiagSum(ID3D11Texture2D* tex)
{
    if (!tex || !m_device || !m_context4) return 0;
    if (!m_diag_stage) {
        D3D11_TEXTURE2D_DESC src_desc = {};
        tex->GetDesc(&src_desc);
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = 16;
        desc.Height = 16;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = src_desc.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_diag_stage))) return 0;
    }
    D3D11_BOX box;
    box.left = m_width / 2 - 8;
    box.right = m_width / 2 + 8;
    box.top = m_height / 2 - 8;
    box.bottom = m_height / 2 + 8;
    box.front = 0;
    box.back = 1;
    m_context4->CopySubresourceRegion(m_diag_stage.Get(), 0, 0, 0, 0, tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint64_t sum = 0;
    if (SUCCEEDED(m_context4->Map(m_diag_stage.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        for (int y = 0; y < 16; ++y) {
            const uint8_t* row = (const uint8_t*)mapped.pData + y * mapped.RowPitch;
            for (int x = 0; x < 64; ++x) sum += (uint64_t)row[x] * (uint64_t)(x + 1 + y * 64);
        }
        m_context4->Unmap(m_diag_stage.Get(), 0);
    }
    return sum;
}

bool FrameInterpolation::Process(double timestamp)
{
    if (!m_fruc_handle || !m_resources_registered) return false;


    if (!m_context4 && m_device) {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate_ctx;
        m_device->GetImmediateContext(&immediate_ctx);
        if (immediate_ctx) immediate_ctx.As(&m_context4);
    }

    int in_idx = GetNextInputIndex();
    int out_idx = GetNextOutputIndex();
    int prev_idx = (in_idx == 1) ? 2 : 1;
    void* in_dev_ptr = m_cuda_ptrs[in_idx];
    void* out_dev_ptr = m_cuda_ptrs[out_idx];

    // Diagnostics only for the first frames and periodically afterwards
    const bool diag = (m_process_count < 20) || (m_process_count % 600 == 0);
    uint64_t sum_in = 0, sum_prev = 0, sum_out = 0;
    if (diag) {
        sum_in = DiagSum((ID3D11Texture2D*)m_cuda_ptrs[in_idx]);
        sum_prev = DiagSum((ID3D11Texture2D*)m_cuda_ptrs[prev_idx]);
    }

    bool out_frame_repeated = false;

    // First frame initialization logic
    double out_timestamp = timestamp - 166666.0;
    if (m_process_count == 0) {
        out_timestamp = timestamp; // Same as input for first frame
    }

    // For DirectX11Resource, pFrame holds the ID3D11Texture2D*
    NvOFFRUC_PROCESS_IN_PARAMS in_params = {};
    in_params.stFrameDataInput.pFrame = in_dev_ptr;
    in_params.stFrameDataInput.nTimeStamp = timestamp;
    in_params.stFrameDataInput.nCuSurfacePitch = 0;
    in_params.stFrameDataInput.bHasFrameRepetitionOccurred = nullptr;
    in_params.bSkipWarp = (m_process_count == 0) ? 1 : 0; // First frame only initializes state
    
    if (m_context4 && m_fence) {
        // Signal the fence after the input copy, then FLUSH so the copy + signal are
        // actually submitted to the GPU before NvOFFRUC (CUDA) waits on the fence.
        m_fence_value++;
        m_context4->Signal(m_fence.Get(), m_fence_value);
        m_context4->Flush();
        in_params.uSyncWait.FenceWaitValue.uiFenceValueToWaitOn = m_fence_value;
    }
    
    NvOFFRUC_PROCESS_OUT_PARAMS out_params = {};
    out_params.stFrameDataOutput.pFrame = out_dev_ptr;
    out_params.stFrameDataOutput.nTimeStamp = out_timestamp;
    out_params.stFrameDataOutput.nCuSurfacePitch = 0;
    out_params.stFrameDataOutput.bHasFrameRepetitionOccurred = &out_frame_repeated;

    if (m_context4 && m_fence) {
        m_fence_value++;
        out_params.uSyncSignal.FenceSignalValue.uiFenceValueToSignalOn = m_fence_value;
    }

    PushCudaContext();
    NvOFFRUC_STATUS status = m_process(m_fruc_handle, &in_params, &out_params);
    PopCudaContext();
    
    if (m_context4 && m_fence) {
        // GPU-side wait so subsequent D3D11 reads of the output texture happen after FRUC finishes
        m_context4->Wait(m_fence.Get(), m_fence_value);
    }

    m_process_count++;
    if (status == NvOFFRUC_SUCCESS) {
        if (diag) {
            sum_out = DiagSum((ID3D11Texture2D*)out_dev_ptr);
            blog(LOG_INFO, "[RTX-VSR-FRUC-DIAG] n=%llu repeated=%d in[%d]=%llu prev[%d]=%llu out=%llu %s%s",
                 m_process_count, out_frame_repeated ? 1 : 0,
                 in_idx, sum_in, prev_idx, sum_prev, sum_out,
                 (sum_in == sum_prev) ? "IN==PREV " : "",
                 (sum_out == sum_prev) ? "OUT==PREV" : ((sum_out == sum_in) ? "OUT==IN" : "OUT=NEW"));
        }

        if (m_process_count == 1) {
            // First frame: state initialized only, nothing interpolated yet
            return false;
        }

        m_success_count++;
        if (out_frame_repeated) m_repeat_count++;
        else m_interp_count++;

        if (m_success_count % 300 == 0) {
            blog(LOG_INFO, "[RTX-VSR] FRUC: success=%llu interpolated=%llu repeated=%llu",
                 m_success_count, m_interp_count, m_repeat_count);
        }
        return true;
    }

    m_fail_count++;
    if (m_fail_count <= 5 || m_fail_count % 300 == 0) {
        blog(LOG_WARNING, "[RTX-VSR] FRUC: Process failed: status=%d (success=%llu, fail=%llu, total=%llu, ts=%.3f)",
             status, m_success_count, m_fail_count, m_process_count, timestamp);
    }

    return false;
}

