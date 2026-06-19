// Copyright (c) 2026, the FCB project authors.

#include "vm/fcb_patch_runtime.h"

#include <string>
#include <utility>

#include "vm/dart_entry.h"
#include "vm/fcb_patch_entry.h"
#include "vm/fcb_patch_runtime_internal.h"
#include "vm/isolate.h"
#include "vm/object.h"
#include "vm/symbols.h"
#include "vm/unit_test.h"

namespace {

dart::fcb::BytecodeModule ReturningBytecodeClosureModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;

  const uint32_t closure_offset = 0;
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,        // LoadArg 0: captured prefix.
      0x02, 0x01,        // LoadArg 1: captured name.
      0x42, 0x00, 0x02,  // StringConcat 2.
      0xff,              // Return.
  });

  const uint32_t make_offset = static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x01, 0x00, 0x01,  // LoadConst "patched ".
      0x02, 0x00,        // LoadArg 0: name.
      0x54, 0x00, 0x00,  // MakeClosure closureBody;captures:2.
      0xff,              // Return.
  });

  const uint32_t call_offset = static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,              // LoadArg 0: name.
      0x50, 0x00, 0x00, 0x01,  // CallStatic makeGreeting(name).
      0x53, 0x00, 0x00, 0x00,  // CallClosure argc 0.
      0xff,                    // Return.
  });

  const uint32_t personalized_closure_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,        // LoadArg 0: captured prefix.
      0x02, 0x01,        // LoadArg 1: captured name.
      0x02, 0x02,        // LoadArg 2: closure suffix argument.
      0x42, 0x00, 0x03,  // StringConcat 3.
      0xff,              // Return.
  });

  const uint32_t make_personalized_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x01, 0x00, 0x01,  // LoadConst "patched ".
      0x02, 0x00,        // LoadArg 0: name.
      0x54, 0x00, 0x00,  // MakeClosure personalizedClosure;captures:2.
      0xff,              // Return.
  });

  const uint32_t call_personalized_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,              // LoadArg 0: name.
      0x50, 0x00, 0x00, 0x01,  // CallStatic makePersonalizedGreeting(name).
      0x01, 0x00, 0x01,        // LoadConst " friend".
      0x53, 0x00, 0x00, 0x01,  // CallClosure argc 1.
      0xff,                    // Return.
  });

  const uint32_t call_named_personalized_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,              // LoadArg 0: name.
      0x50, 0x00, 0x00, 0x01,  // CallStatic makePersonalizedGreeting(name).
      0x01, 0x00, 0x01,        // LoadConst " friend".
      0x53, 0x00, 0x03, 0x01,  // CallClosure ;named:suffix, argc 1.
      0xff,                    // Return.
  });

  const uint32_t call_too_many_named_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,              // LoadArg 0: name.
      0x50, 0x00, 0x00, 0x01,  // CallStatic makePersonalizedGreeting(name).
      0x01, 0x00, 0x01,        // LoadConst " friend".
      0x53, 0x00, 0x03, 0x01,  // CallClosure ;named:suffix,extra, argc 1.
      0xff,                    // Return.
  });

  const uint32_t call_too_few_args_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,              // LoadArg 0: name.
      0x50, 0x00, 0x00, 0x01,  // CallStatic makePersonalizedGreeting(name).
      0x53, 0x00, 0x00, 0x00,  // CallClosure argc 0.
      0xff,                    // Return.
  });

  const uint32_t call_too_many_args_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,              // LoadArg 0: name.
      0x50, 0x00, 0x00, 0x01,  // CallStatic makePersonalizedGreeting(name).
      0x01, 0x00, 0x01,        // LoadConst " friend".
      0x01, 0x00, 0x02,        // LoadConst " extra".
      0x53, 0x00, 0x00, 0x02,  // CallClosure argc 2.
      0xff,                    // Return.
  });

  const uint32_t throwing_closure_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x01, 0x00, 0x00,  // LoadConst "bytecode-boom".
      0x60,              // Throw.
      0xff,              // Return.
  });

  const uint32_t branch_local_closure_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x02,        // LoadArg 2: closure enabled argument.
      0x31, 0x00, 0x16,  // JumpIfFalse 22.
      0x01, 0x00, 0x00,  // LoadConst " enabled".
      0x04, 0x03,        // StoreLocal 3.
      0x02, 0x00,        // LoadArg 0: captured prefix.
      0x02, 0x01,        // LoadArg 1: captured name.
      0x03, 0x03,        // LoadLocal 3.
      0x42, 0x00, 0x03,  // StringConcat 3.
      0x30, 0x00, 0x24,  // Jump 36.
      0x01, 0x00, 0x01,  // LoadConst " disabled".
      0x04, 0x03,        // StoreLocal 3.
      0x02, 0x00,        // LoadArg 0: captured prefix.
      0x02, 0x01,        // LoadArg 1: captured name.
      0x03, 0x03,        // LoadLocal 3.
      0x42, 0x00, 0x03,  // StringConcat 3.
      0xff,              // Return.
  });

  const uint32_t make_branch_local_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x01, 0x00, 0x01,  // LoadConst "patched ".
      0x02, 0x00,        // LoadArg 0: name.
      0x54, 0x00, 0x00,  // MakeClosure branchLocalClosure;captures:2.
      0xff,              // Return.
  });

  const uint32_t call_branch_local_true_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,              // LoadArg 0: name.
      0x50, 0x00, 0x00, 0x01,  // CallStatic makeBranchLocalGreeting(name).
      0x01, 0x00, 0x01,        // LoadConst true.
      0x53, 0x00, 0x00, 0x01,  // CallClosure argc 1.
      0xff,                    // Return.
  });

  const uint32_t call_branch_local_false_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,              // LoadArg 0: name.
      0x50, 0x00, 0x00, 0x01,  // CallStatic makeBranchLocalGreeting(name).
      0x01, 0x00, 0x01,        // LoadConst false.
      0x53, 0x00, 0x00, 0x01,  // CallClosure argc 1.
      0xff,                    // Return.
  });

  const uint32_t invoke_branch_local_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,              // LoadArg 0: bytecode closure.
      0x02, 0x01,              // LoadArg 1: enabled.
      0x53, 0x00, 0x00, 0x01,  // CallClosure argc 1.
      0xff,                    // Return.
  });

  const uint32_t pass_branch_local_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x02, 0x00,              // LoadArg 0: name.
      0x50, 0x00, 0x00, 0x01,  // CallStatic makeBranchLocalGreeting(name).
      0x01, 0x00, 0x01,        // LoadConst true.
      0x50, 0x00, 0x02, 0x02,  // CallStatic invokeBranchLocalGreeting(closure, true).
      0xff,                    // Return.
  });

  const uint32_t make_throwing_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x54, 0x00, 0x00,  // MakeClosure throwingClosure;captures:0.
      0xff,              // Return.
  });

  const uint32_t call_catch_throwing_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x61, 0x00, 0x10, 0x00, 0x15,  // TryBegin handler 16, end 21.
      0x50, 0x00, 0x00, 0x00,        // CallStatic makeThrowingClosure().
      0x53, 0x00, 0x00, 0x00,        // CallClosure argc 0.
      0x30, 0x00, 0x15,              // Jump 21.
      0x04, 0x00,                    // StoreLocal 0 (exception).
      0x01, 0x00, 0x01,              // LoadConst "caught-bytecode".
      0xff,                          // Return.
  });

  const uint32_t call_uncaught_throwing_offset =
      static_cast<uint32_t>(module.bytecode.size());
  module.bytecode.insert(module.bytecode.end(), {
      0x50, 0x00, 0x00, 0x00,  // CallStatic makeThrowingClosure().
      0x53, 0x00, 0x00, 0x00,  // CallClosure argc 0.
      0xff,                    // Return.
  });

  dart::fcb::BytecodeFunction closure;
  closure.function_id = "package:app/closure.dart::makeGreeting.<closure>()";
  closure.parameter_count = 2;
  closure.register_count = 2;
  closure.bytecode_offset = closure_offset;
  closure.bytecode_length = 8;
  module.functions.push_back(std::move(closure));

  dart::fcb::BytecodeFunction make;
  make.function_id = "package:app/closure.dart::makeGreeting(String)";
  make.parameter_count = 1;
  make.register_count = 1;
  make.bytecode_offset = make_offset;
  make.bytecode_length = 9;
  make.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makeGreeting.<closure>();captures:2"));
  make.constants.push_back(dart::fcb::Value::String("patched "));
  module.functions.push_back(std::move(make));

  dart::fcb::BytecodeFunction call;
  call.function_id = "package:app/closure.dart::callGreeting(String)";
  call.parameter_count = 1;
  call.register_count = 1;
  call.bytecode_offset = call_offset;
  call.bytecode_length = 11;
  call.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makeGreeting(String)"));
  module.functions.push_back(std::move(call));

  dart::fcb::BytecodeFunction personalized_closure;
  personalized_closure.function_id =
      "package:app/closure.dart::makePersonalizedGreeting.<closure>()";
  personalized_closure.parameter_count = 3;
  personalized_closure.register_count = 3;
  personalized_closure.bytecode_offset = personalized_closure_offset;
  personalized_closure.bytecode_length = 10;
  module.functions.push_back(std::move(personalized_closure));

  dart::fcb::BytecodeFunction make_personalized;
  make_personalized.function_id =
      "package:app/closure.dart::makePersonalizedGreeting(String)";
  make_personalized.parameter_count = 1;
  make_personalized.register_count = 1;
  make_personalized.bytecode_offset = make_personalized_offset;
  make_personalized.bytecode_length = 9;
  make_personalized.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makePersonalizedGreeting.<closure>();"
      "captures:2"));
  make_personalized.constants.push_back(dart::fcb::Value::String("patched "));
  module.functions.push_back(std::move(make_personalized));

  dart::fcb::BytecodeFunction call_personalized;
  call_personalized.function_id =
      "package:app/closure.dart::callPersonalizedGreeting(String)";
  call_personalized.parameter_count = 1;
  call_personalized.register_count = 1;
  call_personalized.bytecode_offset = call_personalized_offset;
  call_personalized.bytecode_length = 14;
  call_personalized.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makePersonalizedGreeting(String)"));
  call_personalized.constants.push_back(dart::fcb::Value::String(" friend"));
  module.functions.push_back(std::move(call_personalized));

  dart::fcb::BytecodeFunction call_named_personalized;
  call_named_personalized.function_id =
      "package:app/closure.dart::callNamedPersonalizedGreeting(String)";
  call_named_personalized.parameter_count = 1;
  call_named_personalized.register_count = 1;
  call_named_personalized.bytecode_offset = call_named_personalized_offset;
  call_named_personalized.bytecode_length = 14;
  call_named_personalized.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makePersonalizedGreeting(String)"));
  call_named_personalized.constants.push_back(
      dart::fcb::Value::String(" friend"));
  call_named_personalized.constants.push_back(
      dart::fcb::Value::String(";named:suffix"));
  module.functions.push_back(std::move(call_named_personalized));

  dart::fcb::BytecodeFunction call_too_many_named;
  call_too_many_named.function_id =
      "package:app/closure.dart::callTooManyNamedGreeting(String)";
  call_too_many_named.parameter_count = 1;
  call_too_many_named.register_count = 1;
  call_too_many_named.bytecode_offset = call_too_many_named_offset;
  call_too_many_named.bytecode_length = 14;
  call_too_many_named.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makePersonalizedGreeting(String)"));
  call_too_many_named.constants.push_back(dart::fcb::Value::String(" friend"));
  call_too_many_named.constants.push_back(
      dart::fcb::Value::String(";named:suffix,extra"));
  module.functions.push_back(std::move(call_too_many_named));

  dart::fcb::BytecodeFunction call_too_few_args;
  call_too_few_args.function_id =
      "package:app/closure.dart::callTooFewArgsGreeting(String)";
  call_too_few_args.parameter_count = 1;
  call_too_few_args.register_count = 1;
  call_too_few_args.bytecode_offset = call_too_few_args_offset;
  call_too_few_args.bytecode_length = 11;
  call_too_few_args.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makePersonalizedGreeting(String)"));
  module.functions.push_back(std::move(call_too_few_args));

  dart::fcb::BytecodeFunction call_too_many_args;
  call_too_many_args.function_id =
      "package:app/closure.dart::callTooManyArgsGreeting(String)";
  call_too_many_args.parameter_count = 1;
  call_too_many_args.register_count = 1;
  call_too_many_args.bytecode_offset = call_too_many_args_offset;
  call_too_many_args.bytecode_length = 17;
  call_too_many_args.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makePersonalizedGreeting(String)"));
  call_too_many_args.constants.push_back(dart::fcb::Value::String(" friend"));
  call_too_many_args.constants.push_back(dart::fcb::Value::String(" extra"));
  module.functions.push_back(std::move(call_too_many_args));

  dart::fcb::BytecodeFunction branch_local_closure;
  branch_local_closure.function_id =
      "package:app/closure.dart::makeBranchLocalGreeting.<closure>()";
  branch_local_closure.parameter_count = 3;
  branch_local_closure.register_count = 4;
  branch_local_closure.bytecode_offset = branch_local_closure_offset;
  branch_local_closure.bytecode_length = 37;
  branch_local_closure.constants.push_back(
      dart::fcb::Value::String(" enabled"));
  branch_local_closure.constants.push_back(
      dart::fcb::Value::String(" disabled"));
  module.functions.push_back(std::move(branch_local_closure));

  dart::fcb::BytecodeFunction make_branch_local;
  make_branch_local.function_id =
      "package:app/closure.dart::makeBranchLocalGreeting(String)";
  make_branch_local.parameter_count = 1;
  make_branch_local.register_count = 1;
  make_branch_local.bytecode_offset = make_branch_local_offset;
  make_branch_local.bytecode_length = 9;
  make_branch_local.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makeBranchLocalGreeting.<closure>();"
      "captures:2"));
  make_branch_local.constants.push_back(dart::fcb::Value::String("patched "));
  module.functions.push_back(std::move(make_branch_local));

  dart::fcb::BytecodeFunction call_branch_local_true;
  call_branch_local_true.function_id =
      "package:app/closure.dart::callBranchLocalGreetingTrue(String)";
  call_branch_local_true.parameter_count = 1;
  call_branch_local_true.register_count = 1;
  call_branch_local_true.bytecode_offset = call_branch_local_true_offset;
  call_branch_local_true.bytecode_length = 14;
  call_branch_local_true.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makeBranchLocalGreeting(String)"));
  call_branch_local_true.constants.push_back(dart::fcb::Value::Bool(true));
  module.functions.push_back(std::move(call_branch_local_true));

  dart::fcb::BytecodeFunction call_branch_local_false;
  call_branch_local_false.function_id =
      "package:app/closure.dart::callBranchLocalGreetingFalse(String)";
  call_branch_local_false.parameter_count = 1;
  call_branch_local_false.register_count = 1;
  call_branch_local_false.bytecode_offset = call_branch_local_false_offset;
  call_branch_local_false.bytecode_length = 14;
  call_branch_local_false.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makeBranchLocalGreeting(String)"));
  call_branch_local_false.constants.push_back(dart::fcb::Value::Bool(false));
  module.functions.push_back(std::move(call_branch_local_false));

  dart::fcb::BytecodeFunction invoke_branch_local;
  invoke_branch_local.function_id =
      "package:app/closure.dart::invokeBranchLocalGreeting(Object,bool)";
  invoke_branch_local.parameter_count = 2;
  invoke_branch_local.register_count = 2;
  invoke_branch_local.bytecode_offset = invoke_branch_local_offset;
  invoke_branch_local.bytecode_length = 9;
  module.functions.push_back(std::move(invoke_branch_local));

  dart::fcb::BytecodeFunction pass_branch_local;
  pass_branch_local.function_id =
      "package:app/closure.dart::passBranchLocalGreeting(String)";
  pass_branch_local.parameter_count = 1;
  pass_branch_local.register_count = 1;
  pass_branch_local.bytecode_offset = pass_branch_local_offset;
  pass_branch_local.bytecode_length = 14;
  pass_branch_local.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makeBranchLocalGreeting(String)"));
  pass_branch_local.constants.push_back(dart::fcb::Value::Bool(true));
  pass_branch_local.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::invokeBranchLocalGreeting(Object,bool)"));
  module.functions.push_back(std::move(pass_branch_local));

  dart::fcb::BytecodeFunction throwing_closure;
  throwing_closure.function_id =
      "package:app/closure.dart::makeThrowingClosure.<closure>()";
  throwing_closure.parameter_count = 0;
  throwing_closure.register_count = 0;
  throwing_closure.bytecode_offset = throwing_closure_offset;
  throwing_closure.bytecode_length = 5;
  throwing_closure.constants.push_back(
      dart::fcb::Value::String("bytecode-boom"));
  throwing_closure.source_map.push_back(
      {0, "package:app/closure.dart:77:5"});
  module.functions.push_back(std::move(throwing_closure));

  dart::fcb::BytecodeFunction make_throwing;
  make_throwing.function_id =
      "package:app/closure.dart::makeThrowingClosure()";
  make_throwing.parameter_count = 0;
  make_throwing.register_count = 0;
  make_throwing.bytecode_offset = make_throwing_offset;
  make_throwing.bytecode_length = 4;
  make_throwing.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makeThrowingClosure.<closure>();captures:0"));
  module.functions.push_back(std::move(make_throwing));

  dart::fcb::BytecodeFunction call_catch_throwing;
  call_catch_throwing.function_id =
      "package:app/closure.dart::callCatchThrowingClosure()";
  call_catch_throwing.parameter_count = 0;
  call_catch_throwing.register_count = 1;
  call_catch_throwing.bytecode_offset = call_catch_throwing_offset;
  call_catch_throwing.bytecode_length = 22;
  call_catch_throwing.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makeThrowingClosure()"));
  call_catch_throwing.constants.push_back(
      dart::fcb::Value::String("caught-bytecode"));
  module.functions.push_back(std::move(call_catch_throwing));

  dart::fcb::BytecodeFunction call_uncaught_throwing;
  call_uncaught_throwing.function_id =
      "package:app/closure.dart::callUncaughtThrowingClosure()";
  call_uncaught_throwing.parameter_count = 0;
  call_uncaught_throwing.register_count = 0;
  call_uncaught_throwing.bytecode_offset = call_uncaught_throwing_offset;
  call_uncaught_throwing.bytecode_length = 9;
  call_uncaught_throwing.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::makeThrowingClosure()"));
  call_uncaught_throwing.source_map.push_back(
      {0, "package:app/closure.dart:101:3"});
  module.functions.push_back(std::move(call_uncaught_throwing));

  return module;
}

