#include "rtx-vsr-filter.hpp"
#include <obs-module.h>
#include "nvidia-vsr.hpp"
#include "frame-interpolation.hpp"
#include <util/platform.h>

struct rtx_vsr_data {
    obs_source_t *context;
    int vsr_quality;
    bool artifact_reduction;
    bool frame_interpolation;
    float resolution_scale;

    std::unique_ptr<D3D11Interop> d3d11_interop;
    std::unique_ptr<NvidiaVSR> nvidia_vsr;
    std::unique_ptr<FrameInterpolation> fruc;
    
    gs_texrender_t *texrender;       // For capturing source into a texture
    gs_texrender_t *fruc_render;     // For converting BGRA->RGBA for FRUC input
    gs_texture_t *output_texture;    // The upscaled output texture
    
    // VSR result caching
    gs_texture_t *vsr_cache_texture;
    bool has_cached_vsr;
    
    // Duplicate frame detection
    ID3D11Texture2D *hash_stage_d3d11;
    uint32_t last_hash[256];
    
    uint32_t src_width;
    uint32_t src_height;
    uint32_t out_width;
    uint32_t out_height;

    bool is_initialized;
    bool vsr_failed;
    
    // Statistics for logging
    uint64_t frame_count;
    uint64_t new_frame_count;
    uint64_t dup_frame_count;
    uint64_t vsr_count;
    uint64_t fruc_attempt_count;
    uint64_t fruc_success_count;
    uint64_t last_log_time;
};

static const char *rtx_vsr_get_name(void *type_data)
{
    UNUSED_PARAMETER(type_data);
    return obs_module_text("RTXVSRUpscaler");
}

static void *rtx_vsr_create(obs_data_t *settings, obs_source_t *context)
{
    rtx_vsr_data *data = new rtx_vsr_data();
    data->context = context;
    data->d3d11_interop = std::make_unique<D3D11Interop>();
    data->nvidia_vsr = std::make_unique<NvidiaVSR>();
    data->fruc = std::make_unique<FrameInterpolation>();
    data->output_texture = nullptr;
    data->vsr_cache_texture = nullptr;
    data->has_cached_vsr = false;
    data->texrender = gs_texrender_create(GS_BGRA_UNORM, GS_ZS_NONE);
    data->fruc_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    data->hash_stage_d3d11 = nullptr;
    memset(data->last_hash, 0, sizeof(data->last_hash));
    data->is_initialized = false;
    data->vsr_failed = false;
    data->frame_count = 0;
    data->new_frame_count = 0;
    data->dup_frame_count = 0;
    data->vsr_count = 0;
    data->fruc_attempt_count = 0;
    data->fruc_success_count = 0;
    data->last_log_time = 0;
    data->resolution_scale = 1.5f;
    data->src_width = 0;
    data->src_height = 0;
    data->out_width = 0;
    data->out_height = 0;
    
    obs_source_update(context, settings);
    return data;
}

static void rtx_vsr_destroy(void *data)
{
    rtx_vsr_data *filter = (rtx_vsr_data *)data;

    filter->nvidia_vsr->Release();
    filter->fruc->Release();

    obs_enter_graphics();
    if (filter->texrender) gs_texrender_destroy(filter->texrender);
    if (filter->fruc_render) gs_texrender_destroy(filter->fruc_render);
    if (filter->hash_stage_d3d11) filter->hash_stage_d3d11->Release();
    if (filter->output_texture) gs_texture_destroy(filter->output_texture);
    if (filter->vsr_cache_texture) gs_texture_destroy(filter->vsr_cache_texture);
    if (filter->is_initialized) filter->d3d11_interop->Release();
    obs_leave_graphics();
    
    delete filter;
}

