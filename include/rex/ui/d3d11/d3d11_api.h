/**
 ******************************************************************************
 * ShiftGlue: Direct3D 11 UI backend                                          *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ObjBase.h>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
// For Microsoft::WRL::ComPtr.
#include <wrl/client.h>
// clang-format on
