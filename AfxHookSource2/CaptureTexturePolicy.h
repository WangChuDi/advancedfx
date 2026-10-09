#pragma once

#include <d3d11.h>

namespace AfxCapture {

inline DXGI_FORMAT FormatFamily(DXGI_FORMAT format) {
    switch(format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default:
        return format;
    }
}

// CopyResource cannot resize, and ResolveSubresource cannot convert typed formats.
inline bool CanCopyOrResolve(const D3D11_TEXTURE2D_DESC & source, const D3D11_TEXTURE2D_DESC & target) {
    if(!source.Width || !source.Height || !source.SampleDesc.Count
        || source.Width != target.Width || source.Height != target.Height
        || source.MipLevels != target.MipLevels || source.ArraySize != target.ArraySize
        || target.SampleDesc.Count != 1
        || source.Format == DXGI_FORMAT_UNKNOWN || target.Format == DXGI_FORMAT_UNKNOWN
        || FormatFamily(source.Format) != FormatFamily(target.Format)) return false;
    if(source.SampleDesc.Count == 1) return source.SampleDesc.Quality == target.SampleDesc.Quality;
    return source.Format == target.Format
        || source.Format == FormatFamily(target.Format)
        || target.Format == FormatFamily(source.Format);
}

} // namespace AfxCapture
