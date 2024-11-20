// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "client.h"

#include <zircon/assert.h>

#include "gatt_defs.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/slab_allocator.h"

using bt::HostError;

namespace bt {

using att::StatusCallback;

namespace gatt {
namespace {

MutableByteBufferPtr NewPDU(size_t param_size) {
  auto pdu = NewSlabBuffer(sizeof(att::Header) + param_size);
  if (!pdu) {
    bt_log(TRACE, "att", "out of memory");
  }
  return pdu;
}

template <att::UUIDType Format, typename EntryType = att::InformationData<Format>>
bool ProcessDescriptorDiscoveryResponse(att::Handle range_start, att::Handle range_end,
                                        BufferView entries,
                                        Client::DescriptorCallback desc_callback,
                                        att::Handle* out_last_handle) {
  ZX_DEBUG_ASSERT(out_last_handle);

  if (entries.size() % sizeof(EntryType)) {
    bt_log(TRACE, "gatt", "malformed information data list");
    return false;
  }

  att::Handle last_handle = range_end;
  while (entries.size()) {
    const EntryType& entry = entries.As<EntryType>();

    att::Handle desc_handle = le16toh(entry.handle);

    // Stop and report an error if the server erroneously responds with
    // an attribute outside the requested range.
    if (desc_handle > range_end || desc_handle < range_start) {
      bt_log(TRACE, "gatt",
             "descriptor handle out of range (handle: %#.4x, "
             "range: %#.4x - %#.4x)",
             desc_handle, range_start, range_end);
      return false;
    }

    // The handles must be strictly increasing.
    if (last_handle != range_end && desc_handle <= last_handle) {
      bt_log(TRACE, "gatt", "descriptor handles not strictly increasing");
      return false;
    }

    last_handle = desc_handle;

    // Notify the handler.
    desc_callback(DescriptorData(desc_handle, UUID(entry.uuid)));

    entries = entries.view(sizeof(EntryType));
  }

  *out_last_handle = last_handle;
  return true;
}

}  // namespace

class Impl final : public Client {
 public:
  explicit Impl(fxl::RefPtr<att::Bearer> bearer) : att_(bearer), weak_ptr_factory_(this) {
    ZX_DEBUG_ASSERT(att_);

    auto handler = [this](auto txn_id, const att::PacketReader& pdu) {
      ZX_DEBUG_ASSERT(pdu.opcode() == att::kNotification || pdu.opcode() == att::kIndication);

      if (pdu.payload_size() < sizeof(att::NotificationParams)) {
        // Received a malformed notification. Disconnect the link.
        bt_log(TRACE, "gatt", "malformed notification/indication PDU");
        att_->ShutDown();
        return;
      }

      bool is_ind = pdu.opcode() == att::kIndication;
      const auto& params = pdu.payload<att::NotificationParams>();
      att::Handle handle = le16toh(params.handle);

      // Auto-confirm indications.
      if (is_ind) {
        auto pdu = NewPDU(0u);
        if (pdu) {
          att::PacketWriter(att::kConfirmation, pdu.get());
          att_->Reply(txn_id, std::move(pdu));
        } else {
          att_->ReplyWithError(txn_id, handle, att::ErrorCode::kInsufficientResources);
        }
      }

      // Run the handler
      if (notification_handler_) {
        notification_handler_(is_ind, handle,
                              BufferView(params.value, pdu.payload_size() - sizeof(att::Handle)));
      } else {
        bt_log(SPEW, "gatt", "dropped notification/indication without handler");
      }
    };

    not_handler_id_ = att_->RegisterHandler(att::kNotification, handler);
    ind_handler_id_ = att_->RegisterHandler(att::kIndication, handler);
  }

  ~Impl() override {
    att_->UnregisterHandler(not_handler_id_);
    att_->UnregisterHandler(ind_handler_id_);
  }

 private:
  fxl::WeakPtr<Client> AsWeakPtr() override { return weak_ptr_factory_.GetWeakPtr(); }

  uint16_t mtu() const override { return att_->mtu(); }

  void ExchangeMTU(MTUCallback mtu_cb) override {
    auto pdu = NewPDU(sizeof(att::ExchangeMTURequestParams));
    if (!pdu) {
      mtu_cb(att::Status(HostError::kOutOfMemory), 0);
      return;
    }

    att::PacketWriter writer(att::kExchangeMTURequest, pdu.get());
    auto params = writer.mutable_payload<att::ExchangeMTURequestParams>();
    params->client_rx_mtu = htole16(att_->preferred_mtu());

    auto rsp_cb = BindCallback([this, mtu_cb = mtu_cb.share()](const att::PacketReader& rsp) {
      ZX_DEBUG_ASSERT(rsp.opcode() == att::kExchangeMTUResponse);

      if (rsp.payload_size() != sizeof(att::ExchangeMTUResponseParams)) {
        // Received a malformed response. Disconnect the link.
        att_->ShutDown();

        mtu_cb(att::Status(HostError::kPacketMalformed), 0);
        return;
      }

      const auto& rsp_params = rsp.payload<att::ExchangeMTUResponseParams>();
      uint16_t server_mtu = le16toh(rsp_params.server_rx_mtu);

      // If the minimum value is less than the default MTU, then go with the
      // default MTU (Vol 3, Part F, 3.4.2.2).
      uint16_t final_mtu = std::max(att::kLEMinMTU, std::min(server_mtu, att_->preferred_mtu()));
      att_->set_mtu(final_mtu);

      mtu_cb(att::Status(), final_mtu);
    });

    auto error_cb =
        BindErrorCallback([this, mtu_cb = mtu_cb.share()](att::Status status, att::Handle handle) {
          // "If the Error Response is sent by the server with the Error Code
          // set to Request Not Supported, [...] the default MTU shall be used
          // (Vol 3, Part G, 4.3.1)"
          if (status.is_protocol_error() &&
              status.protocol_error() == att::ErrorCode::kRequestNotSupported) {
            bt_log(TRACE, "gatt", "peer does not support MTU exchange: using default");
            att_->set_mtu(att::kLEMinMTU);
            mtu_cb(status, att::kLEMinMTU);
            return;
          }

          bt_log(TRACE, "gatt", "MTU exchange failed: %s", status.ToString().c_str());
          mtu_cb(status, 0);
        });

    att_->StartTransaction(std::move(pdu), std::move(rsp_cb), std::move(error_cb));
  }

