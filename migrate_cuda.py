import sys

# 1. Update nvidia-vsr.hpp
with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/nvidia-vsr.hpp', 'r', encoding='utf-8') as f:
    hpp = f.read()

# Add mapped NvCVImages for FRUC BGRA buffers
if 'NvCVImage* m_fruc_bgra_mapped[3]' not in hpp:
    hpp = hpp.replace('Microsoft::WRL::ComPtr<ID3D11Texture2D> m_fruc_bgra[3];',
                      'Microsoft::WRL::ComPtr<ID3D11Texture2D> m_fruc_bgra[3];\n    NvCVImage* m_fruc_bgra_mapped[3] = {nullptr, nullptr, nullptr};')

with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/nvidia-vsr.hpp', 'w', encoding='utf-8') as f:
    f.write(hpp)

# 2. Update nvidia-vsr.cpp
with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/nvidia-vsr.cpp', 'r', encoding='utf-8') as f:
    cpp = f.read()

# Make sure we clean up the mapped images
release_block = """    for (int i = 0; i < 3; i++) {
        m_fruc_bgra[i].Reset();
    }"""
new_release_block = """    for (int i = 0; i < 3; i++) {
        if (m_fruc_bgra_mapped[i]) {
            m_fruc_bgra_mapped[i] = nullptr;
        }
        m_fruc_bgra[i].Reset();
    }"""
cpp = cpp.replace(release_block, new_release_block)

# Initialize the mapped images
init_block = """    for (int i = 0; i < 3; i++) {
        HRESULT hr = m_device->CreateTexture2D(&bgra_desc, nullptr, &m_fruc_bgra[i]);
        if (FAILED(hr)) {
            blog(LOG_ERROR, "[RTX-VSR] Failed to create D3D11 BGRA texture %d: 0x%08X", i, hr);
            Release();
            return false;
        }
    }"""
new_init_block = """    for (int i = 0; i < 3; i++) {
        HRESULT hr = m_device->CreateTexture2D(&bgra_desc, nullptr, &m_fruc_bgra[i]);
        if (FAILED(hr)) {
            blog(LOG_ERROR, "[RTX-VSR] Failed to create D3D11 BGRA texture %d: 0x%08X", i, hr);
            Release();
            return false;
        }
        m_fruc_bgra_mapped[i] = GetOrInitImage(m_fruc_bgra[i].Get());
        if (!m_fruc_bgra_mapped[i]) {
            blog(LOG_ERROR, "[RTX-VSR] Failed to init NvCVImage for D3D11 BGRA texture %d", i);
            Release();
            return false;
        }
    }"""
cpp = cpp.replace(init_block, new_init_block)

# Rewrite TransferToFruc and TransferFromFruc
old_transfer_to = """bool NvidiaVSR::TransferToFruc(ID3D11Texture2D* bgra_tex, int fruc_idx) {
    if (!m_ready || fruc_idx < 0 || fruc_idx >= 3) return false;
    if (!bgra_tex || !m_fruc_bgra[fruc_idx]) return false;
    
    m_context->CopyResource(m_fruc_bgra[fruc_idx].Get(), bgra_tex);
    return true;
}"""
new_transfer_to = """bool NvidiaVSR::TransferToFruc(ID3D11Texture2D* rgba_tex, int fruc_idx) {
    if (!m_ready || fruc_idx < 0 || fruc_idx >= 3) return false;
    if (!rgba_tex || !m_fruc_bgra_mapped[fruc_idx]) return false;
    
    NvCVImage* rgba_img = GetOrInitImage(rgba_tex);
    NvCVImage* bgra_img = m_fruc_bgra_mapped[fruc_idx];
    
    NvCVImage_MapResource(rgba_img, m_stream);
    NvCVImage_MapResource(bgra_img, m_stream);
    
    NvCV_Status status = NvCVImage_Transfer(rgba_img, bgra_img, 1.0f, m_stream, NULL);
    
    NvCVImage_UnmapResource(bgra_img, m_stream);
    NvCVImage_UnmapResource(rgba_img, m_stream);
    
    return status == NVCV_SUCCESS;
}"""
cpp = cpp.replace(old_transfer_to, new_transfer_to)

old_transfer_from = """bool NvidiaVSR::TransferFromFruc(int fruc_idx, ID3D11Texture2D* bgra_tex) {
    if (!m_ready || fruc_idx < 0 || fruc_idx >= 3) return false;
    if (!bgra_tex || !m_fruc_bgra[fruc_idx]) return false;
    
    m_context->CopyResource(bgra_tex, m_fruc_bgra[fruc_idx].Get());
    return true;
}"""
new_transfer_from = """bool NvidiaVSR::TransferFromFruc(int fruc_idx, ID3D11Texture2D* rgba_tex) {
    if (!m_ready || fruc_idx < 0 || fruc_idx >= 3) return false;
    if (!rgba_tex || !m_fruc_bgra_mapped[fruc_idx]) return false;
    
    NvCVImage* rgba_img = GetOrInitImage(rgba_tex);
    NvCVImage* bgra_img = m_fruc_bgra_mapped[fruc_idx];
    
    NvCVImage_MapResource(bgra_img, m_stream);
    NvCVImage_MapResource(rgba_img, m_stream);
    
    NvCV_Status status = NvCVImage_Transfer(bgra_img, rgba_img, 1.0f, m_stream, NULL);
    
    NvCVImage_UnmapResource(rgba_img, m_stream);
    NvCVImage_UnmapResource(bgra_img, m_stream);
    
    return status == NVCV_SUCCESS;
}"""
cpp = cpp.replace(old_transfer_from, new_transfer_from)

with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/nvidia-vsr.cpp', 'w', encoding='utf-8') as f:
    f.write(cpp)

# 3. Update rtx-vsr-filter.cpp
with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/rtx-vsr-filter.cpp', 'r', encoding='utf-8') as f:
    rtx = f.read()

# Restore the old simple Transfer logic, removing gs_texrender completely
bad_gs_block = """                    gs_texrender_reset(filter->fruc_render);
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
                    }"""

clean_block = """                    bool t_to_fruc = filter->nvidia_vsr->TransferToFruc(d3d11_dst, fruc_in_idx);
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

rtx = rtx.replace(bad_gs_block, clean_block)
rtx = rtx.replace('gs_texrender_t *fruc_render;', '')
rtx = rtx.replace('data->fruc_render = gs_texrender_create(GS_BGRA_UNORM, GS_ZS_NONE);', '')
rtx = rtx.replace('if (filter->fruc_render) gs_texrender_destroy(filter->fruc_render);', '')

with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/rtx-vsr-filter.cpp', 'w', encoding='utf-8') as f:
    f.write(rtx)

print("Migration to pure CUDA colorspace conversion complete.")
