import sys
import re

# 1. Update nvidia-vsr.hpp
with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/nvidia-vsr.hpp', 'r', encoding='utf-8') as f:
    hpp = f.read()
hpp = hpp.replace('m_fruc_bgra', 'm_fruc_rgba')
hpp = hpp.replace('GetFrucBgraTexture', 'GetFrucRgbaTexture')
with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/nvidia-vsr.hpp', 'w', encoding='utf-8') as f:
    f.write(hpp)

# 2. Update nvidia-vsr.cpp
with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/nvidia-vsr.cpp', 'r', encoding='utf-8') as f:
    cpp = f.read()
cpp = cpp.replace('m_fruc_bgra', 'm_fruc_rgba')
cpp = cpp.replace('DXGI_FORMAT_B8G8R8A8_UNORM', 'DXGI_FORMAT_R8G8B8A8_UNORM')
cpp = cpp.replace('GetFrucBgraTexture', 'GetFrucRgbaTexture')
cpp = cpp.replace('bgra_desc', 'rgba_desc')
cpp = cpp.replace('bgra_tex', 'rgba_tex')
with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/nvidia-vsr.cpp', 'w', encoding='utf-8') as f:
    f.write(cpp)

# 3. Update frame-interpolation.cpp
with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/frame-interpolation.cpp', 'r', encoding='utf-8') as f:
    fi = f.read()
fi = fi.replace('ARGBSurface', 'ABGRSurface')
fi = fi.replace('DXGI_FORMAT_B8G8R8A8_UNORM', 'DXGI_FORMAT_R8G8B8A8_UNORM')
with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/frame-interpolation.cpp', 'w', encoding='utf-8') as f:
    f.write(fi)

# 4. Update rtx-vsr-filter.cpp
with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/rtx-vsr-filter.cpp', 'r', encoding='utf-8') as f:
    rtx = f.read()

# Replace GetFrucBgraTexture
rtx = rtx.replace('GetFrucBgraTexture', 'GetFrucRgbaTexture')

# Remove fruc_render from headers and init
rtx = rtx.replace('gs_texrender_t *fruc_render;', '')
rtx = rtx.replace('data->fruc_render = gs_texrender_create(GS_BGRA_UNORM, GS_ZS_NONE);', '')
rtx = rtx.replace('if (filter->fruc_render) gs_texrender_destroy(filter->fruc_render);', '')

# Replace GS_BGRA_UNORM with GS_RGBA_UNORM in fruc_cache_texture creation
rtx = rtx.replace('filter->fruc_cache_texture = gs_texture_create(target_width, target_height, GS_BGRA_UNORM, 1, nullptr, GS_RENDER_TARGET);',
                  'filter->fruc_cache_texture = gs_texture_create(target_width, target_height, GS_RGBA_UNORM, 1, nullptr, GS_RENDER_TARGET);')

# Replace the complicated gs_texrender block with a simple TransferToFruc
old_block = """                    gs_texrender_reset(filter->fruc_render);
                    if (gs_texrender_begin(filter->fruc_render, target_width, target_height)) {
                        gs_matrix_push();
                        gs_ortho(0.0f, (float)target_width, 0.0f, (float)target_height, -100.0f, 100.0f);
                        gs_blend_state_push();
                        gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
                        
                        gs_effect_set_texture(image, filter->output_texture);
                        while (gs_effect_loop(def_effect, "Draw")) {
                            gs_draw_sprite(filter->output_texture, 0, target_width, target_height);
                        }
                        
                        gs_blend_state_pop();
                        gs_matrix_pop();
                        gs_texrender_end(filter->fruc_render);
                        
                        gs_texture_t* bgra_obs_tex = gs_texrender_get_texture(filter->fruc_render);
                        if (bgra_obs_tex) {
                            ID3D11Texture2D* d3d11_bgra = (ID3D11Texture2D*)gs_texture_get_obj(bgra_obs_tex);
                            if (d3d11_bgra) {
                                bool t_to_fruc = filter->nvidia_vsr->TransferToFruc(d3d11_bgra, fruc_in_idx);
                                if (t_to_fruc) {
                                    static double fruc_simulated_time = 0.0;
                                    fruc_simulated_time += 333333.333333;
                                    bool fruc_success = filter->fruc->Process(fruc_simulated_time);
                                    
                                    if (fruc_success) {
                                        filter->fruc_success_count++;
                                        
                                        if (!filter->fruc_cache_texture) {
                                            filter->fruc_cache_texture = gs_texture_create(target_width, target_height, GS_RGBA_UNORM, 1, nullptr, GS_RENDER_TARGET);
                                        }
                                        if (filter->fruc_cache_texture) {
                                            ID3D11Texture2D* d3d11_fruc_out = (ID3D11Texture2D*)gs_texture_get_obj(filter->fruc_cache_texture);
                                            if (d3d11_fruc_out) {
                                                filter->nvidia_vsr->TransferFromFruc(fruc_out_idx, d3d11_fruc_out);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }"""

new_block = """                    // Use d3d11_dst (filter->output_texture) directly! It's already RGBA.
                    bool t_to_fruc = filter->nvidia_vsr->TransferToFruc(d3d11_dst, fruc_in_idx);
                    if (t_to_fruc) {
                        static double fruc_simulated_time = 0.0;
                        fruc_simulated_time += 333333.333333;
                        bool fruc_success = filter->fruc->Process(fruc_simulated_time);
                        
                        if (fruc_success) {
                            filter->fruc_success_count++;
                            
                            if (!filter->fruc_cache_texture) {
                                filter->fruc_cache_texture = gs_texture_create(target_width, target_height, GS_RGBA_UNORM, 1, nullptr, GS_RENDER_TARGET);
                            }
                            if (filter->fruc_cache_texture) {
                                ID3D11Texture2D* d3d11_fruc_out = (ID3D11Texture2D*)gs_texture_get_obj(filter->fruc_cache_texture);
                                if (d3d11_fruc_out) {
                                    filter->nvidia_vsr->TransferFromFruc(fruc_out_idx, d3d11_fruc_out);
                                }
                            }
                        }
                    }"""

if old_block in rtx:
    rtx = rtx.replace(old_block, new_block)
else:
    print("Warning: old_block not found in rtx-vsr-filter.cpp!")

# also replace ARGBSurface in rtx-vsr-filter.cpp where it's passed as a string
rtx = rtx.replace('"ARGBSurface"', '"ABGRSurface"')

with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/rtx-vsr-filter.cpp', 'w', encoding='utf-8') as f:
    f.write(rtx)

print("Migration to ABGRSurface complete.")
