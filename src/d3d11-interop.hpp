#pragma once

#include <obs-module.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

class D3D11Interop {
public:
    D3D11Interop();
    ~D3D11Interop();

    bool Initialize();
    void Release();

    // Gets the underlying D3D11 device from OBS
    Microsoft::WRL::ComPtr<ID3D11Device> GetDevice() const { return m_device; }
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> GetContext() const { return m_context; }

    // Wraps an OBS texture into a D3D11 Texture2D
    Microsoft::WRL::ComPtr<ID3D11Texture2D> GetD3D11Texture(gs_texture_t* obs_tex);

    // Creates a D3D11Texture2D with D3D11_RESOURCE_MISC_SHARED for CUDA interop
    Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateSharedTexture(uint32_t width, uint32_t height);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
};
