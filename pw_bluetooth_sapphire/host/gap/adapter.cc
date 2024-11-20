// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "adapter.h"

#include <endian.h>

#include "bredr_connection_manager.h"
#include "bredr_discovery_manager.h"
#include "low_energy_address_manager.h"
#include "low_energy_advertising_manager.h"
#include "low_energy_connection_manager.h"
#include "low_energy_discovery_manager.h"
#include "peer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/random.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/connection.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/legacy_low_energy_advertiser.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/legacy_low_energy_scanner.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/low_energy_connector.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/sequential_command_runner.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/transport.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/util.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/channel_manager.h"

namespace bt {
namespace gap {

Adapter::Adapter(fxl::RefPtr<hci::Transport> hci, fbl::RefPtr<gatt::GATT> gatt,
                 std::optional<fbl::RefPtr<data::Domain>> data_domain)
    : identifier_(Random<AdapterId>()),
      dispatcher_(async_get_default_dispatcher()),
      hci_(hci),
      init_state_(State::kNotInitialized),
      max_lmp_feature_page_index_(0),
      gatt_(gatt),
      weak_ptr_factory_(this) {
  ZX_DEBUG_ASSERT(hci_);
  ZX_DEBUG_ASSERT(gatt_);
  ZX_DEBUG_ASSERT_MSG(dispatcher_, "must create on a thread with a dispatcher");

  init_seq_runner_ = std::make_unique<hci::SequentialCommandRunner>(dispatcher_, hci_);

  if (data_domain.has_value()) {
    data_domain_ = *data_domain;
  }

  auto self = weak_ptr_factory_.GetWeakPtr();
  hci_->SetTransportClosedCallback(
      [self] {
        if (self)
          self->OnTransportClosed();
      },
      dispatcher_);
}

Adapter::~Adapter() {
  if (IsInitialized())
    ShutDown();
}

bool Adapter::Initialize(InitializeCallback callback, fit::closure transport_closed_cb) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(callback);
  ZX_DEBUG_ASSERT(transport_closed_cb);

  if (IsInitialized()) {
    bt_log(WARN, "gap", "Adapter already initialized");
    return false;
  }

  ZX_DEBUG_ASSERT(!IsInitializing());

  init_state_ = State::kInitializing;

  ZX_DEBUG_ASSERT(init_seq_runner_->IsReady());
  ZX_DEBUG_ASSERT(!init_seq_runner_->HasQueuedCommands());

  transport_closed_cb_ = std::move(transport_closed_cb);

  // Start by resetting the controller to a clean state and then send
  // informational parameter commands that are not specific to LE or BR/EDR. The
  // commands sent here are mandatory for all LE controllers.
  //
  // NOTE: It's safe to pass capture |this| directly in the callbacks as
  // |init_seq_runner_| will internally invalidate the callbacks if it ever gets
  // deleted.

  // HCI_Reset
  init_seq_runner_->QueueCommand(hci::CommandPacket::New(hci::kReset));

  // HCI_Read_Local_Version_Information
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kReadLocalVersionInfo),
      [this](const hci::EventPacket& cmd_complete) {
        if (hci_is_error(cmd_complete, WARN, "gap", "read local version info failed")) {
          return;
        }
        auto params = cmd_complete.return_params<hci::ReadLocalVersionInfoReturnParams>();
        state_.hci_version_ = params->hci_version;
      });

  // HCI_Read_Local_Supported_Commands
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kReadLocalSupportedCommands),
      [this](const hci::EventPacket& cmd_complete) {
        if (hci_is_error(cmd_complete, WARN, "gap", "read local supported commands failed")) {
          return;
        }
        auto params = cmd_complete.return_params<hci::ReadLocalSupportedCommandsReturnParams>();
        std::memcpy(state_.supported_commands_, params->supported_commands,
                    sizeof(params->supported_commands));
      });

  // HCI_Read_Local_Supported_Features
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kReadLocalSupportedFeatures),
      [this](const hci::EventPacket& cmd_complete) {
        if (hci_is_error(cmd_complete, WARN, "gap", "read local supported features failed")) {
          return;
        }
        auto params = cmd_complete.return_params<hci::ReadLocalSupportedFeaturesReturnParams>();
        state_.features_.SetPage(0, le64toh(params->lmp_features));
      });

  // HCI_Read_BD_ADDR
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kReadBDADDR), [this](const hci::EventPacket& cmd_complete) {
        if (hci_is_error(cmd_complete, WARN, "gap", "read BR_ADDR failed")) {
          return;
        }
        auto params = cmd_complete.return_params<hci::ReadBDADDRReturnParams>();
        state_.controller_address_ = params->bd_addr;
      });

  init_seq_runner_->RunCommands([callback = std::move(callback), this](hci::Status status) mutable {
    if (!status) {
      bt_log(ERROR, "gap", "Failed to obtain initial controller information: %s",
             status.ToString().c_str());
      CleanUp();
      callback(false);
      return;
    }

    InitializeStep2(std::move(callback));
  });

  return true;
}

