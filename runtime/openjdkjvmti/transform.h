/* Copyright (C) 2016 The Android Open Source Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This file implements interfaces from the file jvmti.h. This implementation
 * is licensed under the same terms as the file jvmti.h.  The
 * copyright and license information for the file jvmti.h follows.
 *
 * Copyright (c) 2003, 2011, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#ifndef ART_RUNTIME_OPENJDKJVMTI_TRANSFORM_H_
#define ART_RUNTIME_OPENJDKJVMTI_TRANSFORM_H_

#include <string>

#include <jni.h>

#include "art_jvmti.h"
#include "ti_class_definition.h"
#include "jvmti.h"

namespace openjdkjvmti {

jvmtiError GetClassLocation(ArtJvmTiEnv* env, jclass klass, /*out*/std::string* location);

class Transformer {
 public:
  static jvmtiError RetransformClassesDirect(
      ArtJvmTiEnv* env, art::Thread* self, /*in-out*/std::vector<ArtClassDefinition>* definitions);

  static jvmtiError RetransformClasses(ArtJvmTiEnv* env,
                                       art::Runtime* runtime,
                                       art::Thread* self,
                                       jint class_count,
                                       const jclass* classes,
                                       /*out*/std::string* error_msg);

  // Gets the data surrounding the given class.
  static jvmtiError FillInTransformationData(ArtJvmTiEnv* env,
                                             jclass klass,
                                             ArtClassDefinition* def);

 private:
  static jvmtiError GetDexDataForRetransformation(ArtJvmTiEnv* env,
                                                  art::Handle<art::mirror::Class> klass,
                                                  /*out*/jint* dex_data_length,
                                                  /*out*/unsigned char** dex_data)
      REQUIRES_SHARED(art::Locks::mutator_lock_);
};

}  // namespace openjdkjvmti

#endif  // ART_RUNTIME_OPENJDKJVMTI_TRANSFORM_H_

