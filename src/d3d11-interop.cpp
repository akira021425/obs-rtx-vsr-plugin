#include "d3d11-interop.hpp"
#include <graphics/graphics.h>

D3D11Interop::D3D11Interop()
{
}

D3D11Interop::~D3D11Interop()
{
    Release();
}

bool D3D11Interop::Initialize()
{
    // Attempt to get the D3D11 device
    // NOTE: This assumes OBS is using the Direct3D 11 renderer.
    ID3D11Device *d3d11_dev = (ID3D11Device *)gs_get_device_obj();
    if (!d3d11_dev) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to get D3D11 device from OBS. Is OBS using D3D11?");
        return false;
    }

    m_device = d3d11_dev;
    m_device->GetImmediateContext(&m_context);

    blog(LOG_INFO, "[RTX-VSR] D3D11 Interop initialized successfully.");
    return true;
}

void D3D11Interop::Release()
{
    m_context.Reset();
    m_device.Reset();
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> D3D11Interop::GetD3D11Texture(gs_texture_t* obs_tex)
{
    if (!obs_tex) return nullptr;

    ID3D11Texture2D *d3d11_tex = (ID3D11Texture2D *)gs_texture_get_obj(obs_tex);
    if (!d3d11_tex) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_ptr(d3d11_tex);
    return tex_ptr;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> D3D11Interop::CreateSharedTexture(uint32_t width, uint32_t height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &tex);
    if (FAILED(hr)) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create shared D3D11 texture (hr: 0x%X)", hr);
        return nullptr;
    }

    return tex;
}
