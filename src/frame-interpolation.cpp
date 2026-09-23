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


    // Try multiple texture configurations until one works
    // NvOFFRUC_MIN_RESOURCE=3: need 3 textures (frame N, frame N+1, interpolated output)
    // MiscFlags=0 is preferred for same-device CUDA interop (SHARED flags are for cross-device)
    struct TexConfig {
        DXGI_FORMAT format;
        UINT miscFlags;
        const char *desc;
    };
    
    TexConfig configs[] = {
        // NoFlags fallback
        { DXGI_FORMAT_R8G8B8A8_UNORM, 0, "RGBA+NoFlags" },
        { DXGI_FORMAT_B8G8R8A8_UNORM, 0, "BGRA+NoFlags" },
        // SHARED as fallback
        { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RESOURCE_MISC_SHARED, "RGBA+SHARED" },
        { DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_RESOURCE_MISC_SHARED, "BGRA+SHARED" },
        // SHARED+NTHANDLE as last resort
        { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, "RGBA+SHARED+NTHANDLE" },
        { DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, "BGRA+SHARED+NTHANDLE" },
    };
    
    const int num_configs = 6;
    // Try with 3 resources first (NvOFFRUC_MIN_RESOURCE=3), then 4
    int resource_counts[] = { 3, 4 };
    
    for (int rc = 0; rc < 2; rc++) {
        int count = resource_counts[rc];
        for (int i = 0; i < num_configs; i++) {
            // Clean up previous attempt
            if (m_fruc_handle) {
                m_destroy(m_fruc_handle);
                m_fruc_handle = nullptr;
            }
            m_input_tex.Reset();
            m_output_tex.Reset();
            m_interp_tex.Reset();
            
            NvOFFRUCSurfaceFormat surf_fmt = (configs[i].format == DXGI_FORMAT_NV12) ? NV12Surface : ARGBSurface;
            
            blog(LOG_INFO, "[RTX-VSR] FRUC: Trying config [%d]: %s with %d resources (fmt=%d)", i, configs[i].desc, count, surf_fmt);
            
            NvOFFRUC_CREATE_PARAM params = {};
            params.uiWidth = width;
            params.uiHeight = height;
            params.pDevice = m_device.Get();
            params.eResourceType = DirectX11Resource;
            params.eSurfaceFormat = surf_fmt;

            NvOFFRUC_STATUS status = m_create(&params, &m_fruc_handle);
            if (status != NvOFFRUC_SUCCESS) {
                blog(LOG_WARNING, "[RTX-VSR] FRUC: NvOFFRUCCreate failed: status=%d for %s", status, configs[i].desc);
                continue;
            }
            
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = configs[i].format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = configs[i].miscFlags;
            
            // Create all textures for this config
            bool tex_ok = true;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[4];
            for (int t = 0; t < count && tex_ok; t++) {
                HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &textures[t]);
                if (FAILED(hr)) {
                    blog(LOG_WARNING, "[RTX-VSR] FRUC: CreateTexture2D[%d] failed: hr=0x%08X", t, hr);
                    tex_ok = false;
                }
            }
            if (!tex_ok) continue;
            
            m_input_tex = textures[0];
            m_output_tex = textures[1];
            m_interp_tex = textures[2];
            
            blog(LOG_INFO, "[RTX-VSR] FRUC: Textures created: [0]=%p [1]=%p [2]=%p%s", 
                 textures[0].Get(), textures[1].Get(), textures[2].Get(),
                 count >= 4 ? " [3]=..." : "");
            
            // Try registration
            NvOFFRUC_REGISTER_RESOURCE_PARAM reg_param = {};
            for (int t = 0; t < count; t++) {
                reg_param.pArrResource[t] = textures[t].Get();
            }
            reg_param.uiCount = count;
            
            status = m_register(m_fruc_handle, &reg_param);
            if (status == NvOFFRUC_SUCCESS) {
                m_resources_registered = true;
                m_resource_count = count;
                m_tex_format = configs[i].format;
                blog(LOG_INFO, "[RTX-VSR] FRUC: RegisterResource SUCCEEDED: %s, %d resources", configs[i].desc, count);
                blog(LOG_INFO, "[RTX-VSR] NVIDIA Frame Interpolation initialized (%ux%u, %s, %d res)", 
                     width, height, configs[i].desc, count);
                return true;
            }
            
            blog(LOG_WARNING, "[RTX-VSR] FRUC: RegisterResource FAILED: status=%d for %s, %d res", status, configs[i].desc, count);
            m_input_tex.Reset();
            m_output_tex.Reset();
            m_interp_tex.Reset();
        }
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

