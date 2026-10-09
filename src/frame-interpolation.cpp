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

bool FrameInterpolation::Initialize(void* cuda_ctx, uint32_t width, uint32_t height)
{
    m_cu_ctx = cuda_ctx;
    m_width = width;
    m_height = height;

    if (!m_cu_ctx) return false;

    if (!LoadDLL()) return false;

    // Create 3 CUDA device images for FRUC (input 1, input 2, output)
    for (int i = 0; i < 3; i++) {
        m_cu_tex[i] = new NvCVImage();
        memset(m_cu_tex[i], 0, sizeof(NvCVImage));
        NvCV_Status cv_status = NvCVImage_Alloc(m_cu_tex[i], width, height, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 0);
        if (cv_status != NVCV_SUCCESS) {
            blog(LOG_ERROR, "[RTX-VSR] FRUC: Failed to alloc CUDA image %d (status: %d)", i, cv_status);
            Release();
            return false;
        }
    }

    int count = 3;
    if (m_fruc_handle) {
        m_destroy(m_fruc_handle);
        m_fruc_handle = nullptr;
    }
    
    blog(LOG_INFO, "[RTX-VSR] FRUC: Trying config with %d resources (CudaResource, ARGBSurface)", count);
    
    NvOFFRUC_CREATE_PARAM params = {};
    params.uiWidth = width;
    params.uiHeight = height;
    params.pDevice = m_cu_ctx;
    params.eResourceType = CudaResource;
    params.eSurfaceFormat = ARGBSurface;
    params.eCUDAResourceType = CudaResourceCuDevicePtr;

    NvOFFRUC_STATUS status = m_create(&params, &m_fruc_handle);
    if (status != NvOFFRUC_SUCCESS) {
        blog(LOG_WARNING, "[RTX-VSR] FRUC: NvOFFRUCCreate failed: status=%d", status);
        Release();
        return false;
    }
    
    NvOFFRUC_REGISTER_RESOURCE_PARAM reg_param = {};
    for (int t = 0; t < count; t++) {
        reg_param.pArrResource[t] = m_cu_tex[t]->pixels;
    }
    reg_param.uiCount = count;
    reg_param.pD3D11FenceObj = nullptr; // Not used for CUDA
    
    status = m_register(m_fruc_handle, &reg_param);
    if (status == NvOFFRUC_SUCCESS) {
        m_resources_registered = true;
        m_resource_count = count;
        blog(LOG_INFO, "[RTX-VSR] FRUC: RegisterResource SUCCEEDED with CudaResource (ARGB), %d resources", count);
        return true;
    }
    
    blog(LOG_ERROR, "[RTX-VSR] FRUC: RegisterResource FAILED: status=%d for %d res", status, count);
    Release();
    return false;
}

void FrameInterpolation::Release()
{
    if (m_resources_registered && m_unregister && m_fruc_handle) {
        NvOFFRUC_UNREGISTER_RESOURCE_PARAM unreg = {};
        for (uint32_t t = 0; t < m_resource_count; t++) {
            unreg.pArrResource[t] = m_cu_tex[t]->pixels;
        }
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
    
    for (int i = 0; i < 3; i++) {
        if (m_cu_tex[i]) {
            NvCVImage_Dealloc(m_cu_tex[i]);
            delete m_cu_tex[i];
            m_cu_tex[i] = nullptr;
        }
    }

    m_create = nullptr;
    m_register = nullptr;
    m_unregister = nullptr;
    m_process = nullptr;
    m_destroy = nullptr;
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

bool FrameInterpolation::Process(double timestamp, CUstream stream)
{
    if (!m_fruc_handle || !m_resources_registered) return false;

    int in_idx = GetNextInputIndex();
    int out_idx = GetNextOutputIndex();
    bool out_frame_repeated = false;

    // Synchronize the input stream to ensure VSR output is fully written
    if (stream) {
        typedef int (__stdcall *PFN_cuStreamSynchronize)(void*);
        HMODULE nvcuda = GetModuleHandleA("nvcuda.dll");
        if (nvcuda) {
            PFN_cuStreamSynchronize sync = (PFN_cuStreamSynchronize)GetProcAddress(nvcuda, "cuStreamSynchronize");
            if (sync) sync(stream);
        }
    }

    double interval = 1.0;
    double in_timestamp = m_last_timestamp + interval;
    m_last_timestamp = in_timestamp;
    double out_timestamp = in_timestamp - (interval * 0.5);

    NvOFFRUC_PROCESS_IN_PARAMS in_params = {};
    in_params.stFrameDataInput.pFrame = m_cu_tex[in_idx]->pixels;
    in_params.stFrameDataInput.nTimeStamp = in_timestamp;
    in_params.stFrameDataInput.nCuSurfacePitch = m_cu_tex[in_idx]->pitch;
    in_params.stFrameDataInput.bHasFrameRepetitionOccurred = nullptr;
    
    NvOFFRUC_PROCESS_OUT_PARAMS out_params = {};
    out_params.stFrameDataOutput.pFrame = m_cu_tex[out_idx]->pixels;
    out_params.stFrameDataOutput.nTimeStamp = out_timestamp;
    out_params.stFrameDataOutput.nCuSurfacePitch = m_cu_tex[out_idx]->pitch;
    out_params.stFrameDataOutput.bHasFrameRepetitionOccurred = &out_frame_repeated;
    
    m_process_count++;
    
    NvOFFRUC_STATUS status = m_process(m_fruc_handle, &in_params, &out_params);
    if (status == NvOFFRUC_SUCCESS) {
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
