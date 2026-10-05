import sys

with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/rtx-vsr-filter.cpp', 'r', encoding='utf-8') as f:
    code = f.read()

# Fix the FRUC init to use GetFrucD3D11Texture instead of GetFrucBgraTexture
code = code.replace('d3d11_textures[i] = filter->nvidia_vsr->GetFrucBgraTexture(i);', 'd3d11_textures[i] = filter->nvidia_vsr->GetFrucD3D11Texture(i);')
code = code.replace('d3d11_textures[i] = filter->nvidia_vsr->GetFrucRgbaTexture(i);', 'd3d11_textures[i] = filter->nvidia_vsr->GetFrucD3D11Texture(i);')

# Revert the FRUC processing block to directly use d3d11_dst
old_fruc = '''                    gs_texrender_reset(filter->fruc_render);
                    if (gs_texrender_begin(filter->fruc_render, target_width, target_height)) {
                        gs_effect_set_texture(image, filter->output_texture);
                        while (gs_effect_loop(def_effect, "Draw")) {
                            gs_draw_sprite(filter->output_texture, 0, target_width, target_height);
                        }
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
                                            filter->fruc_cache_texture = gs_texture_create(target_width, target_height, GS_BGRA_UNORM, 1, nullptr, GS_RENDER_TARGET);
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
                    }'''

new_fruc = '''                    bool t_to_fruc = filter->nvidia_vsr->TransferToFruc(d3d11_dst, fruc_in_idx);
                    if (t_to_fruc) {
                        static double fruc_simulated_time = 0.0;
                        fruc_simulated_time += 333333.333333;
                        bool fruc_success = filter->fruc->Process(fruc_simulated_time);
                        
                        if (fruc_success) {
                            filter->fruc_success_count++;
                            filter->nvidia_vsr->TransferFromFruc(fruc_out_idx, d3d11_dst);
                        }
                    }'''

code = code.replace(old_fruc, new_fruc)

# Revert draw block
old_draw = '''    // Draw output
    if (success && filter->output_texture) {
        gs_texture_t *tex_to_draw = filter->output_texture;
        if (is_new_frame && filter->fruc_cache_texture && filter->fruc->IsInitialized() && filter->fruc->IsEnabled()) {
            tex_to_draw = filter->fruc_cache_texture;
        }
        gs_effect_set_texture(image, tex_to_draw);
        while (gs_effect_loop(def_effect, "Draw")) {
            gs_draw_sprite(tex_to_draw, 0, target_width, target_height);
        }
    } else {
        gs_effect_set_texture(image, source_tex);
        while (gs_effect_loop(def_effect, "Draw")) {
            gs_draw_sprite(source_tex, 0, target_width, target_height);
        }
    }'''

new_draw = '''    // Draw output
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
    }'''

code = code.replace(old_draw, new_draw)

# Change surface type
old_init = 'if (!filter->fruc->Initialize(d3d11_textures, 3, target_width, target_height, "DirectX11Resource", "ARGBSurface")) {'
new_init = 'if (!filter->fruc->Initialize(d3d11_textures, 3, target_width, target_height, "DirectX11Resource", "NV12Surface")) {'
code = code.replace(old_init, new_init)

with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/rtx-vsr-filter.cpp', 'w', encoding='utf-8') as f:
    f.write(code)

print("Updated filter")
