/**
 * @file        graphics/d3d11/graphics_system.h
 * @brief       Direct3D 11 (feature level 11_0) graphics system.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::d3d11 {

class D3D11GraphicsSystem : public GraphicsSystem {
 public:
  D3D11GraphicsSystem();
  ~D3D11GraphicsSystem() override;

  static bool IsAvailable();

  std::string name() const override;

 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::d3d11
