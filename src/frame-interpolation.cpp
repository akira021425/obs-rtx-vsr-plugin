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
    if (m_cuCtxPushCurrent && m_cu_ctx) {
        typedef int (__stdcall *PFN_cuCtxPushCurrent)(void*);
        ((PFN_cuCtxPushCurrent)m_cuCtxPushCurrent)(m_cu_ctx);
    }
}

void FrameInterpolation::PopCudaContext() {
    if (m_cuCtxPopCurrent && m_cu_ctx) {
        typedef int (__stdcall *PFN_cuCtxPopCurrent)(void**);
        void* dummy;
        ((PFN_cuCtxPopCurrent)m_cuCtxPopCurrent)(&dummy);
    }
}

bool FrameInterpolation::Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device, uint32_t width, uint32_t height, void** cuda_ptrs, int cuda_pitch, void* cu_ctx)
{
    m_device = d3d11_device;
    m_width = width;
    m_height = height;
    m_cu_ctx = cu_ctx;

    if (!m_nvcuda_dll) {
        m_nvcuda_dll = LoadLibraryA("nvcuda.dll");
        if (m_nvcuda_dll) {
            m_cuCtxPushCurrent = (void*)GetProcAddress(m_nvcuda_dll, "cuCtxPushCurrent_v2");
            if (!m_cuCtxPushCurrent) m_cuCtxPushCurrent = (void*)GetProcAddress(m_nvcuda_dll, "cuCtxPushCurrent");
            m_cuCtxPopCurrent = (void*)GetProcAddress(m_nvcuda_dll, "cuCtxPopCurrent_v2");
            if (!m_cuCtxPopCurrent) m_cuCtxPopCurrent = (void*)GetProcAddress(m_nvcuda_dll, "cuCtxPopCurrent");
            m_cuArrayCreate = (void*)GetProcAddress(m_nvcuda_dll, "cuArrayCreate_v2");
            if (!m_cuArrayCreate) m_cuArrayCreate = (void*)GetProcAddress(m_nvcuda_dll, "cuArrayCreate");
            m_cuMemcpy2DAsync = (void*)GetProcAddress(m_nvcuda_dll, "cuMemcpy2DAsync_v2");
            if (!m_cuMemcpy2DAsync) m_cuMemcpy2DAsync = (void*)GetProcAddress(m_nvcuda_dll, "cuMemcpy2DAsync");
            m_cuArrayDestroy = (void*)GetProcAddress(m_nvcuda_dll, "cuArrayDestroy");
            m_cuCtxSynchronize = (void*)GetProcAddress(m_nvcuda_dll, "cuCtxSynchronize");
        }
    }

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
        
        NvOFFRUCSurfaceFormat surf_fmt = ARGBSurface;
        
        blog(LOG_INFO, "[RTX-VSR] FRUC: Trying config with %d resources", count);
        
        NvOFFRUC_CREATE_PARAM params = {};
        params.uiWidth = width;
        params.uiHeight = height;
        params.pDevice = nullptr;
        params.eResourceType = CudaResource;
        params.eCUDAResourceType = CudaResourceCuArray;
        params.eSurfaceFormat = surf_fmt;

        PushCudaContext();
        NvOFFRUC_STATUS status = m_create(&params, &m_fruc_handle);
        if (status != NvOFFRUC_SUCCESS) {
            PopCudaContext();
            blog(LOG_WARNING, "[RTX-VSR] FRUC: NvOFFRUCCreate failed: status=%d", status);
            continue;
        }
        
        NvOFFRUC_REGISTER_RESOURCE_PARAM reg_param = {};
        
        typedef struct CUDA_ARRAY_DESCRIPTOR_st {
            size_t Width;
            size_t Height;
            int Format;
            unsigned int NumChannels;
        } CUDA_ARRAY_DESCRIPTOR;

        for (int t = 0; t < count; t++) {
            if (!m_cu_arrays[t]) {
                CUDA_ARRAY_DESCRIPTOR desc = {};
                desc.Width = width;
                desc.Height = height; // ARGB is just height
                desc.Format = 0x01; // CU_AD_FORMAT_UNSIGNED_INT8
                desc.NumChannels = 4; // ARGB has 4 channels
                
                typedef int (__stdcall *PFN_cuArrayCreate)(void**, const CUDA_ARRAY_DESCRIPTOR*);
                int res = ((PFN_cuArrayCreate)m_cuArrayCreate)(&m_cu_arrays[t], &desc);
                if (res != 0) {
                    blog(LOG_ERROR, "[RTX-VSR] FRUC: cuArrayCreate failed: %d", res);
                }
            }
            reg_param.pArrResource[t] = m_cu_arrays[t];
            m_cuda_ptrs[t] = cuda_ptrs[t]; // Keep device pointers for memcopy later
        }
        
        reg_param.uiCount = count;
        
        status = m_register(m_fruc_handle, &reg_param);
        PopCudaContext();
        
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
    PushCudaContext();
    if (m_resources_registered && m_unregister && m_fruc_handle) {
        NvOFFRUC_UNREGISTER_RESOURCE_PARAM unreg = {};
        for (uint32_t t = 0; t < m_resource_count; t++) {
            unreg.pArrResource[t] = m_cuda_ptrs[t];
        }
        unreg.uiCount = m_resource_count;
        m_unregister(m_fruc_handle, &unreg);
        m_resources_registered = false;
    }

    for (int t = 0; t < 4; t++) {
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
    PopCudaContext();

    if (m_nvcuda_dll) {
        FreeLibrary(m_nvcuda_dll);
        m_nvcuda_dll = nullptr;
    }
    m_cuCtxPushCurrent = nullptr;
    m_cuCtxPopCurrent = nullptr;

    if (m_fruc_dll) {
        FreeLibrary(m_fruc_dll);
        m_fruc_dll = nullptr;
    }
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

    uint32_t frame_repeated = 0;
    uint32_t out_frame_repeated = 0;

    NvOFFRUC_PROCESS_IN_PARAMS in_params = {};
    in_params.stFrameDataInput.pFrame = in_ptr;
    in_params.stFrameDataInput.nTimeStamp = timestamp;
    in_params.stFrameDataInput.nCuSurfacePitch = 0; // Array doesn't need pitch
    in_params.stFrameDataInput.bHasFrameRepetitionOccurred = (bool*)&frame_repeated;
    in_params.bSkipWarp = 0;
    
    NvOFFRUC_PROCESS_OUT_PARAMS out_params = {};
    out_params.stFrameDataOutput.pFrame = out_ptr;
    out_params.stFrameDataOutput.nCuSurfacePitch = 0;
    out_params.stFrameDataOutput.bHasFrameRepetitionOccurred = (bool*)&out_frame_repeated;

    log_crash_step("FRUC Process: PushContext");
    PushCudaContext();
    
    // Sync before copying to make sure NvCVImage_Transfer is done
    if (m_cuCtxSynchronize) {
        typedef int (__stdcall *PFN_cuCtxSynchronize)();
        ((PFN_cuCtxSynchronize)m_cuCtxSynchronize)();
    }
    
    // Copy input device memory to CUDA array
    typedef struct CUDA_MEMCPY2D_st {
        size_t srcXInBytes, srcY;
        int srcMemoryType;
        const void *srcHost;
        void *srcDevice;
        void *srcArray;
        size_t srcPitch;

        size_t dstXInBytes, dstY;
        int dstMemoryType;
        void *dstHost;
        void *dstDevice;
        void *dstArray;
        size_t dstPitch;

        size_t WidthInBytes;
        size_t Height;
    } CUDA_MEMCPY2D;

    CUDA_MEMCPY2D cpy = {};
    cpy.srcMemoryType = 2; // CU_MEMORYTYPE_DEVICE
    cpy.srcDevice = in_dev_ptr;
    cpy.srcPitch = m_cuda_pitch;
    cpy.dstMemoryType = 3; // CU_MEMORYTYPE_ARRAY
    cpy.dstArray = in_ptr;
    cpy.WidthInBytes = m_width * 4;
    cpy.Height = m_height;
    
    typedef int (__stdcall *PFN_cuMemcpy2DAsync)(const CUDA_MEMCPY2D*, void*);
    int cpy_res = ((PFN_cuMemcpy2DAsync)m_cuMemcpy2DAsync)(&cpy, nullptr);
    if (cpy_res != 0) {
        blog(LOG_ERROR, "[RTX-VSR] FRUC: cuMemcpy2D IN failed: %d", cpy_res);
    }

    log_crash_step("FRUC Process: m_process");
    NvOFFRUC_STATUS status = m_process(m_fruc_handle, &in_params, &out_params);
    
    // Copy output array back to device memory
    if (status == NvOFFRUC_SUCCESS) {
        CUDA_MEMCPY2D cpy_out = {};
        cpy_out.srcMemoryType = 3; // CU_MEMORYTYPE_ARRAY
        cpy_out.srcArray = out_ptr;
        cpy_out.dstMemoryType = 2; // CU_MEMORYTYPE_DEVICE
        cpy_out.dstDevice = out_dev_ptr;
        cpy_out.dstPitch = m_cuda_pitch;
        cpy_out.WidthInBytes = m_width * 4;
        cpy_out.Height = m_height;
        int cpy_out_res = ((PFN_cuMemcpy2DAsync)m_cuMemcpy2DAsync)(&cpy_out, nullptr);
        if (cpy_out_res != 0) {
            blog(LOG_ERROR, "[RTX-VSR] FRUC: cuMemcpy2D OUT failed: %d", cpy_out_res);
        }
        
        if (m_cuCtxSynchronize) {
            typedef int (__stdcall *PFN_cuCtxSynchronize)();
            ((PFN_cuCtxSynchronize)m_cuCtxSynchronize)();
        }
    }
    
    log_crash_step("FRUC Process: PopContext");
    PopCudaContext();

    log_crash_step("FRUC Process: Done");

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
