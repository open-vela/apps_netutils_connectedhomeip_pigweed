// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "advertising_handle_map.h"

namespace bt::hci {

std::optional<AdvertisingHandle> AdvertisingHandleMap::MapHandle(const DeviceAddress& address) {
  if (auto it = addr_to_handle_.find(address); it != addr_to_handle_.end()) {
    return it->second;
  }

  if (Size() >= kMaxElements) {
    return std::nullopt;
  }

  std::optional<AdvertisingHandle> handle = NextHandle();
  ZX_ASSERT(handle);

  addr_to_handle_[address] = handle.value();
  handle_to_addr_[handle.value()] = address;
  return handle;
}

std::optional<DeviceAddress> AdvertisingHandleMap::GetAddress(AdvertisingHandle handle) const {
  if (handle_to_addr_.count(handle) != 0) {
    return handle_to_addr_.at(handle);
  }

  return std::nullopt;
}

void AdvertisingHandleMap::RemoveHandle(AdvertisingHandle handle) {
  if (handle_to_addr_.count(handle) == 0) {
    return;
  }

  const DeviceAddress& address = handle_to_addr_[handle];
  addr_to_handle_.erase(address);
  handle_to_addr_.erase(handle);
}

void AdvertisingHandleMap::RemoveAddress(const DeviceAddress& address) {
  auto node = addr_to_handle_.extract(address);
  if (node.empty()) {
    return;
  }

  AdvertisingHandle handle = node.mapped();
  handle_to_addr_.erase(handle);
}

std::size_t AdvertisingHandleMap::Size() const {
  ZX_ASSERT(addr_to_handle_.size() == handle_to_addr_.size());
  return addr_to_handle_.size();
}

bool AdvertisingHandleMap::Empty() const {
  ZX_ASSERT(addr_to_handle_.empty() == handle_to_addr_.empty());
  return addr_to_handle_.empty();
}

void AdvertisingHandleMap::Clear() {
  last_handle_ = kStartHandle;
  addr_to_handle_.clear();
  handle_to_addr_.clear();
}

std::optional<AdvertisingHandle> AdvertisingHandleMap::NextHandle() {
  if (Size() >= kMaxElements) {
    return std::nullopt;
  }

  AdvertisingHandle handle = last_handle_;
  do {
    handle = uint8_t(handle + 1) % kMaxElements;
  } while (handle_to_addr_.count(handle) != 0);

  last_handle_ = handle;
  return handle;
}

std::optional<AdvertisingHandle> AdvertisingHandleMap::LastUsedHandleForTesting() const {
  if (last_handle_ > kMaxAdvertisingHandle) {
    return std::nullopt;
  }

  return last_handle_;
}
}  // namespace bt::hci