void Adapter::ShutDown() {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  bt_log(TRACE, "gap", "adapter shutting down");

  if (IsInitializing()) {
    ZX_DEBUG_ASSERT(!init_seq_runner_->IsReady());
    init_seq_runner_->Cancel();
  }

  CleanUp();
}

bool Adapter::AddBondedPeer(BondingData bonding_data) {
  return peer_cache()->AddBondedPeer(bonding_data);
}

void Adapter::SetPairingDelegate(fxl::WeakPtr<PairingDelegate> delegate) {
  le_connection_manager()->SetPairingDelegate(delegate);
  bredr_connection_manager()->SetPairingDelegate(delegate);
}

bool Adapter::IsDiscovering() const {
  return (le_discovery_manager_ && le_discovery_manager_->discovering()) ||
         (bredr_discovery_manager_ && bredr_discovery_manager_->discovering());
}

void Adapter::SetLocalName(std::string name, hci::StatusCallback callback) {
  // TODO(jamuraa): set the public LE advertisement name from |name|
  bool null_term = true;
  if (name.size() >= hci::kMaxNameLength) {
    name.resize(hci::kMaxNameLength);
    null_term = false;
  }
  auto write_name =
      hci::CommandPacket::New(hci::kWriteLocalName, sizeof(hci::WriteLocalNameCommandParams));
  auto name_buf =
      MutableBufferView(write_name->mutable_payload<hci::WriteLocalNameCommandParams>()->local_name,
                        hci::kMaxNameLength);
  name_buf.Write(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  if (null_term) {
    name_buf[name.size()] = 0;
  }
  hci_->command_channel()->SendCommand(
      std::move(write_name), dispatcher_,
      [this, name = std::move(name), cb = std::move(callback)](
          auto, const hci::EventPacket& event) mutable {
        if (!hci_is_error(event, WARN, "gap", "set local name failed")) {
          state_.local_name_ = std::move(name);
        }
        cb(event.ToStatus());
      });
}

void Adapter::SetDeviceClass(DeviceClass dev_class, hci::StatusCallback callback) {
  auto write_dev_class = hci::CommandPacket::New(hci::kWriteClassOfDevice,
                                                 sizeof(hci::WriteClassOfDeviceCommandParams));
  write_dev_class->mutable_payload<hci::WriteClassOfDeviceCommandParams>()->class_of_device =
      dev_class;
  hci_->command_channel()->SendCommand(
      std::move(write_dev_class), dispatcher_,
      [cb = std::move(callback)](auto, const hci::EventPacket& event) {
        hci_is_error(event, WARN, "gap", "set device class failed");
        cb(event.ToStatus());
      });
}

void Adapter::InitializeStep2(InitializeCallback callback) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(IsInitializing());

  // Low Energy MUST be supported. We don't support BR/EDR-only controllers.
  if (!state_.IsLowEnergySupported()) {
    bt_log(ERROR, "gap", "Bluetooth LE not supported by controller");
    CleanUp();
    callback(false);
    return;
  }

  // Check the HCI version. We officially only support 4.2+ only but for now we
  // just log a warning message if the version is legacy.
  if (state_.hci_version() < hci::HCIVersion::k4_2) {
    bt_log(WARN, "gap", "controller is using legacy HCI version %s",
           hci::HCIVersionToString(state_.hci_version()).c_str());
  }

  ZX_DEBUG_ASSERT(init_seq_runner_->IsReady());

  // If the controller supports the Read Buffer Size command then send it.
  // Otherwise we'll default to 0 when initializing the ACLDataChannel.
  if (state_.IsCommandSupported(14, hci::SupportedCommand::kReadBufferSize)) {
    // HCI_Read_Buffer_Size
    init_seq_runner_->QueueCommand(
        hci::CommandPacket::New(hci::kReadBufferSize),
        [this](const hci::EventPacket& cmd_complete) {
          if (hci_is_error(cmd_complete, WARN, "gap", "read buffer size failed")) {
            return;
          }
          auto params = cmd_complete.return_params<hci::ReadBufferSizeReturnParams>();
          uint16_t mtu = le16toh(params->hc_acl_data_packet_length);
          uint16_t max_count = le16toh(params->hc_total_num_acl_data_packets);
          if (mtu && max_count) {
            state_.bredr_data_buffer_info_ = hci::DataBufferInfo(mtu, max_count);
          }
        });
  }

  // HCI_LE_Read_Local_Supported_Features
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kLEReadLocalSupportedFeatures),
      [this](const hci::EventPacket& cmd_complete) {
        if (hci_is_error(cmd_complete, WARN, "gap", "LE read local supported features failed")) {
          return;
        }
        auto params = cmd_complete.return_params<hci::LEReadLocalSupportedFeaturesReturnParams>();
        state_.le_state_.supported_features_ = le64toh(params->le_features);
      });

  // HCI_LE_Read_Supported_States
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kLEReadSupportedStates),
      [this](const hci::EventPacket& cmd_complete) {
        if (hci_is_error(cmd_complete, WARN, "gap", "LE read local supported states failed")) {
          return;
        }
        auto params = cmd_complete.return_params<hci::LEReadSupportedStatesReturnParams>();
        state_.le_state_.supported_states_ = le64toh(params->le_states);
      });

  // HCI_LE_Read_Buffer_Size
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kLEReadBufferSize),
      [this](const hci::EventPacket& cmd_complete) {
        if (hci_is_error(cmd_complete, WARN, "gap", "LE read buffer size failed")) {
          return;
        }
        auto params = cmd_complete.return_params<hci::LEReadBufferSizeReturnParams>();
        uint16_t mtu = le16toh(params->hc_le_acl_data_packet_length);
        uint8_t max_count = params->hc_total_num_le_acl_data_packets;
        if (mtu && max_count) {
          state_.le_state_.data_buffer_info_ = hci::DataBufferInfo(mtu, max_count);
        }
        bt_log(TRACE, "gap", "LE read buffer size mtu=%u max_count=%c", mtu, max_count);
      });

  // HCI_LE_Read_Maximum_Data_Length
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kLEReadMaximumDataLength),
      [](const hci::EventPacket& cmd_complete) {
        if (hci_is_error(cmd_complete, WARN, "gap", "LE read maximum data length failed")) {
          return;
        }
        auto params = cmd_complete.return_params<hci::LEReadMaximumDataLengthReturnParams>();
        uint16_t max_tx_octets = le16toh(params->supported_max_tx_octets);
        uint16_t max_tx_time = le16toh(params->supported_max_tx_time);
        uint16_t max_rx_octets = le16toh(params->supported_max_rx_octets);
        uint16_t max_rx_time = le16toh(params->supported_max_rx_time);
        bt_log(TRACE, "gap",
               "LE read max data length tx_octets=%u tx_time=%u rx_octets=%u rx_time=%u",
               max_tx_octets, max_tx_time, max_rx_octets, max_rx_time);
      });

  // HCI_LE_Read_Suggested_Default_Data_Length
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New(hci::kLEReadSuggestedDefaultDataLength),
      [](const hci::EventPacket& cmd_complete) {
        if (hci_is_error(cmd_complete, WARN, "gap",
                         "LE read suggested default data length failed")) {
          return;
        }
        auto params =
            cmd_complete.return_params<hci::LEReadSuggestedDefaultDataLengthReturnParams>();
        uint16_t max_tx_octets = le16toh(params->suggested_max_tx_octets);
        uint16_t max_tx_time = le16toh(params->suggested_max_tx_time);
        bt_log(TRACE, "gap",
               "LE read suggested default data length max_tx_octets=%u max_tx_time=%u",
               max_tx_octets, max_tx_time);
      });

  if (state_.features().HasBit(0u, hci::LMPFeature::kSecureSimplePairing)) {
    // HCI_Write_Simple_Pairing_Mode
    auto write_ssp = hci::CommandPacket::New(hci::kWriteSimplePairingMode,
                                             sizeof(hci::WriteSimplePairingModeCommandParams));
    write_ssp->mutable_payload<hci::WriteSimplePairingModeCommandParams>()->simple_pairing_mode =
        hci::GenericEnableParam::kEnable;
    init_seq_runner_->QueueCommand(std::move(write_ssp), [](const auto& event) {
      // Warn if the command failed
      hci_is_error(event, WARN, "gap", "write simple pairing mode failed");
    });
  }

  // If there are extended features then try to read the first page of the
  // extended features.
  if (state_.features().HasBit(0u, hci::LMPFeature::kExtendedFeatures)) {
    // Page index 1 must be available.
    max_lmp_feature_page_index_ = 1;

    // HCI_Read_Local_Extended_Features
    auto cmd_packet = hci::CommandPacket::New(hci::kReadLocalExtendedFeatures,
                                              sizeof(hci::ReadLocalExtendedFeaturesCommandParams));

    // Try to read page 1.
    cmd_packet->mutable_payload<hci::ReadLocalExtendedFeaturesCommandParams>()->page_number = 1;

    init_seq_runner_->QueueCommand(
        std::move(cmd_packet), [this](const hci::EventPacket& cmd_complete) {
          if (hci_is_error(cmd_complete, WARN, "gap", "read local extended features failed")) {
            return;
          }
          auto params = cmd_complete.return_params<hci::ReadLocalExtendedFeaturesReturnParams>();
          state_.features_.SetPage(1, le64toh(params->extended_lmp_features));
          max_lmp_feature_page_index_ = params->maximum_page_number;
        });
  }

  init_seq_runner_->RunCommands([callback = std::move(callback), this](hci::Status status) mutable {
    if (bt_is_error(status, ERROR, "gap",
                    "failed to obtain initial controller information (step 2)")) {
      CleanUp();
      callback(false);
      return;
    }
    InitializeStep3(std::move(callback));
  });
}