dart::fcb::BytecodeModule EscapingBytecodeClosureModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,        // LoadArg 0: captured prefix.
      0xff,              // Return.
      0x02, 0x00,        // LoadArg 0: prefix.
      0x54, 0x00, 0x00,  // MakeClosure closureBody;captures:1.
      0xff,              // Return.
  };

  dart::fcb::BytecodeFunction closure_body;
  closure_body.function_id =
      "package:app/closure.dart::escapingClosureBody()";
  closure_body.parameter_count = 1;
  closure_body.register_count = 1;
  closure_body.bytecode_offset = 0;
  closure_body.bytecode_length = 3;
  module.functions.push_back(std::move(closure_body));

  dart::fcb::BytecodeFunction make_escaping;
  make_escaping.function_id =
      "package:app/closure.dart::makeEscapingClosure";
  make_escaping.parameter_count = 1;
  make_escaping.register_count = 1;
  make_escaping.bytecode_offset = 3;
  make_escaping.bytecode_length = 6;
  make_escaping.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::escapingClosureBody();captures:1"));
  module.functions.push_back(std::move(make_escaping));

  return module;
}

dart::fcb::BytecodeModule GenericEscapingBytecodeClosureModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,        // LoadArg 0: captured prefix.
      0x02, 0x01,        // LoadArg 1: captured name.
      0x02, 0x02,        // LoadArg 2: generic closure value argument.
      0x42, 0x00, 0x03,  // StringConcat 3.
      0xff,              // Return.
      0x01, 0x00, 0x01,  // LoadConst "generic ".
      0x02, 0x00,        // LoadArg 0: name.
      0x54, 0x00, 0x00,  // MakeClosure body;captures:2;type-params:1.
      0xff,              // Return.
  };

  dart::fcb::BytecodeFunction closure_body;
  closure_body.function_id =
      "package:app/closure.dart::genericEscapingClosureBody()";
  closure_body.parameter_count = 3;
  closure_body.register_count = 3;
  closure_body.bytecode_offset = 0;
  closure_body.bytecode_length = 10;
  module.functions.push_back(std::move(closure_body));

  dart::fcb::BytecodeFunction make_escaping;
  make_escaping.function_id =
      "package:app/closure.dart::makeGenericEscapingClosure(String)";
  make_escaping.parameter_count = 1;
  make_escaping.register_count = 1;
  make_escaping.bytecode_offset = 10;
  make_escaping.bytecode_length = 9;
  make_escaping.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::genericEscapingClosureBody();captures:2;"
      "type-params:1"));
  make_escaping.constants.push_back(dart::fcb::Value::String("generic "));
  module.functions.push_back(std::move(make_escaping));

  return module;
}

