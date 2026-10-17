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
#include <utility>
#include <vector>

#include <SPIRV/GLSL.std.450.h>

#include <rex/assert.h>
#include <rex/graphics/pipeline/shader/spirv_builder.h>

namespace rex::graphics {

spv::Id SpirvBuilder::GetFloat16ArithmeticType(spv::Id float32_type) {
  if (!float16_arithmetic || float32_type == spv::NoType) {
    return spv::NoType;
  }
  int component_count = 1;
  spv::Id scalar_type = float32_type;
  if (isVectorType(float32_type)) {
    component_count = getNumTypeComponents(float32_type);
    scalar_type = getContainedTypeId(float32_type);
  }
  if (!isFloatType(scalar_type) || getScalarTypeWidth(scalar_type) != 32) {
    return spv::NoType;
  }
  spv::Id float16_type = makeFloatType(16);
  return component_count > 1 ? makeVectorType(float16_type, component_count) : float16_type;
}

bool SpirvBuilder::IsFloat16BinOp(spv::Op op_code) {
  switch (op_code) {
    case spv::OpFAdd:
    case spv::OpFSub:
    case spv::OpFMul:
    case spv::OpFDiv:
      return true;
    default:
      return false;
  }
}

bool SpirvBuilder::IsFloat16BuiltinCall(int entry_point) {
  switch (entry_point) {
    case GLSLstd450Round:
    case GLSLstd450RoundEven:
    case GLSLstd450Trunc:
    case GLSLstd450Floor:
    case GLSLstd450Ceil:
    case GLSLstd450Fract:
    case GLSLstd450Sin:
    case GLSLstd450Cos:
    case GLSLstd450Sqrt:
    case GLSLstd450InverseSqrt:
    case GLSLstd450FMin:
    case GLSLstd450FMax:
    case GLSLstd450FClamp:
    case GLSLstd450NMin:
    case GLSLstd450NMax:
    case GLSLstd450NClamp:
      return true;
    default:
      return false;
  }
}

spv::Id SpirvBuilder::createFloat16BuiltinCall(spv::Id result_type, spv::Id float16_type,
                                               spv::Id builtins, int entry_point,
                                               const spv::Id* operands, int operand_count) {
  std::unique_ptr<spv::Instruction> instruction =
      std::make_unique<spv::Instruction>(getUniqueId(), float16_type, spv::OpExtInst);
  instruction->addIdOperand(builtins);
  instruction->addImmediateOperand(entry_point);
  for (int i = 0; i < operand_count; ++i) {
    instruction->addIdOperand(createUnaryOp(spv::OpFConvert, float16_type, operands[i]));
  }
  spv::Id result = instruction->getResultId();
  getBuildPoint()->addInstruction(std::move(instruction));
  return createUnaryOp(spv::OpFConvert, result_type, result);
}

spv::Id SpirvBuilder::createQuadOp(spv::Op op_code, spv::Id type_id, spv::Id operand1,
                                   spv::Id operand2, spv::Id operand3, spv::Id operand4) {
  if (generatingOpCodeForSpecConst) {
    std::vector<spv::Id> operands(4);
    operands[0] = operand1;
    operands[1] = operand2;
    operands[2] = operand3;
    operands[3] = operand4;
    return createSpecConstantOp(op_code, type_id, operands, std::vector<spv::Id>());
  }
  std::unique_ptr<spv::Instruction> op =
      std::make_unique<spv::Instruction>(getUniqueId(), type_id, op_code);
  op->addIdOperand(operand1);
  op->addIdOperand(operand2);
  op->addIdOperand(operand3);
  op->addIdOperand(operand4);
  spv::Id result = op->getResultId();
  buildPoint->addInstruction(std::move(op));
  return result;
}

spv::Id SpirvBuilder::createNoContractionUnaryOp(spv::Op op_code, spv::Id type_id,
                                                 spv::Id operand) {
  spv::Id result = createUnaryOp(op_code, type_id, operand);
  if (!allow_contraction) {
    addDecoration(result, spv::DecorationNoContraction);
  }
  return result;
}

spv::Id SpirvBuilder::createNoContractionBinOp(spv::Op op_code, spv::Id type_id, spv::Id operand1,
                                               spv::Id operand2) {
  spv::Id float16_type = IsFloat16BinOp(op_code) ? GetFloat16ArithmeticType(type_id) : spv::NoType;
  spv::Id result;
  if (float16_type != spv::NoType) {
    result = createBinOp(op_code, float16_type,
                         createUnaryOp(spv::OpFConvert, float16_type, operand1),
                         createUnaryOp(spv::OpFConvert, float16_type, operand2));
  } else {
    result = createBinOp(op_code, type_id, operand1, operand2);
  }
  if (!allow_contraction) {
    addDecoration(result, spv::DecorationNoContraction);
  }
  if (float16_type != spv::NoType) {
    result = createUnaryOp(spv::OpFConvert, type_id, result);
  }
  return result;
}

spv::Id SpirvBuilder::createUnaryBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                                             spv::Id operand) {
  if (IsFloat16BuiltinCall(entry_point)) {
    spv::Id float16_type = GetFloat16ArithmeticType(result_type);
    if (float16_type != spv::NoType) {
      return createFloat16BuiltinCall(result_type, float16_type, builtins, entry_point, &operand,
                                      1);
    }
  }
  std::unique_ptr<spv::Instruction> instruction =
      std::make_unique<spv::Instruction>(getUniqueId(), result_type, spv::OpExtInst);
  instruction->addIdOperand(builtins);
  instruction->addImmediateOperand(entry_point);
  instruction->addIdOperand(operand);
  spv::Id result = instruction->getResultId();
  getBuildPoint()->addInstruction(std::move(instruction));
  return result;
}

