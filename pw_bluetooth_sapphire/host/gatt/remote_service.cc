// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remote_service.h"

#include <lib/async/default.h>

#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/run_or_post.h"
#include "src/connectivity/bluetooth/core/bt-host/common/slab_allocator.h"

namespace bt {
namespace gatt {

using att::Status;
using att::StatusCallback;

namespace {

bool IsInternalUuid(const UUID& uuid) {
  // clang-format off
  return
    uuid == types::kPrimaryService ||
    uuid == types::kSecondaryService ||
    uuid == types::kIncludeDeclaration ||
    uuid == types::kCharacteristicDeclaration ||
    uuid == types::kCharacteristicExtProperties ||
    uuid == types::kCharacteristicUserDescription ||
    uuid == types::kClientCharacteristicConfig ||
    uuid == types::kServerCharacteristicConfig ||
    uuid == types::kCharacteristicFormat ||
    uuid == types::kCharacteristicAggregateFormat;
  // clang-format on
}

void ReportStatus(Status status, StatusCallback callback, async_dispatcher_t* dispatcher) {
  RunOrPost([status, cb = std::move(callback)] { cb(status); }, dispatcher);
}

void ReportValue(att::Status status, const ByteBuffer& value,
                 RemoteService::ReadValueCallback callback, async_dispatcher_t* dispatcher) {
  if (!dispatcher) {
    callback(status, value);
    return;
  }

  // TODO(armansito): Consider making att::Bearer return the ATT PDU buffer
  // directly which would remove the need for a copy.

  auto buffer = NewSlabBuffer(value.size());
  value.Copy(buffer.get());

  async::PostTask(dispatcher, [status, callback = std::move(callback), val = std::move(buffer)] {
    callback(status, *val);
  });
}

void ReportValues(att::Status status, std::vector<RemoteService::ReadByTypeResult> values,
                  RemoteService::ReadByTypeCallback callback, async_dispatcher_t* dispatcher) {
  if (!dispatcher) {
    callback(status, std::move(values));
    return;
  }

  // It is safe to capture |values| because they contain owning pointers to the value buffers
  // (unlike in ReportValue());
  async::PostTask(dispatcher,
                  [status, callback = std::move(callback), values = std::move(values)]() mutable {
                    callback(status, std::move(values));
                  });
}

}  // namespace

// static
constexpr size_t RemoteService::kSentinel;

RemoteService::RemoteService(const ServiceData& service_data, fxl::WeakPtr<Client> client,
                             async_dispatcher_t* gatt_dispatcher)
    : service_data_(service_data),
      gatt_dispatcher_(gatt_dispatcher),
      client_(client),
      remaining_descriptor_requests_(kSentinel),
      shut_down_(false) {
  ZX_DEBUG_ASSERT(client_);
  ZX_DEBUG_ASSERT(gatt_dispatcher_);
}

RemoteService::~RemoteService() {
  std::lock_guard<std::mutex> lock(mtx_);
  ZX_DEBUG_ASSERT(!alive());
}

void RemoteService::ShutDown() {
  ZX_DEBUG_ASSERT(IsOnGattThread());

  std::vector<PendingClosure> rm_handlers;

  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!alive()) {
      return;
    }

    for (auto& chr : characteristics_)
      chr.second.ShutDown();

    shut_down_ = true;
    rm_handlers = std::move(rm_handlers_);
  }

  for (auto& handler : rm_handlers) {
    RunOrPost(std::move(handler.callback), handler.dispatcher);
  }
}

bool RemoteService::AddRemovedHandler(fit::closure handler, async_dispatcher_t* dispatcher) {
  std::lock_guard<std::mutex> lock(mtx_);

  if (!alive())
    return false;

  rm_handlers_.emplace_back(std::move(handler), dispatcher);
  return true;
}

