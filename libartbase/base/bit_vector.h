/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_LIBARTBASE_BASE_BIT_VECTOR_H_
#define ART_LIBARTBASE_BASE_BIT_VECTOR_H_

#include <stdint.h>

#include <iterator>

#include "bit_utils.h"
#include "globals.h"

namespace art {

class Allocator;
class ArenaBitVector;

/*
 * Expanding bitmap, used for tracking resources.  Bits are numbered starting
 * from zero.  All operations on a BitVector are unsynchronized.
 */
class BitVector {
 public:
  static constexpr uint32_t kWordBytes = sizeof(uint32_t);
  static constexpr uint32_t kWordBits = kWordBytes * 8;

  class IndexContainer;

  /**
   * @brief Convenient iterator across the indexes of the BitVector's set bits.
   *
   * @details IndexIterator is a Forward iterator (C++11: 24.2.5) from the lowest
   * to the highest index of the BitVector's set bits. Instances can be retrieved
   * only through BitVector::Indexes() which returns an IndexContainer wrapper
   * object with begin() and end() suitable for range-based loops:
   *   for (uint32_t idx : bit_vector.Indexes()) {
   *     // Use idx.
   *   }
   */
  class IndexIterator :
      public std::iterator<std::forward_iterator_tag, uint32_t, ptrdiff_t, void, uint32_t> {
   public:
    bool operator==(const IndexIterator& other) const;

    bool operator!=(const IndexIterator& other) const {
      return !(*this == other);
    }

    uint32_t operator*() const;

    IndexIterator& operator++();

    IndexIterator operator++(int);

    // Helper function to check for end without comparing with bit_vector.Indexes().end().
    bool Done() const {
      return bit_index_ == BitSize();
    }

   private:
    struct begin_tag { };
    struct end_tag { };

    IndexIterator(const BitVector* bit_vector, begin_tag);
    IndexIterator(const BitVector* bit_vector, end_tag);

    uint32_t BitSize() const {
      return storage_size_ * kWordBits;
    }

    uint32_t FindIndex(uint32_t start_index) const;
    const uint32_t* const bit_storage_;
    const uint32_t storage_size_;  // Size of vector in words.
    uint32_t bit_index_;           // Current index (size in bits).

    friend class BitVector::IndexContainer;
  };

  /**
   * @brief BitVector wrapper class for iteration across indexes of set bits.
   */
  class IndexContainer {
   public:
    explicit IndexContainer(const BitVector* bit_vector) : bit_vector_(bit_vector) { }

    IndexIterator begin() const;
    IndexIterator end() const;

   private:
    const BitVector* const bit_vector_;
  };

  // MoveConstructible but not MoveAssignable, CopyConstructible or CopyAssignable.

  BitVector(const BitVector& other) = delete;
  BitVector& operator=(const BitVector& other) = delete;

  BitVector(BitVector&& other) noexcept
      : storage_(other.storage_),
        storage_size_(other.storage_size_),
        allocator_(other.allocator_),
        expandable_(other.expandable_) {
    other.storage_ = nullptr;
    other.storage_size_ = 0u;
  }

  BitVector(uint32_t start_bits,
            bool expandable,
            Allocator* allocator);

  BitVector(bool expandable,
            Allocator* allocator,
            uint32_t storage_size,
            uint32_t* storage);

  BitVector(const BitVector& src,
            bool expandable,
            Allocator* allocator);

  virtual ~BitVector();

  // The number of words necessary to encode bits.
  static constexpr uint32_t BitsToWords(uint32_t bits) {
    return RoundUp(bits, kWordBits) / kWordBits;
  }

  // Mark the specified bit as "set".
  void SetBit(uint32_t idx) {
    /*
     * TUNING: this could have pathologically bad growth/expand behavior.  Make sure we're
     * not using it badly or change resize mechanism.
     */
    if (idx >= storage_size_ * kWordBits) {
      EnsureSize(idx);
    }
    storage_[WordIndex(idx)] |= BitMask(idx);
  }

  // Mark the specified bit as "unset".
  void ClearBit(uint32_t idx) {
    // If the index is over the size, we don't have to do anything, it is cleared.
    if (idx < storage_size_ * kWordBits) {
      // Otherwise, go ahead and clear it.
      storage_[WordIndex(idx)] &= ~BitMask(idx);
    }
  }

  // Determine whether or not the specified bit is set.
  bool IsBitSet(uint32_t idx) const {
    // If the index is over the size, whether it is expandable or not, this bit does not exist:
    // thus it is not set.
    return (idx < (storage_size_ * kWordBits)) && IsBitSet(storage_, idx);
  }

  // Mark all bits bit as "clear".
  void ClearAllBits();

