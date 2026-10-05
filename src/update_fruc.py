import sys

with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/rtx-vsr-filter.cpp', 'r', encoding='utf-8') as f:
    code = f.read()

# 1. Lift is_new_frame to wider scope
code = code.replace('bool is_new_frame = true;\n            if (filter->hash_stage_d3d11) {', 'is_new_frame = true;\n            if (filter->hash_stage_d3d11) {')
code = code.replace('bool success = false;\n    filter->frame_count++;', 'bool success = false;\n    bool is_new_frame = true;\n    filter->frame_count++;')

# 2. Rewrite the FRUC processing block
old_fruc = '''                    // Copy VSR output (RGBA) directly to FRUC input texture (also RGBA)
                    bool t_to_fruc = filter->nvidia_vsr->TransferToFruc(d3d11_dst, fruc_in_idx);
                    if (filter->frame_count <= 600) {
                        blog(LOG_INFO, "[RTX-VSR-DEBUG] frame=%llu TransferToFruc=%d", filter->frame_count, t_to_fruc);
                    }
                    if (t_to_fruc) {
                        
                        static double fruc_simulated_time = 0.0;
                        // 10,000,000 units = 1 second. 30 fps input = 333,333 units per frame.
                        fruc_simulated_time += 333333.333333;                        bool fruc_success = filter->fruc->Process(fruc_simulated_time);
                        
                        if (filter->frame_count <= 600) {
                            blog(LOG_INFO, "[RTX-VSR-DEBUG] frame=%llu FRUC success=%d", filter->frame_count, fruc_success);
                        }

                        if (fruc_success) {
                            filter->fruc_success_count++;
                            
                            // Copy FRUC output (RGBA) back to d3d11_dst
                            bool t_success = filter->nvidia_vsr->TransferFromFruc(fruc_out_idx, d3d11_dst);
                            if (filter->frame_count <= 600) {
                                blog(LOG_INFO, "[RTX-VSR-DEBUG] frame=%llu TransferFromFruc=%d", filter->frame_count, t_success);
                            }
                        }
                    }'''

new_fruc = '''                    gs_texrender_reset(filter->fruc_render);
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
code = code.replace(old_fruc, new_fruc)

with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/rtx-vsr-filter.cpp', 'w', encoding='utf-8') as f:
    f.write(code)

print("Done replacing FRUC logic")