void RemoteService::DiscoverCharacteristics(CharacteristicCallback callback,
                                            async_dispatcher_t* dispatcher) {
  RunGattTask([this, cb = std::move(callback), dispatcher]() mutable {
    if (shut_down_) {
      ReportCharacteristics(Status(HostError::kFailed), std::move(cb), dispatcher);
      return;
    }

    // Characteristics already discovered. Return success.
    if (HasCharacteristics()) {
      ReportCharacteristics(Status(), std::move(cb), dispatcher);
      return;
    }

    // Queue this request.
    pending_discov_reqs_.emplace_back(std::move(cb), dispatcher);

    // Nothing to do if a write request is already pending.
    if (pending_discov_reqs_.size() > 1u)
      return;

    auto self = fbl::RefPtr(this);
    auto chrc_cb = [self](const CharacteristicData& chr) {
      if (!self->shut_down_) {
        // try_emplace should not fail here; our GATT::Client explicitly ensures that handles are
        // strictly ascending (as described in the spec) so we should never see a handle collision
        self->characteristics_.try_emplace(CharacteristicHandle(chr.value_handle), self->client_,
                                           chr);
      }
    };

    auto res_cb = [self](Status status) mutable {
      if (self->shut_down_) {
        status = Status(HostError::kFailed);
      }

      if (bt_is_error(status, TRACE, "gatt", "characteristic discovery failed")) {
        self->characteristics_.clear();
      }

      if (self->characteristics_.empty()) {
        if (status) {
          // This marks that characteristic discovery has completed
          // successfully.
          self->remaining_descriptor_requests_ = 0u;
        }

        // Skip descriptor discovery and end the procedure as no characteristics
        // were found (or the operation failed).
        self->CompleteCharacteristicDiscovery(status);
        return;
      }

      self->StartDescriptorDiscovery();
    };

    client_->DiscoverCharacteristics(service_data_.range_start, service_data_.range_end,
                                     std::move(chrc_cb), std::move(res_cb));
  });
}

bool RemoteService::IsDiscovered() const {
  // TODO(armansito): Return true only if included services have also been
  // discovered.
  return HasCharacteristics();
}

void RemoteService::ReadCharacteristic(CharacteristicHandle id, ReadValueCallback cb,
                                       async_dispatcher_t* dispatcher) {
  RunGattTask([this, id, cb = std::move(cb), dispatcher]() mutable {
    RemoteCharacteristic* chrc;
    att::Status status = att::Status(GetCharacteristic(id, &chrc));
    ZX_DEBUG_ASSERT(chrc || !status);
    if (!status) {
      ReportValue(status, BufferView(), std::move(cb), dispatcher);
      return;
    }

    if (!(chrc->info().properties & Property::kRead)) {
      bt_log(TRACE, "gatt", "characteristic does not support \"read\"");
      ReportValue(att::Status(HostError::kNotSupported), BufferView(), std::move(cb), dispatcher);
      return;
    }

    SendReadRequest(chrc->info().value_handle, std::move(cb), dispatcher);
  });
}

void RemoteService::ReadLongCharacteristic(CharacteristicHandle id, uint16_t offset,
                                           size_t max_bytes, ReadValueCallback cb,
                                           async_dispatcher_t* dispatcher) {
  RunGattTask([this, id, offset, max_bytes, cb = std::move(cb), dispatcher]() mutable {
    RemoteCharacteristic* chrc;
    att::Status status = att::Status(GetCharacteristic(id, &chrc));
    ZX_DEBUG_ASSERT(chrc || !status);
    if (!status) {
      ReportValue(status, BufferView(), std::move(cb), dispatcher);
      return;
    }

    if (!(chrc->info().properties & Property::kRead)) {
      bt_log(TRACE, "gatt", "characteristic does not support \"read\"");
      ReportValue(att::Status(HostError::kNotSupported), BufferView(), std::move(cb), dispatcher);
      return;
    }

    if (max_bytes == 0) {
      bt_log(SPEW, "gatt", "invalid value for |max_bytes|: 0");
      ReportValue(att::Status(HostError::kInvalidParameters), BufferView(), std::move(cb),
                  dispatcher);
      return;
    }

    // Set up the buffer in which we'll accumulate the blobs.
    auto buffer = NewSlabBuffer(std::min(max_bytes, att::kMaxAttributeValueLength));
    if (!buffer) {
      ReportValue(att::Status(HostError::kOutOfMemory), BufferView(), std::move(cb), dispatcher);
      return;
    }

    ReadLongHelper(chrc->info().value_handle, offset, std::move(buffer), 0u /* bytes_read */,
                   std::move(cb), dispatcher);
  });
}

