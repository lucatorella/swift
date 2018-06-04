//===--- TFUtilities.h - TensorFlow lowering utilities ----------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This defines the shared code that implements the various TensorFlow related
// lowerings and other transformations.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_TENSORFLOW_H
#define SWIFT_SILOPTIMIZER_TENSORFLOW_H

#include <unordered_set>

#include "swift/AST/TensorFlow.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"

namespace swift {
namespace tf {

enum class DeviceType { CPU, GPU, TPU };

static const char DEFAULT_CPU_DEVICE[] = "/device:CPU:0";
static const char DEFAULT_GPU_DEVICE[] = "/device:GPU:0";
static const char DEFAULT_TPU_DEVICE[] = "TPU_SYSTEM";
static const char DEVICE_ATTR[] = "device";

static DeviceType OpDeviceType(StringRef device) {
  if (device.str() == DEFAULT_CPU_DEVICE) return DeviceType::CPU;
  if (device.str() == DEFAULT_GPU_DEVICE) return DeviceType::GPU;
  if (device.str() == DEFAULT_TPU_DEVICE) return DeviceType::TPU;

  // FIXME: Consider also supporting variants of the device string, such as
  // "CPU:0".
  llvm_unreachable("Unknown device type");
}

/// The returned string is compatible with TF device name used in TF graphs.
static std::string getDeviceString(DeviceType deviceType) {
  switch (deviceType) {
    case DeviceType::CPU:
      return DEFAULT_CPU_DEVICE;
    case DeviceType::GPU:
      return DEFAULT_GPU_DEVICE;
    case DeviceType::TPU:
      return DEFAULT_TPU_DEVICE;
  }
}

/// The returned string can be used to construct SIL function names.
static std::string getDeviceShortName(DeviceType deviceType) {
  switch (deviceType) {
    case DeviceType::CPU:
      return "CPU";
    case DeviceType::GPU:
      return "GPU";
    case DeviceType::TPU:
      return "TPU";
  }
}

/// This struct holds information about the global configuration of the graph
/// we are generating.  This can be different between distinct graphs in the
/// same program though.
struct GraphGlobalConfiguration {
  DeviceType deviceType = DeviceType::CPU;
  bool isTPUInfeedEnabled = false;

  // TF devices involved in the tensor computation.
  std::unordered_set<DeviceType> usedDeviceTypes;

  bool isTPUEnabled() const { return deviceType == DeviceType::TPU; }

  // Chooses a device for this op, extends `operands` and `newInstName`
  // accordingly with the device attribute, and tracks the chosen device in
  // `usedDeviceTypes`.
  //
  // If `opDevice` is already set, respects that device choice, and returns
  // false, indicating no change to the input tensor op instruction.
  // Otherwise, choses a device based on this configuration and op kernel
  // device availability, and returns true, indicating a change.
  bool handleDevicePlacement(StringRef opType, StringRef opDevice,
                             /*SILInstruction *inst,*/ SILBuilder &B,
                             SILLocation loc,
                             SmallVectorImpl<SILValue> &operands,
                             std::string &newInstName) {
    // No device placement for this special-case "pseudo-op" for
    // scalar-to-tensor promotion. It will later be translated by compiler (in
    // PartitionCloner) into real TF ops, where device placement is handled at
    // that time.
    if (opType == "tfc.scalarToTensor") {
      assert(opDevice.empty());
      return false;
    }

    auto chosenDevice = chooseDevice(opType, opDevice);
    usedDeviceTypes.insert(chosenDevice);

    // Example output SIL:
    // %2 = string_literal utf8 "/device:GPU:0"        // user: %3
    // %3 = builtin "__tfop_Const,dtype,value$tensor,device"(%0 : $@thin
    // %Float.Type, %1 : $Builtin.FPIEEE64, %2 : $Builtin.RawPointer) :
    // %$TensorHandle<Float> // user: %4
    //
    // Note we generate the StringLiteral inst for op device even when the input
    // `opDevice` is not empty. This is redundant but keeps the code simple, and
    // we expect the original StringLiteral inst for the op device to get DCE'd
    // in a later compiler pass.
    auto deviceString = getDeviceString(chosenDevice);
    auto deviceStrInst =
        B.createStringLiteral(loc, StringRef(deviceString),
                              StringLiteralInst::Encoding::UTF8);
    operands.push_back(deviceStrInst);
    newInstName += ",device";

    return opDevice.empty();
  }

 private:
  DeviceType chooseDevice(StringRef opType, StringRef opDevice) const {
    if (!opDevice.empty()) return OpDeviceType(opDevice);

    // Place this inst on the device given by this configuration.
    // FIXME: Use the op kernel device availability info to select a device for
    // `opType` -- if that op has no available kernel on `deviceType`, a
    // different device should be returned.
    return deviceType;
  }
};

