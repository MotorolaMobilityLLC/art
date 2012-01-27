// Copyright 2011 Google Inc. All Rights Reserved.

/*
 * Maintain a card table from the the write barrier. All writes of
 * non-NULL values to heap addresses should go through an entry in
 * WriteBarrier, and from there to here.
 */

#ifndef DALVIK_ALLOC_CARDTABLE_H_
#define DALVIK_ALLOC_CARDTABLE_H_

#include "globals.h"
#include "logging.h"
#include "mem_map.h"
#include "UniquePtr.h"

namespace art {

class Object;

#define GC_CARD_SHIFT 7
#define GC_CARD_SIZE (1 << GC_CARD_SHIFT)
#define GC_CARD_CLEAN 0
#define GC_CARD_DIRTY 0x70

class CardTable {
 public:

  static CardTable* Create(const byte* heap_begin, size_t heap_capacity);

  // Set the card associated with the given address to GC_CARD_DIRTY.
  void MarkCard(const void *addr) {
    byte* cardAddr = CardFromAddr(addr);
    *cardAddr = GC_CARD_DIRTY;
  }

  // Is the object on a dirty card?
  bool IsDirty(const Object* obj) const {
    return *CardFromAddr(obj) == GC_CARD_DIRTY;
  }

  // Returns a value that when added to a heap address >> GC_CARD_SHIFT will address the appropriate
  // card table byte. For convenience this value is cached in every Thread
  byte* GetBiasedBegin() const {
    return biased_begin_;
  }

  // For every dirty card between begin and end invoke the visitor with the specified argument
  typedef void Callback(Object* obj, void* arg);
  void Scan(byte* begin, byte* end, Callback* visitor, void* arg) const;


  // Assertion used to check the given address is covered by the card table
  void CheckAddrIsInCardTable(const byte* addr) const;

 private:

  CardTable(MemMap* begin, byte* biased_begin, size_t offset) :
    mem_map_(begin), biased_begin_(biased_begin), offset_(offset) {}

  // Resets all of the bytes in the card table to clean.
  void ClearCardTable();

  // Returns the address of the relevant byte in the card table, given an address on the heap.
  byte* CardFromAddr(const void *addr) const {
    byte *cardAddr = biased_begin_ + ((uintptr_t)addr >> GC_CARD_SHIFT);
    // Sanity check the caller was asking for address covered by the card table
    DCHECK(IsValidCard(cardAddr)) << "addr: " << addr
        << " cardAddr: " << reinterpret_cast<void*>(cardAddr);
    return cardAddr;
  }

  // Returns the first address in the heap which maps to this card.
  void* AddrFromCard(const byte *cardAddr) const {
    DCHECK(IsValidCard(cardAddr));
    uintptr_t offset = cardAddr - biased_begin_;
    return (void *)(offset << GC_CARD_SHIFT);
  }

  // Returns true iff the card table address is within the bounds of the card table.
  bool IsValidCard(const byte* cardAddr) const {
    byte* begin = mem_map_->Begin() + offset_;
    byte* end = mem_map_->End();
    return cardAddr >= begin && cardAddr < end;
  }

  // Verifies that all gray objects are on a dirty card.
  void VerifyCardTable();

  // Mmapped pages for the card table
  UniquePtr<MemMap> mem_map_;
  // Value used to compute card table addresses from object addresses, see GetBiasedBegin
  byte* const biased_begin_;
  // Card table doesn't begin at the beginning of the mem_map_, instead it is displaced by offset
  // to allow the byte value of biased_begin_ to equal GC_CARD_DIRTY
  const size_t offset_;
};

}  // namespace art
#endif  // DALVIK_ALLOC_CARDTABLE_H_
