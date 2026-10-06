#pragma once

#include <obs-module.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include "NvOFFRUC.h"
#include <windows.h>
#include "nvCVImage.h"

class FrameInterpolation {
public:
    FrameInterpolation();
    ~FrameInterpolation();

    bool Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device, uint32_t width, uint32_t height);
    void Release();

    int GetNextInputIndex();
    int GetNextOutputIndex();
    ID3D11Texture2D* GetTexture(int index) { return m_tex[index].Get(); }
    bool Process(double timestamp);

    void SetEnabled(bool enable) { m_enabled = enable; }
    bool IsEnabled() const { return m_enabled; }
    bool IsInitialized() const { return m_fruc_handle != nullptr && m_resources_registered; }
    
    // Returns the DXGI format that FRUC textures use (determined during auto-discovery)
    DXGI_FORMAT GetTextureFormat() const { return m_tex_format; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_tex[3];
    
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

    // D3D11 fence for synchronization
    Microsoft::WRL::ComPtr<ID3D11Device5> m_device5;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context4;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_fence;
    HANDLE m_fence_event = nullptr;
    uint64_t m_fence_value = 0;
    
    bool m_resources_registered = false;
    uint32_t m_resource_count = 0;
    double m_last_timestamp = 0.0;
    
    // Statistics
    uint64_t m_process_count = 0;
    uint64_t m_success_count = 0;
    uint64_t m_fail_count = 0;
    uint64_t m_interp_count = 0;   // FRUC produced a real interpolated frame
    uint64_t m_repeat_count = 0;   // FRUC fell back to frame repetition

    // Diagnostics: tiny staging texture to read back center pixels of FRUC textures
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_diag_stage;
    uint64_t DiagSum(ID3D11Texture2D* tex);
    
    bool LoadDLL();

public:
    uint64_t GetInterpCount() const { return m_interp_count; }
    uint64_t GetRepeatCount() const { return m_repeat_count; }
};

