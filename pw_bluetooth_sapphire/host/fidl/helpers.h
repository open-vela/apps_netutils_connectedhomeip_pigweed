// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_FIDL_HELPERS_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_FIDL_HELPERS_H_

#include <fuchsia/bluetooth/cpp/fidl.h>
#include <fuchsia/bluetooth/gatt/cpp/fidl.h>
#include <fuchsia/bluetooth/host/cpp/fidl.h>
#include <fuchsia/bluetooth/le/cpp/fidl.h>

#include <optional>

#include "fuchsia/bluetooth/sys/cpp/fidl.h"
#include "lib/fidl/cpp/type_converter.h"
#include "lib/fidl/cpp/vector.h"
#include "lib/fit/result.h"
#include "src/connectivity/bluetooth/core/bt-host/common/advertising_data.h"
#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/identifier.h"
#include "src/connectivity/bluetooth/core/bt-host/common/status.h"
#include "src/connectivity/bluetooth/core/bt-host/common/uuid.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/adapter.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/gap.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/low_energy_advertising_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/peer.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/types.h"

// Helpers for implementing the Bluetooth FIDL interfaces.

namespace bt::gap {

class DiscoveryFilter;

}  // namespace bt::gap

namespace bthost::fidl_helpers {

// TODO(fxbug.dev/898): Temporary logic for converting between the stack identifier
// type (integer) and FIDL identifier type (string). Remove these once all FIDL
// interfaces have been converted to use integer IDs.
std::optional<bt::PeerId> PeerIdFromString(const std::string& id);

// Convert a string of the form "XX:XX:XX:XX:XX" to the DeviceAddressBytes it represents.
// returns nullopt when the conversion fails (due to wrong format)
std::optional<bt::DeviceAddressBytes> AddressBytesFromString(const std::string& addr);

// Functions for generating a FIDL bluetooth::Status

fuchsia::bluetooth::ErrorCode HostErrorToFidlDeprecated(bt::HostError host_error);

fuchsia::bluetooth::Status NewFidlError(fuchsia::bluetooth::ErrorCode error_code,
                                        std::string description);

template <typename ProtocolErrorCode>
fuchsia::bluetooth::Status StatusToFidlDeprecated(const bt::Status<ProtocolErrorCode>& status,
                                                  std::string msg = "") {
  fuchsia::bluetooth::Status fidl_status;
  if (status.is_success()) {
    return fidl_status;
  }

  auto error = fuchsia::bluetooth::Error::New();
  error->error_code = HostErrorToFidlDeprecated(status.error());
  error->description = msg.empty() ? status.ToString() : std::move(msg);
  if (status.is_protocol_error()) {
    error->protocol_error_code = static_cast<uint32_t>(status.protocol_error());
  }

  fidl_status.error = std::move(error);
  return fidl_status;
}

// Convert a bt::HostError to fuchsia.bluetooth.sys.Error. This function does only
// deals with bt::HostError types and does not support Bluetooth protocol-specific errors; to
// represent such errors use protocol-specific FIDL error types. An |error| value of
// HostError::kNoError is not allowed.
fuchsia::bluetooth::sys::Error HostErrorToFidl(bt::HostError error);

// Convert any bt::Status to a fit::result that uses the fuchsia.bluetooth.sys library error codes.
template <typename ProtocolErrorCode>
fit::result<void, fuchsia::bluetooth::sys::Error> StatusToFidl(
    bt::Status<ProtocolErrorCode> status) {
  if (status) {
    return fit::ok();
  } else {
    return fit::error(HostErrorToFidl(status.error()));
  }
}

// Convert a bt::Status to fuchsia.bluetooth.gatt.Error. |status| must not indicate success.
fuchsia::bluetooth::gatt::Error GattStatusToFidl(bt::Status<bt::att::ErrorCode> status);

bt::UUID UuidFromFidl(const fuchsia::bluetooth::Uuid& input);
fuchsia::bluetooth::Uuid UuidToFidl(const bt::UUID& uuid);

// Functions that convert FIDL types to library objects.
bt::sm::IOCapability IoCapabilityFromFidl(const fuchsia::bluetooth::sys::InputCapability,
                                          const fuchsia::bluetooth::sys::OutputCapability);
bt::gap::LeSecurityMode LeSecurityModeFromFidl(const fuchsia::bluetooth::sys::LeSecurityMode mode);
std::optional<bt::sm::SecurityLevel> SecurityLevelFromFidl(
    const fuchsia::bluetooth::sys::PairingSecurityLevel level);

// fuchsia.bluetooth.sys library helpers.
fuchsia::bluetooth::sys::TechnologyType TechnologyTypeToFidl(bt::gap::TechnologyType type);
fuchsia::bluetooth::sys::HostInfo HostInfoToFidl(const bt::gap::Adapter& adapter);
fuchsia::bluetooth::sys::Peer PeerToFidl(const bt::gap::Peer& peer);

// Functions to convert bonding data structures from FIDL.
std::optional<bt::DeviceAddress> AddressFromFidlBondingData(
    const fuchsia::bluetooth::sys::BondingData& data);
bt::sm::PairingData LePairingDataFromFidl(const fuchsia::bluetooth::sys::LeData& data);
std::optional<bt::sm::LTK> BredrKeyFromFidl(const fuchsia::bluetooth::sys::BredrData& data);
std::vector<bt::UUID> BredrServicesFromFidl(const fuchsia::bluetooth::sys::BredrData& data);

// Function to construct a bonding data structure for a peer.
fuchsia::bluetooth::sys::BondingData PeerToFidlBondingData(const bt::gap::Adapter& adapter,
                                                           const bt::gap::Peer& peer);

// Functions to construct FIDL LE library objects from library objects.
fuchsia::bluetooth::le::RemoteDevicePtr NewLERemoteDevice(const bt::gap::Peer& peer);

// Validates the contents of a ScanFilter.
bool IsScanFilterValid(const fuchsia::bluetooth::le::ScanFilter& fidl_filter);

// Populates a library DiscoveryFilter based on a FIDL ScanFilter. Returns false
// if |fidl_filter| contains any malformed data and leaves |out_filter|
// unmodified.
bool PopulateDiscoveryFilter(const fuchsia::bluetooth::le::ScanFilter& fidl_filter,
                             bt::gap::DiscoveryFilter* out_filter);

// Converts the given |mode_hint| to a stack interval value.
bt::gap::AdvertisingInterval AdvertisingIntervalFromFidl(
    fuchsia::bluetooth::le::AdvertisingModeHint mode_hint);

bt::AdvertisingData AdvertisingDataFromFidl(const fuchsia::bluetooth::le::AdvertisingData& input);
fuchsia::bluetooth::le::AdvertisingData AdvertisingDataToFidl(const bt::AdvertisingData& input);
fuchsia::bluetooth::le::AdvertisingDataDeprecated AdvertisingDataToFidlDeprecated(
    const bt::AdvertisingData& input);

// Constructs a fuchsia.bluetooth.le Peer type from the stack representation.
fuchsia::bluetooth::le::Peer PeerToFidlLe(const bt::gap::Peer& peer);

// Functions that convert FIDL GATT types to library objects.
bt::gatt::ReliableMode ReliableModeFromFidl(
    const fuchsia::bluetooth::gatt::WriteOptions& write_options);

// Constructs a sdp::ServiceRecord from a FIDL ServiceDefinition |definition|
fit::result<bt::sdp::ServiceRecord, fuchsia::bluetooth::ErrorCode> ServiceDefinitionToServiceRecord(
    const fuchsia::bluetooth::bredr::ServiceDefinition& definition);

bt::gap::BrEdrSecurityRequirements FidlToBrEdrSecurityRequirements(
    const fuchsia::bluetooth::bredr::ChannelParameters& fidl);

}  // namespace bthost::fidl_helpers

// fidl::TypeConverter specializations for ByteBuffer and friends.
template <>
struct fidl::TypeConverter<std::vector<uint8_t>, bt::ByteBuffer> {
  static std::vector<uint8_t> Convert(const bt::ByteBuffer& from);
};

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_FIDL_HELPERS_H_
