#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <SPIRV/SpvBuilder.h>

#include <rex/assert.h>

namespace rex::graphics {

// SpvBuilder with extra helpers.

class SpirvBuilder : public spv::Builder {
 public:
  SpirvBuilder(unsigned int spv_version, unsigned int user_number, spv::SpvBuildLogger* logger)
      : spv::Builder(spv_version, user_number, logger) {}

  // Make public rather than protected.
  using spv::Builder::createSelectionMerge;

  spv::Id createQuadOp(spv::Op op_code, spv::Id type_id, spv::Id operand1, spv::Id operand2,
                       spv::Id operand3, spv::Id operand4);

  // When set, the NoContraction helpers leave the result undecorated, so the
  // driver may fuse and reorder (spirv_fast_pixel_math).
  bool allow_contraction = false;

  spv::Id createNoContractionUnaryOp(spv::Op op_code, spv::Id type_id, spv::Id operand);
  spv::Id createNoContractionBinOp(spv::Op op_code, spv::Id type_id, spv::Id operand1,
                                   spv::Id operand2);

  // When set, 32-bit float arithmetic emitted through the NoContraction
  // helpers (FAdd, FSub, FMul, FDiv) and the GLSL.std.450 math helpers (the
  // rounding, min/max/clamp, sqrt, inverse sqrt, sin and cos functions) is
  // performed in 16-bit floats: the operands are converted with OpFConvert
  // right before the operation and the result right after it, so every value
  // outside the operation stays a 32-bit float (fh1_shader_float16). The
  // module must declare the Float16 capability. Sign-only operations (FAbs,
  // FNegate), exp2/log2 (a 16-bit float overflows past 2^16), conversions and
  // packing stay 32-bit.
  bool float16_arithmetic = false;

  // Which operation classes float16_arithmetic applies to, for bisecting
  // image differences (fh1_shader_float16_ops). All classes by default.
  enum Float16OpClass : uint32_t {
    kFloat16OpAdd = 1u << 0,       // FAdd, FSub
    kFloat16OpMul = 1u << 1,       // FMul
    kFloat16OpDiv = 1u << 2,       // FDiv
    kFloat16OpRound = 1u << 3,     // Round, RoundEven, Trunc, Floor, Ceil, Fract
    kFloat16OpTrig = 1u << 4,      // Sin, Cos
    kFloat16OpSqrt = 1u << 5,      // Sqrt, InverseSqrt
    kFloat16OpMinMax = 1u << 6,    // FMin, FMax, FClamp, NMin, NMax, NClamp
    kFloat16OpAll = 0x7Fu,
  };
  uint32_t float16_op_classes = kFloat16OpAll;

  // Clamp each non-constant operand to the finite 16-bit range (+-65504) with
  // NClamp before the OpFConvert to 16 bits, so a 32-bit value past it
  // saturates instead of becoming an infinity (fh1_shader_float16_clamp); the
  // operations that stay 32-bit (exp2, log2, reciprocals) can produce such
  // values. Needs the GLSL.std.450 import in float16_builtins.
  bool float16_clamp_operands = false;
  spv::Id float16_builtins = 0;

  // Sets float16_arithmetic for a scope and restores the previous value.
  class Float16ArithmeticScope {
   public:
    Float16ArithmeticScope(SpirvBuilder& builder, bool enable)
        : builder_(builder), previous_(builder.float16_arithmetic) {
      builder_.float16_arithmetic = enable;
    }
    ~Float16ArithmeticScope() { builder_.float16_arithmetic = previous_; }

   private:
    Float16ArithmeticScope(const Float16ArithmeticScope&) = delete;
    Float16ArithmeticScope& operator=(const Float16ArithmeticScope&) = delete;
    SpirvBuilder& builder_;
    bool previous_;
  };

  spv::Id createUnaryBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                                 spv::Id operand);
  spv::Id createBinBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                               spv::Id operand1, spv::Id operand2);
  spv::Id createTriBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                               spv::Id operand1, spv::Id operand2, spv::Id operand3);

  // Helper to use for building nested control flow with if-then-else with
  // additions over SpvBuilder::If.
  class IfBuilder {
   public:
    IfBuilder(spv::Id condition, unsigned int control, SpirvBuilder& builder,
              unsigned int thenWeight = 0, unsigned int elseWeight = 0);

    ~IfBuilder() {
#ifndef NDEBUG
      assert_true(currentBranch == Branch::kMerge);
#endif
    }

    void makeBeginElse(bool branchToMerge = true);
    void makeEndIf(bool branchToMerge = true);

    // If there's no then/else block that branches to the merge block, the phi
    // parent is the header block - this simplifies then-only usage.
    spv::Id getThenPhiParent() const { return thenPhiParent; }
    spv::Id getElsePhiParent() const { return elsePhiParent; }

    spv::Id createMergePhi(spv::Id then_variable, spv::Id else_variable) const;

   private:
    enum class Branch {
      kThen,
      kElse,
      kMerge,
    };

    IfBuilder(const IfBuilder& ifBuilder) = delete;
    IfBuilder& operator=(const IfBuilder& ifBuilder) = delete;

    SpirvBuilder& builder;
    spv::Id condition;
    unsigned int control;
    unsigned int thenWeight;
    unsigned int elseWeight;

    spv::Function& function;

    spv::Block* headerBlock;
    spv::Block* thenBlock;
    spv::Block* elseBlock;
    spv::Block* mergeBlock;

    spv::Id thenPhiParent;
    spv::Id elsePhiParent;

#ifndef NDEBUG
    Branch currentBranch = Branch::kThen;
#endif
  };

  // Simpler and more flexible (such as multiple cases pointing to the same
  // block) compared to makeSwitch.
  class SwitchBuilder {
   public:
    SwitchBuilder(spv::Id selector, unsigned int selection_control, SpirvBuilder& builder);
    ~SwitchBuilder() { assert_true(current_branch_ == Branch::kMerge); }

    void makeBeginDefault();
    void makeBeginCase(unsigned int literal);
    void addCurrentCaseLiteral(unsigned int literal);
    void makeEndSwitch();

    // If there's no default block that branches to the merge block, the phi
    // parent is the header block - this simplifies case-only usage.
    spv::Id getDefaultPhiParent() const { return default_phi_parent_; }

   private:
    enum class Branch {
      kSelection,
      kDefault,
      kCase,
      kMerge,
    };

    void endSegment();

    SpirvBuilder& builder_;
    spv::Id selector_;
    unsigned int selection_control_;

    spv::Function& function_;

    spv::Block* header_block_;
    spv::Block* merge_block_;
    spv::Block* default_block_ = nullptr;

    std::vector<std::pair<unsigned int, spv::Id>> cases_;

    spv::Id default_phi_parent_;

    Branch current_branch_ = Branch::kSelection;
  };

 private:
  // The 16-bit float type with the shape of a 32-bit float scalar or vector
  // type under float16_arithmetic, or spv::NoType when the type is not one or
  // the mode is off.
  spv::Id GetFloat16ArithmeticType(spv::Id float32_type);
  bool IsFloat16BinOp(spv::Op op_code) const;
  bool IsFloat16BuiltinCall(int entry_point) const;
  spv::Id createFloat16BuiltinCall(spv::Id result_type, spv::Id float16_type, spv::Id builtins,
                                   int entry_point, const spv::Id* operands, int operand_count);
  // The 32-bit operand converted to the 16-bit type, clamped first when
  // float16_clamp_operands is set.
  spv::Id ConvertToFloat16(spv::Id float16_type, spv::Id operand);
};

}  // namespace rex::graphics