void RemoteService::ReadByType(const UUID& type, ReadByTypeCallback callback,
                               async_dispatcher_t* dispatcher) {
  RunGattTask([this, type, cb = std::move(callback), dispatcher]() mutable {
    // Caller should not request a UUID of an internal attribute (e.g. service declaration).
    if (IsInternalUuid(type)) {
      bt_log(SPEW, "gatt", "ReadByType called with internal GATT type (type: %s)", bt_str(type));
      ReportValues(att::Status(HostError::kInvalidParameters), {}, std::move(cb), dispatcher);
      return;
    }

    // Read range is entire service range.
    ReadByTypeHelper(type, service_data_.range_start, service_data_.range_end, {}, std::move(cb),
                     dispatcher);
  });
}

void RemoteService::WriteCharacteristic(CharacteristicHandle id, std::vector<uint8_t> value,
                                        StatusCallback cb, async_dispatcher_t* dispatcher) {
  RunGattTask([this, id, value = std::move(value), cb = std::move(cb), dispatcher]() mutable {
    RemoteCharacteristic* chrc;
    Status status = Status(GetCharacteristic(id, &chrc));
    ZX_DEBUG_ASSERT(chrc || !status);
    if (!status) {
      ReportStatus(status, std::move(cb), dispatcher);
      return;
    }

    if (!(chrc->info().properties & Property::kWrite)) {
      bt_log(TRACE, "gatt", "characteristic does not support \"write\"");
      ReportStatus(Status(HostError::kNotSupported), std::move(cb), dispatcher);
      return;
    }

    SendWriteRequest(chrc->info().value_handle, BufferView(value.data(), value.size()),
                     std::move(cb), dispatcher);
  });
}

void RemoteService::WriteLongCharacteristic(CharacteristicHandle id, uint16_t offset,
                                            std::vector<uint8_t> value, ReliableMode reliable_mode,
                                            StatusCallback cb, async_dispatcher_t* dispatcher) {
  RunGattTask([this, id, offset, value = std::move(value), mode = std::move(reliable_mode),
               cb = std::move(cb), dispatcher]() mutable {
    RemoteCharacteristic* chrc;
    Status status = Status(GetCharacteristic(id, &chrc));
    ZX_DEBUG_ASSERT(chrc || !status);
    if (!status) {
      ReportStatus(status, std::move(cb), dispatcher);
      return;
    }

    // TODO(48704): Validate the |extended_properties| field in the case of ReliableMode::kEnabled.
    if (!(chrc->info().properties & Property::kWrite)) {
      bt_log(TRACE, "gatt", "characteristic does not support \"write\"");
      ReportStatus(Status(HostError::kNotSupported), std::move(cb), dispatcher);
      return;
    }

    SendLongWriteRequest(chrc->info().value_handle, offset, BufferView(value.data(), value.size()),
                         std::move(mode), std::move(cb), dispatcher);
  });
}

void RemoteService::WriteCharacteristicWithoutResponse(CharacteristicHandle id,
                                                       std::vector<uint8_t> value) {
  RunGattTask([this, id, value = std::move(value)]() mutable {
    RemoteCharacteristic* chrc;
    Status status = Status(GetCharacteristic(id, &chrc));
    ZX_DEBUG_ASSERT(chrc || !status);
    if (!status) {
      return;
    }

    if (!(chrc->info().properties & Property::kWriteWithoutResponse)) {
      bt_log(TRACE, "gatt", "characteristic does not support \"write without response\"");
      return;
    }

    client_->WriteWithoutResponse(chrc->info().value_handle,
                                  BufferView(value.data(), value.size()));
  });
}

void RemoteService::ReadDescriptor(DescriptorHandle id, ReadValueCallback cb,
                                   async_dispatcher_t* dispatcher) {
  RunGattTask([this, id, cb = std::move(cb), dispatcher]() mutable {
    const DescriptorData* desc;
    att::Status status = att::Status(GetDescriptor(id, &desc));
    ZX_DEBUG_ASSERT(desc || !status);
    if (!status) {
      ReportValue(status, BufferView(), std::move(cb), dispatcher);
      return;
    }

    SendReadRequest(desc->handle, std::move(cb), dispatcher);
  });
}

