#include "nvidia-vsr.hpp"
#include "nvTransferD3D11.h"
#include <obs-module.h>

NvidiaVSR::NvidiaVSR()
{
}

NvidiaVSR::~NvidiaVSR()
{
    Release();
}

bool NvidiaVSR::Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device, uint32_t width, uint32_t height)
{
    m_device = d3d11_device;
    return LoadEffect();
}

void NvidiaVSR::Release()
{
    if (m_effect) {
        NvVFX_DestroyEffect(m_effect);
        m_effect = nullptr;
    }
    m_device.Reset();
}

void NvidiaVSR::SetQuality(int quality)
{
    m_quality = quality;
    if (m_effect) {
        float strength = (float)(m_quality - 1) / 3.0f; // Map 1-4 to 0.0-1.0
        NvVFX_SetF32(m_effect, NVVFX_STRENGTH, strength);
    }
}

void NvidiaVSR::SetArtifactReduction(bool enable)
{
    m_artifact_reduction = enable;
}

bool NvidiaVSR::LoadEffect()
{
    if (m_effect) {
        NvVFX_DestroyEffect(m_effect);
        m_effect = nullptr;
    }

    // Attempt to load SuperRes effect
    NvCV_Status status = NvVFX_CreateEffect(NVVFX_FX_SR_UPSCALE, &m_effect);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create NVVFX_FX_SR_UPSCALE effect (status: %d)", status);
        return false;
    }

    float strength = (float)(m_quality - 1) / 3.0f; // Map 1-4 to 0.0-1.0
    NvVFX_SetF32(m_effect, NVVFX_STRENGTH, strength);
    // Note: D3D11 interop might require CUDA stream or graph settings here
    
    status = NvVFX_Load(m_effect);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to load NVVFX_FX_SR_UPSCALE model (status: %d)", status);
        NvVFX_DestroyEffect(m_effect);
        m_effect = nullptr;
        return false;
    }

    blog(LOG_INFO, "[RTX-VSR] NVIDIA VSR initialized successfully");
    return true;
}

bool NvidiaVSR::Process(Microsoft::WRL::ComPtr<ID3D11Texture2D> src_tex, Microsoft::WRL::ComPtr<ID3D11Texture2D> dst_tex)
{
    if (!m_effect || !src_tex || !dst_tex) return false;

    NvCVImage src_img, dst_img;
    NvCV_Status status;

    status = NvCVImage_InitFromD3D11Texture(&src_img, src_tex.Get());
    if (status != NVCV_SUCCESS) return false;

    status = NvCVImage_InitFromD3D11Texture(&dst_img, dst_tex.Get());
    if (status != NVCV_SUCCESS) return false;

    // Map resources
    NvCVImage_MapResource(&src_img, nullptr);
    NvCVImage_MapResource(&dst_img, nullptr);

    NvVFX_SetImage(m_effect, NVVFX_INPUT_IMAGE, &src_img);
    NvVFX_SetImage(m_effect, NVVFX_OUTPUT_IMAGE, &dst_img);

    status = NvVFX_Run(m_effect, 0);

    // Unmap resources
    NvCVImage_UnmapResource(&src_img, nullptr);
    NvCVImage_UnmapResource(&dst_img, nullptr);

    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] NvVFX_Run failed: %d", status);
        return false;
    }

    return true;
}
