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

void NvidiaVSR::LogCudaContext(const char* tag)
{
    HMODULE lib = GetModuleHandleA("nvcuda.dll");
    if (!lib) {
        blog(LOG_INFO, "[RTX-VSR] CUDA ctx [%s]: nvcuda.dll not loaded", tag);
        return;
    }
    typedef int (__stdcall *PFN_cuCtxGetCurrent)(void**);
    PFN_cuCtxGetCurrent getCur = (PFN_cuCtxGetCurrent)GetProcAddress(lib, "cuCtxGetCurrent");
    void* ctx = nullptr;
    int res = getCur ? getCur(&ctx) : -1;
    blog(LOG_INFO, "[RTX-VSR] CUDA ctx [%s]: current=%p (res=%d) thread=%lu", tag, ctx, res, GetCurrentThreadId());
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
    status = NvCVImage_Alloc(m_src_gpu, src_width, src_height, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 0);
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
    status = NvCVImage_Alloc(m_dst_gpu, dst_width, dst_height, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 0);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to alloc dst GPU image (status: %d)", status);
        Release();
        return false;
    }

    status = NvCVImage_Create(dst_width, dst_height, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1, &m_dst_bgra_gpu);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to create dst BGRA GPU image (status: %d)", status);
        Release();
        return false;
    }
    status = NvCVImage_Alloc(m_dst_bgra_gpu, dst_width, dst_height, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 0);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to alloc dst BGRA GPU image (status: %d)", status);
        Release();
        return false;
    }





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
    
    struct CudaContextGuard {
        HMODULE nvcuda;
        bool pushed;
        CudaContextGuard(HMODULE lib, void* ctx) : nvcuda(lib), pushed(false) {
            if (nvcuda && ctx) {
                typedef int (__stdcall *PFN_cuCtxPushCurrent)(void*);
                PFN_cuCtxPushCurrent push = (PFN_cuCtxPushCurrent)GetProcAddress(nvcuda, "cuCtxPushCurrent");
                if (push) { push(ctx); pushed = true; }
            }
        }
        ~CudaContextGuard() {
            if (pushed && nvcuda) {
                typedef int (__stdcall *PFN_cuCtxPopCurrent)(void**);
                PFN_cuCtxPopCurrent pop = (PFN_cuCtxPopCurrent)GetProcAddress(nvcuda, "cuCtxPopCurrent");
                if (pop) { void* tmp; pop(&tmp); }
            }
        }
    } ctxGuard(m_nvcuda_dll, m_cu_ctx);

    // 1. Get or initialize wrapped D3D11 textures
    NvCVImage* src_img = GetOrInitImage(src_tex);
    NvCVImage* dst_img = GetOrInitImage(dst_tex);
    
    if (!src_img || !dst_img) {
        return false;
    }

    // 2. Map source texture, transfer to GPU staging buffer
    status = NvCVImage_MapResource(src_img, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] MapResource(src) failed: %d", status);
        static int s_fail_logs = 0;
        if (s_fail_logs++ < 3) {
            LogCudaContext("MapResource(src) failure (inside guard)");
            blog(LOG_ERROR, "[RTX-VSR] expected ctx=%p stream=%p guard_pushed=%d", m_cu_ctx, m_stream, (int)ctxGuard.pushed);
        }
        // Drop the cached wrapper so the next frame re-registers the texture with CUDA.
        auto it = m_tex_map.find(src_tex);
        if (it != m_tex_map.end()) {
            NvCVImage_Destroy(it->second);
            delete it->second;
            m_tex_map.erase(it);
        }
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
    // Both are now RGBA, so we can transfer directly without mismatched types!
    status = NvCVImage_Transfer(m_dst_gpu, dst_img, 1.0f, m_stream, NULL);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] Transfer m_dst_gpu->dst failed: %d", status);
        NvCVImage_UnmapResource(dst_img, m_stream);
        return false;
    }

    status = NvCVImage_UnmapResource(dst_img, m_stream);
    if (status != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[RTX-VSR] UnmapResource(dst) failed: %d", status);
        return false;
    }

    if (m_nvcuda_dll && m_stream) {
        typedef int (__stdcall *PFN_cuStreamSynchronize)(void*);
        PFN_cuStreamSynchronize sync = (PFN_cuStreamSynchronize)GetProcAddress(m_nvcuda_dll, "cuStreamSynchronize");
        if (sync) sync(m_stream);
    }

    return true;
}

bool NvidiaVSR::CopyOutputToD3D11(ID3D11Texture2D *dst_tex)
{
    if (!m_ready || !m_dst_gpu || !dst_tex) return false;

    struct CudaContextGuard {
        HMODULE nvcuda;
        bool pushed;
        CudaContextGuard(HMODULE lib, void* ctx) : nvcuda(lib), pushed(false) {
            if (nvcuda && ctx) {
                typedef int (__stdcall *PFN_cuCtxPushCurrent)(void*);
                PFN_cuCtxPushCurrent push = (PFN_cuCtxPushCurrent)GetProcAddress(nvcuda, "cuCtxPushCurrent");
                if (push) { push(ctx); pushed = true; }
            }
        }
        ~CudaContextGuard() {
            if (pushed && nvcuda) {
                typedef int (__stdcall *PFN_cuCtxPopCurrent)(void**);
                PFN_cuCtxPopCurrent pop = (PFN_cuCtxPopCurrent)GetProcAddress(nvcuda, "cuCtxPopCurrent");
                if (pop) { void* tmp; pop(&tmp); }
            }
        }
    } ctxGuard(m_nvcuda_dll, m_cu_ctx);

    NvCVImage* dst_img = GetOrInitImage(dst_tex);
    if (!dst_img) return false;

    NvCV_Status status = NvCVImage_MapResource(dst_img, m_stream);
    if (status != NVCV_SUCCESS) return false;

    status = NvCVImage_Transfer(m_dst_gpu, dst_img, 1.0f, m_stream, NULL);
    if (status != NVCV_SUCCESS) {
        NvCVImage_UnmapResource(dst_img, m_stream);
        return false;
    }

    status = NvCVImage_UnmapResource(dst_img, m_stream);
    if (status != NVCV_SUCCESS) return false;

    if (m_nvcuda_dll && m_stream) {
        typedef int (__stdcall *PFN_cuStreamSynchronize)(void*);
        PFN_cuStreamSynchronize sync = (PFN_cuStreamSynchronize)GetProcAddress(m_nvcuda_dll, "cuStreamSynchronize");
        if (sync) sync(m_stream);
    }

    return true;
}

