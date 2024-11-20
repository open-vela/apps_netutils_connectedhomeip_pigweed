// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/bluetooth/core/bt-host/gap/bredr_connection_manager.h"

#include "src/connectivity/bluetooth/core/bt-host/common/test_helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/data/fake_domain.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/peer_cache.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/util.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/fake_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/fake_controller_test.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/fake_peer.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_controller.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_packets.h"

namespace bt {
namespace gap {
namespace {

using bt::testing::CommandTransaction;

using TestingBase =
    bt::testing::FakeControllerTest<bt::testing::TestController>;

constexpr hci::ConnectionHandle kConnectionHandle = 0x0BAA;
const DeviceAddress kLocalDevAddr(DeviceAddress::Type::kBREDR,
                                  "00:00:00:00:00:00");
const DeviceAddress kTestDevAddr(DeviceAddress::Type::kBREDR,
                                 "00:00:00:00:00:01");
const DeviceAddress kTestDevAddrLe(DeviceAddress::Type::kLEPublic,
                                   "00:00:00:00:00:02");
const DeviceAddress kTestDevAddr2(DeviceAddress::Type::kBREDR,
                                  "00:00:00:00:00:03");

#define TEST_DEV_ADDR_BYTES_LE 0x01, 0x00, 0x00, 0x00, 0x00, 0x00

// clang-format off

const auto kReadScanEnable = CreateStaticByteBuffer(
    LowerBits(hci::kReadScanEnable), UpperBits(hci::kReadScanEnable),
    0x00  // No parameters
);

#define READ_SCAN_ENABLE_RSP(scan_enable)                                    \
  CreateStaticByteBuffer(hci::kCommandCompleteEventCode, 0x05, 0xF0, \
                                 LowerBits(hci::kReadScanEnable),            \
                                 UpperBits(hci::kReadScanEnable),            \
                                 hci::kSuccess, (scan_enable))

const auto kReadScanEnableRspNone = READ_SCAN_ENABLE_RSP(0x00);
const auto kReadScanEnableRspInquiry = READ_SCAN_ENABLE_RSP(0x01);
const auto kReadScanEnableRspPage = READ_SCAN_ENABLE_RSP(0x02);
const auto kReadScanEnableRspBoth = READ_SCAN_ENABLE_RSP(0x03);

#undef READ_SCAN_ENABLE_RSP

#define WRITE_SCAN_ENABLE_CMD(scan_enable)                               \
  CreateStaticByteBuffer(LowerBits(hci::kWriteScanEnable),       \
                                 UpperBits(hci::kWriteScanEnable), 0x01, \
                                 (scan_enable))

const auto kWriteScanEnableNone = WRITE_SCAN_ENABLE_CMD(0x00);
const auto kWriteScanEnableInq = WRITE_SCAN_ENABLE_CMD(0x01);
const auto kWriteScanEnablePage = WRITE_SCAN_ENABLE_CMD(0x02);
const auto kWriteScanEnableBoth = WRITE_SCAN_ENABLE_CMD(0x03);

#undef WRITE_SCAN_ENABLE_CMD

#define COMMAND_COMPLETE_RSP(opcode)                                         \
  CreateStaticByteBuffer(hci::kCommandCompleteEventCode, 0x04, 0xF0, \
                                 LowerBits((opcode)), UpperBits((opcode)),   \
                                 hci::kSuccess);

const auto kWriteScanEnableRsp = COMMAND_COMPLETE_RSP(hci::kWriteScanEnable);

const auto kWritePageScanActivity = CreateStaticByteBuffer(
    LowerBits(hci::kWritePageScanActivity),
    UpperBits(hci::kWritePageScanActivity),
    0x04,  // parameter_total_size (4 bytes)
    0x00, 0x08,  // 1.28s interval (R1)
    0x11, 0x00  // 10.625ms window (R1)
);

const auto kWritePageScanActivityRsp =
    COMMAND_COMPLETE_RSP(hci::kWritePageScanActivity);

const auto kWritePageScanType = CreateStaticByteBuffer(
    LowerBits(hci::kWritePageScanType), UpperBits(hci::kWritePageScanType),
    0x01,  // parameter_total_size (1 byte)
    0x01   // Interlaced scan
);

const auto kWritePageScanTypeRsp =
    COMMAND_COMPLETE_RSP(hci::kWritePageScanType);


#define COMMAND_STATUS_RSP(opcode, statuscode)                       \
  CreateStaticByteBuffer(hci::kCommandStatusEventCode, 0x04, \
                                 (statuscode), 0xF0,                 \
                                 LowerBits((opcode)), UpperBits((opcode)));
// clang-format on

const auto kConnectionRequest =
    CreateStaticByteBuffer(hci::kConnectionRequestEventCode,
                           0x0A,  // parameter_total_size (10 byte payload)
                           TEST_DEV_ADDR_BYTES_LE,  // peer address
                           0x00, 0x1F, 0x00,  // class_of_device (unspecified)
                           0x01               // link_type (ACL)
    );
const auto kAcceptConnectionRequest =
    CreateStaticByteBuffer(LowerBits(hci::kAcceptConnectionRequest),
                           UpperBits(hci::kAcceptConnectionRequest),
                           0x07,  // parameter_total_size (7 bytes)
                           TEST_DEV_ADDR_BYTES_LE,  // peer address
                           0x00                     // role (become master)
    );

const auto kAcceptConnectionRequestRsp = COMMAND_STATUS_RSP(
    hci::kAcceptConnectionRequest, hci::StatusCode::kSuccess);

const auto kConnectionComplete =
    CreateStaticByteBuffer(hci::kConnectionCompleteEventCode,
                           0x0B,  // parameter_total_size (11 byte payload)
                           hci::StatusCode::kSuccess,  // status
                           0xAA, 0x0B,                 // connection_handle
                           TEST_DEV_ADDR_BYTES_LE,     // peer address
                           0x01,                       // link_type (ACL)
                           0x00                        // encryption not enabled
    );

const auto kConnectionCompleteError = CreateStaticByteBuffer(
    hci::kConnectionCompleteEventCode,
    0x0B,  // parameter_total_size (11 byte payload)
    hci::StatusCode::kConnectionFailedToBeEstablished,  // status
    0x00, 0x00,                                         // connection_handle
    TEST_DEV_ADDR_BYTES_LE,                             // peer address
    0x01,                                               // link_type (ACL)
    0x00  // encryption not enabled
);

const auto kConnectionCompleteCanceled =
    CreateStaticByteBuffer(hci::kConnectionCompleteEventCode,
                           0x0B,  // parameter_total_size (11 byte payload)
                           hci::StatusCode::kUnknownConnectionId,  // status
                           0x00, 0x00,              // connection_handle
                           TEST_DEV_ADDR_BYTES_LE,  // peer address
                           0x01,                    // link_type (ACL)
                           0x00                     // encryption not enabled
    );

const auto kCreateConnection = CreateStaticByteBuffer(
    LowerBits(hci::kCreateConnection), UpperBits(hci::kCreateConnection),
    0x0d,                                   // parameter_total_size (13 bytes)
    TEST_DEV_ADDR_BYTES_LE,                 // peer address
    LowerBits(hci::kEnableAllPacketTypes),  // allowable packet types
    UpperBits(hci::kEnableAllPacketTypes),  // allowable packet types
    0x02,                                   // page_scan_repetition_mode (R2)
    0x00,                                   // reserved
    0x00, 0x00,                             // clock_offset
    0x00                                    // allow_role_switch (don't)
);

const auto kCreateConnectionRsp =
    COMMAND_STATUS_RSP(hci::kCreateConnection, hci::StatusCode::kSuccess);

const auto kCreateConnectionRspError = COMMAND_STATUS_RSP(
    hci::kCreateConnection, hci::StatusCode::kConnectionFailedToBeEstablished);

const auto kCreateConnectionCancel =
    CreateStaticByteBuffer(LowerBits(hci::kCreateConnectionCancel),
                           UpperBits(hci::kCreateConnectionCancel),
                           0x06,  // parameter_total_size (6 bytes)
                           TEST_DEV_ADDR_BYTES_LE  // peer address
    );

const auto kCreateConnectionCancelRsp =
    COMMAND_COMPLETE_RSP(hci::kCreateConnectionCancel);

const auto kRemoteNameRequest = CreateStaticByteBuffer(
    LowerBits(hci::kRemoteNameRequest), UpperBits(hci::kRemoteNameRequest),
    0x0a,                    // parameter_total_size (10 bytes)
    TEST_DEV_ADDR_BYTES_LE,  // peer address
    0x00,                    // page_scan_repetition_mode (R0)
    0x00,                    // reserved
    0x00, 0x00               // clock_offset
);
const auto kRemoteNameRequestRsp =
    COMMAND_STATUS_RSP(hci::kRemoteNameRequest, hci::StatusCode::kSuccess);

const auto kRemoteNameRequestComplete = CreateStaticByteBuffer(
    hci::kRemoteNameRequestCompleteEventCode,
    0x20,                       // parameter_total_size (32)
    hci::StatusCode::kSuccess,  // status
    TEST_DEV_ADDR_BYTES_LE,     // peer address
    'F', 'u', 'c', 'h', 's', 'i', 'a', 0xF0, 0x9F, 0x92, 0x96, 0x00, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    // remote name (Fuchsia 💖)
    // Everything after the 0x00 should be ignored.
);
const auto kReadRemoteVersionInfo =
    CreateStaticByteBuffer(LowerBits(hci::kReadRemoteVersionInfo),
                           UpperBits(hci::kReadRemoteVersionInfo),
                           0x02,       // Parameter_total_size (2 bytes)
                           0xAA, 0x0B  // connection_handle
    );

const auto kReadRemoteVersionInfoRsp =
    COMMAND_STATUS_RSP(hci::kReadRemoteVersionInfo, hci::StatusCode::kSuccess);

const auto kRemoteVersionInfoComplete =
    CreateStaticByteBuffer(hci::kReadRemoteVersionInfoCompleteEventCode,
                           0x08,  // parameter_total_size (8 bytes)
                           hci::StatusCode::kSuccess,  // status
                           0xAA, 0x0B,                 // connection_handle
                           hci::HCIVersion::k4_2,      // lmp_version
                           0xE0, 0x00,  // manufacturer_name (Google)
                           0xAD, 0xDE   // lmp_subversion (anything)
    );

const auto kReadRemoteSupportedFeatures =
    CreateStaticByteBuffer(LowerBits(hci::kReadRemoteSupportedFeatures),
                           UpperBits(hci::kReadRemoteSupportedFeatures),
                           0x02,       // parameter_total_size (2 bytes)
                           0xAA, 0x0B  // connection_handle
    );

const auto kReadRemoteSupportedFeaturesRsp = COMMAND_STATUS_RSP(
    hci::kReadRemoteSupportedFeatures, hci::StatusCode::kSuccess);

const auto kReadRemoteSupportedFeaturesComplete = CreateStaticByteBuffer(
    hci::kReadRemoteSupportedFeaturesCompleteEventCode,
    0x0B,                       // parameter_total_size (11 bytes)
    hci::StatusCode::kSuccess,  // status
    0xAA, 0x0B,                 // connection_handle,
    0xFF, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x80
    // lmp_features
    // Set: 3 slot packets, 5 slot packets, Encryption, Timing Accuracy,
    // Role Switch, Hold Mode, Sniff Mode, LE Supported, Extended Features
);

const auto kReadRemoteExtended1 =
    CreateStaticByteBuffer(LowerBits(hci::kReadRemoteExtendedFeatures),
                           UpperBits(hci::kReadRemoteExtendedFeatures),
                           0x03,        // parameter_total_size (3 bytes)
                           0xAA, 0x0B,  // connection_handle
                           0x01         // page_number (1)
    );

const auto kReadRemoteExtendedFeaturesRsp = COMMAND_STATUS_RSP(
    hci::kReadRemoteExtendedFeatures, hci::StatusCode::kSuccess);

const auto kReadRemoteExtended1Complete = CreateStaticByteBuffer(
    hci::kReadRemoteExtendedFeaturesCompleteEventCode,
    0x0D,                       // parameter_total_size (13 bytes)
    hci::StatusCode::kSuccess,  // status
    0xAA, 0x0B,                 // connection_handle,
    0x01,                       // page_number
    0x03,                       // max_page_number (3 pages)
    0x0F, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00
    // lmp_features
    // Set: Secure Simple Pairing (Host Support), LE Supported (Host),
    //  SimultaneousLEAndBREDR, Secure Connections (Host Support)
);

const auto kReadRemoteExtended2 =
    CreateStaticByteBuffer(LowerBits(hci::kReadRemoteExtendedFeatures),
                           UpperBits(hci::kReadRemoteExtendedFeatures),
                           0x03,        // parameter_total_size (3 bytes)
                           0xAA, 0x0B,  // connection_handle
                           0x02         // page_number (2)
    );

const auto kReadRemoteExtended2Complete =
    CreateStaticByteBuffer(hci::kReadRemoteExtendedFeaturesCompleteEventCode,
                           0x0D,  // parameter_total_size (13 bytes)
                           hci::StatusCode::kSuccess,  // status
                           0xAA, 0x0B,                 // connection_handle,
                           0x02,                       // page_number
                           0x03,  // max_page_number (3 pages)
                           0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0xFF, 0x00
                           // lmp_features  - All the bits should be ignored.
    );

const auto kDisconnect = CreateStaticByteBuffer(
    LowerBits(hci::kDisconnect), UpperBits(hci::kDisconnect),
    0x03,        // parameter_total_size (3 bytes)
    0xAA, 0x0B,  // connection_handle
    0x13         // Reason (Remote User Terminated Connection)
);

const auto kDisconnectRsp =
    COMMAND_STATUS_RSP(hci::kDisconnect, hci::StatusCode::kSuccess);

const auto kDisconnectionComplete =
    CreateStaticByteBuffer(hci::kDisconnectionCompleteEventCode,
                           0x04,  // parameter_total_size (4 bytes)
                           hci::StatusCode::kSuccess,  // status
                           0xAA, 0x0B,                 // connection_handle
                           0x13  // Reason (Remote User Terminated Connection)
    );

class BrEdrConnectionManagerTest : public TestingBase {
 public:
  BrEdrConnectionManagerTest() = default;
  ~BrEdrConnectionManagerTest() override = default;

