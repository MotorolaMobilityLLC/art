/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "indirect_reference_table-inl.h"

#include "android-base/stringprintf.h"

#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/object-inl.h"
#include "scoped_thread_state_change-inl.h"

namespace art {
namespace jni {

using android::base::StringPrintf;

class LocalReferenceTableTest : public CommonRuntimeTest {
 protected:
  LocalReferenceTableTest() {
    use_boot_image_ = true;  // Make the Runtime creation cheaper.
  }
};

static void CheckDump(LocalReferenceTable* lrt, size_t num_objects, size_t num_unique)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::ostringstream oss;
  lrt->Dump(oss);
  if (num_objects == 0) {
    EXPECT_EQ(oss.str().find("java.lang.Object"), std::string::npos) << oss.str();
  } else if (num_objects == 1) {
    EXPECT_NE(oss.str().find("1 of java.lang.Object"), std::string::npos) << oss.str();
  } else {
    EXPECT_NE(oss.str().find(StringPrintf("%zd of java.lang.Object (%zd unique instances)",
                                          num_objects, num_unique)),
              std::string::npos)
                  << "\n Expected number of objects: " << num_objects
                  << "\n Expected unique objects: " << num_unique << "\n"
                  << oss.str();
  }
}

TEST_F(LocalReferenceTableTest, BasicTest) {
  // This will lead to error messages in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);

  ScopedObjectAccess soa(Thread::Current());
  static const size_t kTableMax = 20;
  std::string error_msg;
  LocalReferenceTable lrt;
  bool success = lrt.Initialize(kTableMax, &error_msg);
  ASSERT_TRUE(success) << error_msg;

  StackHandleScope<5> hs(soa.Self());
  Handle<mirror::Class> c =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;"));
  ASSERT_TRUE(c != nullptr);
  Handle<mirror::Object> obj0 = hs.NewHandle(c->AllocObject(soa.Self()));
  ASSERT_TRUE(obj0 != nullptr);
  Handle<mirror::Object> obj1 = hs.NewHandle(c->AllocObject(soa.Self()));
  ASSERT_TRUE(obj1 != nullptr);
  Handle<mirror::Object> obj2 = hs.NewHandle(c->AllocObject(soa.Self()));
  ASSERT_TRUE(obj2 != nullptr);
  Handle<mirror::Object> obj3 = hs.NewHandle(c->AllocObject(soa.Self()));
  ASSERT_TRUE(obj3 != nullptr);

  const LRTSegmentState cookie = kLRTFirstSegment;

  CheckDump(&lrt, 0, 0);

  IndirectRef iref0 = (IndirectRef) 0x11110;
  EXPECT_FALSE(lrt.Remove(cookie, iref0)) << "unexpectedly successful removal";

  // Add three, check, remove in the order in which they were added.
  iref0 = lrt.Add(cookie, obj0.Get(), &error_msg);
  EXPECT_TRUE(iref0 != nullptr);
  CheckDump(&lrt, 1, 1);
  IndirectRef iref1 = lrt.Add(cookie, obj1.Get(), &error_msg);
  EXPECT_TRUE(iref1 != nullptr);
  CheckDump(&lrt, 2, 2);
  IndirectRef iref2 = lrt.Add(cookie, obj2.Get(), &error_msg);
  EXPECT_TRUE(iref2 != nullptr);
  CheckDump(&lrt, 3, 3);

  EXPECT_OBJ_PTR_EQ(obj0.Get(), lrt.Get(iref0));
  EXPECT_OBJ_PTR_EQ(obj1.Get(), lrt.Get(iref1));
  EXPECT_OBJ_PTR_EQ(obj2.Get(), lrt.Get(iref2));

  EXPECT_TRUE(lrt.Remove(cookie, iref0));
  CheckDump(&lrt, 2, 2);
  EXPECT_TRUE(lrt.Remove(cookie, iref1));
  CheckDump(&lrt, 1, 1);
  EXPECT_TRUE(lrt.Remove(cookie, iref2));
  CheckDump(&lrt, 0, 0);

  // Table should be empty now.
  EXPECT_EQ(0U, lrt.Capacity());

  // Check that the entry off the end of the list is not valid.
  // (CheckJNI shall abort for such entries.)
  EXPECT_FALSE(lrt.IsValidReference(iref0, &error_msg));

  // Add three, remove in the opposite order.
  iref0 = lrt.Add(cookie, obj0.Get(), &error_msg);
  EXPECT_TRUE(iref0 != nullptr);
  iref1 = lrt.Add(cookie, obj1.Get(), &error_msg);
  EXPECT_TRUE(iref1 != nullptr);
  iref2 = lrt.Add(cookie, obj2.Get(), &error_msg);
  EXPECT_TRUE(iref2 != nullptr);
  CheckDump(&lrt, 3, 3);

  ASSERT_TRUE(lrt.Remove(cookie, iref2));
  CheckDump(&lrt, 2, 2);
  ASSERT_TRUE(lrt.Remove(cookie, iref1));
  CheckDump(&lrt, 1, 1);
  ASSERT_TRUE(lrt.Remove(cookie, iref0));
  CheckDump(&lrt, 0, 0);

  // Table should be empty now.
  ASSERT_EQ(0U, lrt.Capacity());

  // Add three, remove middle / middle / bottom / top.  (Second attempt
  // to remove middle should fail.)
  iref0 = lrt.Add(cookie, obj0.Get(), &error_msg);
  EXPECT_TRUE(iref0 != nullptr);
  iref1 = lrt.Add(cookie, obj1.Get(), &error_msg);
  EXPECT_TRUE(iref1 != nullptr);
  iref2 = lrt.Add(cookie, obj2.Get(), &error_msg);
  EXPECT_TRUE(iref2 != nullptr);
  CheckDump(&lrt, 3, 3);

  ASSERT_EQ(3U, lrt.Capacity());

  ASSERT_TRUE(lrt.Remove(cookie, iref1));
  CheckDump(&lrt, 2, 2);
  ASSERT_FALSE(lrt.Remove(cookie, iref1));
  CheckDump(&lrt, 2, 2);

  // Check that the reference to the hole is not valid.
  EXPECT_FALSE(lrt.IsValidReference(iref1, &error_msg));

  ASSERT_TRUE(lrt.Remove(cookie, iref2));
  CheckDump(&lrt, 1, 1);
  ASSERT_TRUE(lrt.Remove(cookie, iref0));
  CheckDump(&lrt, 0, 0);

  // Table should be empty now.
  ASSERT_EQ(0U, lrt.Capacity());

  // Add four entries.  Remove #1, add new entry, verify that table size
  // is still 4 (i.e. holes are getting filled).  Remove #1 and #3, verify
  // that we delete one and don't hole-compact the other.
  iref0 = lrt.Add(cookie, obj0.Get(), &error_msg);
  EXPECT_TRUE(iref0 != nullptr);
  iref1 = lrt.Add(cookie, obj1.Get(), &error_msg);
  EXPECT_TRUE(iref1 != nullptr);
  iref2 = lrt.Add(cookie, obj2.Get(), &error_msg);
  EXPECT_TRUE(iref2 != nullptr);
  IndirectRef iref3 = lrt.Add(cookie, obj3.Get(), &error_msg);
  EXPECT_TRUE(iref3 != nullptr);
  CheckDump(&lrt, 4, 4);

  ASSERT_TRUE(lrt.Remove(cookie, iref1));
  CheckDump(&lrt, 3, 3);

  iref1 = lrt.Add(cookie, obj1.Get(), &error_msg);
  EXPECT_TRUE(iref1 != nullptr);

  ASSERT_EQ(4U, lrt.Capacity()) << "hole not filled";
  CheckDump(&lrt, 4, 4);

  ASSERT_TRUE(lrt.Remove(cookie, iref1));
  CheckDump(&lrt, 3, 3);
  ASSERT_TRUE(lrt.Remove(cookie, iref3));
  CheckDump(&lrt, 2, 2);

  ASSERT_EQ(3U, lrt.Capacity()) << "should be 3 after two deletions";

  ASSERT_TRUE(lrt.Remove(cookie, iref2));
  CheckDump(&lrt, 1, 1);
  ASSERT_TRUE(lrt.Remove(cookie, iref0));
  CheckDump(&lrt, 0, 0);

  ASSERT_EQ(0U, lrt.Capacity()) << "not empty after split remove";

  // Add an entry, remove it, add a new entry, and try to use the original
  // iref.  They have the same slot number but are for different objects.
  // With the extended checks in place, this should fail.
  iref0 = lrt.Add(cookie, obj0.Get(), &error_msg);
  EXPECT_TRUE(iref0 != nullptr);
  CheckDump(&lrt, 1, 1);
  ASSERT_TRUE(lrt.Remove(cookie, iref0));
  CheckDump(&lrt, 0, 0);
  iref1 = lrt.Add(cookie, obj1.Get(), &error_msg);
  EXPECT_TRUE(iref1 != nullptr);
  CheckDump(&lrt, 1, 1);
  ASSERT_FALSE(lrt.Remove(cookie, iref0)) << "mismatched del succeeded";
  CheckDump(&lrt, 1, 1);
  ASSERT_TRUE(lrt.Remove(cookie, iref1)) << "switched del failed";
  ASSERT_EQ(0U, lrt.Capacity()) << "switching del not empty";
  CheckDump(&lrt, 0, 0);

  // Same as above, but with the same object.  A more rigorous checker
  // (e.g. with slot serialization) will catch this.
  iref0 = lrt.Add(cookie, obj0.Get(), &error_msg);
  EXPECT_TRUE(iref0 != nullptr);
  CheckDump(&lrt, 1, 1);
  ASSERT_TRUE(lrt.Remove(cookie, iref0));
  CheckDump(&lrt, 0, 0);
  iref1 = lrt.Add(cookie, obj0.Get(), &error_msg);
  EXPECT_TRUE(iref1 != nullptr);
  CheckDump(&lrt, 1, 1);
  if (iref0 != iref1) {
    // Try 0, should not work.
    ASSERT_FALSE(lrt.Remove(cookie, iref0)) << "temporal del succeeded";
  }
  ASSERT_TRUE(lrt.Remove(cookie, iref1)) << "temporal cleanup failed";
  ASSERT_EQ(0U, lrt.Capacity()) << "temporal del not empty";
  CheckDump(&lrt, 0, 0);

  // Stale reference is not valid.
  iref0 = lrt.Add(cookie, obj0.Get(), &error_msg);
  EXPECT_TRUE(iref0 != nullptr);
  CheckDump(&lrt, 1, 1);
  ASSERT_TRUE(lrt.Remove(cookie, iref0));
  EXPECT_FALSE(lrt.IsValidReference(iref0, &error_msg)) << "stale lookup succeeded";
  CheckDump(&lrt, 0, 0);

  // Test table resizing.
  // These ones fit...
  static const size_t kTableInitial = kTableMax / 2;
  IndirectRef manyRefs[kTableInitial];
  for (size_t i = 0; i < kTableInitial; i++) {
    manyRefs[i] = lrt.Add(cookie, obj0.Get(), &error_msg);
    ASSERT_TRUE(manyRefs[i] != nullptr) << "Failed adding " << i;
    CheckDump(&lrt, i + 1, 1);
  }
  // ...this one causes overflow.
  iref0 = lrt.Add(cookie, obj0.Get(), &error_msg);
  ASSERT_TRUE(iref0 != nullptr);
  ASSERT_EQ(kTableInitial + 1, lrt.Capacity());
  CheckDump(&lrt, kTableInitial + 1, 1);

  for (size_t i = 0; i < kTableInitial; i++) {
    ASSERT_TRUE(lrt.Remove(cookie, manyRefs[i])) << "failed removing " << i;
    CheckDump(&lrt, kTableInitial - i, 1);
  }
  // Because of removal order, should have 11 entries, 10 of them holes.
  ASSERT_EQ(kTableInitial + 1, lrt.Capacity());

  ASSERT_TRUE(lrt.Remove(cookie, iref0)) << "multi-remove final failed";

  ASSERT_EQ(0U, lrt.Capacity()) << "multi-del not empty";
  CheckDump(&lrt, 0, 0);
}

TEST_F(LocalReferenceTableTest, Holes) {
  // Test the explicitly named cases from the LRT implementation:
  //
  // 1) Segment with holes (current_num_holes_ > 0), push new segment, add/remove reference
  // 2) Segment with holes (current_num_holes_ > 0), pop segment, add/remove reference
  // 3) Segment with holes (current_num_holes_ > 0), push new segment, pop segment, add/remove
  //    reference
  // 4) Empty segment, push new segment, create a hole, pop a segment, add/remove a reference
  // 5) Base segment, push new segment, create a hole, pop a segment, push new segment, add/remove
  //    reference

  ScopedObjectAccess soa(Thread::Current());
  static const size_t kTableMax = 10;

  StackHandleScope<6> hs(soa.Self());
  Handle<mirror::Class> c = hs.NewHandle(
      class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;"));
  ASSERT_TRUE(c != nullptr);
  Handle<mirror::Object> obj0 = hs.NewHandle(c->AllocObject(soa.Self()));
  ASSERT_TRUE(obj0 != nullptr);
  Handle<mirror::Object> obj1 = hs.NewHandle(c->AllocObject(soa.Self()));
  ASSERT_TRUE(obj1 != nullptr);
  Handle<mirror::Object> obj2 = hs.NewHandle(c->AllocObject(soa.Self()));
  ASSERT_TRUE(obj2 != nullptr);
  Handle<mirror::Object> obj3 = hs.NewHandle(c->AllocObject(soa.Self()));
  ASSERT_TRUE(obj3 != nullptr);
  Handle<mirror::Object> obj4 = hs.NewHandle(c->AllocObject(soa.Self()));
  ASSERT_TRUE(obj4 != nullptr);

  std::string error_msg;

  // 1) Segment with holes (current_num_holes_ > 0), push new segment, add/remove reference.
  {
    LocalReferenceTable lrt;
    bool success = lrt.Initialize(kTableMax, &error_msg);
    ASSERT_TRUE(success) << error_msg;

    const LRTSegmentState cookie0 = kLRTFirstSegment;

    CheckDump(&lrt, 0, 0);

    IndirectRef iref0 = lrt.Add(cookie0, obj0.Get(), &error_msg);
    IndirectRef iref1 = lrt.Add(cookie0, obj1.Get(), &error_msg);
    IndirectRef iref2 = lrt.Add(cookie0, obj2.Get(), &error_msg);

    EXPECT_TRUE(lrt.Remove(cookie0, iref1));

    // New segment.
    const LRTSegmentState cookie1 = lrt.GetSegmentState();

    IndirectRef iref3 = lrt.Add(cookie1, obj3.Get(), &error_msg);

    // Must not have filled the previous hole.
    EXPECT_EQ(lrt.Capacity(), 4u);
    EXPECT_FALSE(lrt.IsValidReference(iref1, &error_msg));
    CheckDump(&lrt, 3, 3);

    UNUSED(iref0, iref1, iref2, iref3);
  }

  // 2) Segment with holes (current_num_holes_ > 0), pop segment, add/remove reference
  {
    LocalReferenceTable lrt;
    bool success = lrt.Initialize(kTableMax, &error_msg);
    ASSERT_TRUE(success) << error_msg;

    const LRTSegmentState cookie0 = kLRTFirstSegment;

    CheckDump(&lrt, 0, 0);

    IndirectRef iref0 = lrt.Add(cookie0, obj0.Get(), &error_msg);

    // New segment.
    const LRTSegmentState cookie1 = lrt.GetSegmentState();

    IndirectRef iref1 = lrt.Add(cookie1, obj1.Get(), &error_msg);
    IndirectRef iref2 = lrt.Add(cookie1, obj2.Get(), &error_msg);
    IndirectRef iref3 = lrt.Add(cookie1, obj3.Get(), &error_msg);

    EXPECT_TRUE(lrt.Remove(cookie1, iref2));

    // Pop segment.
    lrt.SetSegmentState(cookie1);

    IndirectRef iref4 = lrt.Add(cookie1, obj4.Get(), &error_msg);

    EXPECT_EQ(lrt.Capacity(), 2u);
    EXPECT_FALSE(lrt.IsValidReference(iref2, &error_msg));
    CheckDump(&lrt, 2, 2);

    UNUSED(iref0, iref1, iref2, iref3, iref4);
  }

  // 3) Segment with holes (current_num_holes_ > 0), push new segment, pop segment, add/remove
  //    reference.
  {
    LocalReferenceTable lrt;
    bool success = lrt.Initialize(kTableMax, &error_msg);
    ASSERT_TRUE(success) << error_msg;

    const LRTSegmentState cookie0 = kLRTFirstSegment;

    CheckDump(&lrt, 0, 0);

    IndirectRef iref0 = lrt.Add(cookie0, obj0.Get(), &error_msg);

    // New segment.
    const LRTSegmentState cookie1 = lrt.GetSegmentState();

    IndirectRef iref1 = lrt.Add(cookie1, obj1.Get(), &error_msg);
    IndirectRef iref2 = lrt.Add(cookie1, obj2.Get(), &error_msg);

    EXPECT_TRUE(lrt.Remove(cookie1, iref1));

    // New segment.
    const LRTSegmentState cookie2 = lrt.GetSegmentState();

    IndirectRef iref3 = lrt.Add(cookie2, obj3.Get(), &error_msg);

    // Pop segment.
    lrt.SetSegmentState(cookie2);

    IndirectRef iref4 = lrt.Add(cookie1, obj4.Get(), &error_msg);

    EXPECT_EQ(lrt.Capacity(), 3u);
    EXPECT_FALSE(lrt.IsValidReference(iref1, &error_msg));
    CheckDump(&lrt, 3, 3);

    UNUSED(iref0, iref1, iref2, iref3, iref4);
  }

  // 4) Empty segment, push new segment, create a hole, pop a segment, add/remove a reference.
  {
    LocalReferenceTable lrt;
    bool success = lrt.Initialize(kTableMax, &error_msg);
    ASSERT_TRUE(success) << error_msg;

    const LRTSegmentState cookie0 = kLRTFirstSegment;

    CheckDump(&lrt, 0, 0);

    IndirectRef iref0 = lrt.Add(cookie0, obj0.Get(), &error_msg);

    // New segment.
    const LRTSegmentState cookie1 = lrt.GetSegmentState();

    IndirectRef iref1 = lrt.Add(cookie1, obj1.Get(), &error_msg);
    EXPECT_TRUE(lrt.Remove(cookie1, iref1));

    // Emptied segment, push new one.
    const LRTSegmentState cookie2 = lrt.GetSegmentState();

    IndirectRef iref2 = lrt.Add(cookie1, obj1.Get(), &error_msg);
    IndirectRef iref3 = lrt.Add(cookie1, obj2.Get(), &error_msg);
    IndirectRef iref4 = lrt.Add(cookie1, obj3.Get(), &error_msg);

    EXPECT_TRUE(lrt.Remove(cookie1, iref3));

    // Pop segment.
    UNUSED(cookie2);
    lrt.SetSegmentState(cookie1);

    IndirectRef iref5 = lrt.Add(cookie1, obj4.Get(), &error_msg);

    EXPECT_EQ(lrt.Capacity(), 2u);
    EXPECT_FALSE(lrt.IsValidReference(iref3, &error_msg));
    CheckDump(&lrt, 2, 2);

    UNUSED(iref0, iref1, iref2, iref3, iref4, iref5);
  }

  // 5) Base segment, push new segment, create a hole, pop a segment, push new segment, add/remove
  //    reference
  {
    LocalReferenceTable lrt;
    bool success = lrt.Initialize(kTableMax, &error_msg);
    ASSERT_TRUE(success) << error_msg;

    const LRTSegmentState cookie0 = kLRTFirstSegment;

    CheckDump(&lrt, 0, 0);

    IndirectRef iref0 = lrt.Add(cookie0, obj0.Get(), &error_msg);

    // New segment.
    const LRTSegmentState cookie1 = lrt.GetSegmentState();

    IndirectRef iref1 = lrt.Add(cookie1, obj1.Get(), &error_msg);
    IndirectRef iref2 = lrt.Add(cookie1, obj1.Get(), &error_msg);
    IndirectRef iref3 = lrt.Add(cookie1, obj2.Get(), &error_msg);

    EXPECT_TRUE(lrt.Remove(cookie1, iref2));

    // Pop segment.
    lrt.SetSegmentState(cookie1);

    // Push segment.
    const LRTSegmentState cookie1_second = lrt.GetSegmentState();
    UNUSED(cookie1_second);

    IndirectRef iref4 = lrt.Add(cookie1, obj3.Get(), &error_msg);

    EXPECT_EQ(lrt.Capacity(), 2u);
    EXPECT_FALSE(lrt.IsValidReference(iref3, &error_msg));
    CheckDump(&lrt, 2, 2);

    UNUSED(iref0, iref1, iref2, iref3, iref4);
  }
}

TEST_F(LocalReferenceTableTest, Resize) {
  ScopedObjectAccess soa(Thread::Current());
  static const size_t kTableMax = 512;

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> c = hs.NewHandle(
      class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;"));
  ASSERT_TRUE(c != nullptr);
  Handle<mirror::Object> obj0 = hs.NewHandle(c->AllocObject(soa.Self()));
  ASSERT_TRUE(obj0 != nullptr);

  std::string error_msg;
  LocalReferenceTable lrt;
  bool success = lrt.Initialize(kTableMax, &error_msg);
  ASSERT_TRUE(success) << error_msg;

  CheckDump(&lrt, 0, 0);
  const LRTSegmentState cookie = kLRTFirstSegment;

  for (size_t i = 0; i != kTableMax + 1; ++i) {
    lrt.Add(cookie, obj0.Get(), &error_msg);
  }

  EXPECT_EQ(lrt.Capacity(), kTableMax + 1);
}

}  // namespace jni
}  // namespace art
