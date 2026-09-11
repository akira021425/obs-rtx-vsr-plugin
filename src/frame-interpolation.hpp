#pragma once

#include <obs-module.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "third-party/include/NvOFFRUC.h"
#include <windows.h>

class FrameInterpolation {
public:
    FrameInterpolation();
    ~FrameInterpolation();

    bool Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device, uint32_t width, uint32_t height);
    void Release();

    // Returns an interpolated frame if one was generated, or nullptr if none
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Process(Microsoft::WRL::ComPtr<ID3D11Texture2D> src_tex, double timestamp);

    void SetEnabled(bool enable) { m_enabled = enable; }
    bool IsEnabled() const { return m_enabled; }

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

    // We need to keep track of textures for FRUC output
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_output_tex;
    bool m_output_registered = false;
    
    // We also need to keep track of previous inputs to manage timestamp differences and 60fps generation
    bool LoadDLL();
};