  void SetUp() override {
    TestingBase::SetUp();
    InitializeACLDataChannel();

    peer_cache_ = std::make_unique<PeerCache>();
    data_domain_ = data::testing::FakeDomain::Create();
    data_domain_->Initialize();
    auto hci = transport();

    connection_manager_ = std::make_unique<BrEdrConnectionManager>(
        hci, peer_cache_.get(), kLocalDevAddr, data_domain_, true);

    StartTestDevice();

    test_device()->SetTransactionCallback([this] { transaction_count_++; },
                                          async_get_default_dispatcher());
  }

  void TearDown() override {
    // Don't trigger the transaction callback when cleaning up the manager.
    test_device()->ClearTransactionCallback();
    if (connection_manager_ != nullptr) {
      // deallocating the connection manager disables connectivity.
      test_device()->QueueCommandTransaction(
          CommandTransaction(kReadScanEnable, {&kReadScanEnableRspBoth}));
      test_device()->QueueCommandTransaction(
          CommandTransaction(kWriteScanEnableInq, {&kWriteScanEnableRsp}));
      connection_manager_ = nullptr;
    }
    RunLoopUntilIdle();
    test_device()->Stop();
    data_domain_ = nullptr;
    peer_cache_ = nullptr;
    TestingBase::TearDown();
  }

