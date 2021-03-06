// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/js-generic-lowering.h"

#include "src/ast/ast.h"
#include "src/builtins/builtins-constructor.h"
#include "src/codegen/code-factory.h"
#include "src/compiler/common-operator.h"
#include "src/compiler/js-graph.h"
#include "src/compiler/js-heap-broker.h"
#include "src/compiler/machine-operator.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/node-properties.h"
#include "src/compiler/operator-properties.h"
#include "src/compiler/processed-feedback.h"
#include "src/objects/feedback-cell.h"
#include "src/objects/feedback-vector.h"
#include "src/objects/scope-info.h"

namespace v8 {
namespace internal {
namespace compiler {

namespace {

CallDescriptor::Flags FrameStateFlagForCall(Node* node) {
  return OperatorProperties::HasFrameStateInput(node->op())
             ? CallDescriptor::kNeedsFrameState
             : CallDescriptor::kNoFlags;
}

}  // namespace

JSGenericLowering::JSGenericLowering(JSGraph* jsgraph, Editor* editor,
                                     JSHeapBroker* broker)
    : AdvancedReducer(editor), jsgraph_(jsgraph), broker_(broker) {}

JSGenericLowering::~JSGenericLowering() = default;


Reduction JSGenericLowering::Reduce(Node* node) {
  switch (node->opcode()) {
#define DECLARE_CASE(x)  \
    case IrOpcode::k##x: \
      Lower##x(node);    \
      break;
    JS_OP_LIST(DECLARE_CASE)
#undef DECLARE_CASE
    default:
      // Nothing to see.
      return NoChange();
  }
  return Changed(node);
}

#define REPLACE_STUB_CALL(Name)                                              \
  void JSGenericLowering::LowerJS##Name(Node* node) {                        \
    CallDescriptor::Flags flags = FrameStateFlagForCall(node);               \
    Callable callable = Builtins::CallableFor(isolate(), Builtins::k##Name); \
    ReplaceWithStubCall(node, callable, flags);                              \
  }
REPLACE_STUB_CALL(Add)
REPLACE_STUB_CALL(Subtract)
REPLACE_STUB_CALL(Multiply)
REPLACE_STUB_CALL(Divide)
REPLACE_STUB_CALL(Modulus)
REPLACE_STUB_CALL(Exponentiate)
REPLACE_STUB_CALL(BitwiseAnd)
REPLACE_STUB_CALL(BitwiseOr)
REPLACE_STUB_CALL(BitwiseXor)
REPLACE_STUB_CALL(ShiftLeft)
REPLACE_STUB_CALL(ShiftRight)
REPLACE_STUB_CALL(ShiftRightLogical)
REPLACE_STUB_CALL(HasProperty)
REPLACE_STUB_CALL(ToLength)
REPLACE_STUB_CALL(ToNumber)
REPLACE_STUB_CALL(ToNumberConvertBigInt)
REPLACE_STUB_CALL(ToNumeric)
REPLACE_STUB_CALL(ToName)
REPLACE_STUB_CALL(ToObject)
REPLACE_STUB_CALL(ToString)
REPLACE_STUB_CALL(ForInEnumerate)
REPLACE_STUB_CALL(AsyncFunctionEnter)
REPLACE_STUB_CALL(AsyncFunctionReject)
REPLACE_STUB_CALL(AsyncFunctionResolve)
REPLACE_STUB_CALL(FulfillPromise)
REPLACE_STUB_CALL(PerformPromiseThen)
REPLACE_STUB_CALL(PromiseResolve)
REPLACE_STUB_CALL(RejectPromise)
REPLACE_STUB_CALL(ResolvePromise)
#undef REPLACE_STUB_CALL

// TODO(jgruber): Hook in binary and compare op builtins with feedback.

void JSGenericLowering::ReplaceWithStubCall(Node* node,
                                            Callable callable,
                                            CallDescriptor::Flags flags) {
  ReplaceWithStubCall(node, callable, flags, node->op()->properties());
}

void JSGenericLowering::ReplaceWithStubCall(Node* node,
                                            Callable callable,
                                            CallDescriptor::Flags flags,
                                            Operator::Properties properties) {
  const CallInterfaceDescriptor& descriptor = callable.descriptor();
  auto call_descriptor = Linkage::GetStubCallDescriptor(
      zone(), descriptor, descriptor.GetStackParameterCount(), flags,
      properties);
  Node* stub_code = jsgraph()->HeapConstant(callable.code());
  node->InsertInput(zone(), 0, stub_code);
  NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
}


void JSGenericLowering::ReplaceWithRuntimeCall(Node* node,
                                               Runtime::FunctionId f,
                                               int nargs_override) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Operator::Properties properties = node->op()->properties();
  const Runtime::Function* fun = Runtime::FunctionForId(f);
  int nargs = (nargs_override < 0) ? fun->nargs : nargs_override;
  auto call_descriptor =
      Linkage::GetRuntimeCallDescriptor(zone(), f, nargs, properties, flags);
  Node* ref = jsgraph()->ExternalConstant(ExternalReference::Create(f));
  Node* arity = jsgraph()->Int32Constant(nargs);
  node->InsertInput(zone(), 0, jsgraph()->CEntryStubConstant(fun->result_size));
  node->InsertInput(zone(), nargs + 1, ref);
  node->InsertInput(zone(), nargs + 2, arity);
  NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
}

void JSGenericLowering::ReplaceUnaryOpWithBuiltinCall(
    Node* node, Builtins::Name builtin_without_feedback,
    Builtins::Name builtin_with_feedback) {
  DCHECK(node->opcode() == IrOpcode::kJSBitwiseNot ||
         node->opcode() == IrOpcode::kJSDecrement ||
         node->opcode() == IrOpcode::kJSIncrement ||
         node->opcode() == IrOpcode::kJSNegate);
  const FeedbackParameter& p = FeedbackParameterOf(node->op());
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  if (CollectFeedbackInGenericLowering() && p.feedback().IsValid()) {
    Callable callable = Builtins::CallableFor(isolate(), builtin_with_feedback);
    Node* feedback_vector = jsgraph()->HeapConstant(p.feedback().vector);
    Node* slot = jsgraph()->UintPtrConstant(p.feedback().slot.ToInt());
    const CallInterfaceDescriptor& descriptor = callable.descriptor();
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), descriptor, descriptor.GetStackParameterCount(), flags,
        node->op()->properties());
    Node* stub_code = jsgraph()->HeapConstant(callable.code());
    node->InsertInput(zone(), 0, stub_code);
    node->InsertInput(zone(), 2, slot);
    node->InsertInput(zone(), 3, feedback_vector);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  } else {
    Callable callable =
        Builtins::CallableFor(isolate(), builtin_without_feedback);
    ReplaceWithStubCall(node, callable, flags);
  }
}