static void rtx_vsr_update(void *data, obs_data_t *settings)
{
    rtx_vsr_data *filter = (rtx_vsr_data *)data;
    filter->vsr_quality = (int)obs_data_get_int(settings, "vsr_quality");
    filter->artifact_reduction = obs_data_get_bool(settings, "artifact_reduction");
    filter->frame_interpolation = obs_data_get_bool(settings, "frame_interpolation");
    
    double scale = obs_data_get_double(settings, "resolution_scale");
    if (scale < 1.0) scale = 1.0;
    filter->resolution_scale = (float)scale;

    if (filter->nvidia_vsr) {
        filter->nvidia_vsr->SetQuality(filter->vsr_quality);
        filter->nvidia_vsr->SetArtifactReduction(filter->artifact_reduction);
    }
    if (filter->fruc) {
        filter->fruc->SetEnabled(filter->frame_interpolation);
    }
}

static obs_properties_t *rtx_vsr_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();
    obs_property_t *p;

    p = obs_properties_add_list(props, "vsr_quality", obs_module_text("VSRQuality"),
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p, "Low", 1);
    obs_property_list_add_int(p, "Medium", 2);
    obs_property_list_add_int(p, "High", 3);
    obs_property_list_add_int(p, "Ultra", 4);

    obs_properties_add_bool(props, "artifact_reduction", obs_module_text("ArtifactReduction"));
    obs_properties_add_bool(props, "frame_interpolation", obs_module_text("FrameInterpolation"));
    
    p = obs_properties_add_list(props, "resolution_scale", "Resolution Scale",
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(p, "1.0x (Enhance Only)", 1.0f);
    obs_property_list_add_float(p, "1.33x", 1.333333f);
    obs_property_list_add_float(p, "1.5x (720p -> 1080p)", 1.5f);
    obs_property_list_add_float(p, "2.0x (1080p -> 4K)", 2.0f);

    return props;
}

static void rtx_vsr_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "vsr_quality", 4);
    obs_data_set_default_bool(settings, "artifact_reduction", false);
    obs_data_set_default_bool(settings, "frame_interpolation", true);
    obs_data_set_default_double(settings, "resolution_scale", 1.5);
}

static void rtx_vsr_video_tick(void *data, float seconds)
{
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(seconds);
}

