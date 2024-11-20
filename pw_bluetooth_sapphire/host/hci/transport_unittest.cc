// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "transport.h"

#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/test_helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/fake_controller_test.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_controller.h"

namespace bt::hci {

namespace {

using TestingBase = bt::testing::FakeControllerTest<bt::testing::TestController>;

class HCI_TransportTest : public TestingBase {
 public:
  HCI_TransportTest() = default;
  ~HCI_TransportTest() override = default;
};

TEST_F(HCI_TransportTest, CommandChannelTimeoutShutsDownChannelAndNotifiesClosedCallback) {
  size_t closed_cb_count = 0;
  transport()->SetTransportClosedCallback([&] { closed_cb_count++; });

  constexpr zx::duration kCommandTimeout = zx::sec(12);

  auto req_reset = StaticByteBuffer(LowerBits(kReset), UpperBits(kReset),  // HCI_Reset opcode
                                    0x00                                   // parameter_total_size
  );

  // Expect the HCI_Reset command but dont send a reply back to make the command
  // time out.
  test_device()->QueueCommandTransaction(testing::CommandTransaction(req_reset, {}));
  StartTestDevice();

  size_t cb_count = 0;
  CommandChannel::TransactionId id1, id2;
  auto cb = [&cb_count, &id1, &id2](CommandChannel::TransactionId callback_id,
                                    const EventPacket& event) {
    cb_count++;
    EXPECT_TRUE(callback_id == id1 || callback_id == id2);
    EXPECT_EQ(kCommandStatusEventCode, event.event_code());

    const auto params = event.params<CommandStatusEventParams>();
    EXPECT_EQ(StatusCode::kUnspecifiedError, params.status);
    EXPECT_EQ(kReset, params.command_opcode);
  };

  auto packet = CommandPacket::New(kReset);
  id1 = cmd_channel()->SendCommand(std::move(packet), cb);
  ASSERT_NE(0u, id1);

  packet = CommandPacket::New(kReset);
  id2 = cmd_channel()->SendCommand(std::move(packet), cb);
  ASSERT_NE(0u, id2);

  // Run the loop until the command timeout task gets scheduled.
  RunLoopUntilIdle();
  ASSERT_EQ(0u, cb_count);
  EXPECT_EQ(0u, closed_cb_count);

  RunLoopFor(kCommandTimeout);
  EXPECT_EQ(2u, cb_count);
  EXPECT_EQ(1u, closed_cb_count);
}

}  // namespace
}  // namespace bt::hci