#define DEF_UNARY_LOWERING(Name)                                     \
  void JSGenericLowering::LowerJS##Name(Node* node) {                \
    ReplaceUnaryOpWithBuiltinCall(node, Builtins::k##Name,           \
                                  Builtins::k##Name##_WithFeedback); \
  }
DEF_UNARY_LOWERING(BitwiseNot)
DEF_UNARY_LOWERING(Decrement)
DEF_UNARY_LOWERING(Increment)
DEF_UNARY_LOWERING(Negate)
#undef DEF_UNARY_LOWERING

void JSGenericLowering::ReplaceCompareOpWithBuiltinCall(
    Node* node, Builtins::Name builtin_without_feedback,
    Builtins::Name builtin_with_feedback) {
  DCHECK(node->opcode() == IrOpcode::kJSEqual ||
         node->opcode() == IrOpcode::kJSGreaterThan ||
         node->opcode() == IrOpcode::kJSGreaterThanOrEqual ||
         node->opcode() == IrOpcode::kJSLessThan ||
         node->opcode() == IrOpcode::kJSLessThanOrEqual);
  Builtins::Name builtin_id;
  const FeedbackParameter& p = FeedbackParameterOf(node->op());
  if (CollectFeedbackInGenericLowering() && p.feedback().IsValid()) {
    Node* feedback_vector = jsgraph()->HeapConstant(p.feedback().vector);
    Node* slot = jsgraph()->UintPtrConstant(p.feedback().slot.ToInt());
    node->InsertInput(zone(), 2, slot);
    node->InsertInput(zone(), 3, feedback_vector);
    builtin_id = builtin_with_feedback;
  } else {
    builtin_id = builtin_without_feedback;
  }

  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable = Builtins::CallableFor(isolate(), builtin_id);
  ReplaceWithStubCall(node, callable, flags);
}

#define DEF_COMPARE_LOWERING(Name)                                     \
  void JSGenericLowering::LowerJS##Name(Node* node) {                  \
    ReplaceCompareOpWithBuiltinCall(node, Builtins::k##Name,           \
                                    Builtins::k##Name##_WithFeedback); \
  }
DEF_COMPARE_LOWERING(Equal)
DEF_COMPARE_LOWERING(GreaterThan)
DEF_COMPARE_LOWERING(GreaterThanOrEqual)
DEF_COMPARE_LOWERING(LessThan)
DEF_COMPARE_LOWERING(LessThanOrEqual)
#undef DEF_COMPARE_LOWERING

void JSGenericLowering::LowerJSStrictEqual(Node* node) {
  // The === operator doesn't need the current context.
  NodeProperties::ReplaceContextInput(node, jsgraph()->NoContextConstant());
  node->RemoveInput(4);  // control

  Builtins::Name builtin_id;
  const FeedbackParameter& p = FeedbackParameterOf(node->op());
  if (CollectFeedbackInGenericLowering() && p.feedback().IsValid()) {
    Node* feedback_vector = jsgraph()->HeapConstant(p.feedback().vector);
    Node* slot = jsgraph()->UintPtrConstant(p.feedback().slot.ToInt());
    node->InsertInput(zone(), 2, slot);
    node->InsertInput(zone(), 3, feedback_vector);
    builtin_id = Builtins::kStrictEqual_WithFeedback;
  } else {
    builtin_id = Builtins::kStrictEqual;
  }

  Callable callable = Builtins::CallableFor(isolate(), builtin_id);
  ReplaceWithStubCall(node, callable, CallDescriptor::kNoFlags,
                      Operator::kEliminatable);
}

namespace {
bool ShouldUseMegamorphicLoadBuiltin(FeedbackSource const& source,
                                     JSHeapBroker* broker) {
  ProcessedFeedback const& feedback = broker->GetFeedback(source);

  if (feedback.kind() == ProcessedFeedback::kElementAccess) {
    return feedback.AsElementAccess().transition_groups().empty();
  } else if (feedback.kind() == ProcessedFeedback::kNamedAccess) {
    return feedback.AsNamedAccess().maps().empty();
  } else if (feedback.kind() == ProcessedFeedback::kInsufficient) {
    return false;
  }
  UNREACHABLE();
}
}  // namespace

