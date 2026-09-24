#pragma once

#include <obs-module.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "nvVideoEffects.h"
#include "nvCVImage.h"
#include <string>
#include <unordered_map>

class NvidiaVSR {
public:
    NvidiaVSR();
    ~NvidiaVSR();

    bool Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device, 
                     uint32_t src_width, uint32_t src_height,
                     uint32_t dst_width, uint32_t dst_height);
    void Release();

    // Process: call every frame. src_tex must be a D3D11 texture from gs_texture_get_obj.
    // dst_tex must also be a D3D11 texture. Both must match the sizes given at Initialize.
    bool Process(ID3D11Texture2D *src_tex, ID3D11Texture2D *dst_tex);

    // Fast CUDA colorspace conversion (e.g. BGRA <-> NV12)
    bool ConvertColorspaceFrucIn(ID3D11Texture2D *d3d11_dst, int fruc_idx);
    bool ConvertColorspaceFrucOut(int fruc_idx, ID3D11Texture2D *d3d11_dst);

    void** GetFrucCudaPointers() { return m_fruc_cuda_ptrs; }
    int GetFrucCudaPitch() const { return m_fruc_cuda_pitch; }
    NvCVImage* GetFrucNV12Image(int index) { return (index >= 0 && index < 3) ? m_fruc_nv12_gpu[index] : nullptr; }

    void SetQuality(int quality); // 1: Low, 2: Medium, 3: High, 4: Ultra
    void SetArtifactReduction(bool enable);

    bool IsReady() const { return m_ready; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    
    NvVFX_Handle m_effect = nullptr;
    CUstream m_stream = nullptr;
    
    // Persistent NvCVImage objects (heap-allocated, reused every frame)
    NvCVImage *m_src_gpu = nullptr;  // GPU staging buffer for SDK input (RGBA)
    NvCVImage *m_dst_gpu = nullptr;
    NvCVImage *m_dst_bgra_gpu = nullptr;  // GPU staging buffer for SDK output (RGBA)
    
    // FRUC NV12 GPU buffers
    NvCVImage* m_fruc_nv12_gpu[3] = {nullptr, nullptr, nullptr};
    void* m_fruc_cuda_ptrs[3] = {nullptr, nullptr, nullptr};
    int m_fruc_cuda_pitch = 0;
    // Cache for D3D11 wrapper images to avoid duplicate registration
    std::unordered_map<ID3D11Texture2D*, NvCVImage*> m_tex_map;
    NvCVImage* GetOrInitImage(ID3D11Texture2D* tex);
    
    uint32_t m_src_width = 0;
    uint32_t m_src_height = 0;
    uint32_t m_dst_width = 0;
    uint32_t m_dst_height = 0;

    void* m_cu_ctx = nullptr;
    HMODULE m_nvcuda_dll = nullptr;

public:
    void* GetCudaContext() const { return m_cu_ctx; }
    
    int m_quality = 4;
    bool m_artifact_reduction = false;
    bool m_ready = false;
    bool m_images_bound = false;
};