  // Mark specified number of bits as "set". Cannot set all bits like ClearAll since there might
  // be unused bits - setting those to one will confuse the iterator.
  void SetInitialBits(uint32_t num_bits);

  void Copy(const BitVector* src);

  // Intersect with another bit vector.
  void Intersect(const BitVector* src2);

  // Union with another bit vector.
  bool Union(const BitVector* src);

  // Set bits of union_with that are not in not_in.
  bool UnionIfNotIn(const BitVector* union_with, const BitVector* not_in);

  void Subtract(const BitVector* src);

  // Are we equal to another bit vector?  Note: expandability attributes must also match.
  bool Equal(const BitVector* src) const;

  /**
   * @brief Are all the bits set the same?
   * @details expandability and size can differ as long as the same bits are set.
   */
  bool SameBitsSet(const BitVector *src) const;

  bool IsSubsetOf(const BitVector *other) const;

  // Count the number of bits that are set.
  uint32_t NumSetBits() const;

  // Count the number of bits that are set in range [0, end).
  uint32_t NumSetBits(uint32_t end) const;

  IndexContainer Indexes() const {
    return IndexContainer(this);
  }

  uint32_t GetStorageSize() const {
    return storage_size_;
  }

  bool IsExpandable() const {
    return expandable_;
  }

  uint32_t GetRawStorageWord(size_t idx) const {
    return storage_[idx];
  }

  uint32_t* GetRawStorage() {
    return storage_;
  }

  const uint32_t* GetRawStorage() const {
    return storage_;
  }

  size_t GetSizeOf() const {
    return storage_size_ * kWordBytes;
  }

  size_t GetBitSizeOf() const {
    return storage_size_ * kWordBits;
  }

  /**
   * @return the highest bit set, -1 if none are set
   */
  int GetHighestBitSet() const;

  /**
   * @return true if there are any bits set, false otherwise.
   */
  bool IsAnyBitSet() const {
    return GetHighestBitSet() != -1;
  }

  // Minimum number of bits required to store this vector, 0 if none are set.
  size_t GetNumberOfBits() const {
    return GetHighestBitSet() + 1;
  }

  // Is bit set in storage. (No range check.)
  static bool IsBitSet(const uint32_t* storage, uint32_t idx) {
    return (storage[WordIndex(idx)] & BitMask(idx)) != 0;
  }

  // Number of bits set in range [0, end) in storage. (No range check.)
  static uint32_t NumSetBits(const uint32_t* storage, uint32_t end);

  // Fill given memory region with the contents of the vector and zero padding.
  void CopyTo(void* dst, size_t len) const {
    DCHECK_LE(static_cast<size_t>(GetHighestBitSet() + 1), len * kBitsPerByte);
    size_t vec_len = GetSizeOf();
    if (vec_len < len) {
      void* dst_padding = reinterpret_cast<uint8_t*>(dst) + vec_len;
      memcpy(dst, storage_, vec_len);
      memset(dst_padding, 0, len - vec_len);
    } else {
      memcpy(dst, storage_, len);
    }
  }

  void Dump(std::ostream& os, const char* prefix) const;

  Allocator* GetAllocator() const;

 private:
  /**
   * @brief Dump the bitvector into buffer in a 00101..01 format.
   * @param buffer the ostringstream used to dump the bitvector into.
   */
  void DumpHelper(const char* prefix, std::ostringstream& buffer) const;

  // Ensure there is space for a bit at idx.
  void EnsureSize(uint32_t idx);

  // The index of the word within storage.
  static constexpr uint32_t WordIndex(uint32_t idx) {
    return idx >> 5;
  }

  // A bit mask to extract the bit for the given index.
  static constexpr uint32_t BitMask(uint32_t idx) {
    return 1 << (idx & 0x1f);
  }

  uint32_t*  storage_;            // The storage for the bit vector.
  uint32_t   storage_size_;       // Current size, in 32-bit words.
  Allocator* const allocator_;    // Allocator if expandable.
  const bool expandable_;         // Should the bitmap expand if too small?
};

// Helper for dealing with 2d bit-vector arrays packed into a single bit-vec
class BaseBitVectorArray {
 public:
  BaseBitVectorArray(const BaseBitVectorArray& bv) = default;
  BaseBitVectorArray& operator=(const BaseBitVectorArray& other) = default;

  BaseBitVectorArray() : num_columns_(0), num_rows_(0) {}

  BaseBitVectorArray(size_t num_rows, size_t num_columns)
      : num_columns_(RoundUp(num_columns, BitVector::kWordBits)), num_rows_(num_rows) {}

  virtual ~BaseBitVectorArray() {}