void RemoteService::ReadLongDescriptor(DescriptorHandle id, uint16_t offset, size_t max_bytes,
                                       ReadValueCallback cb, async_dispatcher_t* dispatcher) {
  RunGattTask([this, id, offset, max_bytes, cb = std::move(cb), dispatcher]() mutable {
    const DescriptorData* desc;
    att::Status status = att::Status(GetDescriptor(id, &desc));
    ZX_DEBUG_ASSERT(desc || !status);
    if (!status) {
      ReportValue(status, BufferView(), std::move(cb), dispatcher);
      return;
    }

    if (max_bytes == 0) {
      bt_log(SPEW, "gatt", "invalid value for |max_bytes|: 0");
      ReportValue(att::Status(HostError::kInvalidParameters), BufferView(), std::move(cb),
                  dispatcher);
      return;
    }

    // Set up the buffer in which we'll accumulate the blobs.
    auto buffer = NewSlabBuffer(std::min(max_bytes, att::kMaxAttributeValueLength));
    if (!buffer) {
      ReportValue(att::Status(HostError::kOutOfMemory), BufferView(), std::move(cb), dispatcher);
      return;
    }

    ReadLongHelper(desc->handle, offset, std::move(buffer), 0u /* bytes_read */, std::move(cb),
                   dispatcher);
  });
}

void RemoteService::WriteDescriptor(DescriptorHandle id, std::vector<uint8_t> value,
                                    att::StatusCallback cb, async_dispatcher_t* dispatcher) {
  RunGattTask([this, id, value = std::move(value), cb = std::move(cb), dispatcher]() mutable {
    const DescriptorData* desc;
    Status status = Status(GetDescriptor(id, &desc));
    ZX_DEBUG_ASSERT(desc || !status);
    if (!status) {
      ReportStatus(status, std::move(cb), dispatcher);
      return;
    }

    // Do not allow writing to internally reserved descriptors.
    if (desc->type == types::kClientCharacteristicConfig) {
      bt_log(TRACE, "gatt", "writing to CCC descriptor not allowed");
      ReportStatus(Status(HostError::kNotSupported), std::move(cb), dispatcher);
      return;
    }

    SendWriteRequest(desc->handle, BufferView(value.data(), value.size()), std::move(cb),
                     dispatcher);
  });
}

void RemoteService::WriteLongDescriptor(DescriptorHandle id, uint16_t offset,
                                        std::vector<uint8_t> value, att::StatusCallback cb,
                                        async_dispatcher_t* dispatcher) {
  RunGattTask(
      [this, id, offset, value = std::move(value), cb = std::move(cb), dispatcher]() mutable {
        const DescriptorData* desc;
        Status status = Status(GetDescriptor(id, &desc));
        ZX_DEBUG_ASSERT(desc || !status);
        if (!status) {
          ReportStatus(status, std::move(cb), dispatcher);
          return;
        }

        // Do not allow writing to internally reserved descriptors.
        if (desc->type == types::kClientCharacteristicConfig) {
          bt_log(TRACE, "gatt", "writing to CCC descriptor not allowed");
          ReportStatus(Status(HostError::kNotSupported), std::move(cb), dispatcher);
          return;
        }

        // For writing long descriptors, reliable mode is not supported.
        auto mode = ReliableMode::kDisabled;
        SendLongWriteRequest(desc->handle, offset, BufferView(value.data(), value.size()),
                             std::move(mode), std::move(cb), dispatcher);
      });
}

void RemoteService::EnableNotifications(CharacteristicHandle id, ValueCallback callback,
                                        NotifyStatusCallback status_callback,
                                        async_dispatcher_t* dispatcher) {
  RunGattTask([this, id, cb = std::move(callback), status_cb = std::move(status_callback),
               dispatcher]() mutable {
    RemoteCharacteristic* chrc;
    att::Status status = att::Status(GetCharacteristic(id, &chrc));
    ZX_DEBUG_ASSERT(chrc || !status);
    if (!status) {
      RunOrPost([status, cb = std::move(status_cb)] { cb(status, kInvalidId); }, dispatcher);
      return;
    }

    chrc->EnableNotifications(std::move(cb), std::move(status_cb), dispatcher);
  });
}