dart::fcb::BytecodeModule NamedEscapingBytecodeClosureModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,        // LoadArg 0: captured prefix.
      0x02, 0x01,        // LoadArg 1: named suffix.
      0x42, 0x00, 0x02,  // StringConcat 2.
      0xff,              // Return.
      0x02, 0x00,        // LoadArg 0: prefix.
      0x54, 0x00, 0x00,  // MakeClosure closureBody;captures:1;named:suffix.
      0xff,              // Return.
  };

  dart::fcb::BytecodeFunction closure_body;
  closure_body.function_id =
      "package:app/closure.dart::namedEscapingClosureBody()";
  closure_body.parameter_count = 2;
  closure_body.register_count = 2;
  closure_body.bytecode_offset = 0;
  closure_body.bytecode_length = 8;
  module.functions.push_back(std::move(closure_body));

  dart::fcb::BytecodeFunction make_escaping;
  make_escaping.function_id =
      "package:app/closure.dart::makeNamedEscapingClosure(String)";
  make_escaping.parameter_count = 1;
  make_escaping.register_count = 1;
  make_escaping.bytecode_offset = 8;
  make_escaping.bytecode_length = 6;
  make_escaping.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::namedEscapingClosureBody();captures:1;"
      "named:suffix"));
  module.functions.push_back(std::move(make_escaping));

  return module;
}

