/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ART_RUNTIME_CHA_H_
#define ART_RUNTIME_CHA_H_

#include <unordered_map>
#include <unordered_set>

#include "base/enums.h"
#include "base/locks.h"
#include "handle.h"
#include "mirror/class.h"
#include "oat_quick_method_header.h"

namespace art {

class ArtMethod;
class LinearAlloc;

/**
 * Class Hierarchy Analysis (CHA) tries to devirtualize virtual calls into
 * direct calls based on the info generated by analyzing class hierarchies.
 * If a class is not subclassed, or even if it's subclassed but one of its
 * virtual methods isn't overridden, a virtual call for that method can be
 * changed into a direct call.
 *
 * Each virtual method carries a single-implementation status. The status is
 * incrementally maintained at the end of class linking time when method
 * overriding takes effect.
 *
 * Compiler takes advantage of the single-implementation info of a
 * method. If a method A has the single-implementation flag set, the compiler
 * devirtualizes the virtual call for method A into a direct call, and
 * further try to inline the direct call as a result. The compiler will
 * also register a dependency that the compiled code depends on the
 * assumption that method A has single-implementation status.
 *
 * When single-implementation info is updated at the end of class linking,
 * and if method A's single-implementation status is invalidated, all compiled
 * code that depends on the assumption that method A has single-implementation
 * status need to be invalidated. Method entrypoints that have this dependency
 * will be updated as a result. Method A can later be recompiled with less
 * aggressive assumptions.
 *
 * For live compiled code that's on stack, deoptmization will be initiated
 * to force the invalidated compiled code into interpreter mode to guarantee
 * correctness. The deoptimization mechanism used is a hybrid of
 * synchronous and asynchronous deoptimization. The synchronous deoptimization
 * part checks a hidden local variable flag for the method, and if true,
 * initiates deoptimization. The asynchronous deoptimization part issues a
 * checkpoint that walks the stack and for any compiled code on the stack
 * that should be deoptimized, set the hidden local variable value to be true.
 *
 * A cha_lock_ needs to be held for updating single-implementation status,
 * and registering/unregistering CHA dependencies. Registering CHA dependency
 * and making compiled code visible also need to be atomic. Otherwise, we
 * may miss invalidating CHA dependents or making compiled code visible even
 * after it is invalidated. Care needs to be taken between cha_lock_ and
 * JitCodeCache::lock_ to guarantee the atomicity.
 *
 * We base our CHA on dynamically linked class profiles instead of doing static
 * analysis. Static analysis can be too aggressive due to dynamic class loading
 * at runtime, and too conservative since some classes may not be really loaded
 * at runtime.
 */
class ClassHierarchyAnalysis {
 public:
  // Types for recording CHA dependencies.
  // For invalidating CHA dependency, we need to know both the ArtMethod and
  // the method header. If the ArtMethod has compiled code with the method header
  // as the entrypoint, we update the entrypoint to the interpreter bridge.
  // We will also deoptimize frames that are currently executing the code of
  // the method header.
  using MethodAndMethodHeaderPair = std::pair<ArtMethod*, OatQuickMethodHeader*>;
  using ListOfDependentPairs = std::vector<MethodAndMethodHeaderPair>;

  ClassHierarchyAnalysis() {}

  // Add a dependency that compiled code with `dependent_header` for `dependent_method`
  // assumes that virtual `method` has single-implementation.
  void AddDependency(ArtMethod* method,
                     ArtMethod* dependent_method,
                     OatQuickMethodHeader* dependent_header) REQUIRES(Locks::cha_lock_);

  // Return compiled code that assumes that `method` has single-implementation.
  const ListOfDependentPairs& GetDependents(ArtMethod* method) REQUIRES(Locks::cha_lock_);

  // Remove dependency tracking for compiled code that assumes that
  // `method` has single-implementation.
  void RemoveAllDependenciesFor(ArtMethod* method) REQUIRES(Locks::cha_lock_);

  // Remove from cha_dependency_map_ all entries that contain OatQuickMethodHeader from
  // the given `method_headers` set.
  // This is used when some compiled code is freed.
  void RemoveDependentsWithMethodHeaders(
      const std::unordered_set<OatQuickMethodHeader*>& method_headers)
      REQUIRES(Locks::cha_lock_);

  // If a given class belongs to a linear allocation that is about to be deleted, in all its
  // superclasses and superinterfaces reset SingleImplementation fields of their methods
  // that might be affected by the deletion.
  // The method is intended to be called during GC before ReclaimPhase, since it gets info from
  // Java objects that are going to be collected.
  // For the same reason it's important to access objects without read barrier to not revive them.
  void ResetSingleImplementationInHierarchy(ObjPtr<mirror::Class> klass,
                                            const LinearAlloc* alloc,
                                            PointerSize pointer_size)
      const REQUIRES_SHARED(Locks::mutator_lock_);

  // Update CHA info for methods that `klass` overrides, after loading `klass`.
  void UpdateAfterLoadingOf(Handle<mirror::Class> klass) REQUIRES_SHARED(Locks::mutator_lock_);

  // Remove all of the dependencies for a linear allocator. This is called when dex cache unloading
  // occurs.
  void RemoveDependenciesForLinearAlloc(const LinearAlloc* linear_alloc)
      REQUIRES(!Locks::cha_lock_);

 private:
  void InitSingleImplementationFlag(Handle<mirror::Class> klass,
                                    ArtMethod* method,
                                    PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Check/update single-implementation info when one virtual method
  // overrides another.
  // `virtual_method` in `klass` overrides `method_in_super`.
  // This may invalidate some assumptions on single-implementation.
  // Append methods that should have their single-implementation flag invalidated
  // to `invalidated_single_impl_methods`.
  void CheckVirtualMethodSingleImplementationInfo(
      Handle<mirror::Class> klass,
      ArtMethod* virtual_method,
      ArtMethod* method_in_super,
      std::unordered_set<ArtMethod*>& invalidated_single_impl_methods,
      PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Check/update single-implementation info when one method
  // implements an interface method.
  // `implementation_method` in `klass` implements `interface_method`.
  // Append `interface_method` to `invalidated_single_impl_methods`
  // if `interface_method` gets a new implementation.
  void CheckInterfaceMethodSingleImplementationInfo(
      Handle<mirror::Class> klass,
      ArtMethod* interface_method,
      ArtMethod* implementation_method,
      std::unordered_set<ArtMethod*>& invalidated_single_impl_methods,
      PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void InvalidateSingleImplementationMethods(
      std::unordered_set<ArtMethod*>& invalidated_single_impl_methods)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // A map that maps a method to a set of compiled code that assumes that method has a
  // single implementation, which is used to do CHA-based devirtualization.
  std::unordered_map<ArtMethod*, ListOfDependentPairs> cha_dependency_map_
    GUARDED_BY(Locks::cha_lock_);

  DISALLOW_COPY_AND_ASSIGN(ClassHierarchyAnalysis);
};

}  // namespace art

#endif  // ART_RUNTIME_CHA_H_
