// Copyright (c) 2026, the FCB project authors.

#ifndef RUNTIME_VM_FCB_PATCH_ENTRY_H_
#define RUNTIME_VM_FCB_PATCH_ENTRY_H_

#include <string>
#include <vector>

#include "vm/fcb_patch_runtime.h"
#include "vm/raw_object.h"

namespace dart {

class Array;
class Function;
class IsolateGroup;
class Object;
class Thread;
class Zone;

namespace fcb {

// Bridge used by VM invocation paths to decide whether execution continues in
// original AOT/JIT code or enters the VM-adjacent FCB interpreter.
DispatchDecision ResolveFunctionEntry(PatchRuntime* runtime,
                                      const std::string& function_id);

DispatchDecision ResolveFunctionEntry(IsolateGroup* isolate_group,
                                      const std::string& function_id);

// Builds the transitional FunctionId used by the Phase D VM entry hook.
// The long-term linker will replace this name-based id with a stable Kernel
// identity, but using the script URI plus qualified function name lets the VM
// dispatch path run today without changing object layout.
std::string FunctionIdFor(const Function& function);

// Returns true when a static call to [function] should be routed through the
// FCB dispatch probe. Phase D does not require annotations: any non-SDK Dart
// function can be probed, and only active patch table hits enter the
// interpreter.
bool ShouldProbeFunction(Thread* thread, const Function& function);

// Temporarily disables the generic DartEntry FCB hook while VM runtime code is
// deliberately invoking the original function as a fallback.
class ScopedSuppressPatchInvocation {
 public:
  ScopedSuppressPatchInvocation();
  ~ScopedSuppressPatchInvocation();

  ScopedSuppressPatchInvocation(const ScopedSuppressPatchInvocation&) = delete;
  ScopedSuppressPatchInvocation& operator=(
      const ScopedSuppressPatchInvocation&) = delete;
};

// Returns true when the current isolate group has an active interpreted patch
// for [function]. Used by compiler paths to avoid producing optimized direct
// calls or inlining that would bypass the FCB dispatch probe.
bool IsFunctionPatched(Thread* thread, const Function& function);

// Attempts to execute a patched bytecode function for a Dart VM Function.
//
// Returns true when the call was handled by FCB, including interpreter errors.
// In that case out_result is set to an InstancePtr or ErrorPtr. Returns false
// when no patch entry exists and the caller should continue to original AOT/JIT
// code.
bool TryInvokePatchedFunction(Thread* thread,
                              const Function& function,
                              const Array& arguments,
                              ObjectPtr* out_result);

bool TryInvokePatchedFunction(Thread* thread,
                              const Function& function,
                              const std::vector<ObjectPtr>& arguments,
                              ObjectPtr* out_result,
                              ReturnConvention* out_return_convention =
                                  nullptr);

bool TryInvokePatchedAotFunction(Thread* thread,
                                 Zone* zone,
                                 const Function& function,
                                 const ObjectPtr* arguments,
                                 intptr_t argument_count,
                                 ObjectPtr* out_result,
                                 ReturnConvention* out_return_convention =
                                     nullptr);

// Fallback for AOT callsites where the VM cannot recover the original Function
// from the return PC. It only handles the call when exactly one active patch has
// the requested arity.
bool TryInvokeUniquePatchedFunctionByArity(Thread* thread,
                                           const Array& arguments,
                                           ObjectPtr* out_result);

bool TryInvokeUniquePatchedFunctionByArity(
    Thread* thread,
    const std::vector<ObjectPtr>& arguments,
    ObjectPtr* out_result,
    ReturnConvention* out_return_convention = nullptr);

}  // namespace fcb
}  // namespace dart

#endif  // RUNTIME_VM_FCB_PATCH_ENTRY_H_