dart::fcb::BytecodeModule OptionalEscapingBytecodeClosureModule() {
  dart::fcb::BytecodeModule module;
  module.version = 1;
  module.bytecode = {
      0x02, 0x00,        // LoadArg 0: captured prefix.
      0x02, 0x01,        // LoadArg 1: optional suffix.
      0x42, 0x00, 0x02,  // StringConcat 2.
      0xff,              // Return.
      0x02, 0x00,        // LoadArg 0: prefix.
      0x54, 0x00, 0x00,  // MakeClosure positional body;captures:1;optional-pos:1.
      0xff,              // Return.
      0x02, 0x00,        // LoadArg 0: prefix.
      0x54, 0x00, 0x00,  // MakeClosure named body;captures:1;named:?suffix.
      0xff,              // Return.
  };

  dart::fcb::BytecodeFunction positional_body;
  positional_body.function_id =
      "package:app/closure.dart::optionalPositionalClosureBody()";
  positional_body.parameter_count = 2;
  positional_body.register_count = 2;
  positional_body.bytecode_offset = 0;
  positional_body.bytecode_length = 8;
  module.functions.push_back(std::move(positional_body));

  dart::fcb::BytecodeFunction make_positional;
  make_positional.function_id =
      "package:app/closure.dart::makeOptionalPositionalClosure(String)";
  make_positional.parameter_count = 1;
  make_positional.register_count = 1;
  make_positional.bytecode_offset = 8;
  make_positional.bytecode_length = 6;
  make_positional.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::optionalPositionalClosureBody();captures:1;"
      "optional-pos:1"));
  module.functions.push_back(std::move(make_positional));

  dart::fcb::BytecodeFunction named_body;
  named_body.function_id =
      "package:app/closure.dart::optionalNamedClosureBody()";
  named_body.parameter_count = 2;
  named_body.register_count = 2;
  named_body.bytecode_offset = 0;
  named_body.bytecode_length = 8;
  module.functions.push_back(std::move(named_body));

  dart::fcb::BytecodeFunction make_named;
  make_named.function_id =
      "package:app/closure.dart::makeOptionalNamedClosure(String)";
  make_named.parameter_count = 1;
  make_named.register_count = 1;
  make_named.bytecode_offset = 14;
  make_named.bytecode_length = 6;
  make_named.constants.push_back(dart::fcb::Value::String(
      "package:app/closure.dart::optionalNamedClosureBody();captures:1;"
      "named:?suffix"));
  module.functions.push_back(std::move(make_named));

  return module;
}

}  // namespace

