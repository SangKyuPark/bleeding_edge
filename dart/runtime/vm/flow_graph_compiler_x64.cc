// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/globals.h"  // Needed here to get TARGET_ARCH_X64.
#if defined(TARGET_ARCH_X64)

#include "vm/flow_graph_compiler.h"

#include "vm/ast_printer.h"
#include "vm/compiler.h"
#include "vm/dart_entry.h"
#include "vm/deopt_instructions.h"
#include "vm/il_printer.h"
#include "vm/locations.h"
#include "vm/object_store.h"
#include "vm/parser.h"
#include "vm/stack_frame.h"
#include "vm/stub_code.h"
#include "vm/symbols.h"

namespace dart {

DEFINE_FLAG(bool, trap_on_deoptimization, false, "Trap on deoptimization.");
DECLARE_FLAG(int, optimization_counter_threshold);
DECLARE_FLAG(int, reoptimization_counter_threshold);
DECLARE_FLAG(bool, enable_type_checks);
DECLARE_FLAG(bool, eliminate_type_checks);


FlowGraphCompiler::~FlowGraphCompiler() {
  // BlockInfos are zone-allocated, so their destructors are not called.
  // Verify the labels explicitly here.
  for (int i = 0; i < block_info_.length(); ++i) {
    ASSERT(!block_info_[i]->jump_label()->IsLinked());
    ASSERT(!block_info_[i]->jump_label()->HasNear());
  }
}


bool FlowGraphCompiler::SupportsUnboxedMints() {
  return false;
}


bool FlowGraphCompiler::SupportsSinCos() {
  return true;
}


RawDeoptInfo* CompilerDeoptInfo::CreateDeoptInfo(FlowGraphCompiler* compiler,
                                                 DeoptInfoBuilder* builder,
                                                 const Array& deopt_table) {
  if (deopt_env_ == NULL) return DeoptInfo::null();

  intptr_t stack_height = compiler->StackSize();
  AllocateIncomingParametersRecursive(deopt_env_, &stack_height);

  intptr_t slot_ix = 0;
  Environment* current = deopt_env_;

  // Emit all kMaterializeObject instructions describing objects to be
  // materialized on the deoptimization as a prefix to the deoptimization info.
  EmitMaterializations(deopt_env_, builder);

  // The real frame starts here.
  builder->MarkFrameStart();

  // Current PP, FP, and PC.
  builder->AddPp(current->code(), slot_ix++);
  builder->AddPcMarker(Code::Handle(), slot_ix++);
  builder->AddCallerFp(slot_ix++);
  builder->AddReturnAddress(current->code(), deopt_id(), slot_ix++);

  // Emit all values that are needed for materialization as a part of the
  // expression stack for the bottom-most frame. This guarantees that GC
  // will be able to find them during materialization.
  slot_ix = builder->EmitMaterializationArguments(slot_ix);

  // For the innermost environment, set outgoing arguments and the locals.
  for (intptr_t i = current->Length() - 1;
       i >= current->fixed_parameter_count();
       i--) {
    builder->AddCopy(current->ValueAt(i), current->LocationAt(i), slot_ix++);
  }

  Environment* previous = current;
  current = current->outer();
  while (current != NULL) {
    // PP, FP, and PC.
    builder->AddPp(current->code(), slot_ix++);
    builder->AddPcMarker(previous->code(), slot_ix++);
    builder->AddCallerFp(slot_ix++);

    // For any outer environment the deopt id is that of the call instruction
    // which is recorded in the outer environment.
    builder->AddReturnAddress(current->code(),
                              Isolate::ToDeoptAfter(current->deopt_id()),
                              slot_ix++);

    // The values of outgoing arguments can be changed from the inlined call so
    // we must read them from the previous environment.
    for (intptr_t i = previous->fixed_parameter_count() - 1; i >= 0; i--) {
      builder->AddCopy(previous->ValueAt(i),
                       previous->LocationAt(i),
                       slot_ix++);
    }

    // Set the locals, note that outgoing arguments are not in the environment.
    for (intptr_t i = current->Length() - 1;
         i >= current->fixed_parameter_count();
         i--) {
      builder->AddCopy(current->ValueAt(i),
                       current->LocationAt(i),
                       slot_ix++);
    }

    // Iterate on the outer environment.
    previous = current;
    current = current->outer();
  }
  // The previous pointer is now the outermost environment.
  ASSERT(previous != NULL);

  // For the outermost environment, set caller PC, caller PP, and caller FP.
  builder->AddCallerPp(slot_ix++);
  // PC marker.
  builder->AddPcMarker(previous->code(), slot_ix++);
  builder->AddCallerFp(slot_ix++);
  builder->AddCallerPc(slot_ix++);

  // For the outermost environment, set the incoming arguments.
  for (intptr_t i = previous->fixed_parameter_count() - 1; i >= 0; i--) {
    builder->AddCopy(previous->ValueAt(i), previous->LocationAt(i), slot_ix++);
  }

  const DeoptInfo& deopt_info =
      DeoptInfo::Handle(builder->CreateDeoptInfo(deopt_table));
  return deopt_info.raw();
}


void CompilerDeoptInfoWithStub::GenerateCode(FlowGraphCompiler* compiler,
                                             intptr_t stub_ix) {
  // Calls do not need stubs, they share a deoptimization trampoline.
  ASSERT(reason() != kDeoptAtCall);
  Assembler* assem = compiler->assembler();
#define __ assem->
  __ Comment("Deopt stub for id %" Pd "", deopt_id());
  __ Bind(entry_label());
  if (FLAG_trap_on_deoptimization) __ int3();

  ASSERT(deopt_env() != NULL);

  __ Call(&StubCode::DeoptimizeLabel(), PP);
  set_pc_offset(assem->CodeSize());
  __ int3();
#undef __
}


#define __ assembler()->


// Fall through if bool_register contains null.
void FlowGraphCompiler::GenerateBoolToJump(Register bool_register,
                                           Label* is_true,
                                           Label* is_false) {
  Label fall_through;
  __ CompareObject(bool_register, Object::null_object(), PP);
  __ j(EQUAL, &fall_through, Assembler::kNearJump);
  __ CompareObject(bool_register, Bool::True(), PP);
  __ j(EQUAL, is_true);
  __ jmp(is_false);
  __ Bind(&fall_through);
}


// Clobbers RCX.
RawSubtypeTestCache* FlowGraphCompiler::GenerateCallSubtypeTestStub(
    TypeTestStubKind test_kind,
    Register instance_reg,
    Register type_arguments_reg,
    Register temp_reg,
    Label* is_instance_lbl,
    Label* is_not_instance_lbl) {
  const SubtypeTestCache& type_test_cache =
      SubtypeTestCache::ZoneHandle(SubtypeTestCache::New());
  __ LoadObject(temp_reg, type_test_cache, PP);
  __ pushq(temp_reg);  // Subtype test cache.
  __ pushq(instance_reg);  // Instance.
  if (test_kind == kTestTypeOneArg) {
    ASSERT(type_arguments_reg == kNoRegister);
    __ PushObject(Object::null_object(), PP);
    __ Call(&StubCode::Subtype1TestCacheLabel(), PP);
  } else if (test_kind == kTestTypeTwoArgs) {
    ASSERT(type_arguments_reg == kNoRegister);
    __ PushObject(Object::null_object(), PP);
    __ Call(&StubCode::Subtype2TestCacheLabel(), PP);
  } else if (test_kind == kTestTypeThreeArgs) {
    __ pushq(type_arguments_reg);
    __ Call(&StubCode::Subtype3TestCacheLabel(), PP);
  } else {
    UNREACHABLE();
  }
  // Result is in RCX: null -> not found, otherwise Bool::True or Bool::False.
  ASSERT(instance_reg != RCX);
  ASSERT(temp_reg != RCX);
  __ popq(instance_reg);  // Discard.
  __ popq(instance_reg);  // Restore receiver.
  __ popq(temp_reg);  // Discard.
  GenerateBoolToJump(RCX, is_instance_lbl, is_not_instance_lbl);
  return type_test_cache.raw();
}


// Jumps to labels 'is_instance' or 'is_not_instance' respectively, if
// type test is conclusive, otherwise fallthrough if a type test could not
// be completed.
// RAX: instance (must survive).
// Clobbers R10.
RawSubtypeTestCache*
FlowGraphCompiler::GenerateInstantiatedTypeWithArgumentsTest(
    intptr_t token_pos,
    const AbstractType& type,
    Label* is_instance_lbl,
    Label* is_not_instance_lbl) {
  __ Comment("InstantiatedTypeWithArgumentsTest");
  ASSERT(type.IsInstantiated());
  const Class& type_class = Class::ZoneHandle(type.type_class());
  ASSERT((type_class.NumTypeArguments() > 0) || type_class.IsSignatureClass());
  const Register kInstanceReg = RAX;
  Error& malformed_error = Error::Handle();
  const Type& int_type = Type::Handle(Type::IntType());
  const bool smi_is_ok = int_type.IsSubtypeOf(type, &malformed_error);
  // Malformed type should have been handled at graph construction time.
  ASSERT(smi_is_ok || malformed_error.IsNull());
  __ testq(kInstanceReg, Immediate(kSmiTagMask));
  if (smi_is_ok) {
    __ j(ZERO, is_instance_lbl);
  } else {
    __ j(ZERO, is_not_instance_lbl);
  }
  const intptr_t num_type_args = type_class.NumTypeArguments();
  const intptr_t num_type_params = type_class.NumTypeParameters();
  const intptr_t from_index = num_type_args - num_type_params;
  const AbstractTypeArguments& type_arguments =
      AbstractTypeArguments::ZoneHandle(type.arguments());
  const bool is_raw_type = type_arguments.IsNull() ||
      type_arguments.IsRaw(from_index, num_type_params);
  // Signature class is an instantiated parameterized type.
  if (!type_class.IsSignatureClass()) {
    if (is_raw_type) {
      const Register kClassIdReg = R10;
      // dynamic type argument, check only classes.
      __ LoadClassId(kClassIdReg, kInstanceReg);
      __ cmpl(kClassIdReg, Immediate(type_class.id()));
      __ j(EQUAL, is_instance_lbl);
      // List is a very common case.
      if (IsListClass(type_class)) {
        GenerateListTypeCheck(kClassIdReg, is_instance_lbl);
      }
      return GenerateSubtype1TestCacheLookup(
          token_pos, type_class, is_instance_lbl, is_not_instance_lbl);
    }
    // If one type argument only, check if type argument is Object or dynamic.
    if (type_arguments.Length() == 1) {
      const AbstractType& tp_argument = AbstractType::ZoneHandle(
          type_arguments.TypeAt(0));
      ASSERT(!tp_argument.IsMalformed());
      if (tp_argument.IsType()) {
        ASSERT(tp_argument.HasResolvedTypeClass());
        // Check if type argument is dynamic or Object.
        const Type& object_type = Type::Handle(Type::ObjectType());
        if (object_type.IsSubtypeOf(tp_argument, NULL)) {
          // Instance class test only necessary.
          return GenerateSubtype1TestCacheLookup(
              token_pos, type_class, is_instance_lbl, is_not_instance_lbl);
        }
      }
    }
  }
  // Regular subtype test cache involving instance's type arguments.
  const Register kTypeArgumentsReg = kNoRegister;
  const Register kTempReg = R10;
  return GenerateCallSubtypeTestStub(kTestTypeTwoArgs,
                                     kInstanceReg,
                                     kTypeArgumentsReg,
                                     kTempReg,
                                     is_instance_lbl,
                                     is_not_instance_lbl);
}


void FlowGraphCompiler::CheckClassIds(Register class_id_reg,
                                      const GrowableArray<intptr_t>& class_ids,
                                      Label* is_equal_lbl,
                                      Label* is_not_equal_lbl) {
  for (intptr_t i = 0; i < class_ids.length(); i++) {
    __ cmpl(class_id_reg, Immediate(class_ids[i]));
    __ j(EQUAL, is_equal_lbl);
  }
  __ jmp(is_not_equal_lbl);
}


// Testing against an instantiated type with no arguments, without
// SubtypeTestCache.
// RAX: instance to test against (preserved).
// Clobbers R10, R13.
// Returns true if there is a fallthrough.
bool FlowGraphCompiler::GenerateInstantiatedTypeNoArgumentsTest(
    intptr_t token_pos,
    const AbstractType& type,
    Label* is_instance_lbl,
    Label* is_not_instance_lbl) {
  __ Comment("InstantiatedTypeNoArgumentsTest");
  ASSERT(type.IsInstantiated());
  const Class& type_class = Class::Handle(type.type_class());
  ASSERT(type_class.NumTypeArguments() == 0);

  const Register kInstanceReg = RAX;
  __ testq(kInstanceReg, Immediate(kSmiTagMask));
  // If instance is Smi, check directly.
  const Class& smi_class = Class::Handle(Smi::Class());
  if (smi_class.IsSubtypeOf(TypeArguments::Handle(),
                            type_class,
                            TypeArguments::Handle(),
                            NULL)) {
    __ j(ZERO, is_instance_lbl);
  } else {
    __ j(ZERO, is_not_instance_lbl);
  }
  // Compare if the classes are equal.
  const Register kClassIdReg = R10;
  __ LoadClassId(kClassIdReg, kInstanceReg);
  __ cmpl(kClassIdReg, Immediate(type_class.id()));
  __ j(EQUAL, is_instance_lbl);
  // See ClassFinalizer::ResolveSuperTypeAndInterfaces for list of restricted
  // interfaces.
  // Bool interface can be implemented only by core class Bool.
  if (type.IsBoolType()) {
    __ cmpl(kClassIdReg, Immediate(kBoolCid));
    __ j(EQUAL, is_instance_lbl);
    __ jmp(is_not_instance_lbl);
    return false;
  }
  if (type.IsFunctionType()) {
    // Check if instance is a closure.
    __ LoadClassById(R13, kClassIdReg);
    __ movq(R13, FieldAddress(R13, Class::signature_function_offset()));
    __ CompareObject(R13, Object::null_object(), PP);
    __ j(NOT_EQUAL, is_instance_lbl);
  }
  // Custom checking for numbers (Smi, Mint, Bigint and Double).
  // Note that instance is not Smi (checked above).
  if (type.IsSubtypeOf(Type::Handle(Type::Number()), NULL)) {
    GenerateNumberTypeCheck(
        kClassIdReg, type, is_instance_lbl, is_not_instance_lbl);
    return false;
  }
  if (type.IsStringType()) {
    GenerateStringTypeCheck(kClassIdReg, is_instance_lbl, is_not_instance_lbl);
    return false;
  }
  // Otherwise fallthrough.
  return true;
}


// Uses SubtypeTestCache to store instance class and result.
// RAX: instance to test.
// Clobbers R10, R13.
// Immediate class test already done.
// TODO(srdjan): Implement a quicker subtype check, as type test
// arrays can grow too high, but they may be useful when optimizing
// code (type-feedback).
RawSubtypeTestCache* FlowGraphCompiler::GenerateSubtype1TestCacheLookup(
    intptr_t token_pos,
    const Class& type_class,
    Label* is_instance_lbl,
    Label* is_not_instance_lbl) {
  __ Comment("Subtype1TestCacheLookup");
  const Register kInstanceReg = RAX;
  __ LoadClass(R10, kInstanceReg);
  // R10: instance class.
  // Check immediate superclass equality.
  __ movq(R13, FieldAddress(R10, Class::super_type_offset()));
  __ movq(R13, FieldAddress(R13, Type::type_class_offset()));
  __ CompareObject(R13, type_class, PP);
  __ j(EQUAL, is_instance_lbl);

  const Register kTypeArgumentsReg = kNoRegister;
  const Register kTempReg = R10;
  return GenerateCallSubtypeTestStub(kTestTypeOneArg,
                                     kInstanceReg,
                                     kTypeArgumentsReg,
                                     kTempReg,
                                     is_instance_lbl,
                                     is_not_instance_lbl);
}


// Generates inlined check if 'type' is a type parameter or type itself
// RAX: instance (preserved).
// Clobbers RDI, RDX, R10.
RawSubtypeTestCache* FlowGraphCompiler::GenerateUninstantiatedTypeTest(
    intptr_t token_pos,
    const AbstractType& type,
    Label* is_instance_lbl,
    Label* is_not_instance_lbl) {
  __ Comment("UninstantiatedTypeTest");
  ASSERT(!type.IsInstantiated());
  // Skip check if destination is a dynamic type.
  if (type.IsTypeParameter()) {
    const TypeParameter& type_param = TypeParameter::Cast(type);
    // Load instantiator (or null) and instantiator type arguments on stack.
    __ movq(RDX, Address(RSP, 0));  // Get instantiator type arguments.
    // RDX: instantiator type arguments.
    // Check if type argument is dynamic.
    __ CompareObject(RDX, Object::null_object(), PP);
    __ j(EQUAL, is_instance_lbl);
    // Can handle only type arguments that are instances of TypeArguments.
    // (runtime checks canonicalize type arguments).
    Label fall_through;
    __ CompareClassId(RDX, kTypeArgumentsCid);
    __ j(NOT_EQUAL, &fall_through);
    __ movq(RDI,
        FieldAddress(RDX, TypeArguments::type_at_offset(type_param.index())));
    // RDI: Concrete type of type.
    // Check if type argument is dynamic.
    __ CompareObject(RDI, Type::ZoneHandle(Type::DynamicType()), PP);
    __ j(EQUAL,  is_instance_lbl);
    __ CompareObject(RDI, Object::null_object(), PP);
    __ j(EQUAL,  is_instance_lbl);
    const Type& object_type = Type::ZoneHandle(Type::ObjectType());
    __ CompareObject(RDI, object_type, PP);
    __ j(EQUAL,  is_instance_lbl);

    // For Smi check quickly against int and num interfaces.
    Label not_smi;
    __ testq(RAX, Immediate(kSmiTagMask));  // Value is Smi?
    __ j(NOT_ZERO, &not_smi, Assembler::kNearJump);
    __ CompareObject(RDI, Type::ZoneHandle(Type::IntType()), PP);
    __ j(EQUAL,  is_instance_lbl);
    __ CompareObject(RDI, Type::ZoneHandle(Type::Number()), PP);
    __ j(EQUAL,  is_instance_lbl);
    // Smi must be handled in runtime.
    __ jmp(&fall_through);

    __ Bind(&not_smi);
    // RDX: instantiator type arguments.
    // RAX: instance.
    const Register kInstanceReg = RAX;
    const Register kTypeArgumentsReg = RDX;
    const Register kTempReg = R10;
    const SubtypeTestCache& type_test_cache =
        SubtypeTestCache::ZoneHandle(
            GenerateCallSubtypeTestStub(kTestTypeThreeArgs,
                                        kInstanceReg,
                                        kTypeArgumentsReg,
                                        kTempReg,
                                        is_instance_lbl,
                                        is_not_instance_lbl));
    __ Bind(&fall_through);
    return type_test_cache.raw();
  }
  if (type.IsType()) {
    const Register kInstanceReg = RAX;
    const Register kTypeArgumentsReg = RDX;
    __ testq(kInstanceReg, Immediate(kSmiTagMask));  // Is instance Smi?
    __ j(ZERO, is_not_instance_lbl);
    __ movq(kTypeArgumentsReg, Address(RSP, 0));  // Instantiator type args.
    // Uninstantiated type class is known at compile time, but the type
    // arguments are determined at runtime by the instantiator.
    const Register kTempReg = R10;
    return GenerateCallSubtypeTestStub(kTestTypeThreeArgs,
                                       kInstanceReg,
                                       kTypeArgumentsReg,
                                       kTempReg,
                                       is_instance_lbl,
                                       is_not_instance_lbl);
  }
  return SubtypeTestCache::null();
}


// Inputs:
// - RAX: instance to test against (preserved).
// - RDX: optional instantiator type arguments (preserved).
// Clobbers R10, R13.
// Returns:
// - preserved instance in RAX and optional instantiator type arguments in RDX.
// Note that this inlined code must be followed by the runtime_call code, as it
// may fall through to it. Otherwise, this inline code will jump to the label
// is_instance or to the label is_not_instance.
RawSubtypeTestCache* FlowGraphCompiler::GenerateInlineInstanceof(
    intptr_t token_pos,
    const AbstractType& type,
    Label* is_instance_lbl,
    Label* is_not_instance_lbl) {
  __ Comment("InlineInstanceof");
  if (type.IsVoidType()) {
    // A non-null value is returned from a void function, which will result in a
    // type error. A null value is handled prior to executing this inline code.
    return SubtypeTestCache::null();
  }
  if (TypeCheckAsClassEquality(type)) {
    const intptr_t type_cid = Class::Handle(type.type_class()).id();
    const Register kInstanceReg = RAX;
    __ testq(kInstanceReg, Immediate(kSmiTagMask));
    if (type_cid == kSmiCid) {
      __ j(ZERO, is_instance_lbl);
    } else {
      __ j(ZERO, is_not_instance_lbl);
      __ CompareClassId(kInstanceReg, type_cid);
      __ j(EQUAL, is_instance_lbl);
    }
    __ jmp(is_not_instance_lbl);
    return SubtypeTestCache::null();
  }
  if (type.IsInstantiated()) {
    const Class& type_class = Class::ZoneHandle(type.type_class());
    // A class equality check is only applicable with a dst type of a
    // non-parameterized class, non-signature class, or with a raw dst type of
    // a parameterized class.
    if (type_class.IsSignatureClass() || (type_class.NumTypeArguments() > 0)) {
      return GenerateInstantiatedTypeWithArgumentsTest(token_pos,
                                                       type,
                                                       is_instance_lbl,
                                                       is_not_instance_lbl);
      // Fall through to runtime call.
    }
    const bool has_fall_through =
        GenerateInstantiatedTypeNoArgumentsTest(token_pos,
                                                type,
                                                is_instance_lbl,
                                                is_not_instance_lbl);
    if (has_fall_through) {
      // If test non-conclusive so far, try the inlined type-test cache.
      // 'type' is known at compile time.
      return GenerateSubtype1TestCacheLookup(
           token_pos, type_class, is_instance_lbl, is_not_instance_lbl);
    } else {
      return SubtypeTestCache::null();
    }
  }
  return GenerateUninstantiatedTypeTest(token_pos,
                                        type,
                                        is_instance_lbl,
                                        is_not_instance_lbl);
}


// If instanceof type test cannot be performed successfully at compile time and
// therefore eliminated, optimize it by adding inlined tests for:
// - NULL -> return false.
// - Smi -> compile time subtype check (only if dst class is not parameterized).
// - Class equality (only if class is not parameterized).
// Inputs:
// - RAX: object.
// - RDX: instantiator type arguments or raw_null.
// - RCX: instantiator or raw_null.
// Clobbers RCX and RDX.
// Returns:
// - true or false in RAX.
void FlowGraphCompiler::GenerateInstanceOf(intptr_t token_pos,
                                           intptr_t deopt_id,
                                           const AbstractType& type,
                                           bool negate_result,
                                           LocationSummary* locs) {
  ASSERT(type.IsFinalized() && !type.IsMalformedOrMalbounded());

  Label is_instance, is_not_instance;
  __ pushq(RCX);  // Store instantiator on stack.
  __ pushq(RDX);  // Store instantiator type arguments.
  // If type is instantiated and non-parameterized, we can inline code
  // checking whether the tested instance is a Smi.
  if (type.IsInstantiated()) {
    // A null object is only an instance of Object and dynamic, which has
    // already been checked above (if the type is instantiated). So we can
    // return false here if the instance is null (and if the type is
    // instantiated).
    // We can only inline this null check if the type is instantiated at compile
    // time, since an uninstantiated type at compile time could be Object or
    // dynamic at run time.
    __ CompareObject(RAX, Object::null_object(), PP);
    __ j(EQUAL, &is_not_instance);
  }

  // Generate inline instanceof test.
  SubtypeTestCache& test_cache = SubtypeTestCache::ZoneHandle();
  test_cache = GenerateInlineInstanceof(token_pos, type,
                                        &is_instance, &is_not_instance);

  // test_cache is null if there is no fall-through.
  Label done;
  if (!test_cache.IsNull()) {
    // Generate runtime call.
    __ movq(RDX, Address(RSP, 0));  // Get instantiator type arguments.
    __ movq(RCX, Address(RSP, kWordSize));  // Get instantiator.
    __ PushObject(Object::ZoneHandle(), PP);  // Make room for the result.
    __ pushq(RAX);  // Push the instance.
    __ PushObject(type, PP);  // Push the type.
    __ pushq(RCX);  // TODO(srdjan): Pass instantiator instead of null.
    __ pushq(RDX);  // Instantiator type arguments.
    __ LoadObject(RAX, test_cache, PP);
    __ pushq(RAX);
    GenerateRuntimeCall(token_pos,
                        deopt_id,
                        kInstanceofRuntimeEntry,
                        5,
                        locs);
    // Pop the parameters supplied to the runtime entry. The result of the
    // instanceof runtime call will be left as the result of the operation.
    __ Drop(5);
    if (negate_result) {
      __ popq(RDX);
      __ LoadObject(RAX, Bool::True(), PP);
      __ cmpq(RDX, RAX);
      __ j(NOT_EQUAL, &done, Assembler::kNearJump);
      __ LoadObject(RAX, Bool::False(), PP);
    } else {
      __ popq(RAX);
    }
    __ jmp(&done, Assembler::kNearJump);
  }
  __ Bind(&is_not_instance);
  __ LoadObject(RAX, Bool::Get(negate_result), PP);
  __ jmp(&done, Assembler::kNearJump);

  __ Bind(&is_instance);
  __ LoadObject(RAX, Bool::Get(!negate_result), PP);
  __ Bind(&done);
  __ popq(RDX);  // Remove pushed instantiator type arguments.
  __ popq(RCX);  // Remove pushed instantiator.
}


// Optimize assignable type check by adding inlined tests for:
// - NULL -> return NULL.
// - Smi -> compile time subtype check (only if dst class is not parameterized).
// - Class equality (only if class is not parameterized).
// Inputs:
// - RAX: object.
// - RDX: instantiator type arguments or raw_null.
// - RCX: instantiator or raw_null.
// Returns:
// - object in RAX for successful assignable check (or throws TypeError).
// Performance notes: positive checks must be quick, negative checks can be slow
// as they throw an exception.
void FlowGraphCompiler::GenerateAssertAssignable(intptr_t token_pos,
                                                 intptr_t deopt_id,
                                                 const AbstractType& dst_type,
                                                 const String& dst_name,
                                                 LocationSummary* locs) {
  ASSERT(token_pos >= 0);
  ASSERT(!dst_type.IsNull());
  ASSERT(dst_type.IsFinalized());
  // Assignable check is skipped in FlowGraphBuilder, not here.
  ASSERT(dst_type.IsMalformedOrMalbounded() ||
         (!dst_type.IsDynamicType() && !dst_type.IsObjectType()));
  __ pushq(RCX);  // Store instantiator.
  __ pushq(RDX);  // Store instantiator type arguments.
  // A null object is always assignable and is returned as result.
  Label is_assignable, runtime_call;
  __ CompareObject(RAX, Object::null_object(), PP);
  __ j(EQUAL, &is_assignable);

  if (!FLAG_eliminate_type_checks || dst_type.IsMalformed()) {
    // If type checks are not eliminated during the graph building then
    // a transition sentinel can be seen here.
    __ CompareObject(RAX, Object::transition_sentinel(), PP);
    __ j(EQUAL, &is_assignable);
  }

  // Generate throw new TypeError() if the type is malformed or malbounded.
  if (dst_type.IsMalformedOrMalbounded()) {
    __ PushObject(Object::ZoneHandle(), PP);  // Make room for the result.
    __ pushq(RAX);  // Push the source object.
    __ PushObject(dst_name, PP);  // Push the name of the destination.
    __ PushObject(dst_type, PP);  // Push the type of the destination.
    GenerateRuntimeCall(token_pos,
                        deopt_id,
                        kBadTypeErrorRuntimeEntry,
                        3,
                        locs);
    // We should never return here.
    __ int3();

    __ Bind(&is_assignable);  // For a null object.
    __ popq(RDX);  // Remove pushed instantiator type arguments.
    __ popq(RCX);  // Remove pushed instantiator.
    return;
  }

  // Generate inline type check, linking to runtime call if not assignable.
  SubtypeTestCache& test_cache = SubtypeTestCache::ZoneHandle();
  test_cache = GenerateInlineInstanceof(token_pos, dst_type,
                                        &is_assignable, &runtime_call);

  __ Bind(&runtime_call);
  __ movq(RDX, Address(RSP, 0));  // Get instantiator type arguments.
  __ movq(RCX, Address(RSP, kWordSize));  // Get instantiator.
  __ PushObject(Object::ZoneHandle(), PP);  // Make room for the result.
  __ pushq(RAX);  // Push the source object.
  __ PushObject(dst_type, PP);  // Push the type of the destination.
  __ pushq(RCX);  // Instantiator.
  __ pushq(RDX);  // Instantiator type arguments.
  __ PushObject(dst_name, PP);  // Push the name of the destination.
  __ LoadObject(RAX, test_cache, PP);
  __ pushq(RAX);
  GenerateRuntimeCall(token_pos, deopt_id, kTypeCheckRuntimeEntry, 6, locs);
  // Pop the parameters supplied to the runtime entry. The result of the
  // type check runtime call is the checked value.
  __ Drop(6);
  __ popq(RAX);

  __ Bind(&is_assignable);
  __ popq(RDX);  // Remove pushed instantiator type arguments.
  __ popq(RCX);  // Remove pushed instantiator.
}


void FlowGraphCompiler::EmitTrySyncMove(intptr_t dest_offset,
                                        Location loc,
                                        bool* push_emitted) {
  const Address dest(RBP, dest_offset);
  if (loc.IsConstant()) {
    if (!*push_emitted) {
      __ pushq(RAX);
      *push_emitted = true;
    }
    __ LoadObject(RAX, loc.constant(), PP);
    __ movq(dest, RAX);
  } else if (loc.IsRegister()) {
    if (*push_emitted && loc.reg() == RAX) {
      __ movq(RAX, Address(RSP, 0));
      __ movq(dest, RAX);
    } else {
      __ movq(dest, loc.reg());
    }
  } else {
    Address src = loc.ToStackSlotAddress();
    if (!src.Equals(dest)) {
      if (!*push_emitted) {
        __ pushq(RAX);
        *push_emitted = true;
      }
      __ movq(RAX, src);
      __ movq(dest, RAX);
    }
  }
}


void FlowGraphCompiler::EmitTrySync(Instruction* instr, intptr_t try_index) {
  ASSERT(is_optimizing());
  Environment* env = instr->env();
  CatchBlockEntryInstr* catch_block =
      flow_graph().graph_entry()->GetCatchEntry(try_index);
  const GrowableArray<Definition*>* idefs = catch_block->initial_definitions();
  // Parameters.
  intptr_t i = 0;
  bool push_emitted = false;
  const intptr_t num_non_copied_params = flow_graph().num_non_copied_params();
  const intptr_t param_base = kParamEndSlotFromFp + num_non_copied_params;
  for (; i < num_non_copied_params; ++i) {
    if ((*idefs)[i]->IsConstant()) continue;  // Common constants
    Location loc = env->LocationAt(i);
    EmitTrySyncMove((param_base - i) * kWordSize, loc, &push_emitted);
  }

  // Process locals. Skip exception_var and stacktrace_var.
  intptr_t local_base = kFirstLocalSlotFromFp + num_non_copied_params;
  intptr_t ex_idx = local_base - catch_block->exception_var().index();
  intptr_t st_idx = local_base - catch_block->stacktrace_var().index();
  for (; i < flow_graph().variable_count(); ++i) {
    if (i == ex_idx || i == st_idx) continue;
    if ((*idefs)[i]->IsConstant()) continue;
    Location loc = env->LocationAt(i);
    EmitTrySyncMove((local_base - i) * kWordSize, loc, &push_emitted);
    // Update safepoint bitmap to indicate that the target location
    // now contains a pointer.
    instr->locs()->stack_bitmap()->Set(i - num_non_copied_params, true);
  }
  if (push_emitted) {
    __ popq(RAX);
  }
}


void FlowGraphCompiler::EmitInstructionEpilogue(Instruction* instr) {
  if (is_optimizing()) return;
  Definition* defn = instr->AsDefinition();
  if ((defn != NULL) && defn->is_used()) {
    __ pushq(defn->locs()->out().reg());
  }
}


void FlowGraphCompiler::CopyParameters() {
  __ Comment("Copy parameters");
  const Function& function = parsed_function().function();
  LocalScope* scope = parsed_function().node_sequence()->scope();
  const int num_fixed_params = function.num_fixed_parameters();
  const int num_opt_pos_params = function.NumOptionalPositionalParameters();
  const int num_opt_named_params = function.NumOptionalNamedParameters();
  const int num_params =
      num_fixed_params + num_opt_pos_params + num_opt_named_params;
  ASSERT(function.NumParameters() == num_params);
  ASSERT(parsed_function().first_parameter_index() == kFirstLocalSlotFromFp);

  // Check that min_num_pos_args <= num_pos_args <= max_num_pos_args,
  // where num_pos_args is the number of positional arguments passed in.
  const int min_num_pos_args = num_fixed_params;
  const int max_num_pos_args = num_fixed_params + num_opt_pos_params;

  __ movq(RCX,
          FieldAddress(R10, ArgumentsDescriptor::positional_count_offset()));
  // Check that min_num_pos_args <= num_pos_args.
  Label wrong_num_arguments;
  __ CompareImmediate(RCX, Immediate(Smi::RawValue(min_num_pos_args)), PP);
  __ j(LESS, &wrong_num_arguments);
  // Check that num_pos_args <= max_num_pos_args.
  __ CompareImmediate(RCX, Immediate(Smi::RawValue(max_num_pos_args)), PP);
  __ j(GREATER, &wrong_num_arguments);

  // Copy positional arguments.
  // Argument i passed at fp[kParamEndSlotFromFp + num_args - i] is copied
  // to fp[kFirstLocalSlotFromFp - i].

  __ movq(RBX, FieldAddress(R10, ArgumentsDescriptor::count_offset()));
  // Since RBX and RCX are Smi, use TIMES_4 instead of TIMES_8.
  // Let RBX point to the last passed positional argument, i.e. to
  // fp[kParamEndSlotFromFp + num_args - (num_pos_args - 1)].
  __ subq(RBX, RCX);
  __ leaq(RBX, Address(RBP, RBX, TIMES_4,
                       (kParamEndSlotFromFp + 1) * kWordSize));

  // Let RDI point to the last copied positional argument, i.e. to
  // fp[kFirstLocalSlotFromFp - (num_pos_args - 1)].
  __ SmiUntag(RCX);
  __ movq(RAX, RCX);
  __ negq(RAX);
  // -num_pos_args is in RAX.
  __ leaq(RDI,
          Address(RBP, RAX, TIMES_8, (kFirstLocalSlotFromFp + 1) * kWordSize));
  Label loop, loop_condition;
  __ jmp(&loop_condition, Assembler::kNearJump);
  // We do not use the final allocation index of the variable here, i.e.
  // scope->VariableAt(i)->index(), because captured variables still need
  // to be copied to the context that is not yet allocated.
  const Address argument_addr(RBX, RCX, TIMES_8, 0);
  const Address copy_addr(RDI, RCX, TIMES_8, 0);
  __ Bind(&loop);
  __ movq(RAX, argument_addr);
  __ movq(copy_addr, RAX);
  __ Bind(&loop_condition);
  __ decq(RCX);
  __ j(POSITIVE, &loop, Assembler::kNearJump);

  // Copy or initialize optional named arguments.
  Label all_arguments_processed;
#ifdef DEBUG
    const bool check_correct_named_args = true;
#else
    const bool check_correct_named_args = function.IsClosureFunction();
#endif
  if (num_opt_named_params > 0) {
    // Start by alphabetically sorting the names of the optional parameters.
    LocalVariable** opt_param = new LocalVariable*[num_opt_named_params];
    int* opt_param_position = new int[num_opt_named_params];
    for (int pos = num_fixed_params; pos < num_params; pos++) {
      LocalVariable* parameter = scope->VariableAt(pos);
      const String& opt_param_name = parameter->name();
      int i = pos - num_fixed_params;
      while (--i >= 0) {
        LocalVariable* param_i = opt_param[i];
        const intptr_t result = opt_param_name.CompareTo(param_i->name());
        ASSERT(result != 0);
        if (result > 0) break;
        opt_param[i + 1] = opt_param[i];
        opt_param_position[i + 1] = opt_param_position[i];
      }
      opt_param[i + 1] = parameter;
      opt_param_position[i + 1] = pos;
    }
    // Generate code handling each optional parameter in alphabetical order.
    __ movq(RBX, FieldAddress(R10, ArgumentsDescriptor::count_offset()));
    __ movq(RCX,
            FieldAddress(R10, ArgumentsDescriptor::positional_count_offset()));
    __ SmiUntag(RCX);
    // Let RBX point to the first passed argument, i.e. to
    // fp[kParamEndSlotFromFp + num_args]; num_args (RBX) is Smi.
    __ leaq(RBX,
            Address(RBP, RBX, TIMES_4, kParamEndSlotFromFp * kWordSize));
    // Let RDI point to the entry of the first named argument.
    __ leaq(RDI,
            FieldAddress(R10, ArgumentsDescriptor::first_named_entry_offset()));
    for (int i = 0; i < num_opt_named_params; i++) {
      Label load_default_value, assign_optional_parameter;
      const int param_pos = opt_param_position[i];
      // Check if this named parameter was passed in.
      // Load RAX with the name of the argument.
      __ movq(RAX, Address(RDI, ArgumentsDescriptor::name_offset()));
      ASSERT(opt_param[i]->name().IsSymbol());
      __ CompareObject(RAX, opt_param[i]->name(), PP);
      __ j(NOT_EQUAL, &load_default_value, Assembler::kNearJump);
      // Load RAX with passed-in argument at provided arg_pos, i.e. at
      // fp[kParamEndSlotFromFp + num_args - arg_pos].
      __ movq(RAX, Address(RDI, ArgumentsDescriptor::position_offset()));
      // RAX is arg_pos as Smi.
      // Point to next named entry.
      __ AddImmediate(
          RDI, Immediate(ArgumentsDescriptor::named_entry_size()), PP);
      __ negq(RAX);
      Address argument_addr(RBX, RAX, TIMES_4, 0);  // RAX is a negative Smi.
      __ movq(RAX, argument_addr);
      __ jmp(&assign_optional_parameter, Assembler::kNearJump);
      __ Bind(&load_default_value);
      // Load RAX with default argument.
      const Object& value = Object::ZoneHandle(
          parsed_function().default_parameter_values().At(
              param_pos - num_fixed_params));
      __ LoadObject(RAX, value, PP);
      __ Bind(&assign_optional_parameter);
      // Assign RAX to fp[kFirstLocalSlotFromFp - param_pos].
      // We do not use the final allocation index of the variable here, i.e.
      // scope->VariableAt(i)->index(), because captured variables still need
      // to be copied to the context that is not yet allocated.
      const intptr_t computed_param_pos = kFirstLocalSlotFromFp - param_pos;
      const Address param_addr(RBP, computed_param_pos * kWordSize);
      __ movq(param_addr, RAX);
    }
    delete[] opt_param;
    delete[] opt_param_position;
    if (check_correct_named_args) {
      // Check that RDI now points to the null terminator in the arguments
      // descriptor.
      __ LoadObject(TMP, Object::null_object(), PP);
      __ cmpq(Address(RDI, 0), TMP);
      __ j(EQUAL, &all_arguments_processed, Assembler::kNearJump);
    }
  } else {
    ASSERT(num_opt_pos_params > 0);
    __ movq(RCX,
            FieldAddress(R10, ArgumentsDescriptor::positional_count_offset()));
    __ SmiUntag(RCX);
    for (int i = 0; i < num_opt_pos_params; i++) {
      Label next_parameter;
      // Handle this optional positional parameter only if k or fewer positional
      // arguments have been passed, where k is param_pos, the position of this
      // optional parameter in the formal parameter list.
      const int param_pos = num_fixed_params + i;
      __ CompareImmediate(RCX, Immediate(param_pos), PP);
      __ j(GREATER, &next_parameter, Assembler::kNearJump);
      // Load RAX with default argument.
      const Object& value = Object::ZoneHandle(
          parsed_function().default_parameter_values().At(i));
      __ LoadObject(RAX, value, PP);
      // Assign RAX to fp[kFirstLocalSlotFromFp - param_pos].
      // We do not use the final allocation index of the variable here, i.e.
      // scope->VariableAt(i)->index(), because captured variables still need
      // to be copied to the context that is not yet allocated.
      const intptr_t computed_param_pos = kFirstLocalSlotFromFp - param_pos;
      const Address param_addr(RBP, computed_param_pos * kWordSize);
      __ movq(param_addr, RAX);
      __ Bind(&next_parameter);
    }
    if (check_correct_named_args) {
      __ movq(RBX, FieldAddress(R10, ArgumentsDescriptor::count_offset()));
      __ SmiUntag(RBX);
      // Check that RCX equals RBX, i.e. no named arguments passed.
      __ cmpq(RCX, RBX);
      __ j(EQUAL, &all_arguments_processed, Assembler::kNearJump);
    }
  }

  __ Bind(&wrong_num_arguments);
  if (function.IsClosureFunction()) {
    // Invoke noSuchMethod function passing "call" as the original name.
    const int kNumArgsChecked = 1;
    const ICData& ic_data = ICData::ZoneHandle(
        ICData::New(function, Symbols::Call(), Object::empty_array(),
                    Isolate::kNoDeoptId, kNumArgsChecked));
    __ LoadObject(RBX, ic_data, PP);
    __ LeaveDartFrame();  // The arguments are still on the stack.
    __ jmp(&StubCode::CallNoSuchMethodFunctionLabel());
    // The noSuchMethod call may return to the caller, but not here.
    __ int3();
  } else if (check_correct_named_args) {
    __ Stop("Wrong arguments");
  }

  __ Bind(&all_arguments_processed);
  // Nullify originally passed arguments only after they have been copied and
  // checked, otherwise noSuchMethod would not see their original values.
  // This step can be skipped in case we decide that formal parameters are
  // implicitly final, since garbage collecting the unmodified value is not
  // an issue anymore.

  // R10 : arguments descriptor array.
  __ movq(RCX, FieldAddress(R10, ArgumentsDescriptor::count_offset()));
  __ SmiUntag(RCX);
  __ LoadObject(R12, Object::null_object(), PP);
  Label null_args_loop, null_args_loop_condition;
  __ jmp(&null_args_loop_condition, Assembler::kNearJump);
  const Address original_argument_addr(
      RBP, RCX, TIMES_8, (kParamEndSlotFromFp + 1) * kWordSize);
  __ Bind(&null_args_loop);
  __ movq(original_argument_addr, R12);
  __ Bind(&null_args_loop_condition);
  __ decq(RCX);
  __ j(POSITIVE, &null_args_loop, Assembler::kNearJump);
}


void FlowGraphCompiler::GenerateInlinedGetter(intptr_t offset) {
  // TOS: return address.
  // +1 : receiver.
  // Sequence node has one return node, its input is load field node.
  __ Comment("Inlined Getter");
  __ movq(RAX, Address(RSP, 1 * kWordSize));
  __ movq(RAX, FieldAddress(RAX, offset));
  __ ret();
}


void FlowGraphCompiler::GenerateInlinedSetter(intptr_t offset) {
  // TOS: return address.
  // +1 : value
  // +2 : receiver.
  // Sequence node has one store node and one return NULL node.
  __ Comment("Inlined Setter");
  __ movq(RAX, Address(RSP, 2 * kWordSize));  // Receiver.
  __ movq(RBX, Address(RSP, 1 * kWordSize));  // Value.
  __ StoreIntoObject(RAX, FieldAddress(RAX, offset), RBX);
  __ LoadObject(RAX, Object::null_object(), PP);
  __ ret();
}


void FlowGraphCompiler::EmitFrameEntry() {
  const Function& function = parsed_function().function();
  Register new_pp = kNoRegister;
  Register new_pc = kNoRegister;
  if (CanOptimizeFunction() &&
      function.is_optimizable() &&
      (!is_optimizing() || may_reoptimize())) {
    const Register function_reg = RDI;
    new_pp = R13;
    new_pc = R12;

    Label next;
    __ nop(4);  // Need a fixed size sequence on frame entry.
    __ call(&next);
    __ Bind(&next);

    const intptr_t object_pool_pc_dist =
        Instructions::HeaderSize() - Instructions::object_pool_offset() +
        __ CodeSize();
    const intptr_t offset =
        Assembler::kEntryPointToPcMarkerOffset - __ CodeSize();
    __ popq(new_pc);
    if (offset != 0) {
      __ addq(new_pc, Immediate(offset));
    }

    // Load callee's pool pointer.
    __ movq(new_pp, Address(new_pc, -object_pool_pc_dist - offset));

    // Load function object using the callee's pool pointer.
    __ LoadObject(function_reg, function, new_pp);

    // Patch point is after the eventually inlined function object.
    AddCurrentDescriptor(PcDescriptors::kEntryPatch,
                         Isolate::kNoDeoptId,
                         0);  // No token position.
    if (is_optimizing()) {
      // Reoptimization of an optimized function is triggered by counting in
      // IC stubs, but not at the entry of the function.
      __ CompareImmediate(
          FieldAddress(function_reg, Function::usage_counter_offset()),
          Immediate(FLAG_reoptimization_counter_threshold),
          new_pp);
    } else {
      __ incq(FieldAddress(function_reg, Function::usage_counter_offset()));
      __ CompareImmediate(
          FieldAddress(function_reg, Function::usage_counter_offset()),
          Immediate(FLAG_optimization_counter_threshold),
          new_pp);
    }
    ASSERT(function_reg == RDI);
    __ J(GREATER_EQUAL, &StubCode::OptimizeFunctionLabel(), R13);
  } else if (!flow_graph().IsCompiledForOsr()) {
    // We have to load the PP here too because a load of an external label
    // may be patched at the AddCurrentDescriptor below.
    new_pp = R13;
    new_pc = R12;

    Label next;
    __ nop(4);  // Need a fixed size sequence on frame entry.
    __ call(&next);
    __ Bind(&next);

    const intptr_t object_pool_pc_dist =
        Instructions::HeaderSize() - Instructions::object_pool_offset() +
        __ CodeSize();
    const intptr_t offset =
        Assembler::kEntryPointToPcMarkerOffset - __ CodeSize();
    __ popq(new_pc);
    if (offset != 0) {
      __ addq(new_pc, Immediate(offset));
    }

    // Load callee's pool pointer.
    __ movq(new_pp, Address(new_pc, -object_pool_pc_dist - offset));
    AddCurrentDescriptor(PcDescriptors::kEntryPatch,
                         Isolate::kNoDeoptId,
                         0);  // No token position.
  }
  __ Comment("Enter frame");
  if (flow_graph().IsCompiledForOsr()) {
    intptr_t extra_slots = StackSize()
        - flow_graph().num_stack_locals()
        - flow_graph().num_copied_params();
    ASSERT(extra_slots >= 0);
    __ EnterOsrFrame(extra_slots * kWordSize, new_pp, new_pc);
  } else {
    ASSERT(StackSize() >= 0);
    __ EnterDartFrameWithInfo(StackSize() * kWordSize, new_pp, new_pc);
  }
}


void FlowGraphCompiler::CompileGraph() {
  InitCompiler();

  TryIntrinsify();

  EmitFrameEntry();

  const Function& function = parsed_function().function();

  const int num_fixed_params = function.num_fixed_parameters();
  const int num_copied_params = parsed_function().num_copied_params();
  const int num_locals = parsed_function().num_stack_locals();

  // We check the number of passed arguments when we have to copy them due to
  // the presence of optional parameters.
  // No such checking code is generated if only fixed parameters are declared,
  // unless we are in debug mode or unless we are compiling a closure.
  if (num_copied_params == 0) {
#ifdef DEBUG
    ASSERT(!parsed_function().function().HasOptionalParameters());
    const bool check_arguments = !flow_graph().IsCompiledForOsr();
#else
    const bool check_arguments =
        function.IsClosureFunction() && !flow_graph().IsCompiledForOsr();
#endif
    if (check_arguments) {
      __ Comment("Check argument count");
      // Check that exactly num_fixed arguments are passed in.
      Label correct_num_arguments, wrong_num_arguments;
      __ movq(RAX, FieldAddress(R10, ArgumentsDescriptor::count_offset()));
      __ CompareImmediate(RAX, Immediate(Smi::RawValue(num_fixed_params)), PP);
      __ j(NOT_EQUAL, &wrong_num_arguments, Assembler::kNearJump);
      __ cmpq(RAX,
              FieldAddress(R10,
                           ArgumentsDescriptor::positional_count_offset()));
      __ j(EQUAL, &correct_num_arguments, Assembler::kNearJump);

      __ Bind(&wrong_num_arguments);
      if (function.IsClosureFunction()) {
        // Invoke noSuchMethod function passing the original function name.
        // For closure functions, use "call" as the original name.
        const String& name =
            String::Handle(function.IsClosureFunction()
                             ? Symbols::Call().raw()
                             : function.name());
        const int kNumArgsChecked = 1;
        const ICData& ic_data = ICData::ZoneHandle(
            ICData::New(function, name, Object::empty_array(),
                        Isolate::kNoDeoptId, kNumArgsChecked));
        __ LoadObject(RBX, ic_data, PP);
        __ LeaveDartFrame();  // The arguments are still on the stack.
        __ jmp(&StubCode::CallNoSuchMethodFunctionLabel());
        // The noSuchMethod call may return to the caller, but not here.
        __ int3();
      } else {
        __ Stop("Wrong number of arguments");
      }
      __ Bind(&correct_num_arguments);
    }
  } else if (!flow_graph().IsCompiledForOsr()) {
    CopyParameters();
  }

  // In unoptimized code, initialize (non-argument) stack allocated slots to
  // null.
  if (!is_optimizing() && (num_locals > 0)) {
    __ Comment("Initialize spill slots");
    const intptr_t slot_base = parsed_function().first_stack_local_index();
    __ LoadObject(RAX, Object::null_object(), PP);
    for (intptr_t i = 0; i < num_locals; ++i) {
      // Subtract index i (locals lie at lower addresses than RBP).
      __ movq(Address(RBP, (slot_base - i) * kWordSize), RAX);
    }
  }

  ASSERT(!block_order().is_empty());
  VisitBlocks();

  __ int3();
  GenerateDeferredCode();
  // Emit function patching code. This will be swapped with the first 13 bytes
  // at entry point.
  AddCurrentDescriptor(PcDescriptors::kPatchCode,
                       Isolate::kNoDeoptId,
                       0);  // No token position.
  // This is patched up to a point in FrameEntry where the PP for the
  // current function is in R13 instead of PP.
  __ JmpPatchable(&StubCode::FixCallersTargetLabel(), R13);

  // TOOD(zra): Is this descriptor used?
  AddCurrentDescriptor(PcDescriptors::kLazyDeoptJump,
                       Isolate::kNoDeoptId,
                       0);  // No token position.
  __ Jmp(&StubCode::DeoptimizeLazyLabel(), PP);
}


void FlowGraphCompiler::GenerateCall(intptr_t token_pos,
                                     const ExternalLabel* label,
                                     PcDescriptors::Kind kind,
                                     LocationSummary* locs) {
  __ Call(label, PP);
  AddCurrentDescriptor(kind, Isolate::kNoDeoptId, token_pos);
  RecordSafepoint(locs);
}


void FlowGraphCompiler::GenerateDartCall(intptr_t deopt_id,
                                         intptr_t token_pos,
                                         const ExternalLabel* label,
                                         PcDescriptors::Kind kind,
                                         LocationSummary* locs) {
  __ CallPatchable(label);
  AddCurrentDescriptor(kind, deopt_id, token_pos);
  RecordSafepoint(locs);
  // Marks either the continuation point in unoptimized code or the
  // deoptimization point in optimized code, after call.
  const intptr_t deopt_id_after = Isolate::ToDeoptAfter(deopt_id);
  if (is_optimizing()) {
    AddDeoptIndexAtCall(deopt_id_after, token_pos);
  } else {
    // Add deoptimization continuation point after the call and before the
    // arguments are removed.
    AddCurrentDescriptor(PcDescriptors::kDeopt, deopt_id_after, token_pos);
  }
}


void FlowGraphCompiler::GenerateRuntimeCall(intptr_t token_pos,
                                            intptr_t deopt_id,
                                            const RuntimeEntry& entry,
                                            intptr_t argument_count,
                                            LocationSummary* locs) {
  __ CallRuntime(entry, argument_count);
  AddCurrentDescriptor(PcDescriptors::kOther, deopt_id, token_pos);
  RecordSafepoint(locs);
  if (deopt_id != Isolate::kNoDeoptId) {
    // Marks either the continuation point in unoptimized code or the
    // deoptimization point in optimized code, after call.
    const intptr_t deopt_id_after = Isolate::ToDeoptAfter(deopt_id);
    if (is_optimizing()) {
      AddDeoptIndexAtCall(deopt_id_after, token_pos);
    } else {
      // Add deoptimization continuation point after the call and before the
      // arguments are removed.
      AddCurrentDescriptor(PcDescriptors::kDeopt, deopt_id_after, token_pos);
    }
  }
}


void FlowGraphCompiler::EmitUnoptimizedStaticCall(
    const Function& target_function,
    const Array& arguments_descriptor,
    intptr_t argument_count,
    intptr_t deopt_id,
    intptr_t token_pos,
    LocationSummary* locs) {
  // TODO(srdjan): Improve performance of function recognition.
  MethodRecognizer::Kind recognized_kind =
      MethodRecognizer::RecognizeKind(target_function);
  int num_args_checked = 0;
  if ((recognized_kind == MethodRecognizer::kMathMin) ||
      (recognized_kind == MethodRecognizer::kMathMax)) {
    num_args_checked = 2;
  }
  const ICData& ic_data = ICData::ZoneHandle(
      ICData::New(parsed_function().function(),  // Caller function.
                  String::Handle(target_function.name()),
                  arguments_descriptor,
                  deopt_id,
                  num_args_checked));  // No arguments checked.
  ic_data.AddTarget(target_function);
  uword label_address = 0;
  if (ic_data.num_args_tested() == 0) {
    label_address = StubCode::ZeroArgsUnoptimizedStaticCallEntryPoint();
  } else if (ic_data.num_args_tested() == 2) {
    label_address = StubCode::TwoArgsUnoptimizedStaticCallEntryPoint();
  } else {
    UNIMPLEMENTED();
  }
  ExternalLabel target_label("StaticCallICStub", label_address);
  __ LoadObject(RBX, ic_data, PP);
  GenerateDartCall(deopt_id,
                   token_pos,
                   &target_label,
                   PcDescriptors::kUnoptStaticCall,
                   locs);
  __ Drop(argument_count);
}


void FlowGraphCompiler::EmitEdgeCounter() {
  // We do not check for overflow when incrementing the edge counter.  The
  // function should normally be optimized long before the counter can
  // overflow; and though we do not reset the counters when we optimize or
  // deoptimize, there is a bound on the number of
  // optimization/deoptimization cycles we will attempt.
  const Array& counter = Array::ZoneHandle(Array::New(1, Heap::kOld));
  counter.SetAt(0, Smi::Handle(Smi::New(0)));
  __ Comment("Edge counter");
  __ LoadObject(RAX, counter, PP);
  __ AddImmediate(FieldAddress(RAX, Array::element_offset(0)),
                  Immediate(Smi::RawValue(1)), PP);
}


void FlowGraphCompiler::EmitOptimizedInstanceCall(
    ExternalLabel* target_label,
    const ICData& ic_data,
    intptr_t argument_count,
    intptr_t deopt_id,
    intptr_t token_pos,
    LocationSummary* locs) {
  // Each ICData propagated from unoptimized to optimized code contains the
  // function that corresponds to the Dart function of that IC call. Due
  // to inlining in optimized code, that function may not correspond to the
  // top-level function (parsed_function().function()) which could be
  // reoptimized and which counter needs to be incremented.
  // Pass the function explicitly, it is used in IC stub.
  __ LoadObject(RDI, parsed_function().function(), PP);
  __ LoadObject(RBX, ic_data, PP);
  GenerateDartCall(deopt_id,
                   token_pos,
                   target_label,
                   PcDescriptors::kIcCall,
                   locs);
  __ Drop(argument_count);
}


void FlowGraphCompiler::EmitInstanceCall(ExternalLabel* target_label,
                                         const ICData& ic_data,
                                         intptr_t argument_count,
                                         intptr_t deopt_id,
                                         intptr_t token_pos,
                                         LocationSummary* locs) {
  __ LoadObject(RBX, ic_data, PP);
  GenerateDartCall(deopt_id,
                   token_pos,
                   target_label,
                   PcDescriptors::kIcCall,
                   locs);
  __ Drop(argument_count);
}


void FlowGraphCompiler::EmitMegamorphicInstanceCall(
    const ICData& ic_data,
    intptr_t argument_count,
    intptr_t deopt_id,
    intptr_t token_pos,
    LocationSummary* locs) {
  MegamorphicCacheTable* table = Isolate::Current()->megamorphic_cache_table();
  const String& name = String::Handle(ic_data.target_name());
  const Array& arguments_descriptor =
      Array::ZoneHandle(ic_data.arguments_descriptor());
  ASSERT(!arguments_descriptor.IsNull());
  const MegamorphicCache& cache =
      MegamorphicCache::ZoneHandle(table->Lookup(name, arguments_descriptor));
  Label not_smi, load_cache;
  __ movq(RAX, Address(RSP, (argument_count - 1) * kWordSize));
  __ testq(RAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &not_smi, Assembler::kNearJump);
  __ LoadImmediate(RAX, Immediate(Smi::RawValue(kSmiCid)), PP);
  __ jmp(&load_cache);

  __ Bind(&not_smi);
  __ LoadClassId(RAX, RAX);
  __ SmiTag(RAX);

  // RAX: class ID of the receiver (smi).
  __ Bind(&load_cache);
  __ LoadObject(RBX, cache, PP);
  __ movq(RDI, FieldAddress(RBX, MegamorphicCache::buckets_offset()));
  __ movq(RBX, FieldAddress(RBX, MegamorphicCache::mask_offset()));
  // RDI: cache buckets array.
  // RBX: mask.
  __ movq(RCX, RAX);

  Label loop, update, call_target_function;
  __ jmp(&loop);

  __ Bind(&update);
  __ AddImmediate(RCX, Immediate(Smi::RawValue(1)), PP);
  __ Bind(&loop);
  __ andq(RCX, RBX);
  const intptr_t base = Array::data_offset();
  // RCX is smi tagged, but table entries are two words, so TIMES_8.
  __ movq(RDX, FieldAddress(RDI, RCX, TIMES_8, base));

  ASSERT(kIllegalCid == 0);
  __ testq(RDX, RDX);
  __ j(ZERO, &call_target_function, Assembler::kNearJump);
  __ cmpq(RDX, RAX);
  __ j(NOT_EQUAL, &update, Assembler::kNearJump);

  __ Bind(&call_target_function);
  // Call the target found in the cache.  For a class id match, this is a
  // proper target for the given name and arguments descriptor.  If the
  // illegal class id was found, the target is a cache miss handler that can
  // be invoked as a normal Dart function.
  __ movq(RAX, FieldAddress(RDI, RCX, TIMES_8, base + kWordSize));
  __ movq(RBX, FieldAddress(RAX, Function::code_offset()));
  if (FLAG_collect_code) {
    // If we are collecting code, the code object may be null.
    Label is_compiled;
    const Immediate& raw_null =
        Immediate(reinterpret_cast<intptr_t>(Object::null()));
    __ cmpq(RBX, raw_null);
    __ j(NOT_EQUAL, &is_compiled, Assembler::kNearJump);
    __ call(&StubCode::CompileFunctionRuntimeCallLabel());
    AddCurrentDescriptor(PcDescriptors::kRuntimeCall,
                         Isolate::kNoDeoptId,
                         token_pos);
    RecordSafepoint(locs);
    __ movq(RBX, FieldAddress(RAX, Function::code_offset()));
    __ Bind(&is_compiled);
  }
  __ movq(RAX, FieldAddress(RBX, Code::instructions_offset()));
  __ LoadObject(RBX, ic_data, PP);
  __ LoadObject(R10, arguments_descriptor, PP);
  __ AddImmediate(
      RAX, Immediate(Instructions::HeaderSize() - kHeapObjectTag), PP);
  __ call(RAX);
  AddCurrentDescriptor(PcDescriptors::kOther, Isolate::kNoDeoptId, token_pos);
  RecordSafepoint(locs);
  AddDeoptIndexAtCall(Isolate::ToDeoptAfter(deopt_id), token_pos);
  __ Drop(argument_count);
}


void FlowGraphCompiler::EmitOptimizedStaticCall(
    const Function& function,
    const Array& arguments_descriptor,
    intptr_t argument_count,
    intptr_t deopt_id,
    intptr_t token_pos,
    LocationSummary* locs) {
  __ LoadObject(R10, arguments_descriptor, PP);
  // Do not use the code from the function, but let the code be patched so that
  // we can record the outgoing edges to other code.
  GenerateDartCall(deopt_id,
                   token_pos,
                   &StubCode::CallStaticFunctionLabel(),
                   PcDescriptors::kOptStaticCall,
                   locs);
  AddStaticCallTarget(function);
  __ Drop(argument_count);
}


void FlowGraphCompiler::EmitEqualityRegConstCompare(Register reg,
                                                    const Object& obj,
                                                    bool needs_number_check,
                                                    intptr_t token_pos) {
  ASSERT(!needs_number_check ||
         (!obj.IsMint() && !obj.IsDouble() && !obj.IsBigint()));

  if (obj.IsSmi() && (Smi::Cast(obj).Value() == 0)) {
    ASSERT(!needs_number_check);
    __ testq(reg, reg);
    return;
  }

  if (needs_number_check) {
    __ pushq(reg);
    __ PushObject(obj, PP);
    if (is_optimizing()) {
      __ CallPatchable(&StubCode::OptimizedIdenticalWithNumberCheckLabel());
    } else {
      __ CallPatchable(&StubCode::UnoptimizedIdenticalWithNumberCheckLabel());
    }
    AddCurrentDescriptor(PcDescriptors::kRuntimeCall,
                         Isolate::kNoDeoptId,
                         token_pos);
    __ popq(reg);  // Discard constant.
    __ popq(reg);  // Restore 'reg'.
    return;
  }

  __ CompareObject(reg, obj, PP);
}


void FlowGraphCompiler::EmitEqualityRegRegCompare(Register left,
                                                  Register right,
                                                  bool needs_number_check,
                                                  intptr_t token_pos) {
  if (needs_number_check) {
    __ pushq(left);
    __ pushq(right);
    if (is_optimizing()) {
      __ CallPatchable(&StubCode::OptimizedIdenticalWithNumberCheckLabel());
    } else {
      __ CallPatchable(&StubCode::UnoptimizedIdenticalWithNumberCheckLabel());
    }
    AddCurrentDescriptor(PcDescriptors::kRuntimeCall,
                         Isolate::kNoDeoptId,
                         token_pos);
    // Stub returns result in flags (result of a cmpl, we need ZF computed).
    __ popq(right);
    __ popq(left);
  } else {
    __ cmpl(left, right);
  }
}


// This function must be in sync with FlowGraphCompiler::RecordSafepoint and
// FlowGraphCompiler::SlowPathEnvironmentFor.
void FlowGraphCompiler::SaveLiveRegisters(LocationSummary* locs) {
  // TODO(vegorov): consider saving only caller save (volatile) registers.
  const intptr_t xmm_regs_count = locs->live_registers()->FpuRegisterCount();
  if (xmm_regs_count > 0) {
    __ AddImmediate(RSP, Immediate(-xmm_regs_count * kFpuRegisterSize), PP);
    // Store XMM registers with the lowest register number at the lowest
    // address.
    intptr_t offset = 0;
    for (intptr_t reg_idx = 0; reg_idx < kNumberOfXmmRegisters; ++reg_idx) {
      XmmRegister xmm_reg = static_cast<XmmRegister>(reg_idx);
      if (locs->live_registers()->ContainsFpuRegister(xmm_reg)) {
        __ movups(Address(RSP, offset), xmm_reg);
        offset += kFpuRegisterSize;
      }
    }
    ASSERT(offset == (xmm_regs_count * kFpuRegisterSize));
  }

  // Store general purpose registers with the highest register number at the
  // lowest address.
  for (intptr_t reg_idx = 0; reg_idx < kNumberOfCpuRegisters; ++reg_idx) {
    Register reg = static_cast<Register>(reg_idx);
    if (locs->live_registers()->ContainsRegister(reg)) {
      __ pushq(reg);
    }
  }
}


void FlowGraphCompiler::RestoreLiveRegisters(LocationSummary* locs) {
  // General purpose registers have the highest register number at the
  // lowest address.
  for (intptr_t reg_idx = kNumberOfCpuRegisters - 1; reg_idx >= 0; --reg_idx) {
    Register reg = static_cast<Register>(reg_idx);
    if (locs->live_registers()->ContainsRegister(reg)) {
      __ popq(reg);
    }
  }

  const intptr_t xmm_regs_count = locs->live_registers()->FpuRegisterCount();
  if (xmm_regs_count > 0) {
    // XMM registers have the lowest register number at the lowest address.
    intptr_t offset = 0;
    for (intptr_t reg_idx = 0; reg_idx < kNumberOfXmmRegisters; ++reg_idx) {
      XmmRegister xmm_reg = static_cast<XmmRegister>(reg_idx);
      if (locs->live_registers()->ContainsFpuRegister(xmm_reg)) {
        __ movups(xmm_reg, Address(RSP, offset));
        offset += kFpuRegisterSize;
      }
    }
    ASSERT(offset == (xmm_regs_count * kFpuRegisterSize));
    __ AddImmediate(RSP, Immediate(offset), PP);
  }
}


void FlowGraphCompiler::EmitTestAndCall(const ICData& ic_data,
                                        Register class_id_reg,
                                        intptr_t argument_count,
                                        const Array& argument_names,
                                        Label* deopt,
                                        intptr_t deopt_id,
                                        intptr_t token_index,
                                        LocationSummary* locs) {
  ASSERT(is_optimizing());
  ASSERT(!ic_data.IsNull() && (ic_data.NumberOfChecks() > 0));
  Label match_found;
  const intptr_t len = ic_data.NumberOfChecks();
  GrowableArray<CidTarget> sorted(len);
  SortICDataByCount(ic_data, &sorted);
  ASSERT(class_id_reg != R10);
  ASSERT(len > 0);  // Why bother otherwise.
  const Array& arguments_descriptor =
      Array::ZoneHandle(ArgumentsDescriptor::New(argument_count,
                                                 argument_names));
  __ LoadObject(R10, arguments_descriptor, PP);
  for (intptr_t i = 0; i < len; i++) {
    const bool is_last_check = (i == (len - 1));
    Label next_test;
    assembler()->cmpl(class_id_reg, Immediate(sorted[i].cid));
    if (is_last_check) {
      assembler()->j(NOT_EQUAL, deopt);
    } else {
      assembler()->j(NOT_EQUAL, &next_test);
    }
    // Do not use the code from the function, but let the code be patched so
    // that we can record the outgoing edges to other code.
    GenerateDartCall(deopt_id,
                     token_index,
                     &StubCode::CallStaticFunctionLabel(),
                     PcDescriptors::kOptStaticCall,
                     locs);
    const Function& function = *sorted[i].target;
    AddStaticCallTarget(function);
    __ Drop(argument_count);
    if (!is_last_check) {
      assembler()->jmp(&match_found);
    }
    assembler()->Bind(&next_test);
  }
  assembler()->Bind(&match_found);
}


FieldAddress FlowGraphCompiler::ElementAddressForIntIndex(intptr_t cid,
                                                          intptr_t index_scale,
                                                          Register array,
                                                          intptr_t index) {
  const int64_t disp =
      static_cast<int64_t>(index) * index_scale + DataOffsetFor(cid);
  ASSERT(Utils::IsInt(32, disp));
  return FieldAddress(array, static_cast<int32_t>(disp));
}


static ScaleFactor ToScaleFactor(intptr_t index_scale) {
  // Note that index is expected smi-tagged, (i.e, times 2) for all arrays with
  // index scale factor > 1. E.g., for Uint8Array and OneByteString the index is
  // expected to be untagged before accessing.
  ASSERT(kSmiTagShift == 1);
  switch (index_scale) {
    case 1: return TIMES_1;
    case 2: return TIMES_1;
    case 4: return TIMES_2;
    case 8: return TIMES_4;
    case 16: return TIMES_8;
    default:
      UNREACHABLE();
      return TIMES_1;
  }
}


FieldAddress FlowGraphCompiler::ElementAddressForRegIndex(intptr_t cid,
                                                          intptr_t index_scale,
                                                          Register array,
                                                          Register index) {
  return FieldAddress(array,
                      index,
                      ToScaleFactor(index_scale),
                      DataOffsetFor(cid));
}


Address FlowGraphCompiler::ExternalElementAddressForIntIndex(
    intptr_t index_scale,
    Register array,
    intptr_t index) {
  return Address(array, index * index_scale);
}


Address FlowGraphCompiler::ExternalElementAddressForRegIndex(
    intptr_t index_scale,
    Register array,
    Register index) {
  return Address(array, index, ToScaleFactor(index_scale), 0);
}


#undef __
#define __ compiler_->assembler()->


void ParallelMoveResolver::EmitMove(int index) {
  MoveOperands* move = moves_[index];
  const Location source = move->src();
  const Location destination = move->dest();

  if (source.IsRegister()) {
    if (destination.IsRegister()) {
      __ movq(destination.reg(), source.reg());
    } else {
      ASSERT(destination.IsStackSlot());
      __ movq(destination.ToStackSlotAddress(), source.reg());
    }
  } else if (source.IsStackSlot()) {
    if (destination.IsRegister()) {
      __ movq(destination.reg(), source.ToStackSlotAddress());
    } else {
      ASSERT(destination.IsStackSlot());
      MoveMemoryToMemory(destination.ToStackSlotAddress(),
                         source.ToStackSlotAddress());
    }
  } else if (source.IsFpuRegister()) {
    if (destination.IsFpuRegister()) {
      // Optimization manual recommends using MOVAPS for register
      // to register moves.
      __ movaps(destination.fpu_reg(), source.fpu_reg());
    } else {
      if (destination.IsDoubleStackSlot()) {
        __ movsd(destination.ToStackSlotAddress(), source.fpu_reg());
      } else {
        ASSERT(destination.IsQuadStackSlot());
        __ movups(destination.ToStackSlotAddress(), source.fpu_reg());
      }
    }
  } else if (source.IsDoubleStackSlot()) {
    if (destination.IsFpuRegister()) {
      __ movsd(destination.fpu_reg(), source.ToStackSlotAddress());
    } else {
      ASSERT(destination.IsDoubleStackSlot());
      __ movsd(XMM0, source.ToStackSlotAddress());
      __ movsd(destination.ToStackSlotAddress(), XMM0);
    }
  } else if (source.IsQuadStackSlot()) {
    if (destination.IsFpuRegister()) {
      __ movups(destination.fpu_reg(), source.ToStackSlotAddress());
    } else {
      ASSERT(destination.IsQuadStackSlot());
      __ movups(XMM0, source.ToStackSlotAddress());
      __ movups(destination.ToStackSlotAddress(), XMM0);
    }
  } else {
    ASSERT(source.IsConstant());
    if (destination.IsRegister()) {
      const Object& constant = source.constant();
      if (constant.IsSmi() && (Smi::Cast(constant).Value() == 0)) {
        __ xorq(destination.reg(), destination.reg());
      } else {
        __ LoadObject(destination.reg(), constant, PP);
      }
    } else {
      ASSERT(destination.IsStackSlot());
      StoreObject(destination.ToStackSlotAddress(), source.constant());
    }
  }

  move->Eliminate();
}


void ParallelMoveResolver::EmitSwap(int index) {
  MoveOperands* move = moves_[index];
  const Location source = move->src();
  const Location destination = move->dest();

  if (source.IsRegister() && destination.IsRegister()) {
    __ xchgq(destination.reg(), source.reg());
  } else if (source.IsRegister() && destination.IsStackSlot()) {
    Exchange(source.reg(), destination.ToStackSlotAddress());
  } else if (source.IsStackSlot() && destination.IsRegister()) {
    Exchange(destination.reg(), source.ToStackSlotAddress());
  } else if (source.IsStackSlot() && destination.IsStackSlot()) {
    Exchange(destination.ToStackSlotAddress(), source.ToStackSlotAddress());
  } else if (source.IsFpuRegister() && destination.IsFpuRegister()) {
    __ movaps(XMM0, source.fpu_reg());
    __ movaps(source.fpu_reg(), destination.fpu_reg());
    __ movaps(destination.fpu_reg(), XMM0);
  } else if (source.IsFpuRegister() || destination.IsFpuRegister()) {
    ASSERT(destination.IsDoubleStackSlot() ||
           destination.IsQuadStackSlot() ||
           source.IsDoubleStackSlot() ||
           source.IsQuadStackSlot());
    bool double_width = destination.IsDoubleStackSlot() ||
                        source.IsDoubleStackSlot();
    XmmRegister reg = source.IsFpuRegister() ? source.fpu_reg()
                                             : destination.fpu_reg();
    Address slot_address = source.IsFpuRegister()
        ? destination.ToStackSlotAddress()
        : source.ToStackSlotAddress();

    if (double_width) {
      __ movsd(XMM0, slot_address);
      __ movsd(slot_address, reg);
    } else {
      __ movups(XMM0, slot_address);
      __ movups(slot_address, reg);
    }
    __ movaps(reg, XMM0);
  } else if (source.IsDoubleStackSlot() && destination.IsDoubleStackSlot()) {
    const Address& source_slot_address = source.ToStackSlotAddress();
    const Address& destination_slot_address = destination.ToStackSlotAddress();

    ScratchFpuRegisterScope ensure_scratch(this, XMM0);
    __ movsd(XMM0, source_slot_address);
    __ movsd(ensure_scratch.reg(), destination_slot_address);
    __ movsd(destination_slot_address, XMM0);
    __ movsd(source_slot_address, ensure_scratch.reg());
  } else if (source.IsQuadStackSlot() && destination.IsQuadStackSlot()) {
    const Address& source_slot_address = source.ToStackSlotAddress();
    const Address& destination_slot_address = destination.ToStackSlotAddress();

    ScratchFpuRegisterScope ensure_scratch(this, XMM0);
    __ movups(XMM0, source_slot_address);
    __ movups(ensure_scratch.reg(), destination_slot_address);
    __ movups(destination_slot_address, XMM0);
    __ movups(source_slot_address, ensure_scratch.reg());
  } else {
    UNREACHABLE();
  }

  // The swap of source and destination has executed a move from source to
  // destination.
  move->Eliminate();

  // Any unperformed (including pending) move with a source of either
  // this move's source or destination needs to have their source
  // changed to reflect the state of affairs after the swap.
  for (int i = 0; i < moves_.length(); ++i) {
    const MoveOperands& other_move = *moves_[i];
    if (other_move.Blocks(source)) {
      moves_[i]->set_src(destination);
    } else if (other_move.Blocks(destination)) {
      moves_[i]->set_src(source);
    }
  }
}


void ParallelMoveResolver::MoveMemoryToMemory(const Address& dst,
                                              const Address& src) {
  __ MoveMemoryToMemory(dst, src);
}


void ParallelMoveResolver::StoreObject(const Address& dst, const Object& obj) {
  __ StoreObject(dst, obj, PP);
}


void ParallelMoveResolver::Exchange(Register reg, const Address& mem) {
  __ Exchange(reg, mem);
}


void ParallelMoveResolver::Exchange(const Address& mem1, const Address& mem2) {
  __ Exchange(mem1, mem2);
}


void ParallelMoveResolver::Exchange(Register reg, intptr_t stack_offset) {
  UNREACHABLE();
}


void ParallelMoveResolver::Exchange(intptr_t stack_offset1,
                                    intptr_t stack_offset2) {
  UNREACHABLE();
}


void ParallelMoveResolver::SpillScratch(Register reg) {
  __ pushq(reg);
}


void ParallelMoveResolver::RestoreScratch(Register reg) {
  __ popq(reg);
}


void ParallelMoveResolver::SpillFpuScratch(FpuRegister reg) {
  __ AddImmediate(RSP, Immediate(-kFpuRegisterSize), PP);
  __ movups(Address(RSP, 0), reg);
}


void ParallelMoveResolver::RestoreFpuScratch(FpuRegister reg) {
  __ movups(reg, Address(RSP, 0));
  __ AddImmediate(RSP, Immediate(kFpuRegisterSize), PP);
}


#undef __

}  // namespace dart

#endif  // defined TARGET_ARCH_X64
