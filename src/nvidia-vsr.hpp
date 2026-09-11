#pragma once

#include <obs-module.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "nvVideoEffects.h"
#include <string>

class NvidiaVSR {
public:
    NvidiaVSR();
    ~NvidiaVSR();

    bool Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device, uint32_t width, uint32_t height);
    void Release();

    bool Process(Microsoft::WRL::ComPtr<ID3D11Texture2D> src_tex, Microsoft::WRL::ComPtr<ID3D11Texture2D> dst_tex);

    void SetQuality(int quality); // 1: Low, 2: Medium, 3: High, 4: Ultra
    void SetArtifactReduction(bool enable);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    
    NvVFX_Handle m_effect = nullptr;
    int m_quality = 2;
    bool m_artifact_reduction = false;
    
    bool LoadEffect();
};