namespace dart {

UNIT_TEST_CASE(FcbPatchRuntimeReturningBytecodeClosureCapturesContext) {
  fcb::PatchRuntime runtime;
  std::string error;
  EXPECT(runtime.LoadModule(ReturningBytecodeClosureModule(), &error));

  fcb::InterpretResult closure_result = runtime.Interpret(
      "package:app/closure.dart::makeGreeting(String)",
      {fcb::Value::String("Ada")});
  EXPECT(closure_result.ok);
  EXPECT(closure_result.value.kind == fcb::ValueKind::kBytecodeClosure);
  EXPECT(closure_result.value.closure_captures.size() == 2);

  fcb::InterpretResult call_result = runtime.Interpret(
      "package:app/closure.dart::callGreeting(String)",
      {fcb::Value::String("Ada")});
  EXPECT(call_result.ok);
  EXPECT(call_result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("patched Ada", call_result.value.string_value.c_str());

  fcb::InterpretResult personalized_result = runtime.Interpret(
      "package:app/closure.dart::callPersonalizedGreeting(String)",
      {fcb::Value::String("Ada")});
  EXPECT(personalized_result.ok);
  EXPECT(personalized_result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("patched Ada friend",
               personalized_result.value.string_value.c_str());

  fcb::InterpretResult named_personalized_result = runtime.Interpret(
      "package:app/closure.dart::callNamedPersonalizedGreeting(String)",
      {fcb::Value::String("Ada")});
  EXPECT(named_personalized_result.ok);
  EXPECT(named_personalized_result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("patched Ada friend",
               named_personalized_result.value.string_value.c_str());

  fcb::InterpretResult too_many_named_result = runtime.Interpret(
      "package:app/closure.dart::callTooManyNamedGreeting(String)",
      {fcb::Value::String("Ada")});
  EXPECT(!too_many_named_result.ok);
  EXPECT(too_many_named_result.error.find(
             "too many named bytecode closure arguments") !=
         std::string::npos);

  fcb::InterpretResult too_few_args_result = runtime.Interpret(
      "package:app/closure.dart::callTooFewArgsGreeting(String)",
      {fcb::Value::String("Ada")});
  EXPECT(!too_few_args_result.ok);
  EXPECT(too_few_args_result.error.find(
             "bytecode closure argument count mismatch") != std::string::npos);

  fcb::InterpretResult too_many_args_result = runtime.Interpret(
      "package:app/closure.dart::callTooManyArgsGreeting(String)",
      {fcb::Value::String("Ada")});
  EXPECT(!too_many_args_result.ok);
  EXPECT(too_many_args_result.error.find(
             "bytecode closure argument count mismatch") != std::string::npos);

  fcb::InterpretResult branch_local_true_result = runtime.Interpret(
      "package:app/closure.dart::callBranchLocalGreetingTrue(String)",
      {fcb::Value::String("Ada")});
  EXPECT(branch_local_true_result.ok);
  EXPECT(branch_local_true_result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("patched Ada enabled",
               branch_local_true_result.value.string_value.c_str());

  fcb::InterpretResult branch_local_false_result = runtime.Interpret(
      "package:app/closure.dart::callBranchLocalGreetingFalse(String)",
      {fcb::Value::String("Ada")});
  EXPECT(branch_local_false_result.ok);
  EXPECT(branch_local_false_result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("patched Ada disabled",
               branch_local_false_result.value.string_value.c_str());

  fcb::InterpretResult pass_branch_local_result = runtime.Interpret(
      "package:app/closure.dart::passBranchLocalGreeting(String)",
      {fcb::Value::String("Ada")});
  EXPECT(pass_branch_local_result.ok);
  EXPECT(pass_branch_local_result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("patched Ada enabled",
               pass_branch_local_result.value.string_value.c_str());

  fcb::InterpretResult catch_throwing_result = runtime.Interpret(
      "package:app/closure.dart::callCatchThrowingClosure()", {});
  EXPECT(catch_throwing_result.ok);
  EXPECT(catch_throwing_result.value.kind == fcb::ValueKind::kString);
  EXPECT_STREQ("caught-bytecode",
               catch_throwing_result.value.string_value.c_str());
  EXPECT(fcb::internal::PatchStackTraceLocationCount() == 0);

  fcb::InterpretResult uncaught_throwing_result = runtime.Interpret(
      "package:app/closure.dart::callUncaughtThrowingClosure()", {});
  EXPECT(!uncaught_throwing_result.ok);
  EXPECT(uncaught_throwing_result.error.find(
             "CallClosure failed") != std::string::npos);
  EXPECT(fcb::internal::PatchStackTraceLocationCount() >= 2);
  EXPECT_STREQ("package:app/closure.dart:77:5",
               fcb::internal::PatchStackTraceLocationAt(0));
  EXPECT_STREQ("package:app/closure.dart:101:3",
               fcb::internal::PatchStackTraceLocationAt(1));
}

ISOLATE_UNIT_TEST_CASE(FcbPatchEntryMaterializesEscapingBytecodeClosure) {
  IsolateGroup* isolate_group = IsolateGroup::Current();
  ASSERT(thread != nullptr);
  ASSERT(isolate_group != nullptr);

  fcb::PatchRuntime* runtime = isolate_group->EnsureFcbPatchRuntime();
  std::string error;
  if (!runtime->LoadModule(EscapingBytecodeClosureModule(), &error)) {
    OS::PrintErr("EscapingBytecodeClosureModule failed: %s\n", error.c_str());
    EXPECT(false);
  }

  fcb::InterpretResult closure_result = runtime->Interpret(
      "package:app/closure.dart::makeEscapingClosure",
      {fcb::Value::String("patched")});
  EXPECT(closure_result.ok);
  EXPECT(closure_result.value.kind == fcb::ValueKind::kBytecodeClosure);

  const Object& closure_object =
      Object::Handle(thread->zone(), closure_result.value.ToDart());
  EXPECT(closure_object.IsClosure());

  const Array& closure_arguments = Array::Handle(thread->zone(), Array::New(1));
  closure_arguments.SetAt(0, closure_object, thread);
  const Object& call_result = Object::Handle(
      thread->zone(), DartEntry::InvokeClosure(thread, closure_arguments));
  EXPECT(call_result.IsString());
  EXPECT_STREQ("patched", String::Cast(call_result).ToCString());

  const fcb::DispatchDecision decision =
      runtime->Resolve("package:app/closure.dart::makeEscapingClosure");
  EXPECT(decision.state == fcb::PatchState::kPatchedInterpreted);
  isolate_group->ClearFcbPatchRuntime();
}

ISOLATE_UNIT_TEST_CASE(FcbPatchEntryMaterializesGenericBytecodeClosure) {
  IsolateGroup* isolate_group = IsolateGroup::Current();
  ASSERT(thread != nullptr);
  ASSERT(isolate_group != nullptr);

  fcb::PatchRuntime* runtime = isolate_group->EnsureFcbPatchRuntime();
  std::string error;
  if (!runtime->LoadModule(GenericEscapingBytecodeClosureModule(), &error)) {
    OS::PrintErr("GenericEscapingBytecodeClosureModule failed: %s\n",
                 error.c_str());
    EXPECT(false);
  }

  fcb::InterpretResult closure_result = runtime->Interpret(
      "package:app/closure.dart::makeGenericEscapingClosure(String)",
      {fcb::Value::String("Ada ")});
  EXPECT(closure_result.ok);
  EXPECT(closure_result.value.kind == fcb::ValueKind::kBytecodeClosure);
  EXPECT(closure_result.value.closure_type_parameter_count == 1);

  const Object& closure_object =
      Object::Handle(thread->zone(), closure_result.value.ToDart());
  EXPECT(closure_object.IsClosure());

  Zone* zone = thread->zone();
  const TypeArguments& type_args =
      TypeArguments::Handle(zone, TypeArguments::New(1));
  type_args.SetTypeAt(0, Type::Handle(zone, Type::StringType()));
  const Array& descriptor = Array::Handle(
      zone, ArgumentsDescriptor::NewBoxed(
                /*type_args_len=*/1, /*num_arguments=*/2));
  const Array& closure_arguments = Array::Handle(zone, Array::New(3));
  closure_arguments.SetAt(0, type_args, thread);
  closure_arguments.SetAt(1, closure_object, thread);
  closure_arguments.SetAt(2, String::Handle(zone, String::New("value")),
                          thread);

  const Object& call_result = Object::Handle(
      zone, DartEntry::InvokeClosure(thread, closure_arguments, descriptor));
  EXPECT(call_result.IsString());
  EXPECT_STREQ("generic Ada value", String::Cast(call_result).ToCString());

  isolate_group->ClearFcbPatchRuntime();
}

ISOLATE_UNIT_TEST_CASE(FcbPatchEntryMaterializesNamedBytecodeClosure) {
  IsolateGroup* isolate_group = IsolateGroup::Current();
  ASSERT(thread != nullptr);
  ASSERT(isolate_group != nullptr);

  fcb::PatchRuntime* runtime = isolate_group->EnsureFcbPatchRuntime();
  std::string error;
  if (!runtime->LoadModule(NamedEscapingBytecodeClosureModule(), &error)) {
    OS::PrintErr("NamedEscapingBytecodeClosureModule failed: %s\n",
                 error.c_str());
    EXPECT(false);
  }

  fcb::InterpretResult closure_result = runtime->Interpret(
      "package:app/closure.dart::makeNamedEscapingClosure(String)",
      {fcb::Value::String("patched ")});
  EXPECT(closure_result.ok);
  EXPECT(closure_result.value.kind == fcb::ValueKind::kBytecodeClosure);

  const Object& closure_object =
      Object::Handle(thread->zone(), closure_result.value.ToDart());
  EXPECT(closure_object.IsClosure());

  const Array& argument_names =
      Array::Handle(thread->zone(), Array::New(1));
  const String& suffix_name =
      String::Handle(thread->zone(), Symbols::New(thread, "suffix"));
  argument_names.SetAt(0, suffix_name, thread);
  const Array& descriptor = Array::Handle(
      thread->zone(), ArgumentsDescriptor::NewBoxed(
                          /*type_args_len=*/0, /*num_arguments=*/2,
                          argument_names));
  const Array& closure_arguments =
      Array::Handle(thread->zone(), Array::New(2));
  closure_arguments.SetAt(0, closure_object, thread);
  const String& suffix =
      String::Handle(thread->zone(), String::New("friend"));
  closure_arguments.SetAt(1, suffix, thread);

  const Object& call_result = Object::Handle(
      thread->zone(),
      DartEntry::InvokeClosure(thread, closure_arguments, descriptor));
  EXPECT(call_result.IsString());
  EXPECT_STREQ("patched friend", String::Cast(call_result).ToCString());

  isolate_group->ClearFcbPatchRuntime();
}

ISOLATE_UNIT_TEST_CASE(FcbPatchEntryMaterializesOptionalBytecodeClosure) {
  IsolateGroup* isolate_group = IsolateGroup::Current();
  ASSERT(thread != nullptr);
  ASSERT(isolate_group != nullptr);

  fcb::PatchRuntime* runtime = isolate_group->EnsureFcbPatchRuntime();
  std::string error;
  if (!runtime->LoadModule(OptionalEscapingBytecodeClosureModule(), &error)) {
    OS::PrintErr("OptionalEscapingBytecodeClosureModule failed: %s\n",
                 error.c_str());
    EXPECT(false);
  }

  fcb::InterpretResult positional_result = runtime->Interpret(
      "package:app/closure.dart::makeOptionalPositionalClosure(String)",
      {fcb::Value::String("patched ")});
  EXPECT(positional_result.ok);
  const Object& positional_closure =
      Object::Handle(thread->zone(), positional_result.value.ToDart());
  EXPECT(positional_closure.IsClosure());

  const Array& no_positional_arguments =
      Array::Handle(thread->zone(), Array::New(1));
  no_positional_arguments.SetAt(0, positional_closure, thread);
  const Object& default_positional_result = Object::Handle(
      thread->zone(),
      DartEntry::InvokeClosure(thread, no_positional_arguments));
  EXPECT(default_positional_result.IsString());
  EXPECT_STREQ("patched null",
               String::Cast(default_positional_result).ToCString());

  const Array& with_positional_arguments =
      Array::Handle(thread->zone(), Array::New(2));
  with_positional_arguments.SetAt(0, positional_closure, thread);
  with_positional_arguments.SetAt(
      1, String::Handle(thread->zone(), String::New("friend")), thread);
  const Object& with_positional_result = Object::Handle(
      thread->zone(),
      DartEntry::InvokeClosure(thread, with_positional_arguments));
  EXPECT(with_positional_result.IsString());
  EXPECT_STREQ("patched friend",
               String::Cast(with_positional_result).ToCString());

  fcb::InterpretResult named_result = runtime->Interpret(
      "package:app/closure.dart::makeOptionalNamedClosure(String)",
      {fcb::Value::String("patched ")});
  EXPECT(named_result.ok);
  const Object& named_closure =
      Object::Handle(thread->zone(), named_result.value.ToDart());
  EXPECT(named_closure.IsClosure());

  const Array& no_named_arguments =
      Array::Handle(thread->zone(), Array::New(1));
  no_named_arguments.SetAt(0, named_closure, thread);
  const Object& default_named_result = Object::Handle(
      thread->zone(), DartEntry::InvokeClosure(thread, no_named_arguments));
  EXPECT(default_named_result.IsString());
  EXPECT_STREQ("patched null",
               String::Cast(default_named_result).ToCString());

  const Array& argument_names =
      Array::Handle(thread->zone(), Array::New(1));
  const String& suffix_name =
      String::Handle(thread->zone(), Symbols::New(thread, "suffix"));
  argument_names.SetAt(0, suffix_name, thread);
  const Array& descriptor = Array::Handle(
      thread->zone(), ArgumentsDescriptor::NewBoxed(
                          /*type_args_len=*/0, /*num_arguments=*/2,
                          argument_names));
  const Array& with_named_arguments =
      Array::Handle(thread->zone(), Array::New(2));
  with_named_arguments.SetAt(0, named_closure, thread);
  with_named_arguments.SetAt(
      1, String::Handle(thread->zone(), String::New("friend")), thread);
  const Object& with_named_result = Object::Handle(
      thread->zone(),
      DartEntry::InvokeClosure(thread, with_named_arguments, descriptor));
  EXPECT(with_named_result.IsString());
  EXPECT_STREQ("patched friend", String::Cast(with_named_result).ToCString());

  isolate_group->ClearFcbPatchRuntime();
}

}  // namespace dart
