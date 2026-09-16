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

bool NvidiaVSR::Initialize(Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device,
                           uint32_t src_width, uint32_t src_height,
                           uint32_t dst_width, uint32_t dst_height)
{
    Release(); // Clean up any previous state
    
    m_device = d3d11_device;
    m_src_width = src_width;
    m_src_height = src_height;
    m_dst_width = dst_width;
    m_dst_height = dst_height;

    NvCV_Status status;

    // 1. Create CUDA stream (REQUIRED - official OBS does this)
    status = NvVFX_CudaStreamCreate(&m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create CUDA stream (status: %d)", status);
        return false;
    }

    // 2. Create the Super Resolution effect
    status = NvVFX_CreateEffect(NVVFX_FX_SR_UPSCALE, &m_effect);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create SR_UPSCALE effect (status: %d)", status);
        Release();
        return false;
    }

    // 3. Set CUDA stream on the effect
    status = NvVFX_SetCudaStream(m_effect, NVVFX_CUDA_STREAM, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to set CUDA stream (status: %d)", status);
        Release();
        return false;
    }

    // 4. Set strength (0.0 - 1.0)
    float strength = (float)(m_quality - 1) / 3.0f;
    status = NvVFX_SetF32(m_effect, NVVFX_STRENGTH, strength);
    if (status != NVCV_SUCCESS) {
        blog(LOG_WARNING, "[RTX-VSR] Failed to set strength (status: %d)", status);
    }

    // 5. Create persistent GPU NvCVImage buffers for SDK input/output
    // These are standalone GPU buffers that the SDK reads from / writes to
    status = NvCVImage_Create(src_width, src_height, NVCV_RGB, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1, &m_src_gpu);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create src GPU image (status: %d)", status);
        Release();
        return false;
    }
    status = NvCVImage_Alloc(m_src_gpu, src_width, src_height, NVCV_RGB, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to alloc src GPU image (status: %d)", status);
        Release();
        return false;
    }

    status = NvCVImage_Create(dst_width, dst_height, NVCV_RGB, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1, &m_dst_gpu);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create dst GPU image (status: %d)", status);
        Release();
        return false;
    }
    status = NvCVImage_Alloc(m_dst_gpu, dst_width, dst_height, NVCV_RGB, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to alloc dst GPU image (status: %d)", status);
        Release();
        return false;
    }

    // 6. Wrapper NvCVImage objects for D3D11 textures will be created dynamically in Process()


    // 7. Bind the GPU staging images to the effect (done ONCE, not every frame)
    NvVFX_SetImage(m_effect, NVVFX_INPUT_IMAGE, m_src_gpu);
    NvVFX_SetImage(m_effect, NVVFX_OUTPUT_IMAGE, m_dst_gpu);

    // 8. Load/compile the effect model
    status = NvVFX_Load(m_effect);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to load SR_UPSCALE model (status: %d)", status);
        Release();
        return false;
    }

    m_ready = true;
    blog(LOG_INFO, "[RTX-VSR] NVIDIA VSR initialized: %ux%u -> %ux%u", src_width, src_height, dst_width, dst_height);
    return true;
}

void NvidiaVSR::Release()
{
    m_ready = false;
    m_images_bound = false;
    m_last_src_tex = nullptr;
    m_last_dst_tex = nullptr;

    if (m_src_img) { NvCVImage_Destroy(m_src_img); delete m_src_img; m_src_img = nullptr; }
    if (m_dst_img) { NvCVImage_Destroy(m_dst_img); delete m_dst_img; m_dst_img = nullptr; }
    if (m_src_gpu) { NvCVImage_Destroy(m_src_gpu); m_src_gpu = nullptr; }
    if (m_dst_gpu) { NvCVImage_Destroy(m_dst_gpu); m_dst_gpu = nullptr; }

    if (m_effect) {
        NvVFX_DestroyEffect(m_effect);
        m_effect = nullptr;
    }
    if (m_stream) {
        NvVFX_CudaStreamDestroy(m_stream);
        m_stream = nullptr;
    }
    m_device.Reset();
}

void NvidiaVSR::SetQuality(int quality)
{
    m_quality = quality;
    if (m_effect) {
        float strength = (float)(m_quality - 1) / 3.0f;
        NvVFX_SetF32(m_effect, NVVFX_STRENGTH, strength);
    }
}

void NvidiaVSR::SetArtifactReduction(bool enable)
{
    m_artifact_reduction = enable;
}

bool NvidiaVSR::Process(ID3D11Texture2D *src_tex, ID3D11Texture2D *dst_tex)
{
    if (!m_ready || !m_effect || !src_tex || !dst_tex) return false;

    NvCV_Status status;

    // 1. Bind D3D11 textures to wrapper NvCVImages (only if changed)
    if (m_last_src_tex != src_tex) {
        if (m_src_img) { NvCVImage_Destroy(m_src_img); delete m_src_img; m_src_img = nullptr; }
        m_src_img = new NvCVImage();
        status = NvCVImage_InitFromD3D11Texture(m_src_img, src_tex);
        if (status != NVCV_SUCCESS) {
            blog(LOG_ERROR, "[RTX-VSR] InitFromD3D11Texture(src) failed: %d", status);
            return false;
        }
        m_last_src_tex = src_tex;
    }

    if (m_last_dst_tex != dst_tex) {
        if (m_dst_img) { NvCVImage_Destroy(m_dst_img); delete m_dst_img; m_dst_img = nullptr; }
        m_dst_img = new NvCVImage();
        status = NvCVImage_InitFromD3D11Texture(m_dst_img, dst_tex);
        if (status != NVCV_SUCCESS) {
            blog(LOG_ERROR, "[RTX-VSR] InitFromD3D11Texture(dst) failed: %d", status);
            return false;
        }
        m_last_dst_tex = dst_tex;
    }

    // 2. Map source texture, transfer to GPU staging buffer
    status = NvCVImage_MapResource(m_src_img, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] MapResource(src) failed: %d", status);
        return false;
    }

    status = NvCVImage_Transfer(m_src_img, m_src_gpu, 1.0f, m_stream, NULL);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Transfer src->gpu failed: %d", status);
        NvCVImage_UnmapResource(m_src_img, m_stream);
        return false;
    }

    status = NvCVImage_UnmapResource(m_src_img, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] UnmapResource(src) failed: %d", status);
        return false;
    }

    // 3. Run the AI upscaler
    status = NvVFX_Run(m_effect, 0);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] NvVFX_Run failed: %d", status);
        return false;
    }

    // 4. Map destination texture, transfer result from GPU staging buffer
    status = NvCVImage_MapResource(m_dst_img, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] MapResource(dst) failed: %d", status);
        return false;
    }

    status = NvCVImage_Transfer(m_dst_gpu, m_dst_img, 1.0f, m_stream, NULL);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Transfer gpu->dst failed: %d", status);
        NvCVImage_UnmapResource(m_dst_img, m_stream);
        return false;
    }

    status = NvCVImage_UnmapResource(m_dst_img, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] UnmapResource(dst) failed: %d", status);
        return false;
    }

    return true;
}
