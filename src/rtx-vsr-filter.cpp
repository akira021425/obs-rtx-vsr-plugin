#include "rtx-vsr-filter.hpp"
#include <obs-module.h>
#include "nvidia-vsr.hpp"
#include "frame-interpolation.hpp"

#include <atomic>

struct rtx_vsr_data {
    obs_source_t *context;
    int vsr_quality;
    bool artifact_reduction;
    bool frame_interpolation;
    float resolution_scale;
    std::atomic<bool> new_frame_arrived;

    std::unique_ptr<D3D11Interop> d3d11_interop;
    std::unique_ptr<NvidiaVSR> nvidia_vsr;
    std::unique_ptr<FrameInterpolation> fruc;
    
    gs_texrender_t *texrender;       // For capturing source into a texture
    gs_texrender_t *fruc_render;     // For converting BGRA to RGBA for FRUC
    gs_texture_t *output_texture;    // The upscaled output texture (plain D3D11, no SHARED flag)
    
    uint32_t src_width;
    uint32_t src_height;
    uint32_t out_width;
    uint32_t out_height;

    bool is_initialized;
    bool vsr_failed;  // If VSR init fails, don't retry every frame
    uint64_t frame_count;
    uint64_t render_count;
};

// Intermediate textures for format conversion (not needed when using NVCV_RGB staging)
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
    data->texrender = gs_texrender_create(GS_BGRA_UNORM, GS_ZS_NONE);
    data->fruc_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    data->is_initialized = false;
    data->vsr_failed = false;
    data->frame_count = 0;
    data->render_count = 0;
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

    // Release NVIDIA SDK first (before destroying textures it references)
    filter->nvidia_vsr->Release();
    filter->fruc->Release();

    obs_enter_graphics();
    if (filter->texrender) {
        gs_texrender_destroy(filter->texrender);
    }
    if (filter->fruc_render) {
        gs_texrender_destroy(filter->fruc_render);
    }
    if (filter->output_texture) {
        gs_texture_destroy(filter->output_texture);
    }
    if (filter->is_initialized) {
        filter->d3d11_interop->Release();
    }
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
    obs_data_set_default_bool(settings, "frame_interpolation", false);  // Disabled by default (NvOFFRUC.dll rarely available)
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

    // Check if source resolution changed - need to reinitialize
    if (filter->is_initialized && (filter->src_width != width || filter->src_height != height ||
                                    filter->out_width != target_width || filter->out_height != target_height)) {
        // Resolution changed, tear down and reinitialize
        filter->nvidia_vsr->Release();
        if (filter->output_texture) {
            gs_texture_destroy(filter->output_texture);
            filter->output_texture = nullptr;
        }
        filter->is_initialized = false;
        filter->vsr_failed = false;
    }

    // One-time initialization
    if (!filter->is_initialized && !filter->vsr_failed) {
        if (filter->d3d11_interop->Initialize()) {
            // Create output texture for VSR (must be BGRA for NvCVImage_InitFromD3D11Texture to succeed)
            filter->output_texture = gs_texture_create(target_width, target_height, GS_BGRA_UNORM, 1, nullptr, GS_RENDER_TARGET);
            if (!filter->output_texture) {
                blog(LOG_ERROR, "[RTX-VSR] Failed to create output texture");
                filter->vsr_failed = true;
                obs_source_skip_video_filter(filter->context);
                return;
            }

            // Initialize NVIDIA VSR with proper dimensions
              auto d3d11_dev = filter->d3d11_interop->GetDevice();
              
              // Flush OBS graphics pipeline before NVIDIA SDK accesses D3D11
              gs_flush();
              
              // Frame interpolation MUST be initialized FIRST!
              // NvOFFRUC creates a CUDA context that can override the thread's current context.
              // If initialized after VSR, it corrupts VSR's resource mapping (-1400 error).
              if (!filter->fruc->Initialize(d3d11_dev, target_width, target_height)) {
                  blog(LOG_WARNING, "[RTX-VSR] Frame interpolation not available");
              }

              // Initialize NVIDIA VSR with proper dimensions
              if (!filter->nvidia_vsr->Initialize(d3d11_dev, width, height, target_width, target_height)) {
                  blog(LOG_ERROR, "[RTX-VSR] Failed to initialize NVIDIA VSR SDK");
                  filter->vsr_failed = true;
                  // Don't return - we can still do pass-through
              }
  
              filter->src_width = width;
            filter->src_height = height;
            filter->out_width = target_width;
            filter->out_height = target_height;
            filter->is_initialized = true;
        } else {
            blog(LOG_ERROR, "[RTX-VSR] D3D11 interop init failed");
            filter->vsr_failed = true;
            obs_source_skip_video_filter(filter->context);
            return;
        }
    }

    // Capture the source frame into a texture
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

    if (filter->is_initialized && filter->nvidia_vsr->IsReady() && filter->output_texture) {
        // Get the raw D3D11 texture pointers (no ComPtr wrapping = no ref count changes)
        ID3D11Texture2D *d3d11_src = (ID3D11Texture2D *)gs_texture_get_obj(source_tex);
        ID3D11Texture2D *d3d11_dst = (ID3D11Texture2D *)gs_texture_get_obj(filter->output_texture);
        
        if (d3d11_src && d3d11_dst) {
            // Run VSR on the current source frame
            if (filter->resolution_scale > 1.01f) {
                gs_flush();
                success = filter->nvidia_vsr->Process(d3d11_src, d3d11_dst);
            } else {
                auto context = filter->d3d11_interop->GetContext();
                if (context) {
                    context->CopyResource(d3d11_dst, d3d11_src);
                    success = true;
                }
            }

            if (success && filter->fruc->IsEnabled()) {
                // Convert VSR output to FRUC input
                gs_texrender_reset(filter->fruc_render);
                if (gs_texrender_begin(filter->fruc_render, target_width, target_height)) {
                    gs_effect_set_texture(image, filter->output_texture);
                    while (gs_effect_loop(def_effect, "Draw")) {
                        gs_draw_sprite(filter->output_texture, 0, target_width, target_height);
                    }
                    gs_texrender_end(filter->fruc_render);
                }

                gs_texture_t *fruc_rgba_tex = gs_texrender_get_texture(filter->fruc_render);
                if (fruc_rgba_tex) {
                    double timestamp = filter->render_count++ * (1.0 / 60.0);
                    ID3D11Texture2D *d3d11_fruc_in = (ID3D11Texture2D *)gs_texture_get_obj(fruc_rgba_tex);
                    auto fruc_out = filter->fruc->Process(d3d11_fruc_in, timestamp);
                    
                    if (fruc_out) {
                        auto context = filter->d3d11_interop->GetContext();
                        if (context) {
                            context->CopyResource(d3d11_fruc_in, fruc_out.Get());
                        }
                        
                        gs_effect_set_texture(image, fruc_rgba_tex);
                        while (gs_effect_loop(def_effect, "Draw")) {
                            gs_draw_sprite(fruc_rgba_tex, 0, target_width, target_height);
                        }
                        return; // Done drawing the FRUC frame
                    }
                }
            }
        }
    }

    // Draw the VSR result directly (Fallback if FRUC is disabled or failed)
    if (success && filter->output_texture) {
        gs_effect_set_texture(image, filter->output_texture);
        while (gs_effect_loop(def_effect, "Draw")) {
            gs_draw_sprite(filter->output_texture, 0, target_width, target_height);
        }
    } else {
        // Fallback: pass through source
        gs_effect_set_texture(image, source_tex);
        while (gs_effect_loop(def_effect, "Draw")) {
            gs_draw_sprite(source_tex, 0, target_width, target_height);
        }
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

