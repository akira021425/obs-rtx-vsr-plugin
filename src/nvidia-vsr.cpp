#include "nvidia-vsr.hpp"
#include <stdio.h>
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
    m_device->GetImmediateContext(&m_context);
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

    if (!m_nvcuda_dll) {
        m_nvcuda_dll = LoadLibraryA("nvcuda.dll");
        if (m_nvcuda_dll) {
            typedef int (__stdcall *PFN_cuStreamGetCtx)(void* hStream, void** pctx);
            PFN_cuStreamGetCtx cuStreamGetCtx = (PFN_cuStreamGetCtx)GetProcAddress(m_nvcuda_dll, "cuStreamGetCtx");
            if (cuStreamGetCtx) {
                int res = cuStreamGetCtx(m_stream, &m_cu_ctx);
                if (res == 0 && m_cu_ctx) {
                    blog(LOG_INFO, "[RTX-VSR] Successfully retrieved CUDA context: %p", m_cu_ctx);
                }
            }
        }
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


    status = NvCVImage_Create(dst_width, dst_height, (NvCVImage_PixelFormat)NVCV_NV12, (NvCVImage_ComponentType)NVCV_U8, NVCV_PLANAR, NVCV_GPU, 1, &m_staging_nv12_gpu);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create NV12 staging image (status: %d)", status);
        Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = dst_width;
    desc.Height = dst_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // REQUIRED for CUDA interop inside NvOFFRUC!

    for (int i = 0; i < 3; i++) {
        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_fruc_d3d11[i]);
        if (FAILED(hr)) {
            blog(LOG_ERROR, "[RTX-VSR] Failed to create D3D11 NV12 texture %d: 0x%08X", i, hr);
            Release();
            return false;
        }
        
        m_fruc_d3d11_mapped[i] = GetOrInitImage(m_fruc_d3d11[i].Get());
        if (!m_fruc_d3d11_mapped[i]) {
            blog(LOG_ERROR, "[RTX-VSR] Failed to init NvCVImage for D3D11 NV12 texture %d", i);
            Release();
            return false;
        }
    }

    // Create separate RGBA textures for NvOFFRUC registration
    // NvOFFRUC with DirectX11Resource requires SHARED|SHARED_NTHANDLE and ARGBSurface
    D3D11_TEXTURE2D_DESC rgba_desc = {};
    rgba_desc.Width = dst_width;
    rgba_desc.Height = dst_height;
    rgba_desc.MipLevels = 1;
    rgba_desc.ArraySize = 1;
    rgba_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rgba_desc.SampleDesc.Count = 1;
    rgba_desc.Usage = D3D11_USAGE_DEFAULT;
    rgba_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    rgba_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    for (int i = 0; i < 3; i++) {
        HRESULT hr = m_device->CreateTexture2D(&rgba_desc, nullptr, &m_fruc_rgba[i]);
        if (FAILED(hr)) {
            blog(LOG_ERROR, "[RTX-VSR] Failed to create FRUC RGBA texture %d: 0x%08X", i, hr);
            Release();
            return false;
        }
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

    if (m_staging_nv12_gpu) { NvCVImage_Destroy(m_staging_nv12_gpu); m_staging_nv12_gpu = nullptr; }
    for (int i = 0; i < 3; i++) {
        m_fruc_d3d11_mapped[i] = nullptr; // cleaned up by m_tex_map
        m_fruc_d3d11[i].Reset();
        m_fruc_rgba[i].Reset();
    }

    if (m_effect) {
        NvVFX_DestroyEffect(m_effect);
        m_effect = nullptr;
    }
    if (m_stream) {
        NvVFX_CudaStreamDestroy(m_stream);
        m_stream = nullptr;
    }
    if (m_nvcuda_dll) {
        FreeLibrary(m_nvcuda_dll);
        m_nvcuda_dll = nullptr;
    }
    m_cu_ctx = nullptr;
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
    // Convert RGBA GPU to BGRA GPU to avoid NVCV_ERR_UNIMPLEMENTED
    status = NvCVImage_Transfer(m_dst_gpu, m_dst_bgra_gpu, 1.0f, m_stream, NULL);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Transfer m_dst_gpu->m_dst_bgra_gpu failed: %d", status);
        NvCVImage_UnmapResource(dst_img, m_stream);
        return false;
    }
    
    status = NvCVImage_Transfer(m_dst_bgra_gpu, dst_img, 1.0f, m_stream, NULL);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Transfer m_dst_bgra_gpu->dst failed: %d", status);
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