 protected:
  static constexpr const int kIncomingConnTransactions = 6;

  BrEdrConnectionManager* connmgr() const { return connection_manager_.get(); }
  void SetConnectionManager(std::unique_ptr<BrEdrConnectionManager> mgr) {
    connection_manager_ = std::move(mgr);
  }

  PeerCache* peer_cache() const { return peer_cache_.get(); }

  data::testing::FakeDomain* data_domain() const { return data_domain_.get(); }

  int transaction_count() const { return transaction_count_; }

  // Add expectations and simulated responses for the outbound commands sent
  // after an inbound Connection Request Event is received. Results in
  // |kIncomingConnTransactions| transactions.
  void QueueSuccessfulIncomingConn() const {
    test_device()->QueueCommandTransaction(CommandTransaction(
        kAcceptConnectionRequest,
        {&kAcceptConnectionRequestRsp, &kConnectionComplete}));
    QueueSuccessfulInterrogation(kTestDevAddr, kConnectionHandle);
  }

  void QueueSuccessfulCreateConnection(Peer* peer,
                                       hci::ConnectionHandle conn) const {
    const DynamicByteBuffer complete_packet =
        testing::ConnectionCompletePacket(peer->address(), conn);
    test_device()->QueueCommandTransaction(
        CommandTransaction(testing::CreateConnectionPacket(peer->address()),
                           {&kCreateConnectionRsp, &complete_packet}));
  }
  void QueueSuccessfulInterrogation(DeviceAddress addr,
                                    hci::ConnectionHandle conn) const {
    const DynamicByteBuffer remote_name_complete_packet =
        testing::RemoteNameRequestCompletePacket(addr);
    const DynamicByteBuffer remote_version_complete_packet =
        testing::ReadRemoteVersionInfoCompletePacket(conn);
    const DynamicByteBuffer remote_supported_complete_packet =
        testing::ReadRemoteSupportedFeaturesCompletePacket(conn);
    const DynamicByteBuffer remote_extended1_complete_packet =
        testing::ReadRemoteExtended1CompletePacket(conn);
    const DynamicByteBuffer remote_extended2_complete_packet =
        testing::ReadRemoteExtended2CompletePacket(conn);

    test_device()->QueueCommandTransaction(CommandTransaction(
        testing::RemoteNameRequestPacket(addr),
        {&kRemoteNameRequestRsp, &remote_name_complete_packet}));
    test_device()->QueueCommandTransaction(CommandTransaction(
        testing::ReadRemoteVersionInfoPacket(conn),
        {&kReadRemoteVersionInfoRsp, &remote_version_complete_packet}));
    test_device()->QueueCommandTransaction(CommandTransaction(
        testing::ReadRemoteSupportedFeaturesPacket(conn),
        {&kReadRemoteSupportedFeaturesRsp, &remote_supported_complete_packet}));
    test_device()->QueueCommandTransaction(CommandTransaction(
        testing::ReadRemoteExtended1Packet(conn),
        {&kReadRemoteExtendedFeaturesRsp, &remote_extended1_complete_packet}));
    test_device()->QueueCommandTransaction(CommandTransaction(
        testing::ReadRemoteExtended2Packet(conn),
        {&kReadRemoteExtendedFeaturesRsp, &remote_extended2_complete_packet}));
  }

  void QueueDisconnection(hci::ConnectionHandle conn) const {
    const DynamicByteBuffer disconnect_complete =
        testing::DisconnectionCompletePacket(conn);
    test_device()->QueueCommandTransaction(
        CommandTransaction(testing::DisconnectPacket(conn),
                           {&kDisconnectRsp, &disconnect_complete}));
  }

 private:
  std::unique_ptr<BrEdrConnectionManager> connection_manager_;
  std::unique_ptr<PeerCache> peer_cache_;
  fbl::RefPtr<data::testing::FakeDomain> data_domain_;
  int transaction_count_ = 0;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(BrEdrConnectionManagerTest);
};

using GAP_BrEdrConnectionManagerTest = BrEdrConnectionManagerTest;

TEST_F(GAP_BrEdrConnectionManagerTest, DisableConnectivity) {
  size_t cb_count = 0;
  auto cb = [&cb_count](const auto& status) {
    cb_count++;
    EXPECT_TRUE(status);
  };

  test_device()->QueueCommandTransaction(
      CommandTransaction(kReadScanEnable, {&kReadScanEnableRspPage}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kWriteScanEnableNone, {&kWriteScanEnableRsp}));

  connmgr()->SetConnectable(false, cb);

  RunLoopUntilIdle();

  EXPECT_EQ(1u, cb_count);

  test_device()->QueueCommandTransaction(
      CommandTransaction(kReadScanEnable, {&kReadScanEnableRspBoth}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kWriteScanEnableInq, {&kWriteScanEnableRsp}));

  connmgr()->SetConnectable(false, cb);

  RunLoopUntilIdle();

  EXPECT_EQ(2u, cb_count);
}

TEST_F(GAP_BrEdrConnectionManagerTest, EnableConnectivity) {
  size_t cb_count = 0;
  auto cb = [&cb_count](const auto& status) {
    cb_count++;
    EXPECT_TRUE(status);
  };

  test_device()->QueueCommandTransaction(
      CommandTransaction(kWritePageScanActivity, {&kWritePageScanActivityRsp}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kWritePageScanType, {&kWritePageScanTypeRsp}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kReadScanEnable, {&kReadScanEnableRspNone}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kWriteScanEnablePage, {&kWriteScanEnableRsp}));

  connmgr()->SetConnectable(true, cb);

  RunLoopUntilIdle();

  EXPECT_EQ(1u, cb_count);

  test_device()->QueueCommandTransaction(
      CommandTransaction(kWritePageScanActivity, {&kWritePageScanActivityRsp}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kWritePageScanType, {&kWritePageScanTypeRsp}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kReadScanEnable, {&kReadScanEnableRspInquiry}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kWriteScanEnableBoth, {&kWriteScanEnableRsp}));

  connmgr()->SetConnectable(true, cb);

  RunLoopUntilIdle();

  EXPECT_EQ(2u, cb_count);
}

// Test: An incoming connection request should trigger an acceptance and
// interrogation should allow a peer that only report the first Extended
// Features page.
TEST_F(GAP_BrEdrConnectionManagerTest,
       IncomingConnection_BrokenExtendedPageResponse) {
  test_device()->QueueCommandTransaction(
      CommandTransaction(kAcceptConnectionRequest,
                         {&kAcceptConnectionRequestRsp, &kConnectionComplete}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kRemoteNameRequest,
      {&kRemoteNameRequestRsp, &kRemoteNameRequestComplete}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kReadRemoteVersionInfo,
      {&kReadRemoteVersionInfoRsp, &kRemoteVersionInfoComplete}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kReadRemoteSupportedFeatures, {&kReadRemoteSupportedFeaturesRsp,
                                     &kReadRemoteSupportedFeaturesComplete}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kReadRemoteExtended1,
      {&kReadRemoteExtendedFeaturesRsp, &kReadRemoteExtended1Complete}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kReadRemoteExtended2,
      {&kReadRemoteExtendedFeaturesRsp, &kReadRemoteExtended1Complete}));

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(6, transaction_count());

  // When we deallocate the connection manager next, we should disconnect.
  test_device()->QueueCommandTransaction(CommandTransaction(
      kDisconnect, {&kDisconnectRsp, &kDisconnectionComplete}));

  // deallocating the connection manager disables connectivity.
  test_device()->QueueCommandTransaction(
      CommandTransaction(kReadScanEnable, {&kReadScanEnableRspBoth}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kWriteScanEnableInq, {&kWriteScanEnableRsp}));

  SetConnectionManager(nullptr);

  RunLoopUntilIdle();

  EXPECT_EQ(9, transaction_count());
}

// Test: An incoming connection request should trigger an acceptance and an
// interrogation to discover capabilities.
TEST_F(GAP_BrEdrConnectionManagerTest, IncomingConnectionSuccess) {
  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));

  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->identifier(), connmgr()->GetPeerId(kConnectionHandle));
  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  // When we deallocate the connection manager next, we should disconnect.
  test_device()->QueueCommandTransaction(CommandTransaction(
      kDisconnect, {&kDisconnectRsp, &kDisconnectionComplete}));

