#pragma once

#include <obs-module.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "NvOFFRUC.h"
#include <windows.h>

class FrameInterpolation {
public:
    FrameInterpolation();
    ~FrameInterpolation();

    bool Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device, uint32_t width, uint32_t height);
    void Release();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> Process(ID3D11Texture2D *src_tex, double timestamp);

    void SetEnabled(bool enable) { m_enabled = enable; }
    bool IsEnabled() const { return m_enabled; }
    bool IsInitialized() const { return m_fruc_handle != nullptr && m_resources_registered; }
    
    // Returns the DXGI format that FRUC textures use (determined during auto-discovery)
    DXGI_FORMAT GetTextureFormat() const { return m_tex_format; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    NvOFFRUCHandle m_fruc_handle = nullptr;
    HMODULE m_fruc_dll = nullptr;

    PtrToFuncNvOFFRUCCreate m_create = nullptr;
    PtrToFuncNvOFFRUCRegisterResource m_register = nullptr;
    PtrToFuncNvOFFRUCUnregisterResource m_unregister = nullptr;
    PtrToFuncNvOFFRUCProcess m_process = nullptr;
    PtrToFuncNvOFFRUCDestroy m_destroy = nullptr;

    bool m_enabled = true;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    DXGI_FORMAT m_tex_format = DXGI_FORMAT_R8G8B8A8_UNORM;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_input_tex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_output_tex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_interp_tex;
    bool m_resources_registered = false;
    uint32_t m_resource_count = 0;
    
    // Statistics
    uint64_t m_process_count = 0;
    uint64_t m_success_count = 0;
    uint64_t m_fail_count = 0;
    
    bool LoadDLL();
};
