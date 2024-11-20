// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "control_packets.h"

#include <zircon/assert.h>

#include "slab_allocators.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/error.h"

namespace bt::hci {
namespace slab_allocators {

// Slab-allocator traits for command packets.
using LargeCommandTraits =
    PacketTraits<hci_spec::CommandHeader, kLargeControlPacketSize, kNumLargeControlPackets>;
using SmallCommandTraits =
    PacketTraits<hci_spec::CommandHeader, kSmallControlPacketSize, kNumSmallControlPackets>;

// Slab-allocator traits for event packets. Since event packets are only
// received (and not sent) and because the packet size cannot be determined
// before the contents are read from the underlying channel, CommandChannel
// always allocates the largest possible buffer for events. Thus, a small buffer
// allocator is not needed.
using EventTraits =
    PacketTraits<hci_spec::EventHeader, kLargeControlPacketSize, kNumLargeControlPackets>;

using LargeCommandAllocator = fbl::SlabAllocator<LargeCommandTraits>;
using SmallCommandAllocator = fbl::SlabAllocator<SmallCommandTraits>;
using EventAllocator = fbl::SlabAllocator<EventTraits>;

}  // namespace slab_allocators

namespace {

std::unique_ptr<CommandPacket> NewCommandPacket(size_t payload_size) {
  ZX_DEBUG_ASSERT(payload_size <= slab_allocators::kLargeControlPayloadSize);

  if (payload_size <= slab_allocators::kSmallControlPayloadSize) {
    auto buffer = slab_allocators::SmallCommandAllocator::New(payload_size);
    if (buffer)
      return buffer;

    // We failed to allocate a small buffer; fall back to the large allocator.
  }

  return slab_allocators::LargeCommandAllocator::New(payload_size);
}

// Returns true and populates the |out_code| field with the status parameter.
// Returns false if |event|'s payload is too small to hold a T. T must have a
// |status| member of type hci_spec::StatusCode for this to compile.
template <typename T>
bool StatusCodeFromEvent(const EventPacket& event, hci_spec::StatusCode* out_code) {
  ZX_DEBUG_ASSERT(out_code);

  if (event.view().payload_size() < sizeof(T))
    return false;

  *out_code = event.params<T>().status;
  return true;
}

// As hci_spec::StatusCodeFromEvent, but for LEMetaEvent subevents.
// Returns true and populates the |out_code| field with the subevent status parameter.
// Returns false if |event|'s payload is too small to hold a LEMetaEvent containing a T. T must have
// a |status| member of type hci_spec::StatusCode for this to compile.
template <typename T>
bool StatusCodeFromSubevent(const EventPacket& event, hci_spec::StatusCode* out_code) {
  ZX_ASSERT(out_code);

  if (event.view().payload_size() < sizeof(hci_spec::LEMetaEventParams) + sizeof(T))
    return false;

  *out_code = event.subevent_params<T>()->status;
  return true;
}

// Specialization for the CommandComplete event.
template <>
bool StatusCodeFromEvent<hci_spec::CommandCompleteEventParams>(const EventPacket& event,
                                                               hci_spec::StatusCode* out_code) {
  ZX_DEBUG_ASSERT(out_code);

  const auto* params = event.return_params<hci_spec::SimpleReturnParams>();
  if (!params)
    return false;

  *out_code = params->status;
  return true;
}

}  // namespace

namespace hci_android = bt::hci_spec::vendor::android;

// static
std::unique_ptr<CommandPacket> CommandPacket::New(hci_spec::OpCode opcode, size_t payload_size) {
  auto packet = NewCommandPacket(payload_size);
  if (!packet)
    return nullptr;

  packet->WriteHeader(opcode);
  return packet;
}

void CommandPacket::WriteHeader(hci_spec::OpCode opcode) {
  mutable_view()->mutable_header()->opcode = htole16(opcode);
  ZX_ASSERT(view().payload_size() < std::numeric_limits<uint8_t>::max());
  mutable_view()->mutable_header()->parameter_total_size =
      static_cast<uint8_t>(view().payload_size());
}

// static
std::unique_ptr<EventPacket> EventPacket::New(size_t payload_size) {
  return slab_allocators::EventAllocator::New(payload_size);
}

bool EventPacket::ToStatusCode(hci_spec::StatusCode* out_code) const {
#define CASE_EVENT_STATUS(event_name)      \
  case hci_spec::k##event_name##EventCode: \
    return StatusCodeFromEvent<hci_spec::event_name##EventParams>(*this, out_code)

#define CASE_SUBEVENT_STATUS(subevent_name)      \
  case hci_spec::k##subevent_name##SubeventCode: \
    return StatusCodeFromSubevent<hci_spec::subevent_name##SubeventParams>(*this, out_code)

#define CASE_ANDROID_SUBEVENT_STATUS(subevent_name) \
  case hci_android::k##subevent_name##SubeventCode: \
    return StatusCodeFromSubevent<hci_android::subevent_name##SubeventParams>(*this, out_code)

  switch (event_code()) {
    CASE_EVENT_STATUS(AuthenticationComplete);
    CASE_EVENT_STATUS(ChangeConnectionLinkKeyComplete);
    CASE_EVENT_STATUS(CommandComplete);
    CASE_EVENT_STATUS(CommandStatus);
    CASE_EVENT_STATUS(ConnectionComplete);
    CASE_EVENT_STATUS(DisconnectionComplete);
    CASE_EVENT_STATUS(InquiryComplete);
    CASE_EVENT_STATUS(EncryptionChange);
    CASE_EVENT_STATUS(EncryptionKeyRefreshComplete);
    CASE_EVENT_STATUS(RemoteNameRequestComplete);
    CASE_EVENT_STATUS(ReadRemoteVersionInfoComplete);
    CASE_EVENT_STATUS(ReadRemoteSupportedFeaturesComplete);
    CASE_EVENT_STATUS(ReadRemoteExtendedFeaturesComplete);
    CASE_EVENT_STATUS(RoleChange);
    CASE_EVENT_STATUS(SimplePairingComplete);
    CASE_EVENT_STATUS(SynchronousConnectionComplete);
    case hci_spec::kLEMetaEventCode: {
      auto subevent_code = params<hci_spec::LEMetaEventParams>().subevent_code;
      switch (subevent_code) {
        CASE_SUBEVENT_STATUS(LEAdvertisingSetTerminated);
        CASE_SUBEVENT_STATUS(LEConnectionComplete);
        CASE_SUBEVENT_STATUS(LEReadRemoteFeaturesComplete);
        default:
          ZX_PANIC("LE subevent (%#.2x) not implemented!", subevent_code);
          break;
      }
    }
    case hci_spec::kVendorDebugEventCode: {
      auto subevent_code = params<hci_spec::VendorEventParams>().subevent_code;
      switch (subevent_code) {
        CASE_ANDROID_SUBEVENT_STATUS(LEMultiAdvtStateChange);
        default:
          ZX_PANIC("Vendor subevent (%#.2x) not implemented!", subevent_code);
          break;
      }
    }

      // TODO(armansito): Complete this list.

    default:
      ZX_PANIC("event (%#.2x) not implemented!", event_code());
      break;
  }
  return false;

#undef CASE_EVENT_STATUS
}

hci::Result<> EventPacket::ToResult() const {
  hci_spec::StatusCode code;
  if (!ToStatusCode(&code)) {
    return bt::ToResult(HostError::kPacketMalformed);
  }
  return bt::ToResult(code);
}

void EventPacket::InitializeFromBuffer() {
  mutable_view()->Resize(view().header().parameter_total_size);
}

}  // namespace bt::hci

DECLARE_STATIC_SLAB_ALLOCATOR_STORAGE(bt::hci::slab_allocators::LargeCommandTraits,
                                      bt::hci::slab_allocators::kMaxNumSlabs, true);
DECLARE_STATIC_SLAB_ALLOCATOR_STORAGE(bt::hci::slab_allocators::SmallCommandTraits,
                                      bt::hci::slab_allocators::kMaxNumSlabs, true);
DECLARE_STATIC_SLAB_ALLOCATOR_STORAGE(bt::hci::slab_allocators::EventTraits,
                                      bt::hci::slab_allocators::kMaxNumSlabs, true);