  // deallocating the connection manager disables connectivity.
  test_device()->QueueCommandTransaction(
      CommandTransaction(kReadScanEnable, {&kReadScanEnableRspBoth}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kWriteScanEnableInq, {&kWriteScanEnableRsp}));

  SetConnectionManager(nullptr);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 3, transaction_count());
}

// Test: An incoming connection request should upgrade a known LE peer with a
// matching address to a dual mode peer.
TEST_F(GAP_BrEdrConnectionManagerTest,
       IncomingConnectionUpgradesKnownLowEnergyPeerToDualMode) {
  const DeviceAddress le_alias_addr(DeviceAddress::Type::kLEPublic,
                                    kTestDevAddr.value());
  Peer* const peer = peer_cache()->NewPeer(le_alias_addr, true);
  ASSERT_TRUE(peer);
  ASSERT_EQ(TechnologyType::kLowEnergy, peer->technology());

  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  ASSERT_EQ(peer, peer_cache()->FindByAddress(kTestDevAddr));
  EXPECT_EQ(peer->identifier(), connmgr()->GetPeerId(kConnectionHandle));
  EXPECT_EQ(TechnologyType::kDualMode, peer->technology());

  // Prepare for disconnection upon teardown.
  QueueDisconnection(kConnectionHandle);
}

// Test: A remote disconnect should correctly remove the connection.
TEST_F(GAP_BrEdrConnectionManagerTest, RemoteDisconnect) {
  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunLoopUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->identifier(), connmgr()->GetPeerId(kConnectionHandle));

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  // Remote end disconnects.
  test_device()->SendCommandChannelPacket(kDisconnectionComplete);

  RunLoopUntilIdle();

  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));

  // deallocating the connection manager disables connectivity.
  test_device()->QueueCommandTransaction(
      CommandTransaction(kReadScanEnable, {&kReadScanEnableRspBoth}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kWriteScanEnableInq, {&kWriteScanEnableRsp}));

  SetConnectionManager(nullptr);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 2, transaction_count());
}

const auto kRemoteNameRequestCompleteFailed =
    CreateStaticByteBuffer(hci::kRemoteNameRequestCompleteEventCode,
                           0x01,  // parameter_total_size (1 bytes)
                           hci::StatusCode::kHardwareFailure);

const auto kReadRemoteSupportedFeaturesCompleteFailed =
    CreateStaticByteBuffer(hci::kRemoteNameRequestCompleteEventCode,
                           0x01,  // parameter_total_size (1 bytes)
                           hci::StatusCode::kHardwareFailure);

// Test: if the interrogation fails, we disconnect.
//  - Receiving extra responses after a command fails will not fail
//  - We don't query extended features if we don't receive an answer.
TEST_F(GAP_BrEdrConnectionManagerTest, IncommingConnectionFailedInterrogation) {
  test_device()->QueueCommandTransaction(
      CommandTransaction(kAcceptConnectionRequest,
                         {&kAcceptConnectionRequestRsp, &kConnectionComplete}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kRemoteNameRequest,
      {&kRemoteNameRequestRsp, &kRemoteNameRequestCompleteFailed}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kReadRemoteVersionInfo,
      {&kReadRemoteVersionInfoRsp, &kRemoteVersionInfoComplete}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kReadRemoteSupportedFeatures,
                         {&kReadRemoteSupportedFeaturesRsp,
                          &kReadRemoteSupportedFeaturesCompleteFailed}));

  test_device()->QueueCommandTransaction(CommandTransaction(
      kDisconnect, {&kDisconnectRsp, &kDisconnectionComplete}));

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(5, transaction_count());
}

const auto kCapabilitiesRequest =
    CreateStaticByteBuffer(hci::kIOCapabilityRequestEventCode,
                           0x06,  // parameter_total_size (6 bytes)
                           TEST_DEV_ADDR_BYTES_LE  // address
    );

const auto kCapabilitiesRequestReply = CreateStaticByteBuffer(
    LowerBits(hci::kIOCapabilityRequestReply),
    UpperBits(hci::kIOCapabilityRequestReply),
    0x09,                    // parameter_total_size (9 bytes)
    TEST_DEV_ADDR_BYTES_LE,  // peer address
    0x03,                    // No input, No output
    0x00,                    // No OOB data present
    0x04                     // MITM Protection Not Required – General Bonding
);

const auto kCapabilitiesRequestReplyRsp =
    CreateStaticByteBuffer(hci::kCommandCompleteEventCode, 0x0A, 0xF0,
                           LowerBits(hci::kIOCapabilityRequestReply),
                           UpperBits(hci::kIOCapabilityRequestReply),
                           hci::kSuccess,          // status
                           TEST_DEV_ADDR_BYTES_LE  // peer address
    );