static void log_crash_step(const char* step) {
    FILE* f = fopen("C:\\Users\\arai5\\obs_crash_debug.txt", "a");
    if (f) {
        fprintf(f, "%s\n", step);
        fclose(f);
    }
}


bool NvidiaVSR::TransferToFruc(ID3D11Texture2D* bgra_tex, int fruc_idx) {
    if (!m_ready || fruc_idx < 0 || fruc_idx >= 3) return false;
    NvCVImage* bgra_img = GetOrInitImage(bgra_tex);
    NvCVImage* rgba_img = GetOrInitImage(m_fruc_rgba[fruc_idx].Get());
    if (!bgra_img || !rgba_img || !m_dst_bgra_gpu || !m_dst_gpu) return false;
    
    NvCV_Status status;
    status = NvCVImage_MapResource(bgra_img, m_stream);
    if (status != NVCV_SUCCESS) return false;
    
    status = NvCVImage_MapResource(rgba_img, m_stream);
    if (status != NVCV_SUCCESS) { NvCVImage_UnmapResource(bgra_img, m_stream); return false; }
    
    // Convert BGRA (D3D11) -> BGRA (GPU) -> RGBA (GPU) -> RGBA (D3D11)
    status = NvCVImage_Transfer(bgra_img, m_dst_bgra_gpu, 1.0f, m_stream, NULL);
    if (status == NVCV_SUCCESS) {
        status = NvCVImage_Transfer(m_dst_bgra_gpu, m_dst_gpu, 1.0f, m_stream, NULL);
        if (status == NVCV_SUCCESS) {
            status = NvCVImage_Transfer(m_dst_gpu, rgba_img, 1.0f, m_stream, NULL);
        }
    }
    
    NvCVImage_UnmapResource(rgba_img, m_stream);
    NvCVImage_UnmapResource(bgra_img, m_stream);
    return status == NVCV_SUCCESS;
}

bool NvidiaVSR::TransferFromFruc(int fruc_idx, ID3D11Texture2D* bgra_tex) {
    if (!m_ready || fruc_idx < 0 || fruc_idx >= 3) return false;
    NvCVImage* bgra_img = GetOrInitImage(bgra_tex);
    NvCVImage* rgba_img = GetOrInitImage(m_fruc_rgba[fruc_idx].Get());
    if (!bgra_img || !rgba_img || !m_dst_bgra_gpu || !m_dst_gpu) return false;
    
    NvCV_Status status;
    status = NvCVImage_MapResource(bgra_img, m_stream);
    if (status != NVCV_SUCCESS) return false;
    
    status = NvCVImage_MapResource(rgba_img, m_stream);
    if (status != NVCV_SUCCESS) { NvCVImage_UnmapResource(bgra_img, m_stream); return false; }
    
    // Convert RGBA (D3D11) -> RGBA (GPU) -> BGRA (GPU) -> BGRA (D3D11)
    status = NvCVImage_Transfer(rgba_img, m_dst_gpu, 1.0f, m_stream, NULL);
    if (status == NVCV_SUCCESS) {
        status = NvCVImage_Transfer(m_dst_gpu, m_dst_bgra_gpu, 1.0f, m_stream, NULL);
        if (status == NVCV_SUCCESS) {
            status = NvCVImage_Transfer(m_dst_bgra_gpu, bgra_img, 1.0f, m_stream, NULL);
        }
    }
    
    NvCVImage_UnmapResource(rgba_img, m_stream);
    NvCVImage_UnmapResource(bgra_img, m_stream);
    return status == NVCV_SUCCESS;
}
