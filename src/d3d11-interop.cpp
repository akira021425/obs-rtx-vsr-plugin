#include "d3d11-interop.hpp"
#include <graphics/graphics.h>

D3D11Interop::D3D11Interop()
{
}

D3D11Interop::~D3D11Interop()
{
    Release();
}

bool D3D11Interop::Initialize()
{
    ID3D11Device *d3d11_dev = (ID3D11Device *)gs_get_device_obj();
    if (!d3d11_dev) {
        blog(LOG_ERROR, "[RTX-VSR] Failed to get D3D11 device from OBS. Is OBS using D3D11?");
        return false;
    }

    m_device = d3d11_dev;
    m_device->GetImmediateContext(&m_context);

    blog(LOG_INFO, "[RTX-VSR] D3D11 Interop initialized successfully.");
    return true;
}

void D3D11Interop::Release()
{
    m_context.Reset();
    m_device.Reset();
}
