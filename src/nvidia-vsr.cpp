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
    status = NvCVImage_Create(src_width, src_height, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1, &m_src_gpu);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create src GPU image (status: %d)", status);
        Release();
        return false;
    }
    status = NvCVImage_Alloc(m_src_gpu, src_width, src_height, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to alloc src GPU image (status: %d)", status);
        Release();
        return false;
    }

    status = NvCVImage_Create(dst_width, dst_height, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1, &m_dst_gpu);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create dst GPU image (status: %d)", status);
        Release();
        return false;
    }
    status = NvCVImage_Alloc(m_dst_gpu, dst_width, dst_height, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to alloc dst GPU image (status: %d)", status);
        Release();
        return false;
    }

    // Create an intermediate BGRA GPU buffer for safe transfer to mapped D3D11
    status = NvCVImage_Create(dst_width, dst_height, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1, &m_dst_bgra_gpu);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create dst BGRA GPU image (status: %d)", status);
        Release();
        return false;
    }

    // Create FRUC NV12 GPU buffers
    for (int i = 0; i < 3; i++) {
        status = NvCVImage_Create(dst_width, dst_height, NVCV_YUV420, NVCV_U8, NVCV_NV12, NVCV_GPU, 1, &m_fruc_nv12_gpu[i]);
        if (status != NVCV_SUCCESS) {
            blog(LOG_ERROR, "[RTX-VSR] Failed to create FRUC NV12 GPU image %d (status: %d)", i, status);
            Release();
            return false;
        }
        
        // Ensure colorspace is set for YUV transfers as required by NvCVImage_Transfer
        m_fruc_nv12_gpu[i]->colorspace = NVCV_709 | NVCV_VIDEO_RANGE | NVCV_CHROMA_INTSTITIAL;
        m_fruc_cuda_ptrs[i] = m_fruc_nv12_gpu[i]->pixels;
        m_fruc_cuda_pitch = m_fruc_nv12_gpu[i]->pitch;
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

    for (auto& pair : m_tex_map) {
        if (pair.second) {
            NvCVImage_Destroy(pair.second);
            delete pair.second;
        }
    }
    m_tex_map.clear();

    if (m_src_gpu) { NvCVImage_Destroy(m_src_gpu); m_src_gpu = nullptr; }
    if (m_dst_gpu) { NvCVImage_Destroy(m_dst_gpu); m_dst_gpu = nullptr; }
    if (m_dst_bgra_gpu) { NvCVImage_Destroy(m_dst_bgra_gpu); m_dst_bgra_gpu = nullptr; }

    for (int i = 0; i < 3; i++) {
        if (m_fruc_nv12_gpu[i]) {
            NvCVImage_Destroy(m_fruc_nv12_gpu[i]);
            m_fruc_nv12_gpu[i] = nullptr;
        }
        m_fruc_cuda_ptrs[i] = nullptr;
    }
    m_fruc_cuda_pitch = 0;

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

NvCVImage* NvidiaVSR::GetOrInitImage(ID3D11Texture2D* tex) {
    if (!tex) return nullptr;
    auto it = m_tex_map.find(tex);
    if (it != m_tex_map.end()) return it->second;

    NvCVImage* img = new NvCVImage();
    memset(img, 0, sizeof(NvCVImage));
    NvCV_Status status = NvCVImage_InitFromD3D11Texture(img, tex);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] GetOrInitImage: InitFromD3D11Texture failed: %d for tex %p", status, tex);
        delete img;
        return nullptr;
    }
    m_tex_map[tex] = img;
    return img;
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

    // 1. Get or initialize wrapped D3D11 textures
    NvCVImage* src_img = GetOrInitImage(src_tex);
    NvCVImage* dst_img = GetOrInitImage(dst_tex);
    
    if (!src_img || !dst_img) return false;

    // 2. Map source texture, transfer to GPU staging buffer
    status = NvCVImage_MapResource(src_img, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] MapResource(src) failed: %d", status);
        return false;
    }

    status = NvCVImage_Transfer(src_img, m_src_gpu, 1.0f, m_stream, NULL);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Transfer src->gpu failed: %d", status);
        NvCVImage_UnmapResource(src_img, m_stream);
        return false;
    }

    status = NvCVImage_UnmapResource(src_img, m_stream);
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
    status = NvCVImage_MapResource(dst_img, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] MapResource(dst) failed: %d", status);
        return false;
    }

    // 4. Transfer output from SDK GPU buffer back to D3D11 texture
    // We cannot transfer RGBA (m_dst_gpu) directly to BGRA mapped D3D11 (dst_img) as it throws -9.
    // However, CUDA-to-CUDA transfer from RGBA to BGRA works!
    status = NvCVImage_Transfer(m_dst_gpu, m_dst_bgra_gpu, 1.0f, m_stream, NULL);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Transfer gpu->bgra_gpu failed: %d", status);
        NvCVImage_UnmapResource(dst_img, m_stream);
        return false;
    }

    // Now transfer BGRA to BGRA (pure CUDA to mapped D3D11)
    status = NvCVImage_Transfer(m_dst_bgra_gpu, dst_img, 1.0f, m_stream, NULL);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Transfer bgra_gpu->dst failed: %d", status);
        NvCVImage_UnmapResource(dst_img, m_stream);
        return false;
    }

    status = NvCVImage_UnmapResource(dst_img, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] UnmapResource(dst) failed: %d", status);
        return false;
    }

    return true;
}

bool NvidiaVSR::ConvertColorspaceFrucIn(ID3D11Texture2D *d3d11_dst, int fruc_idx)
{
    if (!m_ready || !d3d11_dst || fruc_idx < 0 || fruc_idx >= 3) return false;

    NvCVImage* src_img = GetOrInitImage(d3d11_dst);
    NvCVImage* dst_img = m_fruc_nv12_gpu[fruc_idx];
    if (!src_img || !dst_img) return false;

    NvCV_Status status = NvCVImage_MapResource(src_img, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] ConvertFrucIn: MapResource failed: %d", status);
        return false;
    }
    
    // Transfer from BGRA D3D11 to NV12 CUDA natively
    status = NvCVImage_Transfer(src_img, dst_img, 1.0f, m_stream, nullptr);

    NvCVImage_UnmapResource(src_img, m_stream);

    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] ConvertFrucIn failed during transfer: %d", status);
        return false;
    }
    return true;
}

bool NvidiaVSR::ConvertColorspaceFrucOut(int fruc_idx, ID3D11Texture2D *d3d11_dst)
{
    if (!m_ready || !d3d11_dst || fruc_idx < 0 || fruc_idx >= 3) return false;

    NvCVImage* src_img = m_fruc_nv12_gpu[fruc_idx];
    NvCVImage* dst_img = GetOrInitImage(d3d11_dst);
    if (!src_img || !dst_img) return false;

    NvCV_Status status = NvCVImage_MapResource(dst_img, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] ConvertFrucOut: MapResource failed: %d", status);
        return false;
    }
    
    // Transfer from NV12 CUDA back to BGRA D3D11 natively
    status = NvCVImage_Transfer(src_img, dst_img, 1.0f, m_stream, nullptr);

    NvCVImage_UnmapResource(dst_img, m_stream);

    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] ConvertFrucOut failed during transfer: %d", status);
        return false;
    }
    return true;
}
