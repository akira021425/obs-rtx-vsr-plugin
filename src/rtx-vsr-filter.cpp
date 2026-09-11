#include "rtx-vsr-filter.hpp"
#include <obs-module.h>
#include "nvidia-vsr.hpp"
#include "frame-interpolation.hpp"

struct rtx_vsr_data {
    obs_source_t *context;
    int vsr_quality;
    bool artifact_reduction;
    bool frame_interpolation;
    float resolution_scale;

    std::unique_ptr<D3D11Interop> d3d11_interop;
    std::unique_ptr<NvidiaVSR> nvidia_vsr;
    std::unique_ptr<FrameInterpolation> fruc;
    gs_texture_t *render_target; // The final 1080p output texture (from VSR)
    gs_texrender_t *texrender; // For drawing the source into a texture

    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_input_tex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_output_tex;

    bool is_initialized;
    uint64_t frame_count;
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
    data->render_target = nullptr;
    data->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    data->is_initialized = false;
    data->frame_count = 0;
    data->resolution_scale = 1.0f; // Default 1x (Matching source)
    
    obs_source_update(context, settings);
    return data;
}

static void rtx_vsr_destroy(void *data)
{
    rtx_vsr_data *filter = (rtx_vsr_data *)data;
    obs_enter_graphics();
    if (filter->texrender) {
        gs_texrender_destroy(filter->texrender);
    }
    if (filter->render_target) {
        gs_texture_destroy(filter->render_target);
    }
    if (filter->is_initialized) {
        filter->fruc->Release();
        filter->nvidia_vsr->Release();
        filter->d3d11_interop->Release();
    }
    obs_leave_graphics();
    
    filter->shared_input_tex.Reset();
    filter->shared_output_tex.Reset();
    
    delete filter;
}

static void rtx_vsr_update(void *data, obs_data_t *settings)
{
    rtx_vsr_data *filter = (rtx_vsr_data *)data;
    filter->vsr_quality = (int)obs_data_get_int(settings, "vsr_quality");
    filter->artifact_reduction = obs_data_get_bool(settings, "artifact_reduction");
    filter->frame_interpolation = obs_data_get_bool(settings, "frame_interpolation");
    
    // Read the resolution scale multiplier
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
    
    // Read-only info texts can be added via string properties or text properties, 
    // but for simple UI we just use disabled string or generic properties.
    p = obs_properties_add_text(props, "output_res", obs_module_text("OutputResolution"), OBS_TEXT_DEFAULT);
    obs_property_set_enabled(p, false);

    p = obs_properties_add_text(props, "frame_rate", obs_module_text("FrameRate"), OBS_TEXT_DEFAULT);
    obs_property_set_enabled(p, false);

    p = obs_properties_add_bool(props, "perf_info", obs_module_text("PerformanceInformation"));

    return props;
}

static void rtx_vsr_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "vsr_quality", 4);
    obs_data_set_default_bool(settings, "artifact_reduction", false);
    obs_data_set_default_bool(settings, "frame_interpolation", true);
    obs_data_set_default_double(settings, "resolution_scale", 1.5); // Default to 1.5x (720p->1080p)
    obs_data_set_default_string(settings, "output_res", "1920x1080");
    obs_data_set_default_string(settings, "frame_rate", "60 FPS");
    obs_data_set_default_bool(settings, "perf_info", false);
}

static void rtx_vsr_video_tick(void *data, float seconds)
{
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(seconds);
    // Temporal processing tick if needed
}