void Adapter::InitializeStep3(InitializeCallback callback) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(IsInitializing());

  if (!state_.bredr_data_buffer_info().IsAvailable() &&
      !state_.low_energy_state().data_buffer_info().IsAvailable()) {
    bt_log(ERROR, "gap", "Both BR/EDR and LE buffers are unavailable");
    CleanUp();
    callback(false);
    return;
  }

  // Now that we have all the ACL data buffer information it's time to
  // initialize the ACLDataChannel.
  if (!hci_->InitializeACLDataChannel(state_.bredr_data_buffer_info(),
                                      state_.low_energy_state().data_buffer_info())) {
    bt_log(ERROR, "gap", "Failed to initialize ACLDataChannel (step 3)");
    CleanUp();
    callback(false);
    return;
  }

  // Create the data domain, if we haven't been provided one. Doing so here lets us guarantee that
  // AclDataChannel's lifetime is a superset of Data Domain's lifetime.
  // TODO(35228) We currently allow tests to inject their own domain in the adapter constructor,
  // as the adapter_unittests rely on injecting a fake domain to avoid concurrency in the unit
  // tests.  Once we move to a single threaded model, we would like to remove this and have the
  // adapter always be responsible for creating the domain.
  if (!data_domain_) {
    // Initialize the data Domain to make L2CAP available for the next initialization step. The
    // ACLDataChannel must be initialized before creating the data domain
    auto data_domain = data::Domain::Create(hci_, "bt-host (data)");
    if (!data_domain) {
      bt_log(ERROR, "gap", "Failed to initialize Data Domain");
      CleanUp();
      callback(false);
      return;
    }
    // Ensure the initialize task is posted to the data domain before we store it in adapter
    data_domain->Initialize();
    data_domain_ = data_domain;
  }

  ZX_DEBUG_ASSERT(init_seq_runner_->IsReady());
  ZX_DEBUG_ASSERT(!init_seq_runner_->HasQueuedCommands());

  // HCI_Set_Event_Mask
  {
    uint64_t event_mask = BuildEventMask();
    auto cmd_packet =
        hci::CommandPacket::New(hci::kSetEventMask, sizeof(hci::SetEventMaskCommandParams));
    cmd_packet->mutable_payload<hci::SetEventMaskCommandParams>()->event_mask = htole64(event_mask);
    init_seq_runner_->QueueCommand(std::move(cmd_packet), [](const auto& event) {
      hci_is_error(event, WARN, "gap", "set event mask failed");
    });
  }

  // HCI_LE_Set_Event_Mask
  {
    uint64_t event_mask = BuildLEEventMask();
    auto cmd_packet =
        hci::CommandPacket::New(hci::kLESetEventMask, sizeof(hci::LESetEventMaskCommandParams));
    cmd_packet->mutable_payload<hci::LESetEventMaskCommandParams>()->le_event_mask =
        htole64(event_mask);
    init_seq_runner_->QueueCommand(std::move(cmd_packet), [](const auto& event) {
      hci_is_error(event, WARN, "gap", "LE set event mask failed");
    });
  }

  // HCI_Write_LE_Host_Support if the appropriate feature bit is not set AND if
  // the controller supports this command.
  if (!state_.features().HasBit(1, hci::LMPFeature::kLESupportedHost) &&
      state_.IsCommandSupported(24, hci::SupportedCommand::kWriteLEHostSupport)) {
    auto cmd_packet = hci::CommandPacket::New(hci::kWriteLEHostSupport,
                                              sizeof(hci::WriteLEHostSupportCommandParams));
    auto params = cmd_packet->mutable_payload<hci::WriteLEHostSupportCommandParams>();
    params->le_supported_host = hci::GenericEnableParam::kEnable;
    params->simultaneous_le_host = 0x00;  // note: ignored
    init_seq_runner_->QueueCommand(std::move(cmd_packet), [](const auto& event) {
      hci_is_error(event, WARN, "gap", "write LE host support failed");
    });
  }

  // If we know that Page 2 of the extended features bitfield is available, then
  // request it.
  if (max_lmp_feature_page_index_ > 1) {
    auto cmd_packet = hci::CommandPacket::New(hci::kReadLocalExtendedFeatures,
                                              sizeof(hci::ReadLocalExtendedFeaturesCommandParams));

    // Try to read page 2.
    cmd_packet->mutable_payload<hci::ReadLocalExtendedFeaturesCommandParams>()->page_number = 2;

    // HCI_Read_Local_Extended_Features
    init_seq_runner_->QueueCommand(
        std::move(cmd_packet), [this](const hci::EventPacket& cmd_complete) {
          if (hci_is_error(cmd_complete, WARN, "gap", "read local extended features failed")) {
            return;
          }
          auto params = cmd_complete.return_params<hci::ReadLocalExtendedFeaturesReturnParams>();
          state_.features_.SetPage(2, le64toh(params->extended_lmp_features));
          max_lmp_feature_page_index_ = params->maximum_page_number;
        });
  }

  init_seq_runner_->RunCommands([callback = std::move(callback), this](hci::Status status) mutable {
    if (bt_is_error(status, ERROR, "gap",
                    "failed to obtain initial controller information (step 3)")) {
      CleanUp();
      callback(false);
      return;
    }
    InitializeStep4(std::move(callback));
  });
}