  /// If the -tf-dump-intermediates flag has been passed, return a pointer to
  /// the stream that we should print debug dump information to.  Otherwise,
  /// return null.  This is used for integration unit tests and debugging.
  llvm::raw_ostream *getTFDumpIntermediateStream();

  /// If the specified type is the well-known TensorHandle<T> type, then return
  /// "T".  If not, return a null type.
  bool isTensorHandle(SILType ty);

  /// Determine whether the specified type is one of our well-known types, and
  /// if so, which one it is.
  TFValueKind classifyTensorFlowValue(SILType ty);

  /// Return true if the specified type is TensorHandle<T>, ResourceHandle, or
  /// VariantHandle.
  bool isTensorFlowValue(SILType ty);

  /// This function maps a Swift type (either a language type like Float or an
  /// LLVM Builtin type like Builtin.f32) into the TensorFlow TF_DataType value.
  unsigned convertSwiftTypeToTF(Type ty);

  /// Return true if the specified type is a valid tensor element type.  For
  /// example, int128 and pointers are not.
  ///
  /// TODO: This should eventually consider information about the target
  /// deployment.
  inline bool isValidTensorFlowElementType(Type ty) {
    return convertSwiftTypeToTF(ty) != 0;
  }

  /// Looks up a function by `name` in the context of `typeDecl`, `proto` and
  /// `module`, and returns that function.
  SILFunction *findSILFunctionForRequiredProtocolMember(
      NominalTypeDecl *typeDecl, ProtocolDecl *proto, DeclName name,
      ModuleDecl *module, SILModule &silModule);

  /// Represent information about a TensorFlow operation as represented in SIL
  /// as Builtin instructions.
  struct SILTensorOpInfo {
    /// The instruction being analyzed.
    BuiltinInst *inst;

    /// This is the name for the entire builtin that we'll partition out.
    StringRef builtinName;

    /// This is the TensorFlow name for the op.
    StringRef opName;

    /// One of these records exists for every operand that the BuiltinInst has,
    /// classifying the operand into a couple of buckets.  The most coarse grain
    /// classification is "input" vs "attribute": the inputs come first,
    /// followed by the attributes.  However, we need to be able to model the
    /// fact that some input arguments are aggregated together into a single
    /// input that is an array of tensors.  An integer attribute may be either
    /// a Tensor value or an integer-encoded DType, etc.
    enum class OperandClass {
      /// This marks three sorts of things:
      /// 1) A normal tensor input: the value is a TensorHandle.
      /// 2) A scalar input suitable for scalar promotion, used by the
      ///    tf.scalarToTensor pseudo-op, the value is a scalar value.
      /// 3) A tensor array (TensorFlow "InputList").  The value is a metatype
      ///    marker value (so we can represent empty arrays) followed by
      ///    InputElt elements that make up the array.
      Input,
      InputElt,     // Element of an input list.  Always a TensorHandle.

      Normal,       // No modifier.
      DType,        // This integer value is a dtype.
      Tensor,       // This array or scalar should be turned into a TF_Tensor.
      Shape,        // This array of integers is a shape specifier.

      Array,        // This marks a normal array value, the value is a metatype.
      ArrayElement, // This is a continuation element of an attribute array.

      // This is the start of a shape array.  The value is the # elements.
      ShapeArray,
    };

    /// Return the string suffix for the specified attribute modifier.
    static const char *getOperandClassSuffix(OperandClass opClass);

    /// Return the operand class of the specified string form like "tensor"
    static llvm::Optional<OperandClass> getOperandClass(StringRef suffix);

    /// These are the names of any attribute operands at the end of the list.
    SmallVector<std::pair<StringRef, OperandClass>, 4> operandClasses;

    /// Return true if the specified operand is an input (not an attribute).
    bool isInput(unsigned operandNumber) const {
      return operandClasses[operandNumber].second == OperandClass::Input ||
             operandClasses[operandNumber].second == OperandClass::InputElt;
    }

    /// Return true if this apply instruction is to a function that can be
    /// conditionally hoisted into the graph, but don't check the operands to
    /// see if they are actually constants we can handle.
    static bool isDecodableApply(ApplyInst *apply);

    /// If the specified call is to a function that we can promote to an op,
    /// rewrite the instruction and return a new one that does so.  Otherwise,
    /// return the same instruction.
    static SILInstruction *decodeApply(ApplyInst *apply);

    /// Analyze the specified SIL instruction and return a SILTensorOpInfo
    /// result if the instruction is a valid tensor operation.  This is the
    /// way that SILTensorOpInfo's are created.
    static Optional<SILTensorOpInfo> decode(SILInstruction *inst);