// Test: sends replies to Capability Requests
// TODO(jamuraa): returns correct capabilities when we have different
// requirements.
TEST_F(GAP_BrEdrConnectionManagerTest, CapabilityRequest) {
  test_device()->QueueCommandTransaction(kCapabilitiesRequestReply,
                                         {&kCapabilitiesRequestReplyRsp});

  test_device()->SendCommandChannelPacket(kCapabilitiesRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(1, transaction_count());
}

const auto kUserConfirmationRequest =
    CreateStaticByteBuffer(hci::kUserConfirmationRequestEventCode,
                           0x0A,  // parameter_total_size (10 byte payload)
                           TEST_DEV_ADDR_BYTES_LE,  // peer address
                           0x00, 0x00, 0x00, 0x00   // numeric value 000000
    );

const auto kConfirmationRequestReply =
    CreateStaticByteBuffer(LowerBits(hci::kUserConfirmationRequestReply),
                           UpperBits(hci::kUserConfirmationRequestReply),
                           0x06,  // parameter_total_size (6 bytes)
                           TEST_DEV_ADDR_BYTES_LE  // peer address
    );

const auto kConfirmationRequestReplyRsp =
    COMMAND_COMPLETE_RSP(hci::kUserConfirmationRequestReply);

// Test: sends replies to Confirmation Requests
TEST_F(GAP_BrEdrConnectionManagerTest, ConfirmationRequest) {
  test_device()->QueueCommandTransaction(kConfirmationRequestReply,
                                         {&kConfirmationRequestReplyRsp});

  test_device()->SendCommandChannelPacket(kUserConfirmationRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(1, transaction_count());
}

const auto kLinkKeyRequest =
    CreateStaticByteBuffer(hci::kLinkKeyRequestEventCode,
                           0x06,  // parameter_total_size (6 bytes)
                           TEST_DEV_ADDR_BYTES_LE  // peer address
    );

const auto kLinkKeyRequestNegativeReply =
    CreateStaticByteBuffer(LowerBits(hci::kLinkKeyRequestNegativeReply),
                           UpperBits(hci::kLinkKeyRequestNegativeReply),
                           0x06,  // parameter_total_size (6 bytes)
                           TEST_DEV_ADDR_BYTES_LE  // peer address
    );

const auto kLinkKeyRequestNegativeReplyRsp =
    CreateStaticByteBuffer(hci::kCommandCompleteEventCode, 0x0A, 0xF0,
                           LowerBits(hci::kLinkKeyRequestNegativeReply),
                           UpperBits(hci::kLinkKeyRequestNegativeReply),
                           hci::kSuccess,          // status
                           TEST_DEV_ADDR_BYTES_LE  // peer address
    );

// Test: replies negative to Link Key Requests for unknown and unbonded peers
TEST_F(GAP_BrEdrConnectionManagerTest, LinkKeyRequestAndNegativeReply) {
  test_device()->QueueCommandTransaction(kLinkKeyRequestNegativeReply,
                                         {&kLinkKeyRequestNegativeReplyRsp});

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(1, transaction_count());

  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(peer->connected());
  ASSERT_FALSE(peer->bonded());

  test_device()->QueueCommandTransaction(kLinkKeyRequestNegativeReply,
                                         {&kLinkKeyRequestNegativeReplyRsp});

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 2, transaction_count());

  QueueDisconnection(kConnectionHandle);
}

const hci::LinkKey kRawKey({0xc0, 0xde, 0xfa, 0x57, 0x4b, 0xad, 0xf0, 0x0d,
                            0xa7, 0x60, 0x06, 0x1e, 0xca, 0x1e, 0xca, 0xfe},
                           0, 0);
const sm::LTK kLinkKey(
    sm::SecurityProperties(hci::LinkKeyType::kAuthenticatedCombination192),
    kRawKey);

const auto kLinkKeyNotification = CreateStaticByteBuffer(
    hci::kLinkKeyNotificationEventCode,
    0x17,                    // parameter_total_size (17 bytes)
    TEST_DEV_ADDR_BYTES_LE,  // peer address
    0xc0, 0xde, 0xfa, 0x57, 0x4b, 0xad, 0xf0, 0x0d, 0xa7, 0x60, 0x06, 0x1e,
    0xca, 0x1e, 0xca, 0xfe,  // link key
    0x04  // key type (Unauthenticated Combination Key generated from P-192)
);

const auto kLinkKeyRequestReply = CreateStaticByteBuffer(
    LowerBits(hci::kLinkKeyRequestReply), UpperBits(hci::kLinkKeyRequestReply),
    0x16,                    // parameter_total_size (22 bytes)
    TEST_DEV_ADDR_BYTES_LE,  // peer address
    0xc0, 0xde, 0xfa, 0x57, 0x4b, 0xad, 0xf0, 0x0d, 0xa7, 0x60, 0x06, 0x1e,
    0xca, 0x1e, 0xca, 0xfe  // link key
);

const auto kLinkKeyRequestReplyRsp = CreateStaticByteBuffer(
    hci::kCommandCompleteEventCode, 0x0A, 0xF0,
    LowerBits(hci::kLinkKeyRequestReply), UpperBits(hci::kLinkKeyRequestReply),
    hci::kSuccess,          // status
    TEST_DEV_ADDR_BYTES_LE  // peer address
);

// Test: replies to Link Key Requests for bonded peer
TEST_F(GAP_BrEdrConnectionManagerTest, RecallLinkKeyForBondedPeer) {
  ASSERT_TRUE(
      peer_cache()->AddBondedPeer(BondingData{PeerId(999), kTestDevAddr, {},
                                              {}, kLinkKey}));
  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_FALSE(peer->connected());
  ASSERT_TRUE(peer->bonded());

  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  ASSERT_TRUE(peer->connected());

  test_device()->QueueCommandTransaction(kLinkKeyRequestReply,
                                         {&kLinkKeyRequestReplyRsp});

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());

  QueueDisconnection(kConnectionHandle);
}

const auto kLinkKeyNotificationChanged = CreateStaticByteBuffer(
    hci::kLinkKeyNotificationEventCode,
    0x17,                    // parameter_total_size (17 bytes)
    TEST_DEV_ADDR_BYTES_LE,  // peer address
    0xfa, 0xce, 0xb0, 0x0c, 0xa5, 0x1c, 0xcd, 0x15, 0xea, 0x5e, 0xfe, 0xdb,
    0x1d, 0x0d, 0x0a, 0xd5,  // link key
    0x06                     // key type (Changed Combination Key)
);

const auto kLinkKeyRequestReplyChanged = CreateStaticByteBuffer(
    LowerBits(hci::kLinkKeyRequestReply), UpperBits(hci::kLinkKeyRequestReply),
    0x16,                    // parameter_total_size (22 bytes)
    TEST_DEV_ADDR_BYTES_LE,  // peer address
    0xfa, 0xce, 0xb0, 0x0c, 0xa5, 0x1c, 0xcd, 0x15, 0xea, 0x5e, 0xfe, 0xdb,
    0x1d, 0x0d, 0x0a, 0xd5  // link key
);

// Test: stores and recalls link key for a remote peer
TEST_F(GAP_BrEdrConnectionManagerTest, BondPeer) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(peer->connected());
  ASSERT_FALSE(peer->bonded());

  test_device()->SendCommandChannelPacket(kLinkKeyNotification);

  RunLoopUntilIdle();
  EXPECT_TRUE(peer->bonded());

  test_device()->QueueCommandTransaction(kLinkKeyRequestReply,
                                         {&kLinkKeyRequestReplyRsp});

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());

  // Change the link key.
  test_device()->SendCommandChannelPacket(kLinkKeyNotificationChanged);

  RunLoopUntilIdle();
  EXPECT_TRUE(peer->bonded());

  test_device()->QueueCommandTransaction(kLinkKeyRequestReplyChanged,
                                         {&kLinkKeyRequestReplyRsp});

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunLoopUntilIdle();

  EXPECT_TRUE(peer->bonded());
  EXPECT_EQ(kIncomingConnTransactions + 2, transaction_count());

  QueueDisconnection(kConnectionHandle);
}

// Test: can't change the link key of an unbonded peer
TEST_F(GAP_BrEdrConnectionManagerTest, UnbondedPeerChangeLinkKey) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(peer->connected());
  ASSERT_FALSE(peer->bonded());

  // Change the link key.
  test_device()->SendCommandChannelPacket(kLinkKeyNotificationChanged);

  RunLoopUntilIdle();
  EXPECT_FALSE(peer->bonded());

  test_device()->QueueCommandTransaction(kLinkKeyRequestNegativeReply,
                                         {&kLinkKeyRequestReplyRsp});

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunLoopUntilIdle();

  EXPECT_FALSE(peer->bonded());
  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());

  QueueDisconnection(kConnectionHandle);
}