void JSGenericLowering::LowerJSLoadProperty(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  const PropertyAccess& p = PropertyAccessOf(node->op());
  Node* frame_state = NodeProperties::GetFrameStateInput(node);
  Node* outer_state = frame_state->InputAt(kFrameStateOuterStateInput);
  node->InsertInput(zone(), 2,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  if (outer_state->opcode() != IrOpcode::kFrameState) {
    Callable callable = Builtins::CallableFor(
        isolate(), ShouldUseMegamorphicLoadBuiltin(p.feedback(), broker())
                       ? Builtins::kKeyedLoadICTrampoline_Megamorphic
                       : Builtins::kKeyedLoadICTrampoline);
    ReplaceWithStubCall(node, callable, flags);
  } else {
    Callable callable = Builtins::CallableFor(
        isolate(), ShouldUseMegamorphicLoadBuiltin(p.feedback(), broker())
                       ? Builtins::kKeyedLoadIC_Megamorphic
                       : Builtins::kKeyedLoadIC);
    Node* vector = jsgraph()->HeapConstant(p.feedback().vector);
    node->InsertInput(zone(), 3, vector);
    ReplaceWithStubCall(node, callable, flags);
  }
}

void JSGenericLowering::LowerJSLoadNamed(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  NamedAccess const& p = NamedAccessOf(node->op());
  Node* frame_state = NodeProperties::GetFrameStateInput(node);
  Node* outer_state = frame_state->InputAt(kFrameStateOuterStateInput);
  node->InsertInput(zone(), 1, jsgraph()->HeapConstant(p.name()));
  if (!p.feedback().IsValid()) {
    Callable callable =
        Builtins::CallableFor(isolate(), Builtins::kGetProperty);
    ReplaceWithStubCall(node, callable, flags);
    return;
  }
  node->InsertInput(zone(), 2,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  if (outer_state->opcode() != IrOpcode::kFrameState) {
    Callable callable = Builtins::CallableFor(
        isolate(), ShouldUseMegamorphicLoadBuiltin(p.feedback(), broker())
                       ? Builtins::kLoadICTrampoline_Megamorphic
                       : Builtins::kLoadICTrampoline);
    ReplaceWithStubCall(node, callable, flags);
  } else {
    Callable callable = Builtins::CallableFor(
        isolate(), ShouldUseMegamorphicLoadBuiltin(p.feedback(), broker())
                       ? Builtins::kLoadIC_Megamorphic
                       : Builtins::kLoadIC);
    Node* vector = jsgraph()->HeapConstant(p.feedback().vector);
    node->InsertInput(zone(), 3, vector);
    ReplaceWithStubCall(node, callable, flags);
  }
}

void JSGenericLowering::LowerJSLoadGlobal(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  const LoadGlobalParameters& p = LoadGlobalParametersOf(node->op());
  Node* frame_state = NodeProperties::GetFrameStateInput(node);
  Node* outer_state = frame_state->InputAt(kFrameStateOuterStateInput);
  node->InsertInput(zone(), 0, jsgraph()->HeapConstant(p.name()));
  node->InsertInput(zone(), 1,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  if (outer_state->opcode() != IrOpcode::kFrameState) {
    Callable callable = CodeFactory::LoadGlobalIC(isolate(), p.typeof_mode());
    ReplaceWithStubCall(node, callable, flags);
  } else {
    Callable callable =
        CodeFactory::LoadGlobalICInOptimizedCode(isolate(), p.typeof_mode());
    Node* vector = jsgraph()->HeapConstant(p.feedback().vector);
    node->InsertInput(zone(), 2, vector);
    ReplaceWithStubCall(node, callable, flags);
  }
}

void JSGenericLowering::LowerJSGetIterator(Node* node) {
  // TODO(v8:9625): Currently, the GetIterator operator is desugared in the
  // native context specialization phase. Thus, the following generic lowering
  // would never be reachable. We can add a check in native context
  // specialization to avoid desugaring the GetIterator operator when in the
  // case of megamorphic feedback and here, add a call to the
  // 'GetIteratorWithFeedback' builtin. This would reduce the size of the
  // compiled code as it would insert 1 call to the builtin instead of 2 calls
  // resulting from the generic lowering of the LoadNamed and Call operators.
  UNREACHABLE();
}

void JSGenericLowering::LowerJSStoreProperty(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  PropertyAccess const& p = PropertyAccessOf(node->op());
  Node* frame_state = NodeProperties::GetFrameStateInput(node);
  Node* outer_state = frame_state->InputAt(kFrameStateOuterStateInput);
  node->InsertInput(zone(), 3,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  if (outer_state->opcode() != IrOpcode::kFrameState) {
    Callable callable =
        Builtins::CallableFor(isolate(), Builtins::kKeyedStoreICTrampoline);
    ReplaceWithStubCall(node, callable, flags);
  } else {
    Callable callable =
        Builtins::CallableFor(isolate(), Builtins::kKeyedStoreIC);
    Node* vector = jsgraph()->HeapConstant(p.feedback().vector);
    node->InsertInput(zone(), 4, vector);
    ReplaceWithStubCall(node, callable, flags);
  }
}

void JSGenericLowering::LowerJSStoreNamed(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  NamedAccess const& p = NamedAccessOf(node->op());
  Node* frame_state = NodeProperties::GetFrameStateInput(node);
  Node* outer_state = frame_state->InputAt(kFrameStateOuterStateInput);
  node->InsertInput(zone(), 1, jsgraph()->HeapConstant(p.name()));
  if (!p.feedback().IsValid()) {
    ReplaceWithRuntimeCall(node, Runtime::kSetNamedProperty);
    return;
  }
  node->InsertInput(zone(), 3,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  if (outer_state->opcode() != IrOpcode::kFrameState) {
    Callable callable =
        Builtins::CallableFor(isolate(), Builtins::kStoreICTrampoline);
    ReplaceWithStubCall(node, callable, flags);
  } else {
    Callable callable = Builtins::CallableFor(isolate(), Builtins::kStoreIC);
    Node* vector = jsgraph()->HeapConstant(p.feedback().vector);
    node->InsertInput(zone(), 4, vector);
    ReplaceWithStubCall(node, callable, flags);
  }
}

void JSGenericLowering::LowerJSStoreNamedOwn(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  StoreNamedOwnParameters const& p = StoreNamedOwnParametersOf(node->op());
  Node* frame_state = NodeProperties::GetFrameStateInput(node);
  Node* outer_state = frame_state->InputAt(kFrameStateOuterStateInput);
  node->InsertInput(zone(), 1, jsgraph()->HeapConstant(p.name()));
  node->InsertInput(zone(), 3,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  if (outer_state->opcode() != IrOpcode::kFrameState) {
    Callable callable = CodeFactory::StoreOwnIC(isolate());
    ReplaceWithStubCall(node, callable, flags);
  } else {
    Callable callable = CodeFactory::StoreOwnICInOptimizedCode(isolate());
    Node* vector = jsgraph()->HeapConstant(p.feedback().vector);
    node->InsertInput(zone(), 4, vector);
    ReplaceWithStubCall(node, callable, flags);
  }
}

void JSGenericLowering::LowerJSStoreGlobal(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  const StoreGlobalParameters& p = StoreGlobalParametersOf(node->op());
  Node* frame_state = NodeProperties::GetFrameStateInput(node);
  Node* outer_state = frame_state->InputAt(kFrameStateOuterStateInput);
  node->InsertInput(zone(), 0, jsgraph()->HeapConstant(p.name()));
  node->InsertInput(zone(), 2,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  if (outer_state->opcode() != IrOpcode::kFrameState) {
    Callable callable =
        Builtins::CallableFor(isolate(), Builtins::kStoreGlobalICTrampoline);
    ReplaceWithStubCall(node, callable, flags);
  } else {
    Callable callable =
        Builtins::CallableFor(isolate(), Builtins::kStoreGlobalIC);
    Node* vector = jsgraph()->HeapConstant(p.feedback().vector);
    node->InsertInput(zone(), 3, vector);
    ReplaceWithStubCall(node, callable, flags);
  }
}

void JSGenericLowering::LowerJSStoreDataPropertyInLiteral(Node* node) {
  FeedbackParameter const& p = FeedbackParameterOf(node->op());
  RelaxControls(node);
  node->InsertInputs(zone(), 4, 2);
  node->ReplaceInput(4, jsgraph()->HeapConstant(p.feedback().vector));
  node->ReplaceInput(5, jsgraph()->TaggedIndexConstant(p.feedback().index()));
  ReplaceWithRuntimeCall(node, Runtime::kDefineDataPropertyInLiteral);
}

void JSGenericLowering::LowerJSStoreInArrayLiteral(Node* node) {
  Callable callable =
      Builtins::CallableFor(isolate(), Builtins::kStoreInArrayLiteralIC);
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  FeedbackParameter const& p = FeedbackParameterOf(node->op());
  RelaxControls(node);
  node->InsertInput(zone(), 3,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  node->InsertInput(zone(), 4, jsgraph()->HeapConstant(p.feedback().vector));
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSDeleteProperty(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable =
      Builtins::CallableFor(isolate(), Builtins::kDeleteProperty);
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSGetSuperConstructor(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable =
      Builtins::CallableFor(isolate(), Builtins::kGetSuperConstructor);
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSHasInPrototypeChain(Node* node) {
  ReplaceWithRuntimeCall(node, Runtime::kHasInPrototypeChain);
}

void JSGenericLowering::LowerJSInstanceOf(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable = Builtins::CallableFor(isolate(), Builtins::kInstanceOf);
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSOrdinaryHasInstance(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable =
      Builtins::CallableFor(isolate(), Builtins::kOrdinaryHasInstance);
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSHasContextExtension(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSLoadContext(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}


void JSGenericLowering::LowerJSStoreContext(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}


void JSGenericLowering::LowerJSCreate(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable =
      Builtins::CallableFor(isolate(), Builtins::kFastNewObject);
  ReplaceWithStubCall(node, callable, flags);
}


void JSGenericLowering::LowerJSCreateArguments(Node* node) {
  CreateArgumentsType const type = CreateArgumentsTypeOf(node->op());
  switch (type) {
    case CreateArgumentsType::kMappedArguments:
      ReplaceWithRuntimeCall(node, Runtime::kNewSloppyArguments);
      break;
    case CreateArgumentsType::kUnmappedArguments:
      ReplaceWithRuntimeCall(node, Runtime::kNewStrictArguments);
      break;
    case CreateArgumentsType::kRestParameter:
      ReplaceWithRuntimeCall(node, Runtime::kNewRestParameter);
      break;
  }
}


void JSGenericLowering::LowerJSCreateArray(Node* node) {
  CreateArrayParameters const& p = CreateArrayParametersOf(node->op());
  int const arity = static_cast<int>(p.arity());
  auto call_descriptor = Linkage::GetStubCallDescriptor(
      zone(), ArrayConstructorDescriptor{}, arity + 1,
      CallDescriptor::kNeedsFrameState, node->op()->properties());
  Node* stub_code = jsgraph()->ArrayConstructorStubConstant();
  Node* stub_arity = jsgraph()->Int32Constant(arity);
  MaybeHandle<AllocationSite> const maybe_site = p.site();
  Handle<AllocationSite> site;
  Node* type_info = maybe_site.ToHandle(&site) ? jsgraph()->HeapConstant(site)
                                               : jsgraph()->UndefinedConstant();
  Node* receiver = jsgraph()->UndefinedConstant();
  node->InsertInput(zone(), 0, stub_code);
  node->InsertInput(zone(), 3, stub_arity);
  node->InsertInput(zone(), 4, type_info);
  node->InsertInput(zone(), 5, receiver);
  NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
}

void JSGenericLowering::LowerJSCreateArrayIterator(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSCreateAsyncFunctionObject(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSCreateCollectionIterator(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSCreateBoundFunction(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSObjectIsArray(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSCreateObject(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable = Builtins::CallableFor(
      isolate(), Builtins::kCreateObjectWithoutProperties);
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSParseInt(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable = Builtins::CallableFor(isolate(), Builtins::kParseInt);
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSRegExpTest(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable =
      Builtins::CallableFor(isolate(), Builtins::kRegExpPrototypeTestFast);
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSCreateClosure(Node* node) {
  CreateClosureParameters const& p = CreateClosureParametersOf(node->op());
  Handle<SharedFunctionInfo> const shared_info = p.shared_info();
  node->InsertInput(zone(), 0, jsgraph()->HeapConstant(shared_info));
  node->InsertInput(zone(), 1, jsgraph()->HeapConstant(p.feedback_cell()));
  node->RemoveInput(4);  // control

  // Use the FastNewClosure builtin only for functions allocated in new space.
  if (p.allocation() == AllocationType::kYoung) {
    Callable callable =
        Builtins::CallableFor(isolate(), Builtins::kFastNewClosure);
    CallDescriptor::Flags flags = FrameStateFlagForCall(node);
    ReplaceWithStubCall(node, callable, flags);
  } else {
    ReplaceWithRuntimeCall(node, Runtime::kNewClosure_Tenured);
  }
}


void JSGenericLowering::LowerJSCreateFunctionContext(Node* node) {
  const CreateFunctionContextParameters& parameters =
      CreateFunctionContextParametersOf(node->op());
  Handle<ScopeInfo> scope_info = parameters.scope_info();
  int slot_count = parameters.slot_count();
  ScopeType scope_type = parameters.scope_type();
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);

  if (slot_count <= ConstructorBuiltins::MaximumFunctionContextSlots()) {
    Callable callable =
        CodeFactory::FastNewFunctionContext(isolate(), scope_type);
    node->InsertInput(zone(), 0, jsgraph()->HeapConstant(scope_info));
    node->InsertInput(zone(), 1, jsgraph()->Int32Constant(slot_count));
    ReplaceWithStubCall(node, callable, flags);
  } else {
    node->InsertInput(zone(), 0, jsgraph()->HeapConstant(scope_info));
    ReplaceWithRuntimeCall(node, Runtime::kNewFunctionContext);
  }
}

void JSGenericLowering::LowerJSCreateGeneratorObject(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable =
      Builtins::CallableFor(isolate(), Builtins::kCreateGeneratorObject);
  node->RemoveInput(4);  // control
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSCreateIterResultObject(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSCreateStringIterator(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSCreateKeyValueArray(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSCreatePromise(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSCreateTypedArray(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable =
      Builtins::CallableFor(isolate(), Builtins::kCreateTypedArray);
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSCreateLiteralArray(Node* node) {
  CreateLiteralParameters const& p = CreateLiteralParametersOf(node->op());
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  node->InsertInput(zone(), 0, jsgraph()->HeapConstant(p.feedback().vector));
  node->InsertInput(zone(), 1,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  node->InsertInput(zone(), 2, jsgraph()->HeapConstant(p.constant()));

  // Use the CreateShallowArrayLiteratlr builtin only for shallow boilerplates
  // without properties up to the number of elements that the stubs can handle.
  if ((p.flags() & AggregateLiteral::kIsShallow) != 0 &&
      p.length() < ConstructorBuiltins::kMaximumClonedShallowArrayElements) {
    Callable callable =
        Builtins::CallableFor(isolate(), Builtins::kCreateShallowArrayLiteral);
    ReplaceWithStubCall(node, callable, flags);
  } else {
    node->InsertInput(zone(), 3, jsgraph()->SmiConstant(p.flags()));
    ReplaceWithRuntimeCall(node, Runtime::kCreateArrayLiteral);
  }
}

void JSGenericLowering::LowerJSGetTemplateObject(Node* node) {
  UNREACHABLE();  // Eliminated in native context specialization.
}

void JSGenericLowering::LowerJSCreateEmptyLiteralArray(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  FeedbackParameter const& p = FeedbackParameterOf(node->op());
  node->InsertInput(zone(), 0, jsgraph()->HeapConstant(p.feedback().vector));
  node->InsertInput(zone(), 1,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  node->RemoveInput(4);  // control
  Callable callable =
      Builtins::CallableFor(isolate(), Builtins::kCreateEmptyArrayLiteral);
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSCreateArrayFromIterable(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable = Builtins::CallableFor(
      isolate(), Builtins::kIterableToListWithSymbolLookup);
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSCreateLiteralObject(Node* node) {
  CreateLiteralParameters const& p = CreateLiteralParametersOf(node->op());
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  node->InsertInput(zone(), 0, jsgraph()->HeapConstant(p.feedback().vector));
  node->InsertInput(zone(), 1,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  node->InsertInput(zone(), 2, jsgraph()->HeapConstant(p.constant()));
  node->InsertInput(zone(), 3, jsgraph()->SmiConstant(p.flags()));

  // Use the CreateShallowObjectLiteratal builtin only for shallow boilerplates
  // without elements up to the number of properties that the stubs can handle.
  if ((p.flags() & AggregateLiteral::kIsShallow) != 0 &&
      p.length() <=
          ConstructorBuiltins::kMaximumClonedShallowObjectProperties) {
    Callable callable =
        Builtins::CallableFor(isolate(), Builtins::kCreateShallowObjectLiteral);
    ReplaceWithStubCall(node, callable, flags);
  } else {
    ReplaceWithRuntimeCall(node, Runtime::kCreateObjectLiteral);
  }
}

void JSGenericLowering::LowerJSCloneObject(Node* node) {
  CloneObjectParameters const& p = CloneObjectParametersOf(node->op());
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable =
      Builtins::CallableFor(isolate(), Builtins::kCloneObjectIC);
  node->InsertInput(zone(), 1, jsgraph()->SmiConstant(p.flags()));
  node->InsertInput(zone(), 2,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  node->InsertInput(zone(), 3, jsgraph()->HeapConstant(p.feedback().vector));
  ReplaceWithStubCall(node, callable, flags);
}

void JSGenericLowering::LowerJSCreateEmptyLiteralObject(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSCreateLiteralRegExp(Node* node) {
  CreateLiteralParameters const& p = CreateLiteralParametersOf(node->op());
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable =
      Builtins::CallableFor(isolate(), Builtins::kCreateRegExpLiteral);
  node->InsertInput(zone(), 0, jsgraph()->HeapConstant(p.feedback().vector));
  node->InsertInput(zone(), 1,
                    jsgraph()->TaggedIndexConstant(p.feedback().index()));
  node->InsertInput(zone(), 2, jsgraph()->HeapConstant(p.constant()));
  node->InsertInput(zone(), 3, jsgraph()->SmiConstant(p.flags()));
  ReplaceWithStubCall(node, callable, flags);
}


void JSGenericLowering::LowerJSCreateCatchContext(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSCreateWithContext(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSCreateBlockContext(Node* node) {
  Handle<ScopeInfo> scope_info = ScopeInfoOf(node->op());
  node->InsertInput(zone(), 0, jsgraph()->HeapConstant(scope_info));
  ReplaceWithRuntimeCall(node, Runtime::kPushBlockContext);
}

void JSGenericLowering::LowerJSConstructForwardVarargs(Node* node) {
  ConstructForwardVarargsParameters p =
      ConstructForwardVarargsParametersOf(node->op());
  int const arg_count = static_cast<int>(p.arity() - 2);
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable = CodeFactory::ConstructForwardVarargs(isolate());
  auto call_descriptor = Linkage::GetStubCallDescriptor(
      zone(), callable.descriptor(), arg_count + 1, flags);
  Node* stub_code = jsgraph()->HeapConstant(callable.code());
  Node* stub_arity = jsgraph()->Int32Constant(arg_count);
  Node* start_index = jsgraph()->Uint32Constant(p.start_index());
  Node* new_target = node->InputAt(arg_count + 1);
  Node* receiver = jsgraph()->UndefinedConstant();
  node->RemoveInput(arg_count + 1);  // Drop new target.
  node->InsertInput(zone(), 0, stub_code);
  node->InsertInput(zone(), 2, new_target);
  node->InsertInput(zone(), 3, stub_arity);
  node->InsertInput(zone(), 4, start_index);
  node->InsertInput(zone(), 5, receiver);
  NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
}

void JSGenericLowering::LowerJSConstruct(Node* node) {
  ConstructParameters const& p = ConstructParametersOf(node->op());
  int const arg_count = p.arity_without_implicit_args();
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);

  // TODO(jgruber): Understand and document how stack_argument_count is
  // calculated. I've made some educated guesses below but they should be
  // verified and documented in other lowerings as well.
  static constexpr int kReceiver = 1;
  static constexpr int kMaybeFeedbackVector = 1;

  if (CollectFeedbackInGenericLowering() && p.feedback().IsValid()) {
    const int stack_argument_count =
        arg_count + kReceiver + kMaybeFeedbackVector;
    Callable callable =
        Builtins::CallableFor(isolate(), Builtins::kConstruct_WithFeedback);
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), stack_argument_count, flags);
    Node* feedback_vector = jsgraph()->HeapConstant(p.feedback().vector);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());
    Node* new_target = node->InputAt(arg_count + 1);
    Node* stub_arity = jsgraph()->Int32Constant(arg_count);
    Node* slot = jsgraph()->Int32Constant(p.feedback().index());
    Node* receiver = jsgraph()->UndefinedConstant();
    node->RemoveInput(arg_count + 1);  // Drop new target.
    // Register argument inputs are followed by stack argument inputs (such as
    // feedback_vector). Both are listed in ascending order. Note that
    // the receiver is implicitly placed on the stack and is thus inserted
    // between explicitly-specified register and stack arguments.
    // TODO(jgruber): Implement a simpler way to specify these mutations.
    node->InsertInput(zone(), arg_count + 1, feedback_vector);
    node->InsertInput(zone(), 0, stub_code);
    node->InsertInput(zone(), 2, new_target);
    node->InsertInput(zone(), 3, stub_arity);
    node->InsertInput(zone(), 4, slot);
    node->InsertInput(zone(), 5, receiver);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  } else {
    const int stack_argument_count = arg_count + kReceiver;
    Callable callable = Builtins::CallableFor(isolate(), Builtins::kConstruct);
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), stack_argument_count, flags);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());
    Node* stub_arity = jsgraph()->Int32Constant(arg_count);
    Node* new_target = node->InputAt(arg_count + 1);
    Node* receiver = jsgraph()->UndefinedConstant();
    node->RemoveInput(arg_count + 1);  // Drop new target.
    node->InsertInput(zone(), 0, stub_code);
    node->InsertInput(zone(), 2, new_target);
    node->InsertInput(zone(), 3, stub_arity);
    node->InsertInput(zone(), 4, receiver);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  }
}

void JSGenericLowering::LowerJSConstructWithArrayLike(Node* node) {
  ConstructParameters const& p = ConstructParametersOf(node->op());
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  const int arg_count = p.arity_without_implicit_args();
  DCHECK_EQ(arg_count, 1);

  static constexpr int kReceiver = 1;
  static constexpr int kArgumentList = 1;
  static constexpr int kMaybeFeedbackVector = 1;

  if (CollectFeedbackInGenericLowering() && p.feedback().IsValid()) {
    const int stack_argument_count =
        arg_count - kArgumentList + kReceiver + kMaybeFeedbackVector;
    Callable callable = Builtins::CallableFor(
        isolate(), Builtins::kConstructWithArrayLike_WithFeedback);
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), stack_argument_count, flags);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());
    Node* receiver = jsgraph()->UndefinedConstant();
    Node* arguments_list = node->InputAt(1);
    Node* new_target = node->InputAt(2);
    Node* feedback_vector = jsgraph()->HeapConstant(p.feedback().vector);
    Node* slot = jsgraph()->Int32Constant(p.feedback().index());

    node->InsertInput(zone(), 0, stub_code);
    node->ReplaceInput(2, new_target);
    node->ReplaceInput(3, arguments_list);
    // Register argument inputs are followed by stack argument inputs (such as
    // feedback_vector). Both are listed in ascending order. Note that
    // the receiver is implicitly placed on the stack and is thus inserted
    // between explicitly-specified register and stack arguments.
    // TODO(jgruber): Implement a simpler way to specify these mutations.
    node->InsertInput(zone(), 4, slot);
    node->InsertInput(zone(), 5, receiver);
    node->InsertInput(zone(), 6, feedback_vector);

    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  } else {
    const int stack_argument_count = arg_count - kArgumentList + kReceiver;
    Callable callable =
        Builtins::CallableFor(isolate(), Builtins::kConstructWithArrayLike);
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), stack_argument_count, flags);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());
    Node* receiver = jsgraph()->UndefinedConstant();
    Node* arguments_list = node->InputAt(1);
    Node* new_target = node->InputAt(2);
    node->InsertInput(zone(), 0, stub_code);
    node->ReplaceInput(2, new_target);
    node->ReplaceInput(3, arguments_list);
    node->InsertInput(zone(), 4, receiver);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  }
}

void JSGenericLowering::LowerJSConstructWithSpread(Node* node) {
  ConstructParameters const& p = ConstructParametersOf(node->op());
  int const arg_count = p.arity_without_implicit_args();
  int const spread_index = arg_count;
  int const new_target_index = arg_count + 1;
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);

  static constexpr int kReceiver = 1;
  static constexpr int kTheSpread = 1;  // Included in `arg_count`.
  static constexpr int kMaybeFeedbackVector = 1;

  if (CollectFeedbackInGenericLowering() && p.feedback().IsValid()) {
    const int stack_argument_count =
        arg_count + kReceiver + kMaybeFeedbackVector;
    Callable callable = Builtins::CallableFor(
        isolate(), Builtins::kConstructWithSpread_WithFeedback);
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), stack_argument_count, flags);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());
    Node* feedback_vector = jsgraph()->HeapConstant(p.feedback().vector);
    Node* slot = jsgraph()->Int32Constant(p.feedback().index());

    // The single available register is needed for `slot`, thus `spread` remains
    // on the stack here.
    Node* stub_arity = jsgraph()->Int32Constant(arg_count - kTheSpread);
    Node* new_target = node->InputAt(new_target_index);
    Node* receiver = jsgraph()->UndefinedConstant();
    node->RemoveInput(new_target_index);

    // Register argument inputs are followed by stack argument inputs (such as
    // feedback_vector). Both are listed in ascending order. Note that
    // the receiver is implicitly placed on the stack and is thus inserted
    // between explicitly-specified register and stack arguments.
    // TODO(jgruber): Implement a simpler way to specify these mutations.
    node->InsertInput(zone(), arg_count + 1, feedback_vector);
    node->InsertInput(zone(), 0, stub_code);
    node->InsertInput(zone(), 2, new_target);
    node->InsertInput(zone(), 3, stub_arity);
    node->InsertInput(zone(), 4, slot);
    node->InsertInput(zone(), 5, receiver);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  } else {
    const int stack_argument_count = arg_count + kReceiver - kTheSpread;
    Callable callable = CodeFactory::ConstructWithSpread(isolate());
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), stack_argument_count, flags);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());

    // We pass the spread in a register, not on the stack.
    Node* stub_arity = jsgraph()->Int32Constant(arg_count - kTheSpread);
    Node* new_target = node->InputAt(new_target_index);
    Node* spread = node->InputAt(spread_index);
    Node* receiver = jsgraph()->UndefinedConstant();
    DCHECK(new_target_index > spread_index);
    node->RemoveInput(new_target_index);
    node->RemoveInput(spread_index);

    node->InsertInput(zone(), 0, stub_code);
    node->InsertInput(zone(), 2, new_target);
    node->InsertInput(zone(), 3, stub_arity);
    node->InsertInput(zone(), 4, spread);
    node->InsertInput(zone(), 5, receiver);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  }
}

void JSGenericLowering::LowerJSCallForwardVarargs(Node* node) {
  CallForwardVarargsParameters p = CallForwardVarargsParametersOf(node->op());
  int const arg_count = static_cast<int>(p.arity() - 2);
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable = CodeFactory::CallForwardVarargs(isolate());
  auto call_descriptor = Linkage::GetStubCallDescriptor(
      zone(), callable.descriptor(), arg_count + 1, flags);
  Node* stub_code = jsgraph()->HeapConstant(callable.code());
  Node* stub_arity = jsgraph()->Int32Constant(arg_count);
  Node* start_index = jsgraph()->Uint32Constant(p.start_index());
  node->InsertInput(zone(), 0, stub_code);
  node->InsertInput(zone(), 2, stub_arity);
  node->InsertInput(zone(), 3, start_index);
  NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
}

void JSGenericLowering::LowerJSCall(Node* node) {
  CallParameters const& p = CallParametersOf(node->op());
  int const arg_count = p.arity_without_implicit_args();
  ConvertReceiverMode const mode = p.convert_mode();

  if (CollectFeedbackInGenericLowering() && p.feedback().IsValid()) {
    Callable callable = CodeFactory::Call_WithFeedback(isolate(), mode);
    CallDescriptor::Flags flags = FrameStateFlagForCall(node);
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), arg_count + 1, flags);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());
    Node* stub_arity = jsgraph()->Int32Constant(arg_count);
    Node* feedback_vector = jsgraph()->HeapConstant(p.feedback().vector);
    Node* slot = jsgraph()->Int32Constant(p.feedback().index());
    node->InsertInput(zone(), 0, stub_code);
    node->InsertInput(zone(), 2, stub_arity);
    node->InsertInput(zone(), 3, slot);
    node->InsertInput(zone(), 4, feedback_vector);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  } else {
    Callable callable = CodeFactory::Call(isolate(), mode);
    CallDescriptor::Flags flags = FrameStateFlagForCall(node);
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), arg_count + 1, flags);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());
    Node* stub_arity = jsgraph()->Int32Constant(arg_count);
    node->InsertInput(zone(), 0, stub_code);
    node->InsertInput(zone(), 2, stub_arity);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  }
}

void JSGenericLowering::LowerJSCallWithArrayLike(Node* node) {
  CallParameters const& p = CallParametersOf(node->op());
  const int arg_count = p.arity_without_implicit_args();
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);

  DCHECK_EQ(arg_count, 0);
  static constexpr int kReceiver = 1;

  if (CollectFeedbackInGenericLowering() && p.feedback().IsValid()) {
    Callable callable = Builtins::CallableFor(
        isolate(), Builtins::kCallWithArrayLike_WithFeedback);
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), arg_count + kReceiver, flags);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());
    Node* receiver = node->InputAt(1);
    Node* arguments_list = node->InputAt(2);
    Node* feedback_vector = jsgraph()->HeapConstant(p.feedback().vector);
    Node* slot = jsgraph()->Int32Constant(p.feedback().index());
    node->InsertInput(zone(), 0, stub_code);
    node->ReplaceInput(2, arguments_list);
    node->ReplaceInput(3, receiver);
    node->InsertInput(zone(), 3, slot);
    node->InsertInput(zone(), 4, feedback_vector);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  } else {
    Callable callable = CodeFactory::CallWithArrayLike(isolate());
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), arg_count + kReceiver, flags);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());
    Node* receiver = node->InputAt(1);
    Node* arguments_list = node->InputAt(2);
    node->InsertInput(zone(), 0, stub_code);
    node->ReplaceInput(2, arguments_list);
    node->ReplaceInput(3, receiver);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  }
}

void JSGenericLowering::LowerJSCallWithSpread(Node* node) {
  CallParameters const& p = CallParametersOf(node->op());
  int const arg_count = p.arity_without_implicit_args();
  int const spread_index = arg_count + 1;
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);

  static constexpr int kTheSpread = 1;
  static constexpr int kMaybeFeedbackVector = 1;

  if (CollectFeedbackInGenericLowering() && p.feedback().IsValid()) {
    const int stack_argument_count = arg_count + kMaybeFeedbackVector;
    Callable callable = Builtins::CallableFor(
        isolate(), Builtins::kCallWithSpread_WithFeedback);
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), stack_argument_count, flags);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());
    Node* feedback_vector = jsgraph()->HeapConstant(p.feedback().vector);
    Node* slot = jsgraph()->Int32Constant(p.feedback().index());

    // We pass the spread in a register, not on the stack.
    Node* stub_arity = jsgraph()->Int32Constant(arg_count - kTheSpread);
    Node* spread = node->InputAt(spread_index);
    node->RemoveInput(spread_index);

    // Register argument inputs are followed by stack argument inputs (such as
    // feedback_vector). Both are listed in ascending order. Note that
    // the receiver is implicitly placed on the stack and is thus inserted
    // between explicitly-specified register and stack arguments.
    // TODO(jgruber): Implement a simpler way to specify these mutations.
    node->InsertInput(zone(), arg_count + 1, feedback_vector);
    node->InsertInput(zone(), 0, stub_code);
    node->InsertInput(zone(), 2, stub_arity);
    node->InsertInput(zone(), 3, spread);
    node->InsertInput(zone(), 4, slot);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  } else {
    const int stack_argument_count = arg_count;
    Callable callable = CodeFactory::CallWithSpread(isolate());
    auto call_descriptor = Linkage::GetStubCallDescriptor(
        zone(), callable.descriptor(), stack_argument_count, flags);
    Node* stub_code = jsgraph()->HeapConstant(callable.code());

    // We pass the spread in a register, not on the stack.
    Node* stub_arity = jsgraph()->Int32Constant(arg_count - kTheSpread);
    Node* spread = node->InputAt(spread_index);
    node->RemoveInput(spread_index);

    node->InsertInput(zone(), 0, stub_code);
    node->InsertInput(zone(), 2, stub_arity);
    node->InsertInput(zone(), 3, spread);
    NodeProperties::ChangeOp(node, common()->Call(call_descriptor));
  }
}

