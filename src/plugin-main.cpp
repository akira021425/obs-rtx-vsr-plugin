#include <obs-module.h>
#include "plugin-main.hpp"
#include "rtx-vsr-filter.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-rtx-vsr", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
    return "OBS RTX VSR + AI Frame Interpolation Plugin";
}

bool obs_module_load(void)
{
    register_rtx_vsr_filter();
    return true;
}