const auto kLinkKeyNotificationLegacy = CreateStaticByteBuffer(
    hci::kLinkKeyNotificationEventCode,
    0x17,                    // parameter_total_size (17 bytes)
    TEST_DEV_ADDR_BYTES_LE,  // peer address
    0x41, 0x33, 0x7c, 0x0d, 0xef, 0xee, 0xda, 0xda, 0xba, 0xad, 0x0f, 0xf1,
    0xce, 0xc0, 0xff, 0xee,  // link key
    0x00                     // key type (Combination Key)
);

// Test: don't bond if the link key resulted from legacy pairing
TEST_F(GAP_BrEdrConnectionManagerTest, LegacyLinkKeyNotBonded) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(peer->connected());
  ASSERT_FALSE(peer->bonded());

  test_device()->SendCommandChannelPacket(kLinkKeyNotificationLegacy);

  RunLoopUntilIdle();
  EXPECT_FALSE(peer->bonded());

  test_device()->QueueCommandTransaction(kLinkKeyRequestNegativeReply,
                                         {&kLinkKeyRequestReplyRsp});

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunLoopUntilIdle();

  EXPECT_FALSE(peer->bonded());
  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());

  QueueDisconnection(kConnectionHandle);
}

// Test: if L2CAP gets a link error, we disconnect the connection
TEST_F(GAP_BrEdrConnectionManagerTest, DisconnectOnLinkError) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  // When we deallocate the connection manager next, we should disconnect.
  QueueDisconnection(kConnectionHandle);

  data_domain()->TriggerLinkError(kConnectionHandle);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());

  test_device()->QueueCommandTransaction(
      CommandTransaction(kReadScanEnable, {&kReadScanEnableRspBoth}));
  test_device()->QueueCommandTransaction(
      CommandTransaction(kWriteScanEnableInq, {&kWriteScanEnableRsp}));

  SetConnectionManager(nullptr);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 3, transaction_count());
}

TEST_F(GAP_BrEdrConnectionManagerTest, ConnectedPeerTimeout) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_TRUE(peer->connected());

  // We want to make sure the connection doesn't expire.
  RunLoopFor(zx::sec(600));

  // Remote end disconnects.
  test_device()->SendCommandChannelPacket(kDisconnectionComplete);

  RunLoopUntilIdle();

  // Peer should still be there, but not connected anymore
  peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_FALSE(peer->connected());
  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));
}

TEST_F(GAP_BrEdrConnectionManagerTest, ServiceSearch) {
  size_t search_cb_count = 0;
  auto search_cb = [&](auto id, const auto& attributes) {
    auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
    ASSERT_TRUE(peer);
    ASSERT_EQ(id, peer->identifier());
    ASSERT_EQ(1u, attributes.count(sdp::kServiceId));
    search_cb_count++;
  };

  auto search_id = connmgr()->AddServiceSearch(sdp::profile::kAudioSink,
                                               {sdp::kServiceId}, search_cb);

  fbl::RefPtr<l2cap::testing::FakeChannel> sdp_chan;
  std::optional<uint32_t> sdp_request_tid;

  data_domain()->set_channel_callback(
      [&sdp_chan, &sdp_request_tid](auto new_chan) {
        new_chan->SetSendCallback(
            [&sdp_request_tid](auto packet) {
              const auto kSearchExpectedParams = CreateStaticByteBuffer(
                  // ServiceSearchPattern
                  0x35, 0x03,        // Sequence uint8 3 bytes
                  0x19, 0x11, 0x0B,  // UUID (kAudioSink)
                  0xFF, 0xFF,        // MaxAttributeByteCount (no max)
                  // Attribute ID list
                  0x35, 0x03,        // Sequence uint8 3 bytes
                  0x09, 0x00, 0x03,  // uint16_t (kServiceId)
                  0x00               // No continuation state
              );
              // First byte should be type.
              ASSERT_LE(3u, packet->size());
              ASSERT_EQ(sdp::kServiceSearchAttributeRequest, (*packet)[0]);
              ASSERT_EQ(kSearchExpectedParams, packet->view(5));
              sdp_request_tid = (*packet)[1] << 8 || (*packet)[2];
            },
            async_get_default_dispatcher());
        sdp_chan = std::move(new_chan);
      });

  QueueSuccessfulIncomingConn();
  data_domain()->ExpectOutboundL2capChannel(kConnectionHandle, l2cap::kSDP,
                                            0x40, 0x41);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  ASSERT_TRUE(sdp_chan);
  ASSERT_TRUE(sdp_request_tid);
  ASSERT_EQ(0u, search_cb_count);

  sdp::ServiceSearchAttributeResponse rsp;
  rsp.SetAttribute(0, sdp::kServiceId, sdp::DataElement(UUID()));
  auto rsp_ptr = rsp.GetPDU(0xFFFF /* max attribute bytes */, *sdp_request_tid,
                            BufferView());

  sdp_chan->Receive(*rsp_ptr);

  RunLoopUntilIdle();

  ASSERT_EQ(1u, search_cb_count);

  // Remote end disconnects.
  test_device()->SendCommandChannelPacket(kDisconnectionComplete);

  RunLoopUntilIdle();

  sdp_request_tid.reset();

  EXPECT_TRUE(connmgr()->RemoveServiceSearch(search_id));
  EXPECT_FALSE(connmgr()->RemoveServiceSearch(search_id));

  // Second connection is shortened because we have already interrogated,
  // and we don't search for SDP services because none are registered
  test_device()->QueueCommandTransaction(
      CommandTransaction(kAcceptConnectionRequest,
                         {&kAcceptConnectionRequestRsp, &kConnectionComplete}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kReadRemoteExtended1,
      {&kReadRemoteExtendedFeaturesRsp, &kReadRemoteExtended1Complete}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kReadRemoteExtended2,
      {&kReadRemoteExtendedFeaturesRsp, &kReadRemoteExtended2Complete}));

  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunLoopUntilIdle();

  // We shouldn't have searched for anything.
  ASSERT_FALSE(sdp_request_tid);
  ASSERT_EQ(1u, search_cb_count);

  QueueDisconnection(kConnectionHandle);
}

TEST_F(GAP_BrEdrConnectionManagerTest, ConnectUnknownPeer) {
  EXPECT_FALSE(connmgr()->Connect(PeerId(456), {}));
}

TEST_F(GAP_BrEdrConnectionManagerTest, ConnectLowEnergyPeer) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddrLe, true);
  EXPECT_FALSE(connmgr()->Connect(peer->identifier(), {}));
}

// Test: user-initiated disconnection
TEST_F(GAP_BrEdrConnectionManagerTest, DisconnectClosesHciConnection) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  // Disconnecting an unknown peer should do nothing.
  EXPECT_FALSE(connmgr()->Disconnect(PeerId(999)));

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(peer->bredr()->connected());

  QueueDisconnection(kConnectionHandle);

  EXPECT_TRUE(connmgr()->Disconnect(peer->identifier()));

  RunLoopUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());
  EXPECT_FALSE(peer->bredr()->connected());

  // Disconnecting a closed connection returns false.
  EXPECT_FALSE(connmgr()->Disconnect(peer->identifier()));
}