spv::Id SpirvBuilder::createBinBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                                           spv::Id operand1, spv::Id operand2) {
  if (IsFloat16BuiltinCall(entry_point)) {
    spv::Id float16_type = GetFloat16ArithmeticType(result_type);
    if (float16_type != spv::NoType) {
      const spv::Id operands[] = {operand1, operand2};
      return createFloat16BuiltinCall(result_type, float16_type, builtins, entry_point, operands,
                                      2);
    }
  }
  std::unique_ptr<spv::Instruction> instruction =
      std::make_unique<spv::Instruction>(getUniqueId(), result_type, spv::OpExtInst);
  instruction->addIdOperand(builtins);
  instruction->addImmediateOperand(entry_point);
  instruction->addIdOperand(operand1);
  instruction->addIdOperand(operand2);
  spv::Id result = instruction->getResultId();
  getBuildPoint()->addInstruction(std::move(instruction));
  return result;
}

spv::Id SpirvBuilder::createTriBuiltinCall(spv::Id result_type, spv::Id builtins, int entry_point,
                                           spv::Id operand1, spv::Id operand2, spv::Id operand3) {
  if (IsFloat16BuiltinCall(entry_point)) {
    spv::Id float16_type = GetFloat16ArithmeticType(result_type);
    if (float16_type != spv::NoType) {
      const spv::Id operands[] = {operand1, operand2, operand3};
      return createFloat16BuiltinCall(result_type, float16_type, builtins, entry_point, operands,
                                      3);
    }
  }
  std::unique_ptr<spv::Instruction> instruction =
      std::make_unique<spv::Instruction>(getUniqueId(), result_type, spv::OpExtInst);
  instruction->addIdOperand(builtins);
  instruction->addImmediateOperand(entry_point);
  instruction->addIdOperand(operand1);
  instruction->addIdOperand(operand2);
  instruction->addIdOperand(operand3);
  spv::Id result = instruction->getResultId();
  getBuildPoint()->addInstruction(std::move(instruction));
  return result;
}

SpirvBuilder::IfBuilder::IfBuilder(spv::Id condition, unsigned int control, SpirvBuilder& builder,
                                   unsigned int thenWeight, unsigned int elseWeight)
    : builder(builder),
      condition(condition),
      control(control),
      thenWeight(thenWeight),
      elseWeight(elseWeight),
      function(builder.getBuildPoint()->getParent()) {
  // Make the blocks, but only put the then-block into the function, the
  // else-block and merge-block will be added later, in order, after earlier
  // code is emitted.
  thenBlock = new spv::Block(builder.getUniqueId(), function);
  elseBlock = nullptr;
  mergeBlock = new spv::Block(builder.getUniqueId(), function);

  // Save the current block, so that we can add in the flow control split when
  // makeEndIf is called.
  headerBlock = builder.getBuildPoint();

  spv::Id headerBlockId = headerBlock->getId();
  thenPhiParent = headerBlockId;
  elsePhiParent = headerBlockId;

  function.addBlock(thenBlock);
  builder.setBuildPoint(thenBlock);
}

void SpirvBuilder::IfBuilder::makeBeginElse(bool branchToMerge) {
#ifndef NDEBUG
  assert_true(currentBranch == Branch::kThen);
#endif

  // A block may have only one terminator, and glslang's createBranch appends
  // unconditionally - check here, like SwitchBuilder::endSegment does.
  if (branchToMerge && !builder.getBuildPoint()->isTerminated()) {
    // Close out the "then" by having it jump to the mergeBlock.
    thenPhiParent = builder.getBuildPoint()->getId();
    builder.createBranch(mergeBlock);
  }

  // Make the first else block and add it to the function.
  elseBlock = new spv::Block(builder.getUniqueId(), function);
  function.addBlock(elseBlock);

  // Start building the else block.
  builder.setBuildPoint(elseBlock);

#ifndef NDEBUG
  currentBranch = Branch::kElse;
#endif
}