void RemoteService::DisableNotifications(CharacteristicHandle id, IdType handler_id,
                                         StatusCallback status_callback,
                                         async_dispatcher_t* dispatcher) {
  RunGattTask([this, id, handler_id, cb = std::move(status_callback), dispatcher]() mutable {
    RemoteCharacteristic* chrc;
    att::Status status = att::Status(GetCharacteristic(id, &chrc));
    ZX_DEBUG_ASSERT(chrc || !status);
    if (status && !chrc->DisableNotifications(handler_id)) {
      status = att::Status(HostError::kNotFound);
    }
    ReportStatus(status, std::move(cb), dispatcher);
  });
}

void RemoteService::StartDescriptorDiscovery() {
  ZX_DEBUG_ASSERT(IsOnGattThread());
  ZX_DEBUG_ASSERT(!pending_discov_reqs_.empty());

  ZX_DEBUG_ASSERT(characteristics_.size());
  remaining_descriptor_requests_ = characteristics_.size();

  auto self = fbl::RefPtr(this);

  // Callback called for each characteristic. This may be called in any
  // order since we request the descriptors of all characteristics all at
  // once.
  auto desc_done_callback = [self](att::Status status) {
    // Do nothing if discovery was concluded earlier (which would have cleared
    // the pending discovery requests).
    if (self->pending_discov_reqs_.empty())
      return;

    // Report an error if the service was removed.
    if (self->shut_down_) {
      status = att::Status(HostError::kFailed);
    }

    if (status) {
      self->remaining_descriptor_requests_ -= 1;

      // Defer handling
      if (self->remaining_descriptor_requests_ > 0)
        return;

      // HasCharacteristics() should return true now.
      ZX_DEBUG_ASSERT(self->HasCharacteristics());

      // Fall through and notify clients below.
    } else {
      ZX_DEBUG_ASSERT(!self->HasCharacteristics());
      bt_log(TRACE, "gatt", "descriptor discovery failed %s", status.ToString().c_str());
      self->characteristics_.clear();

      // Fall through and notify the clients below.
    }

    self->CompleteCharacteristicDiscovery(status);
  };

  // Characteristics are stored in an (ordered) std::map by value_handle, so we iterate in order;
  // according to the spec (BT 5.0 Vol 3, part G, 3.3), the value handle must appear immediately
  // after the characteristic handle so the handles are also guaranteed to be in order. Therefore
  // we can use the next in the iteration to calculate the handle range.
  for (auto iter = characteristics_.begin(); iter != characteristics_.end(); ++iter) {
    auto next = iter;
    ++next;
    att::Handle end_handle;
    if (next == characteristics_.end()) {
      end_handle = service_data_.range_end;
    } else {
      end_handle = next->second.info().handle - 1;
    }

    ZX_DEBUG_ASSERT(client_);
    iter->second.DiscoverDescriptors(end_handle, desc_done_callback);
  }
}

bool RemoteService::IsOnGattThread() const {
  return async_get_default_dispatcher() == gatt_dispatcher_;
}

HostError RemoteService::GetCharacteristic(CharacteristicHandle id,
                                           RemoteCharacteristic** out_char) {
  ZX_DEBUG_ASSERT(IsOnGattThread());
  ZX_DEBUG_ASSERT(out_char);

  if (shut_down_)
    return HostError::kFailed;

  if (!HasCharacteristics())
    return HostError::kNotReady;

  auto chr = characteristics_.find(id);
  if (chr == characteristics_.end())
    return HostError::kNotFound;

  *out_char = &chr->second;
  return HostError::kNoError;
}

