/**
 * @file        tests/unit/codegen/resume_entry_test.cpp
 * @brief       Synthetic coverage for interior-PC AOT resume entries
 */

#include <array>
#include <map>

#include <catch2/catch_test_macros.hpp>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/test_support.h>

TEST_CASE("FunctionNode: nested framed calls emit resumable direct and indirect return PCs",
          "[codegen][resume]") {
  constexpr uint32_t kBase = 0x82010000u;
  std::array<uint32_t, 16> words = {
      __builtin_bswap32(0x9421FF80u),  // stwu r1,-0x80(r1): abandoned prologue
      __builtin_bswap32(0x48000025u),  // bl kBase + 0x28
      __builtin_bswap32(0x60000000u),  // outer saved LR resumes here
      __builtin_bswap32(0x4E800421u),  // bctrl
      __builtin_bswap32(0x60000001u),  // indirect saved LR; bit 0 is not another call
      __builtin_bswap32(0x4BFFFFF4u),  // backward loop edge to kBase + 8
      __builtin_bswap32(0x48000008u),  // forward edge to shared epilogue
      __builtin_bswap32(0x60000000u),  // nop
      __builtin_bswap32(0x38210080u),  // shared addi r1,r1,0x80 epilogue
      __builtin_bswap32(0x4E800020u),  // shared blr
      __builtin_bswap32(0x9421FFC0u),  // nested stwu r1,-0x40(r1)
      __builtin_bswap32(0x48000011u),  // nested bl kBase + 0x3C
      __builtin_bswap32(0x60000000u),  // nested saved LR resumes here
      __builtin_bswap32(0x38210040u),  // nested addi r1,r1,0x40
      __builtin_bswap32(0x4E800020u),  // nested blr
      __builtin_bswap32(0x4E800020u),  // leaf blr
  };

  rex::codegen::TestModule module;
  module.Load(kBase, reinterpret_cast<const uint8_t*>(words.data()), sizeof(words));
  auto binary = rex::codegen::BinaryView::fromModule(module);
  rex::codegen::RecompilerConfig config;
  auto ctx = rex::codegen::CodegenContext::Create(std::move(binary), std::move(config));

  rex::codegen::AnalyzeTestBinary(
      ctx, "resume",
      {{kBase, "test_owner"}, {kBase + 0x28, "test_callee"}, {kBase + 0x3C, "test_leaf"}},
      kBase,
      reinterpret_cast<const uint8_t*>(words.data()), sizeof(words));

  const auto* owner = ctx.graph.getFunction(kBase);
  REQUIRE(owner != nullptr);
  CHECK(owner->resumableReturnAddresses(ctx.binary()) ==
        std::vector<uint32_t>{kBase + 8, kBase + 0x10});

  const auto* callee = ctx.graph.getFunction(kBase + 0x28);
  REQUIRE(callee != nullptr);
  CHECK(callee->resumableReturnAddresses(ctx.binary()) ==
        std::vector<uint32_t>{kBase + 0x30});

  rex::codegen::EmitContext emit{
      ctx.binary(), ctx.Config(), ctx.graph, kBase + 0x3C, nullptr};
  const std::string output = owner->emitCpp(emit);
  CHECK(output.find("case 0x82010008: goto loc_82010008;") != std::string::npos);
  CHECK(output.find("case 0x82010010: goto loc_82010010;") != std::string::npos);
  CHECK(output.find("loc_82010008:") != std::string::npos);
  CHECK(output.find("loc_82010010:") != std::string::npos);
  CHECK(output.find("goto loc_82010008;") != std::string::npos);
  CHECK(output.find("goto loc_82010020;") != std::string::npos);
  CHECK(output.find("ctx.dispatch_address = 0;") != std::string::npos);

  const size_t resumeSwitch = output.find("switch (rex_dispatch_address)");
  const size_t abandonedPrologue = output.find("// stwu");
  REQUIRE(resumeSwitch != std::string::npos);
  REQUIRE(abandonedPrologue != std::string::npos);
  CHECK(resumeSwitch < abandonedPrologue);

  const std::string nestedOutput = callee->emitCpp(emit);
  CHECK(nestedOutput.find("case 0x82010030: goto loc_82010030;") != std::string::npos);
}

TEST_CASE("FunctionNode: direct guest branches emit guaranteed tail calls",
          "[codegen][tail-call]") {
  constexpr uint32_t kBase = 0x82020000u;
  std::array<uint32_t, 3> words = {
      __builtin_bswap32(0x48000008u),  // b kBase + 8
      __builtin_bswap32(0x60000000u),  // padding
      __builtin_bswap32(0x4E800020u),  // callee blr
  };

  rex::codegen::TestModule module;
  module.Load(kBase, reinterpret_cast<const uint8_t*>(words.data()), sizeof(words));
  auto binary = rex::codegen::BinaryView::fromModule(module);
  rex::codegen::RecompilerConfig config;
  auto ctx = rex::codegen::CodegenContext::Create(std::move(binary), std::move(config));

  rex::codegen::AnalyzeTestBinary(
      ctx, "tail", {{kBase, "test_caller"}, {kBase + 8, "test_callee"}}, kBase,
      reinterpret_cast<const uint8_t*>(words.data()), sizeof(words));

  auto* caller = ctx.graph.getFunction(kBase);
  auto* callee = ctx.graph.getFunction(kBase + 8);
  REQUIRE(caller != nullptr);
  REQUIRE(callee != nullptr);
  ctx.graph.addTailCallToFunction(kBase, kBase, rex::codegen::CallTarget::function(callee));

  rex::codegen::EmitContext emit{
      ctx.binary(), ctx.Config(), ctx.graph, kBase + 8, nullptr};
  const std::string output = caller->emitCpp(emit);
  CHECK(output.find("REX_TAIL_CALL(tail_82020008);") != std::string::npos);
  CHECK(output.find("tail_82020008(ctx, base);\n\treturn;") == std::string::npos);
}
