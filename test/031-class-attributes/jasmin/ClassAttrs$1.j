; Copyright (C) 2017 The Android Open Source Project
;
; Licensed under the Apache License, Version 2.0 (the "License");
; you may not use this file except in compliance with the License.
; You may obtain a copy of the License at
;
;      http://www.apache.org/licenses/LICENSE-2.0
;
; Unless required by applicable law or agreed to in writing, software
; distributed under the License is distributed on an "AS IS" BASIS,
; WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
; See the License for the specific language governing permissions and
; limitations under the License.

; (new OtherClass() { int i = 5; }).getClass()

; ClassAttrs$1.j

; Generated by ClassFileAnalyzer (Can)
; Analyzer and Disassembler for Java class files
; (Jasmin syntax 2, http://jasmin.sourceforge.net)
;
; ClassFileAnalyzer, version 0.7.0

.bytecode 52.0
.source ClassAttrs.java
.class final ClassAttrs$1
.super OtherClass
.enclosing method ClassAttrs/main()V
; OpenJDK javac versions <= 8 consider anonymous classes declared side
; static methods to be static (as is this one), whereas OpenJDK 9 javac
; does not. See http://b/62290080
.inner class static inner ClassAttrs$1 ; <anonymous> <not a member>

.field i I

.method <init>()V
  .limit stack 2
  .limit locals 1
  .line 112
  0: aload_0
  1: invokespecial OtherClass/<init>()V
  4: aload_0
  5: iconst_5
  6: putfield ClassAttrs$1/i I
  9: return
.end method