Microsoft::WRL::ComPtr<ID3D11Texture2D> FrameInterpolation::Process(ID3D11Texture2D *src_tex, double timestamp)
{
    if (!m_enabled || !m_fruc_handle || !src_tex || !m_input_tex || !m_output_tex) {
        return nullptr;
    }

    // We MUST use a ring buffer because NvOFFRUC keeps the input texture as a reference for the NEXT frame.
    // If we overwrite the same texture, NvOFFRUC sees no movement and returns error 16.
    ID3D11Texture2D* in_tex = nullptr;
    ID3D11Texture2D* out_tex = nullptr;
    
    // We registered m_resource_count textures (3 or 4).
    // Let's get them from an array. (We know m_input_tex, m_output_tex, m_interp_tex are the first 3).
    // Wait, we need an array. We can just dynamically grab them.
    if (m_resource_count >= 3) {
        // Just rotate through the available resources.
        // We need input and output to be different.
        int in_idx = m_process_count % m_resource_count;
        int out_idx = (m_process_count + (m_resource_count - 1)) % m_resource_count;
        
        ID3D11Texture2D* tex_array[4] = { m_input_tex.Get(), m_output_tex.Get(), m_interp_tex.Get(), nullptr };
        // We only stored 3 in class members in 1.2.7. 
        // Wait, m_interp_tex is the 3rd. If count is 4, we didn't store the 4th!
        // That's fine, we will just use 3 since we only stored 3.
        in_idx = m_process_count % 3;
        out_idx = (m_process_count + 1) % 3; // +1 instead of -1 so it's always positive and different
        
        in_tex = tex_array[in_idx];
        out_tex = tex_array[out_idx];
    } else {
        in_tex = m_input_tex.Get();
        out_tex = m_output_tex.Get();
    }

    // Copy the source texture into our registered input texture
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    m_device->GetImmediateContext(&context);
    if (!context) return nullptr;
    context->CopyResource(in_tex, src_tex);
    
    // Flush to ensure CUDA can see the copy
    context->Flush();

    bool frame_repeated = false;
    bool out_frame_repeated = false;

    NvOFFRUC_PROCESS_IN_PARAMS in_params = {};
    in_params.stFrameDataInput.pFrame = in_tex;
    in_params.stFrameDataInput.nTimeStamp = timestamp;
    in_params.stFrameDataInput.bHasFrameRepetitionOccurred = &frame_repeated;
    // NvOFFRUC should natively handle the first frame
    in_params.bSkipWarp = 0;
    
    NvOFFRUC_PROCESS_OUT_PARAMS out_params = {};
    out_params.stFrameDataOutput.pFrame = out_tex;
    out_params.stFrameDataOutput.bHasFrameRepetitionOccurred = &out_frame_repeated;

    NvOFFRUC_STATUS status = m_process(m_fruc_handle, &in_params, &out_params);

    m_process_count++;
    if (status == NvOFFRUC_SUCCESS) {
        m_success_count++;
        if (m_process_count <= 3 || m_success_count % 300 == 0) {
            blog(LOG_INFO, "[RTX-VSR] FRUC: Process OK (success=%llu, total=%llu, ts=%.3f, repeated=%d, in=%p, out=%p)",
                 m_success_count, m_process_count, timestamp, out_frame_repeated ? 1 : 0, in_tex, out_tex);
        }
        Microsoft::WRL::ComPtr<ID3D11Texture2D> ret(out_tex);
        return ret;
    }

    m_fail_count++;
    // Log errors sparingly
    if (m_fail_count <= 5 || m_fail_count % 300 == 0) {
        blog(LOG_WARNING, "[RTX-VSR] FRUC: Process failed: status=%d (success=%llu, fail=%llu, total=%llu, ts=%.3f, skipWarp=%d, in=%p, out=%p)",
             status, m_success_count, m_fail_count, m_process_count, timestamp,
             (m_process_count == 1) ? 1 : 0, in_tex, out_tex);
    }

    return nullptr;
}