void Adapter::InitializeStep4(InitializeCallback callback) {
  ZX_DEBUG_ASSERT(IsInitializing());

  // Initialize the scan manager based on current feature support.
  if (state_.low_energy_state().IsFeatureSupported(
          hci::LESupportedFeature::kLEExtendedAdvertising)) {
    bt_log(INFO, "gap", "controller supports extended advertising");

    // TODO(armansito): Initialize |hci_le_*| objects here with extended-mode
    // versions.
    bt_log(WARN, "gap", "5.0 not yet supported; using legacy mode");
  }

  // We use the public controller address as the local LE identity address.
  DeviceAddress adapter_identity(DeviceAddress::Type::kLEPublic, state_.controller_address());

  // Initialize the LE local address manager.
  le_address_manager_ = std::make_unique<LowEnergyAddressManager>(
      adapter_identity, fit::bind_member(this, &Adapter::IsLeRandomAddressChangeAllowed), hci_);

  // Initialize the HCI adapters.
  hci_le_advertiser_ = std::make_unique<hci::LegacyLowEnergyAdvertiser>(hci_);
  hci_le_connector_ = std::make_unique<hci::LowEnergyConnector>(
      hci_, le_address_manager_.get(), dispatcher_,
      fit::bind_member(hci_le_advertiser_.get(), &hci::LowEnergyAdvertiser::OnIncomingConnection));
  hci_le_scanner_ =
      std::make_unique<hci::LegacyLowEnergyScanner>(le_address_manager_.get(), hci_, dispatcher_);

  // Initialize the LE manager objects
  le_discovery_manager_ =
      std::make_unique<LowEnergyDiscoveryManager>(hci_, hci_le_scanner_.get(), &peer_cache_);
  le_discovery_manager_->set_bonded_peer_connectable_callback(
      fit::bind_member(this, &Adapter::OnLeAutoConnectRequest));
  le_connection_manager_ = std::make_unique<LowEnergyConnectionManager>(
      hci_, le_address_manager_.get(), hci_le_connector_.get(), &peer_cache_, data_domain_, gatt_);
  le_advertising_manager_ = std::make_unique<LowEnergyAdvertisingManager>(
      hci_le_advertiser_.get(), le_address_manager_.get());

  // Initialize the BR/EDR manager objects if the controller supports BR/EDR.
  if (state_.IsBREDRSupported()) {
    DeviceAddress local_bredr_address(DeviceAddress::Type::kBREDR, state_.controller_address());

    bredr_connection_manager_ = std::make_unique<BrEdrConnectionManager>(
        hci_, &peer_cache_, local_bredr_address, data_domain_,
        state_.features().HasBit(0, hci::LMPFeature::kInterlacedPageScan));

    hci::InquiryMode mode = hci::InquiryMode::kStandard;
    if (state_.features().HasBit(0, hci::LMPFeature::kExtendedInquiryResponse)) {
      mode = hci::InquiryMode::kExtended;
    } else if (state_.features().HasBit(0, hci::LMPFeature::kRSSIwithInquiryResults)) {
      mode = hci::InquiryMode::kRSSI;
    }

    bredr_discovery_manager_ = std::make_unique<BrEdrDiscoveryManager>(hci_, mode, &peer_cache_);

    sdp_server_ = std::make_unique<sdp::Server>(data_domain_);
  }

  SetLocalName(kDefaultLocalName, [](auto status) {});

  // Set the default device class - a computer with audio.
  // TODO(BT-641): set this from a platform configuration file
  DeviceClass dev_class(DeviceClass::MajorClass::kComputer);
  dev_class.SetServiceClasses({DeviceClass::ServiceClass::kAudio});
  SetDeviceClass(dev_class, [](const auto&) {});

  // This completes the initialization sequence.
  init_state_ = State::kInitialized;
  callback(true);
}

