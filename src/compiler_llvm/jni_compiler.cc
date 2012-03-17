/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jni_compiler.h"

#include "class_linker.h"
#include "compilation_unit.h"
#include "compiled_method.h"
#include "compiler.h"
#include "compiler_llvm.h"
#include "ir_builder.h"
#include "logging.h"
#include "oat_compilation_unit.h"
#include "object.h"
#include "runtime.h"
#include "runtime_support_func.h"
#include "utils_llvm.h"

#include <llvm/Analysis/Verifier.h>
#include <llvm/BasicBlock.h>
#include <llvm/DerivedTypes.h>
#include <llvm/Function.h>
#include <llvm/Type.h>

namespace art {
namespace compiler_llvm {


JniCompiler::JniCompiler(CompilationUnit* cunit,
                         Compiler const& compiler,
                         OatCompilationUnit* oat_compilation_unit)
: cunit_(cunit), compiler_(&compiler), module_(cunit_->GetModule()),
  context_(cunit_->GetLLVMContext()), irb_(*cunit_->GetIRBuilder()),
  oat_compilation_unit_(oat_compilation_unit),
  access_flags_(oat_compilation_unit->access_flags_),
  method_idx_(oat_compilation_unit->method_idx_),
  class_linker_(oat_compilation_unit->class_linker_),
  class_loader_(oat_compilation_unit->class_loader_),
  dex_cache_(oat_compilation_unit->dex_cache_),
  dex_file_(oat_compilation_unit->dex_file_),
  method_(dex_cache_->GetResolvedMethod(method_idx_)) {

  // Check: Ensure that the method is resolved
  CHECK_NE(method_, static_cast<art::Method*>(NULL));

  // Check: Ensure that JNI compiler will only get "native" method
  CHECK((access_flags_ & kAccNative) != 0);
}


CompiledMethod* JniCompiler::Compile() {
  bool is_static = method_->IsStatic();

  CreateFunction();

  // Set argument name
  llvm::Function::arg_iterator arg_begin(func_->arg_begin());
  llvm::Function::arg_iterator arg_end(func_->arg_end());
  llvm::Function::arg_iterator arg_iter(arg_begin);

  DCHECK_NE(arg_iter, arg_end);
  arg_iter->setName("method");
  llvm::Value* method_object_addr = arg_iter++;

  // Actual argument (ignore method)
  arg_begin = arg_iter;

  // Count the number of Object* arguments
  uint32_t sirt_size = (is_static ? 1 : 0); // Class object for static function
  for (unsigned i = 0; arg_iter != arg_end; ++i, ++arg_iter) {
    arg_iter->setName(StringPrintf("a%u", i));
    if (arg_iter->getType() == irb_.getJObjectTy()) {
      ++sirt_size;
    }
  }

  // Start to build IR
  irb_.SetInsertPoint(basic_block_);

  llvm::Value* thread_object_addr =
    irb_.CreateCall(irb_.GetRuntime(runtime_support::GetCurrentThread));

  // Shadow stack
  llvm::StructType* shadow_frame_type = irb_.getShadowFrameTy(sirt_size);
  shadow_frame_ = irb_.CreateAlloca(shadow_frame_type);

  // Zero-initialization of the shadow frame
  llvm::ConstantAggregateZero* zero_initializer =
    llvm::ConstantAggregateZero::get(shadow_frame_type);

  irb_.CreateStore(zero_initializer, shadow_frame_);

  // Variables for GetElementPtr
  llvm::Constant* zero = irb_.getInt32(0);
  llvm::Value* gep_index[] = {
    zero, // No displacement for shadow frame pointer
    zero, // Get the %ArtFrame data structure
    NULL,
  };

  // Store the method pointer
  gep_index[2] = irb_.getInt32(2);
  llvm::Value* method_field_addr = irb_.CreateGEP(shadow_frame_, gep_index);
  irb_.CreateStore(method_object_addr, method_field_addr);

  // Store the number of the pointer slots
  gep_index[2] = irb_.getInt32(0);
  llvm::Value* size_field_addr = irb_.CreateGEP(shadow_frame_, gep_index);
  llvm::ConstantInt* sirt_size_value = irb_.getInt32(sirt_size);
  irb_.CreateStore(sirt_size_value, size_field_addr);

  // Push the shadow frame
  llvm::Value* shadow_frame_upcast = irb_.CreateConstGEP2_32(shadow_frame_, 0, 0);
  irb_.CreateCall(irb_.GetRuntime(runtime_support::PushShadowFrame), shadow_frame_upcast);

  // Set top of managed stack to the method field in the SIRT
  StoreToObjectOffset(thread_object_addr, Thread::TopOfManagedStackOffset().Int32Value(),
                      method_field_addr->getType(), method_field_addr);

  // Get JNIEnv
  llvm::Value* jni_env_object_addr = LoadFromObjectOffset(thread_object_addr,
                                                          Thread::JniEnvOffset().Int32Value(),
                                                          irb_.getJObjectTy());

  // Set thread state to kNative
  StoreToObjectOffset(thread_object_addr, Thread::StateOffset().Int32Value(),
                      irb_.getInt32Ty(), irb_.getInt32(Thread::kNative));

  // Get callee code_addr
  llvm::Value* code_addr =
      LoadFromObjectOffset(method_object_addr,
                           Method::NativeMethodOffset().Int32Value(),
                           GetFunctionType(method_idx_, is_static, true)->getPointerTo());


  // Load actual parameters
  std::vector<llvm::Value*> args;

  args.push_back(jni_env_object_addr);
  //args.push_back(method_object_addr); // method object for callee

  // Store arguments to SIRT, and push back to args
  gep_index[1] = irb_.getInt32(1); // SIRT
  size_t sirt_member_index = 0;

  // Push class argument if this method is static
  if (is_static) {
    llvm::Value* class_object_addr =
        LoadFromObjectOffset(method_object_addr,
                             Method::DeclaringClassOffset().Int32Value(),
                             irb_.getJObjectTy());
    gep_index[2] = irb_.getInt32(sirt_member_index++);
    llvm::Value* sirt_field_addr = irb_.CreateGEP(shadow_frame_, gep_index);
    irb_.CreateStore(class_object_addr, sirt_field_addr);
    args.push_back(irb_.CreateBitCast(sirt_field_addr, irb_.getJObjectTy()));
  }
  for (arg_iter = arg_begin; arg_iter != arg_end; ++arg_iter) {
    if (arg_iter->getType() == irb_.getJObjectTy()) {
      gep_index[2] = irb_.getInt32(sirt_member_index++);
      llvm::Value* sirt_field_addr = irb_.CreateGEP(shadow_frame_, gep_index);
      irb_.CreateStore(arg_iter, sirt_field_addr);
      // Note null is placed in the SIRT but the jobject passed to the native code must be null
      // (not a pointer into the SIRT as with regular references).
      llvm::Value* equal_null = irb_.CreateICmpEQ(arg_iter, irb_.getJNull());
      llvm::Value* arg =
          irb_.CreateSelect(equal_null,
                            irb_.getJNull(),
                            irb_.CreateBitCast(sirt_field_addr, irb_.getJObjectTy()));
      args.push_back(arg);
    } else {
      args.push_back(arg_iter);
    }
  }


  // saved_local_ref_cookie = env->local_ref_cookie
  llvm::Value* saved_local_ref_cookie =
      LoadFromObjectOffset(jni_env_object_addr,
                           JNIEnvExt::LocalRefCookieOffset().Int32Value(),
                           irb_.getInt32Ty());

  // env->local_ref_cookie = env->locals.segment_state
  llvm::Value* segment_state =
      LoadFromObjectOffset(jni_env_object_addr,
                           JNIEnvExt::SegmentStateOffset().Int32Value(),
                           irb_.getInt32Ty());
  StoreToObjectOffset(jni_env_object_addr,
                      JNIEnvExt::LocalRefCookieOffset().Int32Value(),
                      irb_.getInt32Ty(),
                      segment_state);


  // Call!!!
  llvm::Value* retval = irb_.CreateCall(code_addr, args);


  // Set thread state to kRunnable
  StoreToObjectOffset(thread_object_addr, Thread::StateOffset().Int32Value(),
                      irb_.getInt32Ty(), irb_.getInt32(Thread::kRunnable));

  // Get return shorty
  DexFile::MethodId const& method_id = dex_file_->GetMethodId(method_idx_);
  uint32_t shorty_size;
  char ret_shorty = dex_file_->GetMethodShorty(method_id, &shorty_size)[0];
  CHECK_GE(shorty_size, 1u);

  if (ret_shorty == 'L') {
    // If the return value is reference, it may point to SIRT, we should decode it.
    retval = irb_.CreateCall2(irb_.GetRuntime(runtime_support::DecodeJObjectInThread),
                              thread_object_addr, retval);
  }

  // env->locals.segment_state = env->local_ref_cookie
  llvm::Value* local_ref_cookie =
      LoadFromObjectOffset(jni_env_object_addr,
                           JNIEnvExt::LocalRefCookieOffset().Int32Value(),
                           irb_.getInt32Ty());
  StoreToObjectOffset(jni_env_object_addr,
                      JNIEnvExt::SegmentStateOffset().Int32Value(),
                      irb_.getInt32Ty(),
                      local_ref_cookie);

  // env->local_ref_cookie = saved_local_ref_cookie
  StoreToObjectOffset(jni_env_object_addr, JNIEnvExt::LocalRefCookieOffset().Int32Value(),
                      irb_.getInt32Ty(), saved_local_ref_cookie);

  // Pop the shadow frame
  irb_.CreateCall(irb_.GetRuntime(runtime_support::PopShadowFrame));

  // Return!
  if (ret_shorty != 'V') {
    irb_.CreateRet(retval);
  } else {
    irb_.CreateRetVoid();
  }

  // For debug
  //func_->dump();

  // Verify the generated bitcode
  llvm::verifyFunction(*func_, llvm::PrintMessageAction);

  return new CompiledMethod(cunit_->GetInstructionSet(),
                            cunit_->GetElfIndex());
}


void JniCompiler::CreateFunction() {
  // LLVM function name
  std::string func_name(LLVMLongName(method_));

  // Get function type
  llvm::FunctionType* func_type =
    GetFunctionType(method_idx_, method_->IsStatic(), false);

  // Create function
  func_ = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage,
                                 func_name, module_);

