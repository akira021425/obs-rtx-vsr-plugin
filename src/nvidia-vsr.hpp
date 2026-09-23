#pragma once

#include <obs-module.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "nvVideoEffects.h"
#include "nvCVImage.h"
#include <string>

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
    bool ConvertColorspace(ID3D11Texture2D *src_tex, ID3D11Texture2D *dst_tex);

    void SetQuality(int quality); // 1: Low, 2: Medium, 3: High, 4: Ultra
    void SetArtifactReduction(bool enable);

    bool IsReady() const { return m_ready; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    
    NvVFX_Handle m_effect = nullptr;
    CUstream m_stream = nullptr;
    
    // Persistent NvCVImage objects (heap-allocated, reused every frame)
    NvCVImage *m_src_img = nullptr;  // Wraps source D3D11 texture
    NvCVImage *m_dst_img = nullptr;  // Wraps destination D3D11 texture
    NvCVImage *m_src_gpu = nullptr;  // GPU staging buffer for SDK input (RGBA)
    NvCVImage *m_dst_gpu = nullptr;
    NvCVImage *m_dst_bgra_gpu = nullptr;  // GPU staging buffer for SDK output (RGBA)
    
    ID3D11Texture2D *m_last_src_tex = nullptr;
    ID3D11Texture2D *m_last_dst_tex = nullptr;
    
    uint32_t m_src_width = 0;
    uint32_t m_src_height = 0;
    uint32_t m_dst_width = 0;
    uint32_t m_dst_height = 0;
    
    int m_quality = 4;
    bool m_artifact_reduction = false;
    bool m_ready = false;
    bool m_images_bound = false;
};