uint64_t Adapter::BuildEventMask() {
  uint64_t event_mask = 0;

#define ENABLE_EVT(event) event_mask |= static_cast<uint64_t>(hci::EventMask::event)

  // Enable events that are needed for basic functionality. (alphabetic)
  ENABLE_EVT(kAuthenticationCompleteEvent);
  ENABLE_EVT(kConnectionCompleteEvent);
  ENABLE_EVT(kConnectionRequestEvent);
  ENABLE_EVT(kDisconnectionCompleteEvent);
  ENABLE_EVT(kEncryptionChangeEvent);
  ENABLE_EVT(kEncryptionKeyRefreshCompleteEvent);
  ENABLE_EVT(kLinkKeyRequestEvent);
  ENABLE_EVT(kLinkKeyNotificationEvent);
  ENABLE_EVT(kExtendedInquiryResultEvent);
  ENABLE_EVT(kHardwareErrorEvent);
  ENABLE_EVT(kInquiryCompleteEvent);
  ENABLE_EVT(kInquiryResultEvent);
  ENABLE_EVT(kInquiryResultWithRSSIEvent);
  ENABLE_EVT(kIOCapabilityRequestEvent);
  ENABLE_EVT(kIOCapabilityResponseEvent);
  ENABLE_EVT(kLEMetaEvent);
  ENABLE_EVT(kUserConfirmationRequestEvent);
  ENABLE_EVT(kUserPasskeyRequestEvent);
  ENABLE_EVT(kRemoteOOBDataRequestEvent);
  ENABLE_EVT(kRemoteNameRequestCompleteEvent);
  ENABLE_EVT(kReadRemoteSupportedFeaturesCompleteEvent);
  ENABLE_EVT(kReadRemoteVersionInformationCompleteEvent);
  ENABLE_EVT(kReadRemoteExtendedFeaturesCompleteEvent);

#undef ENABLE_EVT

  return event_mask;
}