  // Create basic block
  basic_block_ = llvm::BasicBlock::Create(*context_, "B0", func_);
}


llvm::FunctionType* JniCompiler::GetFunctionType(uint32_t method_idx,
                                                 bool is_static, bool is_target_function) {
  // Get method signature
  DexFile::MethodId const& method_id = dex_file_->GetMethodId(method_idx);

  uint32_t shorty_size;
  char const* shorty = dex_file_->GetMethodShorty(method_id, &shorty_size);
  CHECK_GE(shorty_size, 1u);

  // Get return type
  llvm::Type* ret_type = irb_.getJType(shorty[0], kAccurate);

  // Get argument type
  std::vector<llvm::Type*> args_type;

  args_type.push_back(irb_.getJObjectTy()); // method object pointer

  if (!is_static || is_target_function) {
    // "this" object pointer for non-static
    // "class" object pointer for static
    args_type.push_back(irb_.getJType('L', kAccurate));
  }

  for (uint32_t i = 1; i < shorty_size; ++i) {
    args_type.push_back(irb_.getJType(shorty[i], kAccurate));
  }

  return llvm::FunctionType::get(ret_type, args_type, false);
}

llvm::Value* JniCompiler::LoadFromObjectOffset(llvm::Value* object_addr, int32_t offset,
                                               llvm::Type* type) {
  // Convert offset to llvm::value
  llvm::Value* llvm_offset = irb_.getPtrEquivInt(offset);
  // Calculate the value's address
  llvm::Value* value_addr = irb_.CreatePtrDisp(object_addr, llvm_offset, type->getPointerTo());
  // Load
  return irb_.CreateLoad(value_addr);
}

void JniCompiler::StoreToObjectOffset(llvm::Value* object_addr, int32_t offset,
                                      llvm::Type* type, llvm::Value* value) {
  // Convert offset to llvm::value
  llvm::Value* llvm_offset = irb_.getPtrEquivInt(offset);
  // Calculate the value's address
  llvm::Value* value_addr = irb_.CreatePtrDisp(object_addr, llvm_offset, type->getPointerTo());
  // Store
  irb_.CreateStore(value, value_addr);
}

} // namespace compiler_llvm
} // namespace art
