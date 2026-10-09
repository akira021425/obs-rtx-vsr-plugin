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

    bool Initialize(void* cuda_ctx, uint32_t width, uint32_t height);
    void Release();

    int GetNextInputIndex();
    int GetNextOutputIndex();
    NvCVImage* GetCudaImage(int index) { return m_cu_tex[index]; }
    bool Process(double timestamp, CUstream stream);

    void SetEnabled(bool enable) { m_enabled = enable; }
    bool IsEnabled() const { return m_enabled; }
    bool IsInitialized() const { return m_fruc_handle != nullptr && m_resources_registered; }
    
private:
    void* m_cu_ctx = nullptr;
    
    NvCVImage* m_cu_tex[3] = {nullptr, nullptr, nullptr};
    
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

    bool m_resources_registered = false;
    uint32_t m_resource_count = 0;
    double m_last_timestamp = 0.0;
    
    // Statistics
    uint64_t m_process_count = 0;
    uint64_t m_success_count = 0;
    uint64_t m_fail_count = 0;
    uint64_t m_interp_count = 0;   // FRUC produced a real interpolated frame
    uint64_t m_repeat_count = 0;   // FRUC fell back to frame repetition
    
    bool LoadDLL();

public:
    uint64_t GetInterpCount() const { return m_interp_count; }
    uint64_t GetRepeatCount() const { return m_repeat_count; }
};