uint64_t Adapter::BuildLEEventMask() {
  uint64_t event_mask = 0;

#define ENABLE_EVT(event) event_mask |= static_cast<uint64_t>(hci::LEEventMask::event)

  ENABLE_EVT(kLEAdvertisingReport);
  ENABLE_EVT(kLEConnectionComplete);
  ENABLE_EVT(kLEConnectionUpdateComplete);
  ENABLE_EVT(kLELongTermKeyRequest);

#undef ENABLE_EVT

  return event_mask;
}

void Adapter::CleanUp() {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());

  if (init_state_ == State::kNotInitialized) {
    bt_log(TRACE, "gap", "clean up: not initialized");
    return;
  }

  init_state_ = State::kNotInitialized;
  state_ = AdapterState();
  transport_closed_cb_ = nullptr;

  // Destroy objects in reverse order of construction.
  sdp_server_ = nullptr;
  bredr_discovery_manager_ = nullptr;
  le_advertising_manager_ = nullptr;
  le_connection_manager_ = nullptr;
  le_discovery_manager_ = nullptr;

  hci_le_connector_ = nullptr;
  hci_le_advertiser_ = nullptr;
  hci_le_scanner_ = nullptr;

  le_address_manager_ = nullptr;

  // Clean up the data domain as it gets initialized by the Adapter.
  if (data_domain_) {
    data_domain_->ShutDown();
    data_domain_ = nullptr;
  }

  // TODO(armansito): hci::Transport::ShutDown() should send a shutdown message
  // to the bt-hci device, which would be responsible for sending HCI_Reset upon
  // exit.
  if (hci_->IsInitialized())
    hci_->ShutDown();
}