    /// Verify that all operands to this op are correctly formed, e.g. that
    /// attribute operands are passed acceptable constants.  This returns a
    /// non-empty error string to emit if an error is detected.
    std::string checkAndDiagnoseOperands() const;

    /// Replace any indirect memory operands with direct references to the
    /// scalars they reference.  This potentially replaces the builtin
    /// instruction, so it returns the right one to use.
    ///
    /// When `configuration` is non-NULL, also use it to set the TF device for
    /// the output instruction.
    // TODO(clattner): Remove this when deabstraction exists.
    SILInstruction *canonicalizeOperands(
        GraphGlobalConfiguration *configuration);

    /// Return the constant instruction that defines the specified attribute
    /// operand, or null if the defining value isn't a valid constant for an
    /// attribute.
    SingleValueInstruction *getAttrOperand(unsigned operandNumber) const {
      return getAttrOperand(inst->getOperand(operandNumber));
    }
    static SingleValueInstruction *getAttrOperand(SILValue v);


    /// Given an array value on which we recently dropped a consuming use, try
    /// to remove all the computation that produces the array if possible.  If
    /// not, emit a destroy_value instruction to avoid leaking it.
    ///
    /// FIXME: Move this logic to deabstraction when it is done.
    ///
    static void removeOrDestroyArrayValue(SILValue array, SILLocation loc,
                                          SILBuilder &B);

    /// Return the device string associated with `inst`, which is required to
    /// exist.
    StringRef getDeviceString() const;

   private:
    SILTensorOpInfo(BuiltinInst *inst) : inst(inst) {}
    bool decodeBuiltin();
    static SILInstruction *decodeTensorFromScalars(ApplyInst *inst);
    static SILInstruction *decodeTensorFromScalars1D(ApplyInst *inst);
    static SILInstruction *decodeTensorFromScalarsND(ApplyInst *inst);
  };


  //===--------------------------------------------------------------------===//
  // Source location helpers
  //===--------------------------------------------------------------------===//

  /// The SIL location for operations we process are usually deep in the bowels
  /// of the tensor library code, which are all implementation details to the
  /// user.  As such, walk the inlining location of the specified node to return
  /// the first location *outside* of the tensor implementation goop.
  SILDebugLocation skipInternalLocations(SILDebugLocation loc);

  /// Skip over all the internal implementation details to get the source
  ///  location in user code.
  inline SILLocation getUserSourceLocation(SILDebugLocation loc) {
    return skipInternalLocations(loc).getLocation();
  }

  /// Get the user's source location for the specified value.  If it is an
  /// instruction, we can apply various heuristics to improve the precision of
  /// the returned location information.
  SILLocation getUserSourceLocation(SILValue value);
  SILLocation getUserSourceLocation(SILInstruction *inst);

  //===--------------------------------------------------------------------===//
  // Other stuff
  //===--------------------------------------------------------------------===//

  /// This struct provides a an efficient implementation of a predicate that
  /// determines whether a type is or contains a TensorHandle that will be
  /// exposed after deabstraction.  This is a class instead of a simple function
  /// because we memoize state to avoid rechecking types over and over again.
  class TensorFunctionClassifier {
    TypeContainsTensorFlowValue tctfc;
  public:
    TensorFunctionClassifier() {}

    /// Return true if the specified function is the top-level context that
    /// tensor partitioning should be applied to.  This returns false (for
    /// example) for inlined functions that take and return tensors, since we
    /// know that they are either unreachable or will be inlined into any
    /// clients that use them.
    bool shouldBePartitioned(SILFunction *fn);

    /// Return true if the specified function type has TensorFlow values in its
    /// argument or result list, even if they are abstracted by structs or
    /// tuples.
    bool containsTensorFlowValue(CanSILFunctionType fnType);

    /// Return true if the specified type contains a TensorFlow value type that
    /// will be exposed after deabstraction.
    bool containsTensorFlowValue(Type ty) {
      return tctfc.containsTensorFlowValue(ty);
    }

    /// Return true if the specified type contains a TensorFlow value type that
    /// will be exposed after deabstraction.
    bool containsTensorFlowValue(SILType ty) {
      return containsTensorFlowValue(ty.getSwiftRValueType());
    }

  };

  /// Lower the specified SIL function (which was formed by the partitioner)
  /// into a TensorFlow graph, encode into a vector of bytes, and sets
  /// `entryFnName` accordingly for the runtime to call as a TF graph
  /// function.
  ///
  std::vector<char> lowerTFGraph(SILFunction *fn,
                                 const GraphGlobalConfiguration &configuration,
                                 std::string &entryFnName);
} // end namespace tf
} // end namespace swift
#endif