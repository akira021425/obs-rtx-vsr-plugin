import sys

with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/rtx-vsr-filter.cpp', 'r', encoding='utf-8') as f:
    code = f.read()

old_draw = '''    // Draw output
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

new_draw = '''    // Draw output
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

code = code.replace(old_draw, new_draw)

with open('c:/Users/arai5/AndroidStudioProjects/OCVUltimate/obs-rtx-vsr/src/rtx-vsr-filter.cpp', 'w', encoding='utf-8') as f:
    f.write(code)

print("Done replacing draw logic")