void Adapter::OnTransportClosed() {
  bt_log(INFO, "gap", "HCI transport was closed");
  if (transport_closed_cb_)
    transport_closed_cb_();
}

void Adapter::OnLeAutoConnectRequest(PeerId peer_id) {
  ZX_DEBUG_ASSERT(le_connection_manager_);

  // TODO(BT-888): We shouldn't always accept connection requests from all
  // bonded peripherals (e.g. if one is explicitly disconnected). Maybe add an
  // "auto_connect()" property to Peer?
  auto self = weak_ptr_factory_.GetWeakPtr();
  le_connection_manager_->Connect(peer_id, [self](auto status, auto conn) {
    if (!self) {
      bt_log(INFO, "gap", "ignoring auto-connection (adapter destroyed)");
      return;
    }

    if (bt_is_error(status, INFO, "gap", "failed to auto-connect")) {
      return;
    }

    ZX_DEBUG_ASSERT(conn);
    PeerId id = conn->peer_identifier();
    bt_log(INFO, "gap", "peer auto-connected (id: %s)", bt_str(id));
    if (self->auto_conn_cb_) {
      self->auto_conn_cb_(std::move(conn));
    }
  });
}

bool Adapter::IsLeRandomAddressChangeAllowed() {
  return hci_le_advertiser_->AllowsRandomAddressChange() &&
         hci_le_scanner_->AllowsRandomAddressChange() &&
         hci_le_connector_->AllowsRandomAddressChange();
}

}  // namespace gap
}  // namespace bt
