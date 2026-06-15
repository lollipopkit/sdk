// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_api.h"

#include "vm/isolate.h"

namespace dart {
namespace fcb {

namespace {

bool SetError(std::string* error, const char* message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

}  // namespace

bool LoadPatchRuntimeForIsolateGroup(IsolateGroup* isolate_group,
                                     const char* bytecode_path,
                                     std::string* error) {
  if (isolate_group == nullptr) {
    return SetError(error, "FCB patch load requires an isolate group");
  }
  return isolate_group->LoadFcbPatchRuntimeFromFile(bytecode_path, error);
}

bool LoadPatchRuntimeForCurrentIsolateGroup(const char* bytecode_path,
                                            std::string* error) {
  return LoadPatchRuntimeForIsolateGroup(IsolateGroup::Current(), bytecode_path,
                                         error);
}

void ClearPatchRuntimeForIsolateGroup(IsolateGroup* isolate_group) {
  if (isolate_group != nullptr) {
    isolate_group->ClearFcbPatchRuntime();
  }
}

void ClearPatchRuntimeForCurrentIsolateGroup() {
  ClearPatchRuntimeForIsolateGroup(IsolateGroup::Current());
}

}  // namespace fcb
}  // namespace dart