HostError RemoteService::GetDescriptor(DescriptorHandle id, const DescriptorData** out_desc) {
  ZX_DEBUG_ASSERT(IsOnGattThread());
  ZX_DEBUG_ASSERT(out_desc);

  if (shut_down_)
    return HostError::kFailed;

  if (!HasCharacteristics())
    return HostError::kNotReady;

  for (auto iter = characteristics_.begin(); iter != characteristics_.end(); ++iter) {
    auto next = iter;
    ++next;
    if (next == characteristics_.end() || next->second.info().handle > id.value) {
      const auto& descriptors = iter->second.descriptors();
      auto desc = descriptors.find(id);
      if (desc != descriptors.end()) {
        *out_desc = &desc->second;
        return HostError::kNoError;
      }
    }
  }

  return HostError::kNotFound;
}

void RemoteService::RunGattTask(fit::closure task) {
  // Capture a reference to this object to guarantee its lifetime.
  RunOrPost([objref = fbl::RefPtr(this), task = std::move(task)] { task(); }, gatt_dispatcher_);
}

void RemoteService::ReportCharacteristics(Status status, CharacteristicCallback callback,
                                          async_dispatcher_t* dispatcher) {
  ZX_DEBUG_ASSERT(IsOnGattThread());
  RunOrPost(
      [self = fbl::RefPtr(this), status, cb = std::move(callback)] {
        // We return a new copy of only the immutable data of our characteristics and their
        // descriptors. This requires a copy, which *could* be expensive in the (unlikely) case
        // that a service has a very large number of characteristics, but provides much safer
        // guarantees of correctness than returning a reference into our object. If the copy proves
        // too expensive, then we should consider returning some kind of safe reference counting
        // handle.
        CharacteristicMap characteristics;
        for (const auto& [_handle, chrc] : self->characteristics_)
          characteristics.try_emplace(_handle, chrc.info(), chrc.descriptors());

        cb(status, characteristics);
      },
      dispatcher);
}

void RemoteService::CompleteCharacteristicDiscovery(att::Status status) {
  ZX_DEBUG_ASSERT(!pending_discov_reqs_.empty());
  ZX_DEBUG_ASSERT(!status || remaining_descriptor_requests_ == 0u);

  auto pending = std::move(pending_discov_reqs_);
  for (auto& req : pending) {
    ReportCharacteristics(status, std::move(req.callback), req.dispatcher);
  }
}

void RemoteService::SendReadRequest(att::Handle handle, ReadValueCallback cb,
                                    async_dispatcher_t* dispatcher) {
  client_->ReadRequest(
      handle, [cb = std::move(cb), dispatcher](att::Status status, const auto& value) mutable {
        ReportValue(status, value, std::move(cb), dispatcher);
      });
}

void RemoteService::SendWriteRequest(att::Handle handle, const ByteBuffer& value, StatusCallback cb,
                                     async_dispatcher_t* dispatcher) {
  client_->WriteRequest(handle, value, [cb = std::move(cb), dispatcher](Status status) mutable {
    ReportStatus(status, std::move(cb), dispatcher);
  });
}

void RemoteService::SendLongWriteRequest(att::Handle handle, uint16_t offset, BufferView value,
                                         ReliableMode reliable_mode, att::StatusCallback final_cb,
                                         async_dispatcher_t* dispatcher) {
  att::PrepareWriteQueue long_write_queue;
  auto header_ln = sizeof(att::PrepareWriteRequestParams) + sizeof(att::OpCode);
  uint16_t bytes_written = 0;

  // Divide up the long write into it's constituent PreparedWrites and add them
  // to the queue.
  while (bytes_written < value.size()) {
    uint16_t part_value_size = std::min(client_->mtu() - header_ln, value.size() - bytes_written);
    auto part_buffer = value.view(bytes_written, part_value_size);

    long_write_queue.push(att::QueuedWrite(handle, offset, part_buffer));

    bytes_written += part_value_size;
    offset += part_value_size;
  }

  client_->ExecutePrepareWrites(std::move(long_write_queue), std::move(reliable_mode),
                                [cb = std::move(final_cb), dispatcher](Status status) mutable {
                                  ReportStatus(status, std::move(cb), dispatcher);
                                });
}

