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

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
};
