// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_SLAB_BUFFER_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_SLAB_BUFFER_H_

#include <zircon/assert.h>

#include <fbl/slab_allocator.h>

#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/slab_allocator_traits.h"

namespace bt {

template <size_t BackingBufferSize>
class SlabBuffer : public MutableByteBuffer {
 public:
  explicit SlabBuffer(size_t size) : size_(size) {
    ZX_ASSERT(size);
    ZX_ASSERT(size_ <= buffer_.size());
  }

  // ByteBuffer overrides:
  const uint8_t* data() const override { return buffer_.data(); }
  size_t size() const override { return size_; }
  const_iterator cbegin() const override { return buffer_.cbegin(); }
  const_iterator cend() const override { return cbegin() + size_; }

  // MutableByteBuffer overrides:
  uint8_t* mutable_data() override { return buffer_.mutable_data(); }
  void Fill(uint8_t value) override { buffer_.mutable_view(0, size_).Fill(value); }

 private:
  size_t size_;

  // The backing backing buffer can have a different size from what was
  // requested.
  StaticByteBuffer<BackingBufferSize> buffer_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(SlabBuffer);
};

namespace internal {

template <size_t BufferSize, size_t NumBuffers>
class SlabBufferImpl;

}  // namespace internal

template <size_t BufferSize, size_t NumBuffers>
using SlabBufferTraits = SlabAllocatorTraits<internal::SlabBufferImpl<BufferSize, NumBuffers>,
                                             sizeof(SlabBuffer<BufferSize>), NumBuffers>;

namespace internal {

template <size_t BufferSize, size_t NumBuffers>
class SlabBufferImpl : public SlabBuffer<BufferSize>,
                       public fbl::SlabAllocated<SlabBufferTraits<BufferSize, NumBuffers>> {
 public:
  explicit SlabBufferImpl(size_t size) : SlabBuffer<BufferSize>(size) {}

 private:
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(SlabBufferImpl);
};

}  // namespace internal

}  // namespace bt

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_SLAB_BUFFER_H_
