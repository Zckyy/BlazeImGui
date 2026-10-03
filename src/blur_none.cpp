// =============================================================================
// Blur stub for renderer-agnostic builds (BLAZE_RENDERER_DX11=OFF).
// The menu draws on the theme's translucent background without acrylic blur.
// =============================================================================
#include "internal.h"

namespace blaze::detail::blur {

bool Init(ID3D11Device*, ID3D11DeviceContext*) { return false; }
bool Available() { return false; }
void Shutdown() {}
void ReleaseTargets() {}
bool Apply(ID3D11RenderTargetView*, const BlurSettings&, const ImVec4&, float) { return false; }

} // namespace blaze::detail::blur