void JSGenericLowering::LowerJSCallRuntime(Node* node) {
  const CallRuntimeParameters& p = CallRuntimeParametersOf(node->op());
  ReplaceWithRuntimeCall(node, p.id(), static_cast<int>(p.arity()));
}

void JSGenericLowering::LowerJSForInNext(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSForInPrepare(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSLoadMessage(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}


void JSGenericLowering::LowerJSStoreMessage(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSLoadModule(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSStoreModule(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSGeneratorStore(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSGeneratorRestoreContinuation(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSGeneratorRestoreContext(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSGeneratorRestoreInputOrDebugPos(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

void JSGenericLowering::LowerJSGeneratorRestoreRegister(Node* node) {
  UNREACHABLE();  // Eliminated in typed lowering.
}

namespace {

StackCheckKind StackCheckKindOfJSStackCheck(const Operator* op) {
  DCHECK(op->opcode() == IrOpcode::kJSStackCheck);
  return OpParameter<StackCheckKind>(op);
}

}  // namespace

void JSGenericLowering::LowerJSStackCheck(Node* node) {
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);

  Node* limit = effect =
      graph()->NewNode(machine()->Load(MachineType::Pointer()),
                       jsgraph()->ExternalConstant(
                           ExternalReference::address_of_jslimit(isolate())),
                       jsgraph()->IntPtrConstant(0), effect, control);

  StackCheckKind stack_check_kind = StackCheckKindOfJSStackCheck(node->op());
  Node* check = effect = graph()->NewNode(
      machine()->StackPointerGreaterThan(stack_check_kind), limit, effect);
  Node* branch =
      graph()->NewNode(common()->Branch(BranchHint::kTrue), check, control);

  Node* if_true = graph()->NewNode(common()->IfTrue(), branch);
  Node* etrue = effect;

  Node* if_false = graph()->NewNode(common()->IfFalse(), branch);
  NodeProperties::ReplaceControlInput(node, if_false);
  NodeProperties::ReplaceEffectInput(node, effect);
  Node* efalse = if_false = node;

  Node* merge = graph()->NewNode(common()->Merge(2), if_true, if_false);
  Node* ephi = graph()->NewNode(common()->EffectPhi(2), etrue, efalse, merge);

  // Wire the new diamond into the graph, {node} can still throw.
  NodeProperties::ReplaceUses(node, node, ephi, merge, merge);
  NodeProperties::ReplaceControlInput(merge, if_false, 1);
  NodeProperties::ReplaceEffectInput(ephi, efalse, 1);

  // This iteration cuts out potential {IfSuccess} or {IfException} projection
  // uses of the original node and places them inside the diamond, so that we
  // can change the original {node} into the slow-path runtime call.
  for (Edge edge : merge->use_edges()) {
    if (!NodeProperties::IsControlEdge(edge)) continue;
    if (edge.from()->opcode() == IrOpcode::kIfSuccess) {
      NodeProperties::ReplaceUses(edge.from(), nullptr, nullptr, merge);
      NodeProperties::ReplaceControlInput(merge, edge.from(), 1);
      edge.UpdateTo(node);
    }
    if (edge.from()->opcode() == IrOpcode::kIfException) {
      NodeProperties::ReplaceEffectInput(edge.from(), node);
      edge.UpdateTo(node);
    }
  }

  // Turn the stack check into a runtime call. At function entry, the runtime
  // function takes an offset argument which is subtracted from the stack
  // pointer prior to the stack check (i.e. the check is `sp - offset >=
  // limit`).
  if (stack_check_kind == StackCheckKind::kJSFunctionEntry) {
    node->InsertInput(zone(), 0,
                      graph()->NewNode(machine()->LoadStackCheckOffset()));
    ReplaceWithRuntimeCall(node, Runtime::kStackGuardWithGap);
  } else {
    ReplaceWithRuntimeCall(node, Runtime::kStackGuard);
  }
}

void JSGenericLowering::LowerJSDebugger(Node* node) {
  CallDescriptor::Flags flags = FrameStateFlagForCall(node);
  Callable callable = CodeFactory::HandleDebuggerStatement(isolate());
  ReplaceWithStubCall(node, callable, flags);
}

Zone* JSGenericLowering::zone() const { return graph()->zone(); }


Isolate* JSGenericLowering::isolate() const { return jsgraph()->isolate(); }


Graph* JSGenericLowering::graph() const { return jsgraph()->graph(); }


CommonOperatorBuilder* JSGenericLowering::common() const {
  return jsgraph()->common();
}


MachineOperatorBuilder* JSGenericLowering::machine() const {
  return jsgraph()->machine();
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