  void DiscoverPrimaryServices(ServiceCallback svc_callback,
                               StatusCallback status_callback) override {
    DiscoverPrimaryServicesInternal(att::kHandleMin, att::kHandleMax, std::move(svc_callback),
                                    std::move(status_callback));
  }

  void DiscoverPrimaryServicesInternal(att::Handle start, att::Handle end,
                                       ServiceCallback svc_callback,
                                       StatusCallback status_callback) {
    auto pdu = NewPDU(sizeof(att::ReadByGroupTypeRequestParams16));
    if (!pdu) {
      status_callback(att::Status(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kReadByGroupTypeRequest, pdu.get());
    auto* params = writer.mutable_payload<att::ReadByGroupTypeRequestParams16>();
    params->start_handle = htole16(start);
    params->end_handle = htole16(end);
    params->type = htole16(types::kPrimaryService16);

    auto rsp_cb =
        BindCallback([this, svc_cb = std::move(svc_callback),
                      res_cb = status_callback.share()](const att::PacketReader& rsp) mutable {
          ZX_DEBUG_ASSERT(rsp.opcode() == att::kReadByGroupTypeResponse);

          if (rsp.payload_size() < sizeof(att::ReadByGroupTypeResponseParams)) {
            // Received malformed response. Disconnect the link.
            bt_log(TRACE, "gatt", "received malformed Read By Group Type response");
            att_->ShutDown();
            res_cb(att::Status(HostError::kPacketMalformed));
            return;
          }

          const auto& rsp_params = rsp.payload<att::ReadByGroupTypeResponseParams>();
          uint8_t entry_length = rsp_params.length;

          // We expect the returned attribute value to be a 16-bit or 128-bit
          // service UUID.
          constexpr size_t kAttrDataSize16 =
              sizeof(att::AttributeGroupDataEntry) + sizeof(att::AttributeType16);
          constexpr size_t kAttrDataSize128 =
              sizeof(att::AttributeGroupDataEntry) + sizeof(att::AttributeType128);

          if (entry_length != kAttrDataSize16 && entry_length != kAttrDataSize128) {
            bt_log(TRACE, "gatt", "invalid attribute data length");
            att_->ShutDown();
            res_cb(att::Status(HostError::kPacketMalformed));
            return;
          }

          BufferView attr_data_list(rsp_params.attribute_data_list, rsp.payload_size() - 1);
          if (attr_data_list.size() % entry_length) {
            bt_log(TRACE, "gatt", "malformed attribute data list");
            att_->ShutDown();
            res_cb(att::Status(HostError::kPacketMalformed));
            return;
          }

          att::Handle last_handle = att::kHandleMax;
          while (attr_data_list.size()) {
            const auto& entry = attr_data_list.As<att::AttributeGroupDataEntry>();

            ServiceData service;
            service.range_start = le16toh(entry.start_handle);
            service.range_end = le16toh(entry.group_end_handle);

            if (service.range_end < service.range_start) {
              bt_log(TRACE, "gatt", "received malformed service range values");
              res_cb(att::Status(HostError::kPacketMalformed));
              return;
            }

            last_handle = service.range_end;

            BufferView value(entry.value, entry_length - (2 * sizeof(att::Handle)));

            // This must succeed as we have performed the appropriate checks above.
            __UNUSED bool result = UUID::FromBytes(value, &service.type);
            ZX_DEBUG_ASSERT(result);

            // Notify the handler.
            svc_cb(service);

            attr_data_list = attr_data_list.view(entry_length);
          }

          // The procedure is over if we have reached the end of the handle range.
          if (last_handle == att::kHandleMax) {
            res_cb(att::Status());
            return;
          }

          // Request the next batch.
          DiscoverPrimaryServicesInternal(last_handle + 1, att::kHandleMax, std::move(svc_cb),
                                          std::move(res_cb));
        });

    auto error_cb = BindErrorCallback(
        [res_cb = status_callback.share()](att::Status status, att::Handle handle) {
          // An Error Response code of "Attribute Not Found" indicates the end
          // of the procedure (v5.0, Vol 3, Part G, 4.4.1).
          if (status.is_protocol_error() &&
              status.protocol_error() == att::ErrorCode::kAttributeNotFound) {
            res_cb(att::Status());
            return;
          }

          res_cb(status);
        });

    att_->StartTransaction(std::move(pdu), std::move(rsp_cb), std::move(error_cb));
  }

  void DiscoverPrimaryServicesByUUID(ServiceCallback svc_callback, StatusCallback status_callback,
                                     UUID uuid) override {
    DiscoverPrimaryServicesByUUIDInternal(att::kHandleMin, att::kHandleMax, std::move(svc_callback),
                                          std::move(status_callback), uuid);
  }

  void DiscoverPrimaryServicesByUUIDInternal(att::Handle start, att::Handle end,
                                             ServiceCallback svc_callback,
                                             StatusCallback status_callback, UUID uuid) {
    size_t uuid_size_bytes = uuid.CompactSize(/* allow 32 bit UUIDs */ false);
    auto pdu = NewPDU(sizeof(att::FindByTypeValueRequestParams) + uuid_size_bytes);
    if (!pdu) {
      status_callback(att::Status(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kFindByTypeValueRequest, pdu.get());
    auto* params = writer.mutable_payload<att::FindByTypeValueRequestParams>();
    params->start_handle = htole16(start);
    params->end_handle = htole16(end);
    params->type = htole16(types::kPrimaryService16);
    MutableBufferView value_view(params->value, uuid_size_bytes);
    uuid.ToBytes(&value_view, /* allow 32 bit UUIDs */ false);

    auto rsp_cb =
        BindCallback([this, svc_cb = std::move(svc_callback), res_cb = status_callback.share(),
                      uuid](const att::PacketReader& rsp) mutable {
          ZX_DEBUG_ASSERT(rsp.opcode() == att::kFindByTypeValueResponse);

          size_t payload_size = rsp.payload_size();
          if (payload_size < 1 || payload_size % sizeof(att::FindByTypeValueResponseParams) != 0) {
            // Received malformed response. Disconnect the link.
            bt_log(TRACE, "gatt", "received malformed Find By Type Value response with size %zu", payload_size);
            att_->ShutDown();
            res_cb(att::Status(HostError::kPacketMalformed));
            return;
          }

          BufferView handle_list = rsp.payload_data();

          att::Handle last_handle = att::kHandleMax;
          while (handle_list.size()) {
            const auto& entry = handle_list.As<att::HandlesInformationList>();

            ServiceData service;
            service.range_start = le16toh(entry.handle);
            service.range_end = le16toh(entry.group_end_handle);

            if (service.range_end < service.range_start) {
              bt_log(TRACE, "gatt", "received malformed service range values");
              res_cb(att::Status(HostError::kPacketMalformed));
              return;
            }

            service.type = uuid;

            // Notify the handler.
            svc_cb(service);

            // HandlesInformationList is a single element of the list.
            size_t entry_length = sizeof(att::HandlesInformationList);
            handle_list = handle_list.view(entry_length);

            last_handle = service.range_end;
          }

          // The procedure is over if we have reached the end of the handle range.
          if (last_handle == att::kHandleMax) {
            res_cb(att::Status());
            return;
          }

          // Request the next batch.
          DiscoverPrimaryServicesByUUIDInternal(last_handle + 1, att::kHandleMax, std::move(svc_cb),
                                                std::move(res_cb), uuid);
        });

    auto error_cb = BindErrorCallback(
        [res_cb = status_callback.share()](att::Status status, att::Handle handle) {
          // An Error Response code of "Attribute Not Found" indicates the end
          // of the procedure (v5.0, Vol 3, Part G, 4.4.2).
          if (status.is_protocol_error() &&
              status.protocol_error() == att::ErrorCode::kAttributeNotFound) {
            res_cb(att::Status());
            return;
          }

          res_cb(status);
        });

    att_->StartTransaction(std::move(pdu), std::move(rsp_cb), std::move(error_cb));
  }

  // TODO(49794): refactor to use ReadByTypeRequest()
  void DiscoverCharacteristics(att::Handle range_start, att::Handle range_end,
                               CharacteristicCallback chrc_callback,
                               StatusCallback status_callback) override {
    ZX_DEBUG_ASSERT(range_start <= range_end);
    ZX_DEBUG_ASSERT(chrc_callback);
    ZX_DEBUG_ASSERT(status_callback);

    if (range_start == range_end) {
      status_callback(att::Status());
      return;
    }

    auto pdu = NewPDU(sizeof(att::ReadByTypeRequestParams16));
    if (!pdu) {
      status_callback(att::Status(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kReadByTypeRequest, pdu.get());
    auto* params = writer.mutable_payload<att::ReadByTypeRequestParams16>();
    params->start_handle = htole16(range_start);
    params->end_handle = htole16(range_end);
    params->type = htole16(types::kCharacteristicDeclaration16);

    auto rsp_cb = BindCallback([this, range_start, range_end, chrc_cb = std::move(chrc_callback),
                                res_cb =
                                    status_callback.share()](const att::PacketReader& rsp) mutable {
      ZX_DEBUG_ASSERT(rsp.opcode() == att::kReadByTypeResponse);

      if (rsp.payload_size() < sizeof(att::ReadByTypeResponseParams)) {
        bt_log(TRACE, "gatt", "received malformed Read By Type response");
        att_->ShutDown();
        res_cb(att::Status(HostError::kPacketMalformed));
        return;
      }

      const auto& rsp_params = rsp.payload<att::ReadByTypeResponseParams>();
      uint8_t entry_length = rsp_params.length;

      // The characteristic declaration value contains:
      // 1 octet: properties
      // 2 octets: value handle
      // 2 or 16 octets: UUID
      constexpr size_t kCharacDeclSize16 =
          sizeof(Properties) + sizeof(att::Handle) + sizeof(att::AttributeType16);
      constexpr size_t kCharacDeclSize128 =
          sizeof(Properties) + sizeof(att::Handle) + sizeof(att::AttributeType128);

      constexpr size_t kAttributeDataSize16 = sizeof(att::AttributeData) + kCharacDeclSize16;
      constexpr size_t kAttributeDataSize128 = sizeof(att::AttributeData) + kCharacDeclSize128;

      if (entry_length != kAttributeDataSize16 && entry_length != kAttributeDataSize128) {
        bt_log(TRACE, "gatt", "invalid attribute data length");
        att_->ShutDown();
        res_cb(att::Status(HostError::kPacketMalformed));
        return;
      }

      BufferView attr_data_list(rsp_params.attribute_data_list, rsp.payload_size() - 1);
      if (attr_data_list.size() % entry_length) {
        bt_log(TRACE, "gatt", "malformed attribute data list");
        att_->ShutDown();
        res_cb(att::Status(HostError::kPacketMalformed));
        return;
      }

      att::Handle last_handle = range_end;
      while (attr_data_list.size()) {
        const auto& entry = attr_data_list.As<att::AttributeData>();
        BufferView value(entry.value, entry_length - sizeof(att::Handle));

        att::Handle chrc_handle = le16toh(entry.handle);
        Properties properties = value[0];
        att::Handle value_handle = le16toh(value.view(1, 2).As<att::Handle>());

        // Vol 3, Part G, 3.3: "The Characteristic Value declaration shall
        // exist immediately following the characteristic declaration."
        if (value_handle != chrc_handle + 1) {
          bt_log(TRACE, "gatt", "characteristic value doesn't follow decl");
          res_cb(att::Status(HostError::kPacketMalformed));
          return;
        }

        // Stop and report an error if the server erroneously responds with
        // an attribute outside the requested range.
        if (chrc_handle > range_end || chrc_handle < range_start) {
          bt_log(TRACE, "gatt",
                 "characteristic handle out of range (handle: %#.4x, "
                 "range: %#.4x - %#.4x)",
                 chrc_handle, range_start, range_end);
          res_cb(att::Status(HostError::kPacketMalformed));
          return;
        }

        // The handles must be strictly increasing. Check this so that a
        // server cannot fool us into sending requests forever.
        if (last_handle != range_end && chrc_handle <= last_handle) {
          bt_log(TRACE, "gatt", "handles are not strictly increasing");
          res_cb(att::Status(HostError::kPacketMalformed));
          return;
        }

        last_handle = chrc_handle;

        // This must succeed as we have performed the necessary checks
        // above.
        UUID type;
        __UNUSED bool result = UUID::FromBytes(value.view(3), &type);
        ZX_DEBUG_ASSERT(result);

        // Notify the handler. By default, there are no extended properties to report.
        chrc_cb(CharacteristicData(properties, /*extended_properties=*/std::nullopt, chrc_handle,
                                   value_handle, type));

        attr_data_list = attr_data_list.view(entry_length);
      }

      // The procedure is over if we have reached the end of the handle
      // range.
      if (last_handle == range_end) {
        res_cb(att::Status());
        return;
      }

      // Request the next batch.
      DiscoverCharacteristics(last_handle + 1, range_end, std::move(chrc_cb), std::move(res_cb));
    });

    auto error_cb = BindErrorCallback(
        [res_cb = status_callback.share()](att::Status status, att::Handle handle) {
          // An Error Response code of "Attribute Not Found" indicates the end
          // of the procedure (v5.0, Vol 3, Part G, 4.6.1).
          if (status.is_protocol_error() &&
              status.protocol_error() == att::ErrorCode::kAttributeNotFound) {
            res_cb(att::Status());
            return;
          }

          res_cb(status);
        });

    att_->StartTransaction(std::move(pdu), std::move(rsp_cb), std::move(error_cb));
  }

  void DiscoverDescriptors(att::Handle range_start, att::Handle range_end,
                           DescriptorCallback desc_callback,
                           StatusCallback status_callback) override {
    ZX_DEBUG_ASSERT(range_start <= range_end);
    ZX_DEBUG_ASSERT(desc_callback);
    ZX_DEBUG_ASSERT(status_callback);

    auto pdu = NewPDU(sizeof(att::FindInformationRequestParams));
    if (!pdu) {
      status_callback(att::Status(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kFindInformationRequest, pdu.get());
    auto* params = writer.mutable_payload<att::FindInformationRequestParams>();
    params->start_handle = htole16(range_start);
    params->end_handle = htole16(range_end);

    auto rsp_cb =
        BindCallback([this, range_start, range_end, desc_cb = std::move(desc_callback),
                      res_cb = status_callback.share()](const att::PacketReader& rsp) mutable {
          ZX_DEBUG_ASSERT(rsp.opcode() == att::kFindInformationResponse);

          if (rsp.payload_size() < sizeof(att::FindInformationResponseParams)) {
            bt_log(TRACE, "gatt", "received malformed Find Information response");
            att_->ShutDown();
            res_cb(att::Status(HostError::kPacketMalformed));
            return;
          }

          const auto& rsp_params = rsp.payload<att::FindInformationResponseParams>();
          BufferView entries = rsp.payload_data().view(sizeof(rsp_params.format));

          att::Handle last_handle;
          bool result;
          switch (rsp_params.format) {
            case att::UUIDType::k16Bit:
              result = ProcessDescriptorDiscoveryResponse<att::UUIDType::k16Bit>(
                  range_start, range_end, entries, desc_cb.share(), &last_handle);
              break;
            case att::UUIDType::k128Bit:
              result = ProcessDescriptorDiscoveryResponse<att::UUIDType::k128Bit>(
                  range_start, range_end, entries, desc_cb.share(), &last_handle);
              break;
            default:
              bt_log(TRACE, "gatt", "invalid information data format");
              result = false;
              break;
          }

          if (!result) {
            att_->ShutDown();
            res_cb(att::Status(HostError::kPacketMalformed));
            return;
          }

          // The procedure is over if we have reached the end of the handle range.
          if (last_handle == range_end) {
            res_cb(att::Status());
            return;
          }

          // Request the next batch.
          DiscoverDescriptors(last_handle + 1, range_end, std::move(desc_cb), std::move(res_cb));
        });

    auto error_cb = BindErrorCallback(
        [res_cb = status_callback.share()](att::Status status, att::Handle handle) {
          // An Error Response code of "Attribute Not Found" indicates the end
          // of the procedure (v5.0, Vol 3, Part G, 4.7.1).
          if (status.is_protocol_error() &&
              status.protocol_error() == att::ErrorCode::kAttributeNotFound) {
            res_cb(att::Status());
            return;
          }

          res_cb(status);
        });

    att_->StartTransaction(std::move(pdu), std::move(rsp_cb), std::move(error_cb));
  }

  void ReadRequest(att::Handle handle, ReadCallback callback) override {
    auto pdu = NewPDU(sizeof(att::ReadRequestParams));
    if (!pdu) {
      callback(att::Status(HostError::kOutOfMemory), BufferView());
      return;
    }

    att::PacketWriter writer(att::kReadRequest, pdu.get());
    auto params = writer.mutable_payload<att::ReadRequestParams>();
    params->handle = htole16(handle);

    auto rsp_cb = BindCallback([callback = callback.share()](const att::PacketReader& rsp) {
      ZX_DEBUG_ASSERT(rsp.opcode() == att::kReadResponse);
      callback(att::Status(), rsp.payload_data());
    });

    auto error_cb =
        BindErrorCallback([callback = callback.share()](att::Status status, att::Handle handle) {
          bt_log(TRACE, "gatt", "read request failed: %s, handle %#.4x", status.ToString().c_str(),
                 handle);
          callback(status, BufferView());
        });

    if (!att_->StartTransaction(std::move(pdu), std::move(rsp_cb), std::move(error_cb))) {
      callback(att::Status(HostError::kPacketMalformed), BufferView());
    }
  }

  void ReadByTypeRequest(const UUID& type, att::Handle start_handle, att::Handle end_handle,
                         ReadByTypeCallback callback) override {
    size_t type_size = type.CompactSize(/*allow_32bit=*/false);
    ZX_ASSERT(type_size == sizeof(uint16_t) || type_size == sizeof(UInt128));
    auto pdu = NewPDU(type_size == sizeof(uint16_t) ? sizeof(att::ReadByTypeRequestParams16)
                                                    : sizeof(att::ReadByTypeRequestParams128));
    if (!pdu) {
      callback(fit::error(ReadByTypeError{att::Status(HostError::kOutOfMemory), std::nullopt}));
      return;
    }

    att::PacketWriter writer(att::kReadByTypeRequest, pdu.get());
    if (type_size == sizeof(uint16_t)) {
      auto params = writer.mutable_payload<att::ReadByTypeRequestParams16>();
      params->start_handle = htole16(start_handle);
      params->end_handle = htole16(end_handle);
      auto type_view = MutableBufferView(&params->type, sizeof(params->type));
      type.ToBytes(&type_view, /*allow_32bit=*/false);
    } else {
      auto params = writer.mutable_payload<att::ReadByTypeRequestParams128>();
      params->start_handle = htole16(start_handle);
      params->end_handle = htole16(end_handle);
      auto type_view = MutableBufferView(&params->type, sizeof(params->type));
      type.ToBytes(&type_view, /*allow_32bit=*/false);
    }

    auto rsp_cb = BindCallback([callback = callback.share(), start_handle,
                                end_handle](const att::PacketReader& rsp) {
      ZX_ASSERT(rsp.opcode() == att::kReadByTypeResponse);
      if (rsp.payload_size() < sizeof(att::ReadByTypeResponseParams)) {
        callback(
            fit::error(ReadByTypeError{att::Status(HostError::kPacketMalformed), std::nullopt}));
        return;
      }

      const auto& params = rsp.payload<att::ReadByTypeResponseParams>();
      // The response contains a list of attribute handle-value pairs of uniform length.
      const size_t list_size = rsp.payload_size() - sizeof(params.length);
      const size_t pair_size = params.length;

      // Success response must:
      // a) Specify valid pair length (at least the size of a handle).
      // b) Have at least 1 pair (otherwise the Attribute Not Found error should have been
      //    sent).
      // c) Have a list size that is evenly divisible by pair size.
      if (pair_size < sizeof(att::Handle) || list_size < sizeof(att::Handle) ||
          list_size % pair_size != 0) {
        callback(
            fit::error(ReadByTypeError{att::Status(HostError::kPacketMalformed), std::nullopt}));
        return;
      }

      std::vector<ReadByTypeValue> attributes;
      BufferView attr_list_view(params.attribute_data_list,
                                rsp.payload_size() - sizeof(params.length));
      while (attr_list_view.size() >= params.length) {
        const BufferView pair_view = attr_list_view.view(0, pair_size);
        const att::Handle handle = letoh16(pair_view.As<att::Handle>());

        if (handle < start_handle || handle > end_handle) {
          bt_log(SPEW, "gatt",
                 "client received read by type response with handle outside of requested range");
          callback(
              fit::error(ReadByTypeError{att::Status(HostError::kPacketMalformed), std::nullopt}));
          return;
        }

        if (!attributes.empty() && attributes.back().handle >= handle) {
          bt_log(SPEW, "gatt",
                 "client received read by type response with handles in non-increasing order");
          callback(
              fit::error(ReadByTypeError{att::Status(HostError::kPacketMalformed), std::nullopt}));
          return;
        }

        auto value_view = pair_view.view(sizeof(att::Handle));
        attributes.push_back(ReadByTypeValue{handle, value_view});

        // Advance list view to next pair (or end of list).
        attr_list_view = attr_list_view.view(pair_size);
      }
      ZX_ASSERT(attr_list_view.size() == 0);

      callback(fit::ok(std::move(attributes)));
    });

    auto error_cb =
        BindErrorCallback([callback = callback.share()](att::Status status, att::Handle handle) {
          bt_log(TRACE, "gatt", "read by type request failed: %s, handle %#.4x", bt_str(status),
                 handle);
          // Only some errors have handles.
          std::optional<att::Handle> cb_handle = handle ? std::optional(handle) : std::nullopt;
          callback(fit::error(ReadByTypeError{status, cb_handle}));
        });

    if (!att_->StartTransaction(std::move(pdu), std::move(rsp_cb), std::move(error_cb))) {
      callback(fit::error(ReadByTypeError{att::Status(HostError::kPacketMalformed), std::nullopt}));
    }
  }

  void ReadBlobRequest(att::Handle handle, uint16_t offset, ReadCallback callback) override {
    auto pdu = NewPDU(sizeof(att::ReadBlobRequestParams));
    if (!pdu) {
      callback(att::Status(HostError::kOutOfMemory), BufferView());
      return;
    }

    att::PacketWriter writer(att::kReadBlobRequest, pdu.get());
    auto params = writer.mutable_payload<att::ReadBlobRequestParams>();
    params->handle = htole16(handle);
    params->offset = htole16(offset);

    auto rsp_cb = BindCallback([callback = callback.share()](const att::PacketReader& rsp) {
      ZX_DEBUG_ASSERT(rsp.opcode() == att::kReadBlobResponse);
      callback(att::Status(), rsp.payload_data());
    });

    auto error_cb =
        BindErrorCallback([callback = callback.share()](att::Status status, att::Handle handle) {
          bt_log(TRACE, "gatt", "read blob request failed: %s, handle: %#.4x",
                 status.ToString().c_str(), handle);
          callback(status, BufferView());
        });

    if (!att_->StartTransaction(std::move(pdu), std::move(rsp_cb), std::move(error_cb))) {
      callback(att::Status(HostError::kPacketMalformed), BufferView());
    }
  }

  void WriteRequest(att::Handle handle, const ByteBuffer& value, StatusCallback callback) override {
    const size_t payload_size = sizeof(att::WriteRequestParams) + value.size();
    if (sizeof(att::OpCode) + payload_size > att_->mtu()) {
      bt_log(SPEW, "gatt", "write request payload exceeds MTU");
      callback(att::Status(HostError::kPacketMalformed));
      return;
    }

    auto pdu = NewPDU(payload_size);
    if (!pdu) {
      callback(att::Status(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kWriteRequest, pdu.get());
    auto params = writer.mutable_payload<att::WriteRequestParams>();
    params->handle = htole16(handle);

    auto value_view = writer.mutable_payload_data().mutable_view(sizeof(att::Handle));
    value.Copy(&value_view);

    auto rsp_cb = BindCallback([this, callback = callback.share()](const att::PacketReader& rsp) {
      ZX_DEBUG_ASSERT(rsp.opcode() == att::kWriteResponse);

      if (rsp.payload_size()) {
        att_->ShutDown();
        callback(att::Status(HostError::kPacketMalformed));
        return;
      }

      callback(att::Status());
    });

    auto error_cb =
        BindErrorCallback([callback = callback.share()](att::Status status, att::Handle handle) {
          bt_log(TRACE, "gatt", "write request failed: %s, handle: %#.2x",
                 status.ToString().c_str(), handle);
          callback(status);
        });

    if (!att_->StartTransaction(std::move(pdu), std::move(rsp_cb), std::move(error_cb))) {
      callback(att::Status(HostError::kPacketMalformed));
    }
  }

  // An internal object for storing the write queue, callback, and reliability mode
  // of a long write operation.
  struct PreparedWrite {
    bt::att::PrepareWriteQueue prep_write_queue;
    bt::att::StatusCallback callback;
    ReliableMode reliable_mode;
  };

  void ExecutePrepareWrites(att::PrepareWriteQueue prep_write_queue, ReliableMode reliable_mode,
                            att::StatusCallback callback) override {
    PreparedWrite new_request;
    new_request.prep_write_queue = std::move(prep_write_queue);
    new_request.callback = std::move(callback);
    new_request.reliable_mode = std::move(reliable_mode);
    long_write_queue_.push(std::move(new_request));

    // If the |long_write_queue| has a pending request, then appending this
    // request will be sufficient, otherwise kick off the request.
    if (long_write_queue_.size() == 1) {
      ProcessWriteQueue(std::move(long_write_queue_.front()));
    }
  }

  void ProcessWriteQueue(PreparedWrite prep_write) {
    if (!prep_write.prep_write_queue.empty()) {
      att::QueuedWrite prep_write_request = std::move(prep_write.prep_write_queue.front());
      // A copy of the |prep_write_request| is made to pass into the capture
      // list for |prep_write_cb|. It will be used to validate the echoed blob.
      auto prep_write_copy = att::QueuedWrite(
          prep_write_request.handle(), prep_write_request.offset(), prep_write_request.value());
      prep_write.prep_write_queue.pop();

      auto prep_write_cb = [this, prep_write = std::move(prep_write),
                            requested_blob = std::move(prep_write_copy)](
                               att::Status status, const ByteBuffer& blob) mutable {
        // If the write fails, cancel the prep writes and then move on to the next
        // long write in the queue.
        // The device will echo the value written in the blob, according to the
        // spec (Vol 3, Part G, 4.9.4). The offset and value will be verified if the
        // requested |prep_write.mode| is enabled (Vol 3, Part G, 4.9.5).

        if (prep_write.reliable_mode == ReliableMode::kEnabled) {
          if (blob.size() < sizeof(att::PrepareWriteResponseParams)) {
            // The response blob is malformed.
            status = att::Status(HostError::kNotReliable);
          } else {
            auto blob_offset = le16toh(blob.As<att::PrepareWriteResponseParams>().offset);
            auto blob_value = blob.view(sizeof(att::PrepareWriteResponseParams));
            if ((blob_offset != requested_blob.offset()) ||
                !(blob_value == requested_blob.value())) {
              status = att::Status(HostError::kNotReliable);
            }
          }
        }

        if (!status) {
          auto exec_write_cb = [this, callback = std::move(prep_write.callback),
                                prep_write_status = status](att::Status status) mutable {
            // In this case return the original failure status. This effectively
            // overrides the ExecuteWrite status.
            callback(prep_write_status);
            // Now that this request is complete, remove it from the overall
            // queue.
            ZX_DEBUG_ASSERT(!long_write_queue_.empty());
            long_write_queue_.pop();

            if (long_write_queue_.size() > 0) {
              ProcessWriteQueue(std::move(long_write_queue_.front()));
            }
          };

          ExecuteWriteRequest(att::ExecuteWriteFlag::kCancelAll, std::move(exec_write_cb));

          return;
        }

        ProcessWriteQueue(std::move(prep_write));
      };

      PrepareWriteRequest(prep_write_request.handle(), prep_write_request.offset(),
                          std::move(prep_write_request.value()), std::move(prep_write_cb));
    }
    // End of this write, send and prepare for next item in overall write queue
    else {
      auto exec_write_cb = [this,
                            callback = std::move(prep_write.callback)](att::Status status) mutable {
        callback(status);
        // Now that this request is complete, remove it from the overall
        // queue.
        ZX_DEBUG_ASSERT(!long_write_queue_.empty());
        long_write_queue_.pop();

        // If the super queue still has any long writes left to execute,
        // initiate them
        if (long_write_queue_.size() > 0) {
          ProcessWriteQueue(std::move(long_write_queue_.front()));
        }
      };

      ExecuteWriteRequest(att::ExecuteWriteFlag::kWritePending, std::move(exec_write_cb));
    }
  }

  void PrepareWriteRequest(att::Handle handle, uint16_t offset, const ByteBuffer& part_value,
                           PrepareCallback callback) override {
    const size_t payload_size = sizeof(att::PrepareWriteRequestParams) + part_value.size();
    if (sizeof(att::OpCode) + payload_size > att_->mtu()) {
      bt_log(SPEW, "gatt", "prepare write request payload exceeds MTU");
      callback(att::Status(HostError::kPacketMalformed), BufferView());
      return;
    }

    auto pdu = NewPDU(payload_size);
    if (!pdu) {
      callback(att::Status(HostError::kOutOfMemory), BufferView());
      return;
    }

    att::PacketWriter writer(att::kPrepareWriteRequest, pdu.get());
    auto params = writer.mutable_payload<att::PrepareWriteRequestParams>();
    params->handle = htole16(handle);
    params->offset = htole16(offset);

    auto header_size = sizeof(att::Handle) + sizeof(uint16_t);
    auto value_view = writer.mutable_payload_data().mutable_view(header_size);
    part_value.Copy(&value_view);

    auto rsp_cb = BindCallback([callback = callback.share()](const att::PacketReader& rsp) {
      ZX_DEBUG_ASSERT(rsp.opcode() == att::kPrepareWriteResponse);
      callback(att::Status(), rsp.payload_data());
    });

    auto error_cb =
        BindErrorCallback([callback = callback.share()](att::Status status, att::Handle handle) {
          bt_log(TRACE, "gatt",
                 "prepare write request failed: %s, handle:"
                 "%#.4x",
                 status.ToString().c_str(), handle);
          callback(status, BufferView());
        });

    if (!att_->StartTransaction(std::move(pdu), std::move(rsp_cb), std::move(error_cb))) {
      callback(att::Status(HostError::kPacketMalformed), BufferView());
    }
  }

  void ExecuteWriteRequest(att::ExecuteWriteFlag flag, att::StatusCallback callback) override {
    const size_t payload_size = sizeof(att::ExecuteWriteRequestParams);
    if (sizeof(att::OpCode) + payload_size > att_->mtu()) {
      // This really shouldn't happen because we aren't consuming any actual
      // payload here, but just in case...
      bt_log(SPEW, "gatt", "execute write request size exceeds MTU");
      callback(att::Status(HostError::kPacketMalformed));
      return;
    }

    auto pdu = NewPDU(payload_size);
    if (!pdu) {
      callback(att::Status(HostError::kOutOfMemory));
      return;
    }

    att::PacketWriter writer(att::kExecuteWriteRequest, pdu.get());
    auto params = writer.mutable_payload<att::ExecuteWriteRequestParams>();
    params->flags = flag;

    auto rsp_cb = BindCallback([this, callback = callback.share()](const att::PacketReader& rsp) {
      ZX_DEBUG_ASSERT(rsp.opcode() == att::kExecuteWriteResponse);

      if (rsp.payload_size()) {
        att_->ShutDown();
        callback(att::Status(HostError::kPacketMalformed));
        return;
      }

      callback(att::Status());
    });

    auto error_cb =
        BindErrorCallback([callback = callback.share()](att::Status status, att::Handle handle) {
          bt_log(TRACE, "gatt", "execute write request failed: %s", status.ToString().c_str());
          callback(status);
        });

    if (!att_->StartTransaction(std::move(pdu), std::move(rsp_cb), std::move(error_cb))) {
      callback(att::Status(HostError::kPacketMalformed));
    }
  }

  void WriteWithoutResponse(att::Handle handle, const ByteBuffer& value) override {
    const size_t payload_size = sizeof(att::WriteRequestParams) + value.size();
    if (sizeof(att::OpCode) + payload_size > att_->mtu()) {
      bt_log(SPEW, "gatt", "write request payload exceeds MTU");
      return;
    }

    auto pdu = NewPDU(payload_size);
    if (!pdu) {
      return;
    }

    att::PacketWriter writer(att::kWriteCommand, pdu.get());
    auto params = writer.mutable_payload<att::WriteRequestParams>();
    params->handle = htole16(handle);

    auto value_view = writer.mutable_payload_data().mutable_view(sizeof(att::Handle));
    value.Copy(&value_view);

    att_->SendWithoutResponse(std::move(pdu));
  }

  void SetNotificationHandler(NotificationCallback handler) override {
    notification_handler_ = std::move(handler);
  }

  // Wraps |callback| in a TransactionCallback that only runs if this Client is
  // still alive.
  att::Bearer::TransactionCallback BindCallback(att::Bearer::TransactionCallback callback) {
    return
        [self = weak_ptr_factory_.GetWeakPtr(), callback = std::move(callback)](const auto& rsp) {
          if (self) {
            callback(rsp);
          }
        };
  }

  // Wraps |callback| in a ErrorCallback that only runs if this Client is still
  // alive.
  att::Bearer::ErrorCallback BindErrorCallback(att::Bearer::ErrorCallback callback) {
    return [self = weak_ptr_factory_.GetWeakPtr(), callback = std::move(callback)](
               att::Status status, att::Handle handle) {
      if (self) {
        callback(status, handle);
      }
    };
  }

  fxl::RefPtr<att::Bearer> att_;
  att::Bearer::HandlerId not_handler_id_;
  att::Bearer::HandlerId ind_handler_id_;

  NotificationCallback notification_handler_;
  // |long_write_queue_| contains long write requests, their
  // associated callbacks and reliable write modes.
  // Series of PrepareWrites are executed or cancelled at the same time so
  // this is used to block while a single series is processed.
  //
  // While the top element is processed, the |PrepareWriteQueue| and callback
  // will be empty and will be popped once the queue is cancelled or executed.
  // Following the processing of each queue, the client will automatically
  // process the next queue in the |long_write_queue_|.
  std::queue<PreparedWrite> long_write_queue_;
  fxl::WeakPtrFactory<Client> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Impl);
};

// static
std::unique_ptr<Client> Client::Create(fxl::RefPtr<att::Bearer> bearer) {
  return std::make_unique<Impl>(std::move(bearer));
}

}  // namespace gatt
}  // namespace bt