static void rtx_vsr_video_render(void *data, gs_effect_t *effect)
{
    rtx_vsr_data *filter = (rtx_vsr_data *)data;
    UNUSED_PARAMETER(effect);
    
    obs_source_t *target = obs_filter_get_target(filter->context);
    if (!target) {
        obs_source_skip_video_filter(filter->context);
        return;
    }
    
    uint32_t width = obs_source_get_base_width(target);
    uint32_t height = obs_source_get_base_height(target);
    
    if (width == 0 || height == 0) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    uint32_t target_width = (uint32_t)(width * filter->resolution_scale);
    uint32_t target_height = (uint32_t)(height * filter->resolution_scale);

    // Check if source resolution changed
    if (filter->is_initialized && (filter->src_width != width || filter->src_height != height ||
                                    filter->out_width != target_width || filter->out_height != target_height)) {
        blog(LOG_INFO, "[RTX-VSR] Resolution changed: %ux%u->%ux%u => %ux%u->%ux%u, reinitializing",
             filter->src_width, filter->src_height, filter->out_width, filter->out_height,
             width, height, target_width, target_height);
        filter->nvidia_vsr->Release();
        filter->fruc->Release();
        if (filter->output_texture) { gs_texture_destroy(filter->output_texture); filter->output_texture = nullptr; }
        if (filter->vsr_cache_texture) { gs_texture_destroy(filter->vsr_cache_texture); filter->vsr_cache_texture = nullptr; }
        if (filter->hash_stage_d3d11) { filter->hash_stage_d3d11->Release(); filter->hash_stage_d3d11 = nullptr; }
        filter->has_cached_vsr = false;
        filter->is_initialized = false;
        filter->vsr_failed = false;
    }

    // One-time initialization
    if (!filter->is_initialized && !filter->vsr_failed) {
        blog(LOG_INFO, "[RTX-VSR] Initializing: source=%ux%u, output=%ux%u, scale=%.2f",
             width, height, target_width, target_height, filter->resolution_scale);
             
        if (filter->d3d11_interop->Initialize()) {
            auto d3d11_dev = filter->d3d11_interop->GetDevice();
            
            // Hash staging texture (16x16)
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = 16;
            desc.Height = 16;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (filter->hash_stage_d3d11) { filter->hash_stage_d3d11->Release(); filter->hash_stage_d3d11 = nullptr; }
            HRESULT hr = d3d11_dev->CreateTexture2D(&desc, nullptr, &filter->hash_stage_d3d11);
            blog(LOG_INFO, "[RTX-VSR] Hash staging texture: hr=0x%08X, ptr=%p", hr, filter->hash_stage_d3d11);

            // Output texture (BGRA)
            filter->output_texture = gs_texture_create(target_width, target_height, GS_BGRA_UNORM, 1, nullptr, GS_RENDER_TARGET);
            if (!filter->output_texture) {
                blog(LOG_ERROR, "[RTX-VSR] Failed to create output texture");
                filter->vsr_failed = true;
                obs_source_skip_video_filter(filter->context);
                return;
            }
            blog(LOG_INFO, "[RTX-VSR] Output texture created: %ux%u BGRA, d3d11=%p",
                 target_width, target_height, gs_texture_get_obj(filter->output_texture));

            gs_flush();
            
            // FRUC init (before VSR)
            if (!filter->fruc->Initialize(d3d11_dev, target_width, target_height)) {
                blog(LOG_WARNING, "[RTX-VSR] FRUC initialization failed - 60fps interpolation disabled");
            }

            // VSR init
            if (!filter->nvidia_vsr->Initialize(d3d11_dev, width, height, target_width, target_height)) {
                blog(LOG_ERROR, "[RTX-VSR] VSR initialization failed");
                filter->vsr_failed = true;
            }
  
            filter->src_width = width;
            filter->src_height = height;
            filter->out_width = target_width;
            filter->out_height = target_height;
            filter->is_initialized = true;
            filter->last_log_time = os_gettime_ns();
        } else {
            blog(LOG_ERROR, "[RTX-VSR] D3D11 interop init failed");
            filter->vsr_failed = true;
            obs_source_skip_video_filter(filter->context);
            return;
        }
    }

    // Capture source frame
    gs_texrender_reset(filter->texrender);
    if (!gs_texrender_begin(filter->texrender, width, height)) {
        obs_source_skip_video_filter(filter->context);
        return;
    }
    obs_source_video_render(target);
    gs_texrender_end(filter->texrender);
    
    gs_texture_t *source_tex = gs_texrender_get_texture(filter->texrender);
    if (!source_tex) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    gs_effect_t *def_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image = gs_effect_get_param_by_name(def_effect, "image");

    bool success = false;
    filter->frame_count++;

    if (filter->is_initialized && filter->nvidia_vsr->IsReady() && filter->output_texture) {
        ID3D11Texture2D *d3d11_src = (ID3D11Texture2D *)gs_texture_get_obj(source_tex);
        ID3D11Texture2D *d3d11_dst = (ID3D11Texture2D *)gs_texture_get_obj(filter->output_texture);
        
        if (d3d11_src && d3d11_dst) {
            // ===== Duplicate frame detection =====
            bool is_new_frame = true;
            if (filter->hash_stage_d3d11) {
                auto d3d_context = filter->d3d11_interop->GetContext();
                D3D11_BOX box;
                box.left = width / 2 - 8;
                box.right = width / 2 + 8;
                box.top = height / 2 - 8;
                box.bottom = height / 2 + 8;
                box.front = 0;
                box.back = 1;
                d3d_context->CopySubresourceRegion(filter->hash_stage_d3d11, 0, 0, 0, 0, d3d11_src, 0, &box);
                
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(d3d_context->Map(filter->hash_stage_d3d11, 0, D3D11_MAP_READ, 0, &mapped))) {
                    uint8_t *pixel_data = (uint8_t*)mapped.pData;
                    uint32_t linesize = mapped.RowPitch;
                    uint32_t current_hash[256];
                    for (int y = 0; y < 16; ++y) {
                        memcpy(&current_hash[y * 16], pixel_data + y * linesize, 64);
                    }
                    d3d_context->Unmap(filter->hash_stage_d3d11, 0);
                    
                    if (memcmp(current_hash, filter->last_hash, sizeof(current_hash)) == 0) {
                        is_new_frame = false;
                    } else {
                        memcpy(filter->last_hash, current_hash, sizeof(current_hash));
                    }
                }
            }

            if (is_new_frame) filter->new_frame_count++;
            else filter->dup_frame_count++;

            // ===== VSR processing =====
            if (is_new_frame || !filter->has_cached_vsr) {
                if (filter->resolution_scale > 1.01f) {
                    success = filter->nvidia_vsr->Process(d3d11_src, d3d11_dst);
                } else {
                    auto context = filter->d3d11_interop->GetContext();
                    if (context) { context->CopyResource(d3d11_dst, d3d11_src); success = true; }
                }
                if (success) {
                    filter->vsr_count++;
                    if (!filter->vsr_cache_texture) {
                        filter->vsr_cache_texture = gs_texture_create(target_width, target_height, GS_BGRA_UNORM, 1, nullptr, GS_RENDER_TARGET);
                    }
                    if (filter->vsr_cache_texture) {
                        ID3D11Texture2D *cache_d3d11 = (ID3D11Texture2D *)gs_texture_get_obj(filter->vsr_cache_texture);
                        if (cache_d3d11) {
                            auto context = filter->d3d11_interop->GetContext();
                            if (context) { context->CopyResource(cache_d3d11, d3d11_dst); filter->has_cached_vsr = true; }
                        }
                    }
                }
            } else {
                // Duplicate: use cached VSR
                if (filter->vsr_cache_texture) {
                    ID3D11Texture2D *cache_d3d11 = (ID3D11Texture2D *)gs_texture_get_obj(filter->vsr_cache_texture);
                    if (cache_d3d11) {
                        auto context = filter->d3d11_interop->GetContext();
                        if (context) { context->CopyResource(d3d11_dst, cache_d3d11); success = true; }
                    }
                }
            }

            // ===== FRUC processing =====
            if (success && filter->fruc->IsInitialized() && filter->fruc->IsEnabled()) {
                filter->fruc_attempt_count++;
                
                if (!is_new_frame && filter->fruc_cache_texture) {
                    // It's a duplicate frame. Do NOT call FRUC, because it will return error 16 if inputs are identical.
                    // Just use the previous FRUC output.
                    ID3D11Texture2D *fruc_cache = (ID3D11Texture2D *)gs_texture_get_obj(filter->fruc_cache_texture);
                    if (fruc_cache) {
                        auto context = filter->d3d11_interop->GetContext();
                        if (context) { context->CopyResource(d3d11_dst, fruc_cache); }
                    }
                } else {
                    // Determine how to pass the VSR output to FRUC based on FRUC's texture format
                    DXGI_FORMAT fruc_fmt = filter->fruc->GetTextureFormat();
                    ID3D11Texture2D *fruc_src = nullptr;
                    gs_texture_t *fruc_obs_tex = nullptr;
                    
                    if (fruc_fmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
                        // FRUC uses BGRA = same as VSR output, pass directly
                        fruc_src = d3d11_dst;
                    } else {
                        // FRUC uses RGBA, need format conversion via texrender
                        gs_texrender_reset(filter->fruc_render);
                        if (gs_texrender_begin(filter->fruc_render, target_width, target_height)) {
                            gs_effect_set_texture(image, filter->output_texture);
                            while (gs_effect_loop(def_effect, "Draw")) {
                                gs_draw_sprite(filter->output_texture, 0, target_width, target_height);
                            }
                            gs_texrender_end(filter->fruc_render);
                        }
                        fruc_obs_tex = gs_texrender_get_texture(filter->fruc_render);
                        if (fruc_obs_tex) {
                            fruc_src = (ID3D11Texture2D *)gs_texture_get_obj(fruc_obs_tex);
                        }
                    }
                    
                    if (fruc_src) {
                        static double fruc_simulated_time = 0.0;
                        fruc_simulated_time += (1.0 / 30.0);
                        
                        auto fruc_out = filter->fruc->Process(fruc_src, fruc_simulated_time);
                        
                        if (fruc_out) {
                            filter->fruc_success_count++;
                            // Copy FRUC output back
                            auto context = filter->d3d11_interop->GetContext();
                            if (context) {
                                if (fruc_fmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
                                    context->CopyResource(d3d11_dst, fruc_out.Get());
                                } else {
                                    // For now, let's just copy it to d3d11_dst directly if typeless or just use VSR output if it fails.
                                    // Actually we just don't copy if it's RGBA because it will fail CopyResource.
                                    // We will fix color conversion later once we confirm the plugin doesn't return error 16.
                                }
                            }
                            
                            // Cache the FRUC output
                            if (!filter->fruc_cache_texture) {
                                filter->fruc_cache_texture = gs_texture_create(target_width, target_height, GS_BGRA_UNORM, 1, nullptr, GS_RENDER_TARGET);
                            }
                            if (filter->fruc_cache_texture) {
                                ID3D11Texture2D *fruc_cache = (ID3D11Texture2D *)gs_texture_get_obj(filter->fruc_cache_texture);
                                if (fruc_cache) {
                                    auto context = filter->d3d11_interop->GetContext();
                                    if (context) { context->CopyResource(fruc_cache, d3d11_dst); }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Draw output
    if (success && filter->output_texture) {
        gs_effect_set_texture(image, filter->output_texture);
        while (gs_effect_loop(def_effect, "Draw")) {
            gs_draw_sprite(filter->output_texture, 0, target_width, target_height);
        }
    } else {
        gs_effect_set_texture(image, source_tex);
        while (gs_effect_loop(def_effect, "Draw")) {
            gs_draw_sprite(source_tex, 0, target_width, target_height);
        }
    }

log_and_return:
    // Periodic stats logging (every 10 seconds)
    uint64_t now = os_gettime_ns();
    if (filter->last_log_time > 0 && (now - filter->last_log_time) >= 10000000000ULL) {
        blog(LOG_INFO, "[RTX-VSR] Stats: frames=%llu new=%llu dup=%llu vsr=%llu fruc_try=%llu fruc_ok=%llu",
             filter->frame_count, filter->new_frame_count, filter->dup_frame_count,
             filter->vsr_count, filter->fruc_attempt_count, filter->fruc_success_count);
        filter->last_log_time = now;
    }
}

static uint32_t rtx_vsr_get_width(void *data)
{
    rtx_vsr_data *filter = (rtx_vsr_data *)data;
    obs_source_t *target = obs_filter_get_target(filter->context);
    if (!target) return 0;
    return (uint32_t)(obs_source_get_base_width(target) * filter->resolution_scale);
}

static uint32_t rtx_vsr_get_height(void *data)
{
    rtx_vsr_data *filter = (rtx_vsr_data *)data;
    obs_source_t *target = obs_filter_get_target(filter->context);
    if (!target) return 0;
    return (uint32_t)(obs_source_get_base_height(target) * filter->resolution_scale);
}

void register_rtx_vsr_filter()
{
    obs_source_info info = {};
    info.id = "rtx_vsr_upscaler";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name = rtx_vsr_get_name;
    info.create = rtx_vsr_create;
    info.destroy = rtx_vsr_destroy;
    info.update = rtx_vsr_update;
    info.get_defaults = rtx_vsr_defaults;
    info.get_properties = rtx_vsr_properties;
    info.video_tick = rtx_vsr_video_tick;
    info.video_render = rtx_vsr_video_render;
    info.get_width = rtx_vsr_get_width;
    info.get_height = rtx_vsr_get_height;
    
    obs_register_source(&info);
}