TEST_F(GAP_BrEdrConnectionManagerTest, AddServiceSearchAll) {
  size_t search_cb_count = 0;
  auto search_cb = [&](auto id, const auto&) {
    auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
    ASSERT_TRUE(peer);
    ASSERT_EQ(id, peer->identifier());
    search_cb_count++;
  };

  connmgr()->AddServiceSearch(sdp::profile::kAudioSink, {}, search_cb);

  fbl::RefPtr<l2cap::testing::FakeChannel> sdp_chan;
  std::optional<uint32_t> sdp_request_tid;

  data_domain()->set_channel_callback(
      [&sdp_chan, &sdp_request_tid](auto new_chan) {
        new_chan->SetSendCallback(
            [&sdp_request_tid](auto packet) {
              const auto kSearchExpectedParams = CreateStaticByteBuffer(
                  // ServiceSearchPattern
                  0x35, 0x03,        // Sequence uint8 3 bytes
                  0x19, 0x11, 0x0B,  // UUID (kAudioSink)
                  0xFF, 0xFF,        // MaxAttributeByteCount (none)
                  // Attribute ID list
                  0x35, 0x05,                    // Sequence uint8 5 bytes
                  0x0A, 0x00, 0x00, 0xFF, 0xFF,  // uint32_t (all attributes)
                  0x00                           // No continuation state
              );
              // First byte should be type.
              ASSERT_LE(3u, packet->size());
              ASSERT_EQ(sdp::kServiceSearchAttributeRequest, (*packet)[0]);
              ASSERT_EQ(kSearchExpectedParams, packet->view(5));
              sdp_request_tid = (*packet)[1] << 8 || (*packet)[2];
            },
            async_get_default_dispatcher());
        sdp_chan = std::move(new_chan);
      });

  QueueSuccessfulIncomingConn();
  data_domain()->ExpectOutboundL2capChannel(kConnectionHandle, l2cap::kSDP,
                                            0x40, 0x41);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunLoopUntilIdle();

  ASSERT_TRUE(sdp_chan);
  ASSERT_TRUE(sdp_request_tid);
  ASSERT_EQ(0u, search_cb_count);

  sdp::ServiceSearchAttributeResponse rsp;
  rsp.SetAttribute(0, sdp::kServiceId, sdp::DataElement(UUID()));
  auto rsp_ptr = rsp.GetPDU(0xFFFF /* max attribute bytes */, *sdp_request_tid,
                            BufferView());

  sdp_chan->Receive(*rsp_ptr);

  RunLoopUntilIdle();

  ASSERT_EQ(1u, search_cb_count);

  QueueDisconnection(kConnectionHandle);
}

std::string FormatConnectionState(Peer::ConnectionState s) {
  switch (s) {
    case Peer::ConnectionState::kConnected:
      return "kConnected";
    case Peer::ConnectionState::kInitializing:
      return "kInitializing";
    case Peer::ConnectionState::kNotConnected:
      return "kNotConnected";
  }
  return "<Invalid state>";
}

::testing::AssertionResult IsInitializing(Peer* peer) {
  if (Peer::ConnectionState::kInitializing !=
      peer->bredr()->connection_state()) {
    return ::testing::AssertionFailure()
           << "Expected peer connection_state: kInitializing, found "
           << FormatConnectionState(peer->bredr()->connection_state());
  }
  return ::testing::AssertionSuccess();
}
::testing::AssertionResult IsConnected(Peer* peer) {
  if (Peer::ConnectionState::kConnected != peer->bredr()->connection_state()) {
    return ::testing::AssertionFailure()
           << "Expected peer connection_state: kConnected, found "
           << FormatConnectionState(peer->bredr()->connection_state());
  }
  if (peer->temporary()) {
    return ::testing::AssertionFailure()
           << "Expected peer to be non-temporary, but found temporary";
  }
  return ::testing::AssertionSuccess();
}
::testing::AssertionResult NotConnected(Peer* peer) {
  if (Peer::ConnectionState::kNotConnected !=
      peer->bredr()->connection_state()) {
    return ::testing::AssertionFailure()
           << "Expected peer connection_state: kNotConnected, found "
           << FormatConnectionState(peer->bredr()->connection_state());
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult HasConnectionTo(Peer* peer, BrEdrConnection* conn) {
  if (!conn) {
    return ::testing::AssertionFailure()
           << "Expected BrEdrConnection, but found nullptr";
  }
  if (peer->identifier() != conn->peer_id()) {
    return ::testing::AssertionFailure()
           << "Expected connection peer_id " << bt_str(peer->identifier())
           << " but found " << bt_str(conn->peer_id());
  }
  return ::testing::AssertionSuccess();
}

#define CALLBACK_EXPECT_FAILURE(status_param)       \
  ([&status_param](auto cb_status, auto conn_ref) { \
    EXPECT_FALSE(conn_ref);                         \
    status_param = cb_status;                       \
  })

// An error is received via the HCI Command cb_status event
TEST_F(GAP_BrEdrConnectionManagerTest, ConnectSinglePeerErrorStatus) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, true);

  test_device()->QueueCommandTransaction(
      CommandTransaction(kCreateConnection, {&kCreateConnectionRspError}));

  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(NotConnected(peer));

  hci::Status status;
  EXPECT_TRUE(
      connmgr()->Connect(peer->identifier(), CALLBACK_EXPECT_FAILURE(status)));
  EXPECT_TRUE(IsInitializing(peer));
  RunLoopUntilIdle();

  EXPECT_TRUE(status.is_protocol_error());
  EXPECT_EQ(hci::StatusCode::kConnectionFailedToBeEstablished,
            status.protocol_error());
  EXPECT_TRUE(NotConnected(peer));
}

::testing::AssertionResult StatusEqual(hci::StatusCode expected,
                                       hci::StatusCode actual) {
  if (expected == actual)
    return ::testing::AssertionSuccess();
  else
    return ::testing::AssertionFailure()
           << expected << " is '" << StatusCodeToString(expected) << "', "
           << actual << " is '" << StatusCodeToString(actual) << "'";
}

// Connection Complete event reports error
TEST_F(GAP_BrEdrConnectionManagerTest, ConnectSinglePeerFailure) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, true);

  test_device()->QueueCommandTransaction(CommandTransaction(
      kCreateConnection, {&kCreateConnectionRsp, &kConnectionCompleteError}));

  hci::Status status(HostError::kFailed);
  bool callback_run = false;

  auto callback = [&status, &callback_run](auto cb_status, auto conn_ref) {
    EXPECT_FALSE(conn_ref);
    status = cb_status;
    callback_run = true;
  };
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));

  RunLoopUntilIdle();

  EXPECT_TRUE(callback_run);

  EXPECT_TRUE(status.is_protocol_error());
  EXPECT_TRUE(StatusEqual(hci::StatusCode::kConnectionFailedToBeEstablished,
                          status.protocol_error()));
  EXPECT_TRUE(NotConnected(peer));
}

TEST_F(GAP_BrEdrConnectionManagerTest, ConnectSinglePeerTimeout) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, true);

  test_device()->QueueCommandTransaction(
      CommandTransaction(kCreateConnection, {&kCreateConnectionRsp}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kCreateConnectionCancel,
      {&kCreateConnectionCancelRsp, &kConnectionCompleteCanceled}));

  hci::Status status;
  auto callback = [&status](auto cb_status, auto conn_ref) {
    EXPECT_FALSE(conn_ref);
    status = cb_status;
  };

  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));
  RunLoopFor(kBrEdrCreateConnectionTimeout);
  RunLoopFor(kBrEdrCreateConnectionTimeout);
  EXPECT_FALSE(status);
  EXPECT_EQ(HostError::kTimedOut, status.error()) << status.ToString();
  EXPECT_TRUE(NotConnected(peer));
}

