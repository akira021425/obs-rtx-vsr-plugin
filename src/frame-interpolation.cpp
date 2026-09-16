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

    NvOFFRUC_CREATE_PARAM params = {};
    params.uiWidth = width;
    params.uiHeight = height;
    params.pDevice = m_device.Get();
    params.eResourceType = DirectX11Resource;
    params.eSurfaceFormat = ARGBSurface;

    NvOFFRUC_STATUS status = m_create(&params, &m_fruc_handle);
    if (status != NvOFFRUC_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] NvOFFRUCCreate failed with status %d", status);
        return false;
    }

    // Create an output texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    
    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_output_tex);
    if (FAILED(hr)) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create FRUC output texture");
        return false;
    }

    // Register output texture
    NvOFFRUC_REGISTER_RESOURCE_PARAM reg_param = {};
    reg_param.pArrResource[0] = m_output_tex.Get();
    reg_param.uiCount = 1;
    
    status = m_register(m_fruc_handle, &reg_param);
    if (status != NvOFFRUC_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] NvOFFRUCRegisterResource failed %d", status);
        return false;
    }
    m_output_registered = true;

    blog(LOG_INFO, "[RTX-VSR] NVIDIA Frame Interpolation initialized successfully");
    return true;
}

void FrameInterpolation::Release()
{
    if (m_output_registered && m_unregister) {
        NvOFFRUC_UNREGISTER_RESOURCE_PARAM unreg = {};
        unreg.pArrResource[0] = m_output_tex.Get();
        unreg.uiCount = 1;
        m_unregister(m_fruc_handle, &unreg);
        m_output_registered = false;
    }

    if (m_fruc_handle && m_destroy) {
        m_destroy(m_fruc_handle);
        m_fruc_handle = nullptr;
    }

    if (m_fruc_dll) {
        FreeLibrary(m_fruc_dll);
        m_fruc_dll = nullptr;
    }

    m_output_tex.Reset();
    m_device.Reset();
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> FrameInterpolation::Process(ID3D11Texture2D *src_tex, double timestamp)
{
    if (!m_enabled || !m_fruc_handle || !src_tex) {
        return nullptr;
    }

    // Register input texture (normally we should pool these, but for a simple impl we register and unregister, 
    // or assume the caller manages it. Since OBS might give us different textures, we register it temporarily).
    // Note: for production, a texture pool is better, but this satisfies Phase 8-10 auto-generation.
    NvOFFRUC_REGISTER_RESOURCE_PARAM reg_param = {};
    reg_param.pArrResource[0] = src_tex;
    reg_param.uiCount = 1;
    
    if (m_register(m_fruc_handle, &reg_param) != NvOFFRUC_SUCCESS) {
        return nullptr;
    }

    NvOFFRUC_PROCESS_IN_PARAMS in_params = {};
    in_params.stFrameDataInput.pFrame = src_tex;
    in_params.stFrameDataInput.nTimeStamp = timestamp;
    in_params.bSkipWarp = 0;
    
    NvOFFRUC_PROCESS_OUT_PARAMS out_params = {};
    out_params.stFrameDataOutput.pFrame = m_output_tex.Get();

    NvOFFRUC_STATUS status = m_process(m_fruc_handle, &in_params, &out_params);

    // Unregister input texture
    NvOFFRUC_UNREGISTER_RESOURCE_PARAM unreg_param = {};
    unreg_param.pArrResource[0] = src_tex;
    unreg_param.uiCount = 1;
    m_unregister(m_fruc_handle, &unreg_param);

    if (status == NvOFFRUC_SUCCESS) {
        // We have a new interpolated frame!
        return m_output_tex;
    }

    return nullptr;
}
