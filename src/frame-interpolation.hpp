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

    bool Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device, uint32_t width, uint32_t height, void** cuda_ptrs, int cuda_pitch, void* cu_ctx);
    void Release();

    void* GetNextInputPointer();
    void* GetNextOutputPointer();
    int GetNextInputIndex();
    int GetNextOutputIndex();
    bool Process(double timestamp);

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

    void* m_cu_ctx = nullptr;
    HMODULE m_nvcuda_dll = nullptr;
    void* m_cuCtxPushCurrent = nullptr;
    void* m_cuCtxPopCurrent = nullptr;
    void* m_cuArrayCreate = nullptr;
    void* m_cuMemcpy2DAsync = nullptr;
    void* m_cuArrayDestroy = nullptr;

    void PushCudaContext();
    void PopCudaContext();

    bool m_enabled = true;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    DXGI_FORMAT m_tex_format = DXGI_FORMAT_NV12;

    void* m_cuda_ptrs[3] = {nullptr, nullptr, nullptr};
    int m_cuda_pitch = 0;
    
    void* m_cu_arrays[4] = {nullptr, nullptr, nullptr, nullptr};
    
    bool m_resources_registered = false;
    uint32_t m_resource_count = 0;
    
    // Statistics
    uint64_t m_process_count = 0;
    uint64_t m_success_count = 0;
    uint64_t m_fail_count = 0;
    
    bool LoadDLL();
};