static void rtx_vsr_video_render(void *data, gs_effect_t *effect)
{
    rtx_vsr_data *filter = (rtx_vsr_data *)data;
    UNUSED_PARAMETER(effect);
    
    obs_source_t *target = obs_filter_get_target(filter->context);
    if (!target) {
        return;
    }
    
    uint32_t width = obs_source_get_base_width(target);
    uint32_t height = obs_source_get_base_height(target);
    
    if (width == 0 || height == 0) {
        return;
    }

    uint32_t target_width = (uint32_t)(width * filter->resolution_scale);
    uint32_t target_height = (uint32_t)(height * filter->resolution_scale);

    // Handle resolution changes by re-initializing the render target
    if (filter->is_initialized && filter->render_target) {
        uint32_t current_rt_width = gs_texture_get_width(filter->render_target);
        uint32_t current_rt_height = gs_texture_get_height(filter->render_target);
        if (current_rt_width != target_width || current_rt_height != target_height) {
            gs_texture_destroy(filter->render_target);
            filter->render_target = gs_texture_create(target_width, target_height, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
            filter->shared_input_tex = filter->d3d11_interop->CreateSharedTexture(width, height);
            filter->shared_output_tex = filter->d3d11_interop->CreateSharedTexture(target_width, target_height);
            filter->nvidia_vsr->Initialize(filter->d3d11_interop->GetDevice(), target_width, target_height);
        }
    }

    if (!filter->is_initialized) {
        // Initialize D3D11 interop (Phase 3)
        if (filter->d3d11_interop->Initialize()) {
            filter->is_initialized = true;
            // Target output resolution
            filter->render_target = gs_texture_create(target_width, target_height, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
            filter->shared_input_tex = filter->d3d11_interop->CreateSharedTexture(width, height);
            filter->shared_output_tex = filter->d3d11_interop->CreateSharedTexture(target_width, target_height);
            
            // Initialize NVIDIA SDK (Phase 4)
            auto d3d11_dev = filter->d3d11_interop->GetDevice();
            if (!filter->nvidia_vsr->Initialize(d3d11_dev, target_width, target_height)) {
                blog(LOG_ERROR, "[RTX-VSR] Failed to initialize NVIDIA SDK");
                // Continue running with pass-through or basic scaling fallback
            }

            if (!filter->fruc->Initialize(d3d11_dev, width, height)) {
                blog(LOG_ERROR, "[RTX-VSR] Failed to initialize Frame Interpolation SDK");
            }
        } else {
            // Fallback
            blog(LOG_ERROR, "[RTX-VSR] Interop init failed, falling back.");
        }
    }

    // Capture the source frame into a texture (Phase 2)
    if (gs_texrender_begin(filter->texrender, width, height)) {
        obs_source_video_render(target);
        gs_texrender_end(filter->texrender);
    }
    
    gs_texture_t *source_tex = gs_texrender_get_texture(filter->texrender);
    
    if (filter->is_initialized && source_tex) {
        auto d3d11_src_tex = filter->d3d11_interop->GetD3D11Texture(source_tex);
        auto d3d11_dst_tex = filter->d3d11_interop->GetD3D11Texture(filter->render_target);
        
        bool success = false;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> final_tex;
        
        if (d3d11_src_tex && d3d11_dst_tex && filter->shared_input_tex && filter->shared_output_tex) {
            auto context = filter->d3d11_interop->GetContext();
            
            // Flush OBS graphics before D3D11 raw operations
            gs_flush();
            
            // 1. Copy OBS input to shared texture
            context->CopyResource(filter->shared_input_tex.Get(), d3d11_src_tex.Get());

            // Process VSR (Phase 5, 6, 7) using shared textures
            success = filter->nvidia_vsr->Process(filter->shared_input_tex, filter->shared_output_tex);
            
            if (success) {
                final_tex = filter->shared_output_tex;
                
                // Process Frame Interpolation (Phase 8-10)
                if (filter->fruc->IsEnabled()) {
                    // Very simple naive pass-through of timestamp
                    double timestamp = filter->frame_count++ * (1.0 / 30.0);
                    auto interp_tex = filter->fruc->Process(filter->shared_output_tex, timestamp);
                    if (interp_tex) {
                        final_tex = interp_tex;
                    }
                }
                
                // 3. Copy final result back to OBS render target
                context->CopyResource(d3d11_dst_tex.Get(), final_tex.Get());
            }
        }
        
        gs_effect_t *def_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_eparam_t *image = gs_effect_get_param_by_name(def_effect, "image");
        gs_texture_t *rt = filter->render_target;

        if (success) {
            // Processing succeeded, draw the upscale/interpolated output texture
            gs_effect_set_texture(image, rt);
            while (gs_effect_loop(def_effect, "Draw")) {
                gs_draw_sprite(rt, 0, target_width, target_height);
            }
        } else {
            // Fallback: Just pass through the source directly to the output with scaling
            gs_effect_set_texture(image, source_tex);
            while (gs_effect_loop(def_effect, "Draw")) {
                gs_draw_sprite(source_tex, 0, target_width, target_height);
            }
        }
    } else {
        // Fallback: Just pass through the source
        gs_effect_t *def_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_eparam_t *image = gs_effect_get_param_by_name(def_effect, "image");
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