void RemoteService::ReadLongHelper(att::Handle value_handle, uint16_t offset,
                                   MutableByteBufferPtr buffer, size_t bytes_read,
                                   ReadValueCallback callback, async_dispatcher_t* dispatcher) {
  ZX_DEBUG_ASSERT(IsOnGattThread());
  ZX_DEBUG_ASSERT(callback);
  ZX_DEBUG_ASSERT(buffer);
  ZX_DEBUG_ASSERT(!shut_down_);

  // Capture a reference so that this object is alive when the callback runs.
  auto self = fbl::RefPtr(this);
  auto read_blob_cb = [self, value_handle, offset, buffer = std::move(buffer), bytes_read,
                       cb = std::move(callback),
                       dispatcher](att::Status status, const ByteBuffer& blob) mutable {
    if (self->shut_down_) {
      // The service was removed. Report an error.
      ReportValue(att::Status(HostError::kCanceled), BufferView(), std::move(cb), dispatcher);
      return;
    }

    if (!status) {
      ReportValue(status, BufferView(), std::move(cb), dispatcher);
      return;
    }

    // Copy the blob into our |buffer|. |blob| may be truncated depending on the
    // size of |buffer|.
    ZX_DEBUG_ASSERT(bytes_read < buffer->size());
    size_t copy_size = std::min(blob.size(), buffer->size() - bytes_read);
    buffer->Write(blob.view(0, copy_size), bytes_read);
    bytes_read += copy_size;

    // We are done if the blob is smaller than (ATT_MTU - 1) or we have read the
    // maximum number of bytes requested.
    ZX_DEBUG_ASSERT(bytes_read <= buffer->size());
    if (blob.size() < (self->client_->mtu() - 1) || bytes_read == buffer->size()) {
      ReportValue(att::Status(), buffer->view(0, bytes_read), std::move(cb), dispatcher);
      return;
    }

    // We have more bytes to read. Read the next blob.
    self->ReadLongHelper(value_handle, offset + blob.size(), std::move(buffer), bytes_read,
                         std::move(cb), dispatcher);
  };

  client_->ReadBlobRequest(value_handle, offset, std::move(read_blob_cb));
}

void RemoteService::ReadByTypeHelper(const UUID& type, att::Handle start, att::Handle end,
                                     std::vector<RemoteService::ReadByTypeResult> values,
                                     ReadByTypeCallback callback, async_dispatcher_t* dispatcher) {
  if (start > end) {
    ReportValues(att::Status(), std::move(values), std::move(callback), dispatcher);
    return;
  }

  auto read_cb = [self = fbl::RefPtr(this), type, end, values_accum = std::move(values),
                  cb = std::move(callback), dispatcher](
                     att::Status status, std::vector<Client::ReadByTypeResult> values) mutable {
    if (!status.is_success()) {
      // Treat kAttributeNotFound error as success, since it's used to indicate when a sequence of
      // reads has read all matching attributes.
      if (status.is_protocol_error() &&
          status.protocol_error() == att::ErrorCode::kAttributeNotFound) {
        status = att::Status();
      }

      ReportValues(status, std::move(values_accum), std::move(cb), dispatcher);
      return;
    }

    // Client checks for invalid response where status is success but no values are returned.
    ZX_ASSERT(!values.empty());

    // Convert and accumulate values.
    for (const auto& result : values) {
      auto buffer = NewSlabBuffer(result.value.size());
      result.value.Copy(buffer.get());
      values_accum.push_back(
          ReadByTypeResult{CharacteristicHandle(result.handle), std::move(buffer)});
    }

    // Start next read right after last returned attribute.
    att::Handle start_next = values.back().handle + 1;

    self->ReadByTypeHelper(type, start_next, end, std::move(values_accum), std::move(cb),
                           dispatcher);
  };
  client_->ReadByTypeRequest(type, start, end, std::move(read_cb));
}

void RemoteService::HandleNotification(att::Handle value_handle, const ByteBuffer& value) {
  ZX_DEBUG_ASSERT(IsOnGattThread());

  if (shut_down_)
    return;

  auto iter = characteristics_.find(CharacteristicHandle(value_handle));
  if (iter != characteristics_.end()) {
    iter->second.HandleNotification(value);
  }
}

}  // namespace gatt
}  // namespace bt
