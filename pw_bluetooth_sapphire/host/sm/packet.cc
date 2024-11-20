// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "packet.h"

namespace bt {
namespace sm {

PacketReader::PacketReader(const ByteBuffer* buffer)
    : PacketView<Header>(buffer, buffer->size() - sizeof(Header)) {}

PacketWriter::PacketWriter(Code code, MutableByteBuffer* buffer)
    : MutablePacketView<Header>(buffer, buffer->size() - sizeof(Header)) {
  mutable_header()->code = code;
}

}  // namespace sm
}  // namespace bt
