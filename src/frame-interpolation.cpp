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
        params.eSurfaceFormat = ARGBSurface; // BGRA textures, not NV12!

        // DO NOT push any context here. NvOFFRUC creates its own context during m_create.
        NvOFFRUC_STATUS status = m_create(&params, &m_fruc_handle);
        
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
        
        if (status == NvOFFRUC_SUCCESS) {
            m_resources_registered = true;
            m_resource_count = count;
            m_tex_format = DXGI_FORMAT_B8G8R8A8_UNORM;
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
        NvOFFRUC_UNREGISTER_RESOURCE_PARAM unreg = {};
        for (uint32_t t = 0; t < m_resource_count; t++) {
            unreg.pArrResource[t] = m_cuda_ptrs[t];
        }
        unreg.uiCount = m_resource_count;
        m_unregister(m_fruc_handle, &unreg);
        m_resources_registered = false;
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

    if (m_fruc_dll) {
        FreeLibrary(m_fruc_dll);
        m_fruc_dll = nullptr;
    }
    
    if (m_fence_event) {
        CloseHandle(m_fence_event);
        m_fence_event = nullptr;
    }
    m_fence.Reset();
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

static void log_crash_step(const char* step) {
    FILE* f = fopen("C:\\Users\\arai5\\obs_crash_debug.txt", "a");
    if (f) {
        fprintf(f, "%s\n", step);
        fclose(f);
    }
}

bool FrameInterpolation::Process(double timestamp)
{
    log_crash_step("FRUC Process: Start");
    if (!m_fruc_handle || !m_resources_registered) return false;

    int in_idx = GetNextInputIndex();
    int out_idx = GetNextOutputIndex();
    void* in_ptr = m_cu_arrays[in_idx];
    void* out_ptr = m_cu_arrays[out_idx];
    void* in_dev_ptr = m_cuda_ptrs[in_idx];
    void* out_dev_ptr = m_cuda_ptrs[out_idx];

    bool out_frame_repeated = false;

    // For DirectX11Resource, in_dev_ptr and out_dev_ptr hold the ID3D11Texture2D*
    NvOFFRUC_PROCESS_IN_PARAMS in_params = {};
    in_params.stFrameDataInput.pFrame = in_dev_ptr;
    in_params.stFrameDataInput.nTimeStamp = timestamp;
    in_params.stFrameDataInput.nCuSurfacePitch = m_width * 4; // BGRA pitch
    in_params.stFrameDataInput.bHasFrameRepetitionOccurred = nullptr;
    
    if (m_fence) {
        in_params.uSyncWait.FenceWaitValue.uiFenceValueToWaitOn = m_fence_value;
    }
    
    NvOFFRUC_PROCESS_OUT_PARAMS out_params = {};
    out_params.stFrameDataOutput.pFrame = out_dev_ptr;
    out_params.stFrameDataOutput.nTimeStamp = timestamp - (333333.333333 / 2.0); // Output timestamp is exactly halfway between frames
    out_params.stFrameDataOutput.nCuSurfacePitch = m_width * 4; // BGRA pitch
    out_params.stFrameDataOutput.bHasFrameRepetitionOccurred = &out_frame_repeated;

    if (m_fence) {
        m_fence_value++;
        out_params.uSyncSignal.FenceSignalValue.uiFenceValueToSignalOn = m_fence_value;
    }

    log_crash_step("FRUC Process: m_process");
    NvOFFRUC_STATUS status = m_process(m_fruc_handle, &in_params, &out_params);
    
    log_crash_step("FRUC Process: Done");

    m_process_count++;
    if (status == NvOFFRUC_SUCCESS) {
        m_success_count++;
        if (m_process_count <= 3 || m_success_count % 300 == 0) {
            blog(LOG_INFO, "[RTX-VSR] FRUC: Process OK (success=%llu, total=%llu, ts=%.3f, repeated=%d, in=%p, out=%p)",
                 m_success_count, m_process_count, timestamp, out_frame_repeated ? 1 : 0, in_dev_ptr, out_dev_ptr);
        }
        return true;
    }

    m_fail_count++;
    // Log errors sparingly
    if (m_fail_count <= 5 || m_fail_count % 300 == 0) {
        blog(LOG_WARNING, "[RTX-VSR] FRUC: Process failed: status=%d (success=%llu, fail=%llu, total=%llu, ts=%.3f, skipWarp=%d, in=%p, out=%p)",
             status, m_success_count, m_fail_count, m_process_count, timestamp,
             (m_process_count == 1) ? 1 : 0, in_dev_ptr, out_dev_ptr);
    }

    return false;
}
