#include "frame-interpolation.hpp"
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

bool FrameInterpolation::Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device, uint32_t width, uint32_t height)
{
    m_device = d3d11_device;
    m_width = width;
    m_height = height;

    if (!LoadDLL()) return false;

    // We must create the handle AFTER we find a working texture configuration,
    // because NvOFFRUCCreate requires eSurfaceFormat, which depends on the texture format.
    // Wait, NvOFFRUCRegisterResource requires the handle!
    // So we must create the handle for each config we try, or recreate it if the format changes!
    // Actually, let's just try to create it inside the loop.


    // Try with 3 resources first (NvOFFRUC_MIN_RESOURCE=3), then 4
    int resource_counts[] = { 3, 4 };
    
    for (int rc = 0; rc < 2; rc++) {
        int count = resource_counts[rc];
        // Clean up previous attempt
        if (m_fruc_handle) {
            m_destroy(m_fruc_handle);
            m_fruc_handle = nullptr;
        }
        
        NvOFFRUCSurfaceFormat surf_fmt = NV12Surface; // We now ONLY support NV12 for CUDA!
        
        blog(LOG_INFO, "[RTX-VSR] FRUC: Trying config with %d resources", count);
        
        NvOFFRUC_CREATE_PARAM params = {};
        params.uiWidth = width;
        params.uiHeight = height;
        params.pDevice = m_device.Get();
        if (cuda_ptrs) {
            params.eResourceType = CudaResource;
            params.eCUDAResourceType = CudaResourceCuDevicePtr;
        } else {
            params.eResourceType = DirectX11Resource;
        }
        params.eSurfaceFormat = surf_fmt;

        NvOFFRUC_STATUS status = m_create(&params, &m_fruc_handle);
        if (status != NvOFFRUC_SUCCESS) {
            blog(LOG_WARNING, "[RTX-VSR] FRUC: NvOFFRUCCreate failed: status=%d", status);
            continue;
        }
        
        NvOFFRUC_REGISTER_RESOURCE_PARAM reg_param = {};
        
        if (cuda_ptrs) {
            for (int t = 0; t < count; t++) {
                reg_param.pArrResource[t] = cuda_ptrs[t];
                m_cuda_ptrs[t] = cuda_ptrs[t];
            }
            m_cuda_pitch = cuda_pitch;
        } else {
            // Should not reach here because we always pass CUDA pointers now.
            blog(LOG_ERROR, "[RTX-VSR] FRUC: Expected CUDA pointers but got NULL");
            return false;
        }
        
        reg_param.uiCount = count;
        
        status = m_register(m_fruc_handle, &reg_param);
        if (status == NvOFFRUC_SUCCESS) {
            m_resources_registered = true;
            m_resource_count = count;
            m_tex_format = DXGI_FORMAT_NV12;
            blog(LOG_INFO, "[RTX-VSR] FRUC: RegisterResource SUCCEEDED with CUDA, %d resources", count);
            blog(LOG_INFO, "[RTX-VSR] NVIDIA Frame Interpolation initialized (%ux%u, CUDA NV12, %d res)", 
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
        unreg.pArrResource[0] = m_input_tex.Get();
        unreg.pArrResource[1] = m_output_tex.Get();
        if (m_resource_count >= 3 && m_interp_tex) unreg.pArrResource[2] = m_interp_tex.Get();
        unreg.uiCount = m_resource_count;
        m_unregister(m_fruc_handle, &unreg);
        m_resources_registered = false;
    }

    if (m_fruc_handle && m_destroy) {
        m_destroy(m_fruc_handle);
        m_fruc_handle = nullptr;
    }

    if (m_fruc_dll) {
        FreeLibrary(m_fruc_dll);
        m_fruc_dll = nullptr;
    }

    m_input_tex.Reset();
    m_output_tex.Reset();
    m_interp_tex.Reset();
    m_device.Reset();
}

int FrameInterpolation::GetNextInputIndex() {
    if (!m_resources_registered || m_resource_count < 3) return 0;
    return m_process_count % 3;
}

int FrameInterpolation::GetNextOutputIndex() {
    if (!m_resources_registered || m_resource_count < 3) return 1;
    return (m_process_count + 1) % 3;
}

void* FrameInterpolation::GetNextInputPointer() {
    return m_cuda_ptrs[GetNextInputIndex()];
}

void* FrameInterpolation::GetNextOutputPointer() {
    return m_cuda_ptrs[GetNextOutputIndex()];
}

bool FrameInterpolation::Process(double timestamp)
{
    if (!m_fruc_handle || !m_resources_registered) return false;

    void* in_ptr = GetNextInputPointer();
    void* out_ptr = GetNextOutputPointer();

    bool frame_repeated = false;
    bool out_frame_repeated = false;

    NvOFFRUC_PROCESS_IN_PARAMS in_params = {};
    in_params.stFrameDataInput.pFrame = in_ptr;
    in_params.stFrameDataInput.nTimeStamp = timestamp;
    in_params.stFrameDataInput.nCuSurfacePitch = m_cuda_pitch;
    in_params.stFrameDataInput.bHasFrameRepetitionOccurred = &frame_repeated;
    in_params.bSkipWarp = 0;
    
    NvOFFRUC_PROCESS_OUT_PARAMS out_params = {};
    out_params.stFrameDataOutput.pFrame = out_ptr;
    out_params.stFrameDataOutput.nCuSurfacePitch = m_cuda_pitch;
    out_params.stFrameDataOutput.bHasFrameRepetitionOccurred = &out_frame_repeated;

    NvOFFRUC_STATUS status = m_process(m_fruc_handle, &in_params, &out_params);

    m_process_count++;
    if (status == NvOFFRUC_SUCCESS) {
        m_success_count++;
        if (m_process_count <= 3 || m_success_count % 300 == 0) {
            blog(LOG_INFO, "[RTX-VSR] FRUC: Process OK (success=%llu, total=%llu, ts=%.3f, repeated=%d, in=%p, out=%p)",
                 m_success_count, m_process_count, timestamp, out_frame_repeated ? 1 : 0, in_ptr, out_ptr);
        }
        return true;
    }

    m_fail_count++;
    // Log errors sparingly
    if (m_fail_count <= 5 || m_fail_count % 300 == 0) {
        blog(LOG_WARNING, "[RTX-VSR] FRUC: Process failed: status=%d (success=%llu, fail=%llu, total=%llu, ts=%.3f, skipWarp=%d, in=%p, out=%p)",
             status, m_success_count, m_fail_count, m_process_count, timestamp,
             (m_process_count == 1) ? 1 : 0, in_ptr, out_ptr);
    }

    return false;
}
