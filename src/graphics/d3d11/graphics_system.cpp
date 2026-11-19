/**
 * @file        graphics/d3d11/graphics_system.cpp
 * @brief       Direct3D 11 (feature level 11_0) graphics system.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/d3d11/graphics_system.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

D3D11GraphicsSystem::D3D11GraphicsSystem() {}

D3D11GraphicsSystem::~D3D11GraphicsSystem() {}

bool D3D11GraphicsSystem::IsAvailable() {
  return rex::ui::d3d11::D3D11Provider::IsD3D11APIAvailable();
}

std::string D3D11GraphicsSystem::name() const {
  auto d3d11_command_processor = static_cast<D3D11CommandProcessor*>(command_processor());
  if (d3d11_command_processor != nullptr) {
    return d3d11_command_processor->GetWindowTitleText();
  }
  return "Direct3D 11";
}

void D3D11GraphicsSystem::CreateProvider(bool /*with_presentation*/) {
  // Swap chains are created per window by the presenter.
  provider_ = rex::ui::d3d11::D3D11Provider::Create();
}

std::unique_ptr<CommandProcessor> D3D11GraphicsSystem::CreateCommandProcessor() {
  return std::unique_ptr<CommandProcessor>(new D3D11CommandProcessor(this, kernel_state_));
}

}  // namespace rex::graphics::d3d11