void SpirvBuilder::IfBuilder::makeEndIf(bool branchToMerge) {
#ifndef NDEBUG
  assert_true(currentBranch == Branch::kThen || currentBranch == Branch::kElse);
#endif

  // Don't add a second terminator, as in makeBeginElse.
  if (branchToMerge && !builder.getBuildPoint()->isTerminated()) {
    // Jump to the merge block.
    (elseBlock ? elsePhiParent : thenPhiParent) = builder.getBuildPoint()->getId();
    builder.createBranch(mergeBlock);
  }

  // Go back to the headerBlock and make the flow control split.
  builder.setBuildPoint(headerBlock);
  builder.createSelectionMerge(mergeBlock, control);
  {
    spv::Block* falseBlock = elseBlock ? elseBlock : mergeBlock;
    std::unique_ptr<spv::Instruction> branch =
        std::make_unique<spv::Instruction>(spv::OpBranchConditional);
    branch->addIdOperand(condition);
    branch->addIdOperand(thenBlock->getId());
    branch->addIdOperand(falseBlock->getId());
    if (thenWeight || elseWeight) {
      branch->addImmediateOperand(thenWeight);
      branch->addImmediateOperand(elseWeight);
    }
    builder.getBuildPoint()->addInstruction(std::move(branch));
    thenBlock->addPredecessor(builder.getBuildPoint());
    falseBlock->addPredecessor(builder.getBuildPoint());
  }

  // Add the merge block to the function.
  function.addBlock(mergeBlock);
  builder.setBuildPoint(mergeBlock);

#ifndef NDEBUG
  currentBranch = Branch::kMerge;
#endif
}

spv::Id SpirvBuilder::IfBuilder::createMergePhi(spv::Id then_variable,
                                                spv::Id else_variable) const {
  assert_true(builder.getBuildPoint() == mergeBlock);
  return builder.createQuadOp(spv::OpPhi, builder.getTypeId(then_variable), then_variable,
                              getThenPhiParent(), else_variable, getElsePhiParent());
}

SpirvBuilder::SwitchBuilder::SwitchBuilder(spv::Id selector, unsigned int selection_control,
                                           SpirvBuilder& builder)
    : builder_(builder),
      selector_(selector),
      selection_control_(selection_control),
      function_(builder.getBuildPoint()->getParent()),
      header_block_(builder.getBuildPoint()),
      default_phi_parent_(builder.getBuildPoint()->getId()) {
  merge_block_ = new spv::Block(builder_.getUniqueId(), function_);
}

void SpirvBuilder::SwitchBuilder::makeBeginDefault() {
  assert_null(default_block_);

  endSegment();

  default_block_ = new spv::Block(builder_.getUniqueId(), function_);
  function_.addBlock(default_block_);
  default_block_->addPredecessor(header_block_);
  builder_.setBuildPoint(default_block_);

  current_branch_ = Branch::kDefault;
}

void SpirvBuilder::SwitchBuilder::makeBeginCase(unsigned int literal) {
  endSegment();

  auto case_block = new spv::Block(builder_.getUniqueId(), function_);
  function_.addBlock(case_block);
  cases_.emplace_back(literal, case_block->getId());
  case_block->addPredecessor(header_block_);
  builder_.setBuildPoint(case_block);

  current_branch_ = Branch::kCase;
}

void SpirvBuilder::SwitchBuilder::addCurrentCaseLiteral(unsigned int literal) {
  assert_true(current_branch_ == Branch::kCase);

  cases_.emplace_back(literal, cases_.back().second);
}

void SpirvBuilder::SwitchBuilder::makeEndSwitch() {
  endSegment();

  builder_.setBuildPoint(header_block_);

  builder_.createSelectionMerge(merge_block_, selection_control_);

  std::unique_ptr<spv::Instruction> switch_instruction =
      std::make_unique<spv::Instruction>(spv::OpSwitch);
  switch_instruction->addIdOperand(selector_);
  if (default_block_) {
    switch_instruction->addIdOperand(default_block_->getId());
  } else {
    switch_instruction->addIdOperand(merge_block_->getId());
    merge_block_->addPredecessor(header_block_);
  }
  for (const std::pair<unsigned int, spv::Id>& case_pair : cases_) {
    switch_instruction->addImmediateOperand(case_pair.first);
    switch_instruction->addIdOperand(case_pair.second);
  }
  builder_.getBuildPoint()->addInstruction(std::move(switch_instruction));

  function_.addBlock(merge_block_);
  builder_.setBuildPoint(merge_block_);

  current_branch_ = Branch::kMerge;
}

void SpirvBuilder::SwitchBuilder::endSegment() {
  assert_true(current_branch_ == Branch::kSelection || current_branch_ == Branch::kDefault ||
              current_branch_ == Branch::kCase);

  if (current_branch_ == Branch::kSelection) {
    return;
  }

  if (!builder_.getBuildPoint()->isTerminated()) {
    builder_.createBranch(merge_block_);
    if (current_branch_ == Branch::kDefault) {
      default_phi_parent_ = builder_.getBuildPoint()->getId();
    }
  }

  current_branch_ = Branch::kSelection;
}

}  // namespace rex::graphics
