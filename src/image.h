/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_SRC_IMAGE_H_
#define ART_SRC_IMAGE_H_

#include <string.h>

#include "globals.h"
#include "mirror/object.h"

namespace art {

// header of image files written by ImageWriter, read and validated by Space.
class PACKED(4) ImageHeader {
 public:
  ImageHeader() {}

  ImageHeader(uint32_t image_begin,
              uint32_t image_roots,
              uint32_t oat_checksum,
              uint32_t oat_file_begin,
              uint32_t oat_data_begin,
              uint32_t oat_data_end,
              uint32_t oat_file_end);

  bool IsValid() const {
    if (memcmp(magic_, kImageMagic, sizeof(kImageMagic) != 0)) {
      return false;
    }
    if (memcmp(version_, kImageVersion, sizeof(kImageVersion) != 0)) {
      return false;
    }
    return true;
  }

  const char* GetMagic() const {
    CHECK(IsValid());
    return reinterpret_cast<const char*>(magic_);
  }

  byte* GetImageBegin() const {
    return reinterpret_cast<byte*>(image_begin_);
  }

  uint32_t GetOatChecksum() const {
    return oat_checksum_;
  }

  void SetOatChecksum(uint32_t oat_checksum) {
    oat_checksum_ = oat_checksum;
  }

  byte* GetOatFileBegin() const {
    return reinterpret_cast<byte*>(oat_file_begin_);
  }

  byte* GetOatDataBegin() const {
    return reinterpret_cast<byte*>(oat_data_begin_);
  }

  byte* GetOatDataEnd() const {
    return reinterpret_cast<byte*>(oat_data_end_);
  }

  byte* GetOatFileEnd() const {
    return reinterpret_cast<byte*>(oat_file_end_);
  }

  enum ImageRoot {
    kResolutionMethod,
    kCalleeSaveMethod,
    kRefsOnlySaveMethod,
    kRefsAndArgsSaveMethod,
    kOatLocation,
    kDexCaches,
    kClassRoots,
    kImageRootsMax,
  };

  mirror::Object* GetImageRoot(ImageRoot image_root) const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  mirror::ObjectArray<mirror::Object>* GetImageRoots() const;

  static const byte kImageMagic[4];
  static const byte kImageVersion[4];

  byte magic_[4];
  byte version_[4];

  // Required base address for mapping the image.
  uint32_t image_begin_;

  // Checksum of the oat file we link to for load time sanity check.
  uint32_t oat_checksum_;

  // Start address for oat file. Will be before oat_data_begin_ for .so files.
  uint32_t oat_file_begin_;

  // Required oat address expected by image Method::GetCode() pointers.
  uint32_t oat_data_begin_;

  // End of oat data address range for this image file.
  uint32_t oat_data_end_;

  // End of oat file address range. will be after oat_data_end_ for
  // .so files. Used for positioning a following alloc spaces.
  uint32_t oat_file_end_;

  // Absolute address of an Object[] of objects needed to reinitialize from an image.
  uint32_t image_roots_;

  friend class ImageWriter;
  friend class ImageDumper;  // For GetImageRoots()
};

}  // namespace art

#endif  // ART_SRC_IMAGE_H_
