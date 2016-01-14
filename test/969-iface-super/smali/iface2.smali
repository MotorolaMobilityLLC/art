# /*
#  * Copyright (C) 2015 The Android Open Source Project
#  *
#  * Licensed under the Apache License, Version 2.0 (the "License");
#  * you may not use this file except in compliance with the License.
#  * You may obtain a copy of the License at
#  *
#  *      http://www.apache.org/licenses/LICENSE-2.0
#  *
#  * Unless required by applicable law or agreed to in writing, software
#  * distributed under the License is distributed on an "AS IS" BASIS,
#  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  * See the License for the specific language governing permissions and
#  * limitations under the License.
#  */
#
# public interface iface2 extends iface {
#   public default String SayHi() {
#       return iface.super.SayHi() + iface.super.SayHi();
#   }
# }

.class public abstract interface Liface2;
.super Ljava/lang/Object;
.implements Liface;

.method public SayHi()Ljava/lang/String;
    .locals 2
    invoke-super {p0}, Liface;->SayHi()Ljava/lang/String;
    move-result-object v0
    invoke-super {p0}, Liface;->SayHi()Ljava/lang/String;
    move-result-object v1
    invoke-virtual {v0, v1}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
