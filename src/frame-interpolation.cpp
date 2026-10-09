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

bool FrameInterpolation::Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device, uint32_t width, uint32_t height)
{
    m_device = d3d11_device;
    m_width = width;
    m_height = height;

    if (!m_device) return false;
    m_device->GetImmediateContext(&m_context);

    if (FAILED(m_device.As(&m_device5))) {
        blog(LOG_ERROR, "[RTX-VSR] FRUC: Failed to get ID3D11Device5");
        return false;
    }
    
    if (FAILED(m_device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence)))) {
        blog(LOG_ERROR, "[RTX-VSR] FRUC: Failed to create D3D11Fence");
        return false;
    }

    if (!LoadDLL()) return false;

    // Create 3 D3D11 textures for FRUC (input 1, input 2, output)
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = m_tex_format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    for (int i = 0; i < 3; i++) {
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_tex[i]))) {
            blog(LOG_ERROR, "[RTX-VSR] FRUC: Failed to create D3D11 texture %d", i);
            Release();
            return false;
        }
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
        params.pDevice = m_device.Get();
        params.eResourceType = DirectX11Resource;
        params.eSurfaceFormat = ARGBSurface;

        NvOFFRUC_STATUS status = m_create(&params, &m_fruc_handle);
        
        if (status != NvOFFRUC_SUCCESS) {
            blog(LOG_WARNING, "[RTX-VSR] FRUC: NvOFFRUCCreate failed: status=%d", status);
            return false;
        }
        
        NvOFFRUC_REGISTER_RESOURCE_PARAM reg_param = {};
        
        for (int t = 0; t < count; t++) {
            reg_param.pArrResource[t] = m_tex[t].Get();
        }
        
        reg_param.uiCount = count;
        reg_param.pD3D11FenceObj = m_fence.Get();
        
        status = m_register(m_fruc_handle, &reg_param);
        
        if (status == NvOFFRUC_SUCCESS) {
            m_resources_registered = true;
            m_resource_count = count;
            blog(LOG_INFO, "[RTX-VSR] FRUC: RegisterResource SUCCEEDED with DirectX11Resource (ARGB), %d resources", count);
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
            unreg.pArrResource[t] = m_tex[t].Get();
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
    
    if (m_fence_event) {
        CloseHandle(m_fence_event);
        m_fence_event = nullptr;
    }
    m_fence.Reset();
    m_diag_stage.Reset();
    m_context4.Reset();
    m_context.Reset();
    m_device5.Reset();
    
    for (int i = 0; i < 3; i++) {
        m_tex[i].Reset();
    }
    
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



    int in_idx = GetNextInputIndex();
    int out_idx = GetNextOutputIndex();

    bool out_frame_repeated = false;

    // HighFPSViewer timestamp logic
    double interval = 1.0;
    double in_timestamp = m_last_timestamp + interval;
    m_last_timestamp = in_timestamp;
    double out_timestamp = in_timestamp - (interval * 0.5);

    NvOFFRUC_PROCESS_IN_PARAMS in_params = {};
    in_params.stFrameDataInput.pFrame = m_tex[in_idx].Get();
    in_params.stFrameDataInput.nTimeStamp = in_timestamp;
    in_params.stFrameDataInput.nCuSurfacePitch = 0; // NOT USED for DirectX11Resource
    in_params.stFrameDataInput.bHasFrameRepetitionOccurred = nullptr;
    in_params.uSyncWait.FenceWaitValue.uiFenceValueToWaitOn = m_fence_value;
    
    NvOFFRUC_PROCESS_OUT_PARAMS out_params = {};
    out_params.stFrameDataOutput.pFrame = m_tex[out_idx].Get();
    out_params.stFrameDataOutput.nTimeStamp = out_timestamp;
    out_params.stFrameDataOutput.nCuSurfacePitch = 0; // NOT USED for DirectX11Resource
    out_params.stFrameDataOutput.bHasFrameRepetitionOccurred = &out_frame_repeated;
    
    m_fence_value++;
    out_params.uSyncSignal.FenceSignalValue.uiFenceValueToSignalOn = m_fence_value;

    if (m_process_count < 5) {
        HMODULE lib = GetModuleHandleA("nvcuda.dll");
        void* ctx = nullptr;
        if (lib) {
            typedef int (__stdcall *PFN_cuCtxGetCurrent)(void**);
            PFN_cuCtxGetCurrent getCur = (PFN_cuCtxGetCurrent)GetProcAddress(lib, "cuCtxGetCurrent");
            if (getCur) getCur(&ctx);
        }
        FILE* f = fopen("C:\\Users\\arai5\\obs_crash_debug.txt", "a");
        if (f) {
            fprintf(f, "fruc m_process calling (in_tex=%p, out_tex=%p, wait=%llu, sig=%llu, ctx=%p, thread=%lu)\n",
                    in_params.stFrameDataInput.pFrame, out_params.stFrameDataOutput.pFrame,
                    m_fence_value - 1, m_fence_value, ctx, GetCurrentThreadId());
            fclose(f);
        }
    }

    NvOFFRUC_STATUS status = m_process(m_fruc_handle, &in_params, &out_params);
    
    m_process_count++;
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

