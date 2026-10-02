#pragma once
#include "engine/GraphicsSettings.h"

// ImGui controls for the graphics settings (no window of its own). Returns true if anything changed.
bool drawGraphicsSettings(GraphicsSettings& settings, const RenderCapabilities& caps);
