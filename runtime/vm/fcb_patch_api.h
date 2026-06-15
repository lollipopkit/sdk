// Copyright (c) 2026, the FCB project authors.

#ifndef RUNTIME_VM_FCB_PATCH_API_H_
#define RUNTIME_VM_FCB_PATCH_API_H_

#include <string>

namespace dart {

class IsolateGroup;

namespace fcb {

// Engine-facing bridge for Phase D bytecode registration.
//
// The Flutter Engine fork should call this after updater initialization and
// before the root isolate is made runnable. Loading is atomic: a malformed
// module leaves the previously installed patch runtime unchanged.
bool LoadPatchRuntimeForIsolateGroup(IsolateGroup* isolate_group,
                                     const char* bytecode_path,
                                     std::string* error);

bool LoadPatchRuntimeForCurrentIsolateGroup(const char* bytecode_path,
                                            std::string* error);

void ClearPatchRuntimeForIsolateGroup(IsolateGroup* isolate_group);

void ClearPatchRuntimeForCurrentIsolateGroup();

}  // namespace fcb
}  // namespace dart

#endif  // RUNTIME_VM_FCB_PATCH_API_H_