  bool IsExpandable() const {
    return GetRawData().IsExpandable();
  }

  // Let subclasses provide storage for various types.
  virtual const BitVector& GetRawData() const = 0;
  virtual BitVector& GetRawData() = 0;

  size_t NumRows() const {
    return num_rows_;
  }

  // NB This might be more than the requested size for alignment purposes.
  size_t NumColumns() const {
    return num_columns_;
  }

  void Clear() {
    GetRawData().ClearAllBits();
  }

  // Ensure that we can set all bits in the given range. The actual number of
  // columns might be larger than requested for alignment purposes.
  void Resize(size_t rows, size_t cols, bool clear = true);

  void SetBit(size_t row, size_t col) {
    DCHECK_LT(col, num_columns_);
    DCHECK_LT(row, num_rows_);
    GetRawData().SetBit(row * num_columns_ + col);
  }

  void ClearBit(size_t row, size_t col) {
    DCHECK_LT(col, num_columns_);
    DCHECK_LT(row, num_rows_);
    GetRawData().ClearBit(row * num_columns_ + col);
  }

  bool IsBitSet(size_t row, size_t col) const {
    DCHECK_LT(col, num_columns_);
    DCHECK_LT(row, num_rows_);
    return GetRawData().IsBitSet(row * num_columns_ + col);
  }

  // Union the vector of 'other' into 'dest_row'.
  void UnionRows(size_t dest_row, size_t other);

  static size_t RequiredBitVectorSize(size_t rows, size_t cols) {
    return rows * RoundUp(cols, BitVector::kWordBits);
  }

  static size_t MaxRowsFor(const BitVector& bv, size_t cols) {
    return cols != 0 ? bv.GetBitSizeOf() / RoundUp(cols, BitVector::kWordBits) : 0;
  }

 private:
  size_t num_columns_;
  size_t num_rows_;
};

// A BitVectorArray with a standard owned BitVector providing the backing
// storage. This should be used when the BitVectorArray is the owner of the
// whole BitVector and should use standard allocators for cleanup/allocation.
// Contrast this with ArenaBitVectorArray which uses arena allocators.
class BitVectorArray final : public BaseBitVectorArray {
 public:
  BitVectorArray(const BitVectorArray& bv) = delete;
  BitVectorArray& operator=(const BitVectorArray& other) = delete;

  explicit BitVectorArray(BitVector&& bv) : BaseBitVectorArray(), data_(std::move(bv)) {}
  explicit BitVectorArray(BitVector&& bv, size_t cols)
      : BaseBitVectorArray(BaseBitVectorArray::MaxRowsFor(bv, cols), cols), data_(std::move(bv)) {}
  explicit BitVectorArray(BitVector&& bv, size_t rows, size_t cols)
      : BaseBitVectorArray(rows, cols), data_(std::move(bv)) {}

  BitVectorArray(uint32_t start_rows, uint32_t start_cols, bool expandable, Allocator* allocator)
      : BaseBitVectorArray(start_rows, start_cols),
        data_(BaseBitVectorArray::RequiredBitVectorSize(start_rows, start_cols),
              expandable,
              allocator) {}

  BitVectorArray(const BaseBitVectorArray& src, bool expandable, Allocator* allocator)
      : BaseBitVectorArray(src.NumRows(), src.NumColumns()),
        data_(src.GetRawData(), expandable, allocator) {}

  ~BitVectorArray() override {}

  const BitVector& GetRawData() const override {
    return data_;
  }

  BitVector& GetRawData() override {
    return data_;
  }

 private:
  BitVector data_;
};

// A bit vector array that uses an unowned BitVector reference as it's backing
// data.
class BitVectorArrayWrapper final : public BaseBitVectorArray {
 public:
  BitVectorArrayWrapper& operator=(BitVectorArrayWrapper& other) = default;
  BitVectorArrayWrapper(BitVectorArrayWrapper&) = default;
  explicit BitVectorArrayWrapper(BitVector* bv) : BaseBitVectorArray(), data_(bv) {}
  explicit BitVectorArrayWrapper(BitVector* bv, size_t cols)
      : BaseBitVectorArray(BaseBitVectorArray::MaxRowsFor(*bv, cols), cols), data_(bv) {}
  explicit BitVectorArrayWrapper(BitVector* bv, size_t rows, size_t cols)
      : BaseBitVectorArray(rows, cols), data_(bv) {}

  ~BitVectorArrayWrapper() override {}

  const BitVector& GetRawData() const override {
    return *data_;
  }

  BitVector& GetRawData() override {
    return *data_;
  }

 private:
  BitVector* data_;
};

}  // namespace art

#endif  // ART_LIBARTBASE_BASE_BIT_VECTOR_H_