// Successful connection to single peer
TEST_F(GAP_BrEdrConnectionManagerTest, ConnectSinglePeer) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, true);
  EXPECT_TRUE(peer->temporary());

  // Queue up the connection
  test_device()->QueueCommandTransaction(CommandTransaction(
      kCreateConnection, {&kCreateConnectionRsp, &kConnectionComplete}));
  QueueSuccessfulInterrogation(peer->address(), kConnectionHandle);
  QueueDisconnection(kConnectionHandle);

  // Initialize as error to verify that |callback| assigns success.
  hci::Status status(HostError::kFailed);
  BrEdrConnection* conn_ref;
  auto callback = [&status, &conn_ref](auto cb_status, auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
  };

  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));
  RunLoopUntilIdle();
  EXPECT_TRUE(status);
  EXPECT_EQ(status.ToString(), hci::Status().ToString());
  EXPECT_TRUE(HasConnectionTo(peer, conn_ref));
  EXPECT_TRUE(IsConnected(peer));
}

// Connecting to an already connected peer should complete instantly
TEST_F(GAP_BrEdrConnectionManagerTest, ConnectSinglePeerAlreadyConnected) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, true);
  EXPECT_TRUE(peer->temporary());

  // Queue up the connection
  test_device()->QueueCommandTransaction(CommandTransaction(
      kCreateConnection, {&kCreateConnectionRsp, &kConnectionComplete}));
  QueueSuccessfulInterrogation(peer->address(), kConnectionHandle);
  QueueDisconnection(kConnectionHandle);

  // Initialize as error to verify that |callback| assigns success.
  hci::Status status(HostError::kFailed);
  int num_callbacks = 0;
  BrEdrConnection* conn_ref;
  auto callback = [&status, &conn_ref, &num_callbacks](auto cb_status,
                                                       auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
    ++num_callbacks;
  };

  // Connect to the peer for the first time
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));
  RunLoopUntilIdle();
  EXPECT_TRUE(status);
  EXPECT_EQ(status.ToString(), hci::Status().ToString());
  EXPECT_TRUE(HasConnectionTo(peer, conn_ref));
  EXPECT_TRUE(IsConnected(peer));
  EXPECT_EQ(num_callbacks, 1);

  // Attempt to connect again to the already connected peer
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  RunLoopUntilIdle();
  EXPECT_EQ(num_callbacks, 2);
  EXPECT_TRUE(status);
  EXPECT_EQ(status.ToString(), hci::Status().ToString());
  EXPECT_TRUE(HasConnectionTo(peer, conn_ref));
  EXPECT_TRUE(IsConnected(peer));
}

// Initiating Two Connections to the same (currently unconnected) peer should
// successfully establish both
TEST_F(GAP_BrEdrConnectionManagerTest, ConnectSinglePeerTwoInFlight) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, true);
  EXPECT_TRUE(peer->temporary());

  // Queue up the connection
  test_device()->QueueCommandTransaction(CommandTransaction(
      kCreateConnection, {&kCreateConnectionRsp, &kConnectionComplete}));
  QueueSuccessfulInterrogation(peer->address(), kConnectionHandle);
  QueueDisconnection(kConnectionHandle);

  // Initialize as error to verify that |callback| assigns success.
  hci::Status status(HostError::kFailed);
  int num_callbacks = 0;
  BrEdrConnection* conn_ref;
  auto callback = [&status, &conn_ref, &num_callbacks](auto cb_status,
                                                       auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
    ++num_callbacks;
  };

  // Launch one request, but don't run the loop
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));

  // Launch second inflight request
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));

  // Run the loop which should complete both requests
  RunLoopUntilIdle();

  EXPECT_TRUE(status);
  EXPECT_EQ(status.ToString(), hci::Status().ToString());
  EXPECT_TRUE(HasConnectionTo(peer, conn_ref));
  EXPECT_TRUE(IsConnected(peer));
  EXPECT_EQ(num_callbacks, 2);
}

TEST_F(GAP_BrEdrConnectionManagerTest, ConnectSecondPeerFirstTimesOut) {
  auto* peer_a = peer_cache()->NewPeer(kTestDevAddr, true);
  auto* peer_b = peer_cache()->NewPeer(kTestDevAddr2, true);

  // Enqueue first connection request (which will timeout and be cancelled)
  test_device()->QueueCommandTransaction(
      CommandTransaction(kCreateConnection, {&kCreateConnectionRsp}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kCreateConnectionCancel,
      {&kCreateConnectionCancelRsp, &kConnectionCompleteCanceled}));

  // Enqueue second connection (which will succeed once previous has ended)
  const hci::ConnectionHandle conn = 0x0BAB;
  QueueSuccessfulCreateConnection(peer_b, conn);
  QueueSuccessfulInterrogation(peer_b->address(), conn);
  QueueDisconnection(conn);

  // Initialize as success to verify that |callback_a| assigns failure.
  hci::Status status_a;
  auto callback_a = [&status_a](auto cb_status, auto cb_conn_ref) {
    status_a = cb_status;
    EXPECT_FALSE(cb_conn_ref);
  };

  // Initialize as error to verify that |callback_b| assigns success.
  hci::Status status_b(HostError::kFailed);
  BrEdrConnection* connection;
  auto callback_b = [&status_b, &connection](auto cb_status, auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status_b = cb_status;
    connection = std::move(cb_conn_ref);
  };

  // Launch one request (this will timeout)
  EXPECT_TRUE(connmgr()->Connect(peer_a->identifier(), callback_a));
  ASSERT_TRUE(peer_a->bredr());
  EXPECT_TRUE(IsInitializing(peer_a));

  RunLoopUntilIdle();

  // Launch second inflight request (this will wait for the first)
  EXPECT_TRUE(connmgr()->Connect(peer_b->identifier(), callback_b));
  ASSERT_TRUE(peer_b->bredr());

  // Run the loop which should complete both requests
  RunLoopFor(kBrEdrCreateConnectionTimeout);
  RunLoopFor(kBrEdrCreateConnectionTimeout);

  EXPECT_FALSE(status_a);
  EXPECT_TRUE(status_b);
  EXPECT_EQ(status_b.ToString(), hci::Status().ToString());
  EXPECT_TRUE(HasConnectionTo(peer_b, connection));
  EXPECT_TRUE(NotConnected(peer_a));
  EXPECT_TRUE(IsConnected(peer_b));
}

TEST_F(GAP_BrEdrConnectionManagerTest, DisconnectPendingConnections) {
  auto* peer_a = peer_cache()->NewPeer(kTestDevAddr, true);
  auto* peer_b = peer_cache()->NewPeer(kTestDevAddr2, true);

  // Enqueue first connection request (which will await Connection Complete)
  test_device()->QueueCommandTransaction(
      CommandTransaction(kCreateConnection, {&kCreateConnectionRsp}));
  test_device()->QueueCommandTransaction(CommandTransaction(
      kCreateConnectionCancel,
      {&kCreateConnectionCancelRsp, &kConnectionCompleteCanceled}));

  // No-op connection callbacks
  auto callback_a = [](auto, auto) {};
  auto callback_b = [](auto, auto) {};

  // Launch both requests (second one is queued. Neither completes.)
  EXPECT_TRUE(connmgr()->Connect(peer_a->identifier(), callback_a));
  EXPECT_TRUE(connmgr()->Connect(peer_b->identifier(), callback_b));

  // Put the first connection into flight.
  RETURN_IF_FATAL(RunLoopUntilIdle());

  ASSERT_TRUE(IsInitializing(peer_a));
  ASSERT_TRUE(IsInitializing(peer_b));

  EXPECT_FALSE(connmgr()->Disconnect(peer_a->identifier()));
  EXPECT_FALSE(connmgr()->Disconnect(peer_b->identifier()));
}

// TODO(BT-819) Connecting a peer that's being interrogated

#undef COMMAND_COMPLETE_RSP
#undef COMMAND_STATUS_RSP

}  // namespace
}  // namespace gap
}  // namespace bt
