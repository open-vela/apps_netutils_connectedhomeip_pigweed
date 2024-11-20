// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fragmenter.h"

#include <gtest/gtest.h>

#include "pdu.h"
#include "src/connectivity/bluetooth/core/bt-host/common/test_helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/hci.h"

namespace bt::l2cap {
namespace {

constexpr hci::ConnectionHandle kTestHandle = 0x0001;
constexpr ChannelId kTestChannelId = 0x0001;

TEST(L2CAP_FragmenterTest, OutboundFrameEmptyPayload) {
  StaticByteBuffer kExpectedFrame(
      // Basic L2CAP header (0-length Information Payload)
      0x00, 0x00, 0x01, 0x00);

  OutboundFrame frame(kTestChannelId, BufferView(), FrameCheckSequenceOption::kNoFcs);
  EXPECT_EQ(kExpectedFrame.size(), frame.size());
  decltype(kExpectedFrame) out_buffer;
  frame.WriteToFragment(out_buffer.mutable_view(), 0);

  EXPECT_TRUE(ContainersEqual(kExpectedFrame, out_buffer));
}

TEST(L2CAP_FragmenterTest, OutboundFrameEmptyPayloadWithFcs) {
  StaticByteBuffer kExpectedFrame(
      // Basic L2CAP header (2-byte Information Payload)
      0x02, 0x00, 0x01, 0x00,

      // FCS over preceding header (no other informational data)
      0x00, 0x28);

  OutboundFrame frame(kTestChannelId, BufferView(), FrameCheckSequenceOption::kIncludeFcs);
  EXPECT_EQ(kExpectedFrame.size(), frame.size());
  decltype(kExpectedFrame) out_buffer;
  frame.WriteToFragment(out_buffer.mutable_view(), 0);

  EXPECT_TRUE(ContainersEqual(kExpectedFrame, out_buffer));

  // Reset
  out_buffer.Fill(0xaa);

  // Copy just FCS footer
  frame.WriteToFragment(out_buffer.mutable_view(4), 4);
  EXPECT_EQ(0x00, out_buffer[4]);
  EXPECT_EQ(0x28, out_buffer[5]);
}

TEST(L2CAP_FragmenterTest, OutboundFrameExactFit) {
  auto payload = CreateStaticByteBuffer('T', 'e', 's', 't');

  StaticByteBuffer kExpectedFrame(
      // Basic L2CAP header
      0x04, 0x00, 0x01, 0x00,

      // Payload
      'T', 'e', 's', 't');

  OutboundFrame frame(kTestChannelId, payload, FrameCheckSequenceOption::kNoFcs);
  EXPECT_EQ(kExpectedFrame.size(), frame.size());
  decltype(kExpectedFrame) out_buffer;
  frame.WriteToFragment(out_buffer.mutable_view(), 0);

  EXPECT_TRUE(ContainersEqual(kExpectedFrame, out_buffer));
}

TEST(L2CAP_FragmenterTest, OutboundFrameExactFitWithFcs) {
  // Test data from v5.0, Vol 3, Part A, Section 3.3.5, Example 1
  StaticByteBuffer payload(0x02, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09);

  StaticByteBuffer kExpectedFrame(
      // Basic L2CAP header
      0x0e, 0x00, 0x40, 0x00,

      // Payload
      0x02, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,

      // FCS
      0x38, 0x61);

  OutboundFrame frame(ChannelId(0x0040), payload, FrameCheckSequenceOption::kIncludeFcs);
  EXPECT_EQ(kExpectedFrame.size(), frame.size());
  decltype(kExpectedFrame) out_buffer;
  frame.WriteToFragment(out_buffer.mutable_view(), 0);

  EXPECT_TRUE(ContainersEqual(kExpectedFrame, out_buffer));
}

TEST(L2CAP_FragmenterTest, OutboundFrameOffsetInHeader) {
  auto payload = CreateStaticByteBuffer('T', 'e', 's', 't');

  StaticByteBuffer kExpectedFrameChunk(
      // Basic L2CAP header (minus first byte)
      0x00, 0x01, 0x00,

      // Payload (first byte only, limited by size of output buffer)
      'T', 'e', 's', 't',

      // FCS
      0xa4, 0xc3);

  OutboundFrame frame(kTestChannelId, payload, FrameCheckSequenceOption::kIncludeFcs);
  EXPECT_EQ(sizeof(BasicHeader) + payload.size() + sizeof(FrameCheckSequence), frame.size());
  decltype(kExpectedFrameChunk) out_buffer;
  frame.WriteToFragment(out_buffer.mutable_view(), 1);

  EXPECT_TRUE(ContainersEqual(kExpectedFrameChunk, out_buffer));
}

TEST(L2CAP_FragmenterTest, OutboundFrameOffsetInPayload) {
  auto payload = CreateStaticByteBuffer('T', 'e', 's', 't');

  StaticByteBuffer kExpectedFrameChunk(
      // Payload
      'e', 's', 't',

      // First byte of FCS
      0xa4);

  OutboundFrame frame(kTestChannelId, payload, FrameCheckSequenceOption::kIncludeFcs);
  EXPECT_EQ(sizeof(BasicHeader) + payload.size() + sizeof(FrameCheckSequence), frame.size());
  decltype(kExpectedFrameChunk) out_buffer;
  frame.WriteToFragment(out_buffer.mutable_view(), sizeof(BasicHeader) + 1);

  EXPECT_TRUE(ContainersEqual(kExpectedFrameChunk, out_buffer));
}

TEST(L2CAP_FragmenterTest, OutboundFrameOffsetInFcs) {
  StaticByteBuffer payload('T', 'e', 's', 't');

  // Second and last byte of FCS
  StaticByteBuffer kExpectedFrameChunk(0xc3);

  OutboundFrame frame(kTestChannelId, payload, FrameCheckSequenceOption::kIncludeFcs);
  EXPECT_EQ(sizeof(BasicHeader) + payload.size() + sizeof(FrameCheckSequence), frame.size());
  decltype(kExpectedFrameChunk) out_buffer;
  frame.WriteToFragment(out_buffer.mutable_view(), sizeof(BasicHeader) + payload.size() + 1);

  EXPECT_TRUE(ContainersEqual(kExpectedFrameChunk, out_buffer));
}

// This isn't expected to happen from Fragmenter.
TEST(L2CAP_FragmenterTest, OutboundFrameOutBufferBigger) {
  auto payload = CreateStaticByteBuffer('T', 'e', 's', 't');

  StaticByteBuffer kExpectedFrameChunk(
      // Basic L2CAP header (minus first two bytes)
      0x01, 0x00,

      // Payload
      'T', 'e', 's', 't',

      // Extraneous unused bytes
      0, 0, 0);

  OutboundFrame frame(kTestChannelId, payload, FrameCheckSequenceOption::kNoFcs);
  EXPECT_EQ(sizeof(BasicHeader) + payload.size(), frame.size());
  decltype(kExpectedFrameChunk) out_buffer;
  out_buffer.SetToZeros();
  frame.WriteToFragment(out_buffer.mutable_view(), 2);

  EXPECT_TRUE(ContainersEqual(kExpectedFrameChunk, out_buffer));
}

TEST(L2CAP_FragmenterTest, EmptyPayload) {
  BufferView payload;

  auto expected_fragment = CreateStaticByteBuffer(
      // ACL data header
      0x01, 0x00, 0x04, 0x00,

      // Basic L2CAP header (0-length Information Payload)
      0x00, 0x00, 0x01, 0x00);

  // Make the fragment limit a lot larger than the test frame size.
  Fragmenter fragmenter(kTestHandle, 1024);
  PDU pdu = fragmenter.BuildFrame(kTestChannelId, payload, FrameCheckSequenceOption::kNoFcs);
  ASSERT_TRUE(pdu.is_valid());
  EXPECT_EQ(1u, pdu.fragment_count());

  auto fragments = pdu.ReleaseFragments();

  EXPECT_TRUE(ContainersEqual(expected_fragment, fragments.begin()->view().data()));
}

TEST(L2CAP_FragmenterTest, SingleFragment) {
  auto payload = CreateStaticByteBuffer('T', 'e', 's', 't');

  auto expected_fragment = CreateStaticByteBuffer(
      // ACL data header
      0x01, 0x00, 0x08, 0x00,

      // Basic L2CAP header
      0x04, 0x00, 0x01, 0x00, 'T', 'e', 's', 't');

  // Make the fragment limit a lot larger than the test frame size.
  Fragmenter fragmenter(kTestHandle, 1024);
  PDU pdu = fragmenter.BuildFrame(kTestChannelId, payload, FrameCheckSequenceOption::kNoFcs);
  ASSERT_TRUE(pdu.is_valid());
  EXPECT_EQ(1u, pdu.fragment_count());

  auto fragments = pdu.ReleaseFragments();

  EXPECT_TRUE(ContainersEqual(expected_fragment, fragments.begin()->view().data()));
}

TEST(L2CAP_FragmenterTest, SingleFragmentExactFit) {
  auto payload = CreateStaticByteBuffer('T', 'e', 's', 't');

  auto expected_fragment = CreateStaticByteBuffer(
      // ACL data header
      0x01, 0x00, 0x08, 0x00,

      // Basic L2CAP header
      0x04, 0x00, 0x01, 0x00, 'T', 'e', 's', 't');

  // Make the fragment limit large enough to fit exactly one B-frame containing
  // |payload|.
  Fragmenter fragmenter(kTestHandle, payload.size() + sizeof(BasicHeader));
  PDU pdu = fragmenter.BuildFrame(kTestChannelId, payload, FrameCheckSequenceOption::kNoFcs);
  ASSERT_TRUE(pdu.is_valid());
  EXPECT_EQ(1u, pdu.fragment_count());

  auto fragments = pdu.ReleaseFragments();

  EXPECT_TRUE(ContainersEqual(expected_fragment, fragments.begin()->view().data()));
}

TEST(L2CAP_FragmenterTest, TwoFragmentsOffByOne) {
  auto payload = CreateStaticByteBuffer('T', 'e', 's', 't', '!');

  auto expected_fragment0 = CreateStaticByteBuffer(
      // ACL data header
      0x01, 0x00, 0x08, 0x00,

      // Basic L2CAP header, contains the complete length but a partial payload
      0x05, 0x00, 0x01, 0x00, 'T', 'e', 's', 't');

  auto expected_fragment1 = CreateStaticByteBuffer(
      // ACL data header
      0x01, 0x10, 0x01, 0x00,

      // Continuing payload
      '!');

  // Make the fragment limit large enough to fit exactly one B-frame containing
  // 1 octet less than |payload|. The last octet should be placed in a second
  // fragment.
  Fragmenter fragmenter(kTestHandle, payload.size() + sizeof(BasicHeader) - 1);
  PDU pdu = fragmenter.BuildFrame(kTestChannelId, payload, FrameCheckSequenceOption::kNoFcs);
  ASSERT_TRUE(pdu.is_valid());
  EXPECT_EQ(2u, pdu.fragment_count());

  auto fragments = pdu.ReleaseFragments();

  EXPECT_TRUE(ContainersEqual(expected_fragment0, fragments.begin()->view().data()));
  EXPECT_TRUE(ContainersEqual(expected_fragment1, (++fragments.begin())->view().data()));
}

TEST(L2CAP_FragmenterTest, TwoFragmentsExact) {
  auto payload = CreateStaticByteBuffer('T', 'e', 's', 't');
  ZX_DEBUG_ASSERT_MSG(payload.size() % 2 == 0, "test payload size should be even");

  auto expected_fragment0 = CreateStaticByteBuffer(
      // ACL data header
      0x01, 0x00, 0x04, 0x00,

      // Basic L2CAP header, contains the complete length but a partial payload
      0x04, 0x00, 0x01, 0x00);

  auto expected_fragment1 = CreateStaticByteBuffer(
      // ACL data header
      0x01, 0x10, 0x04, 0x00,

      // Continuing payload
      'T', 'e', 's', 't');

  // Make the fragment limit large enough to fit exactly half a B-frame
  // containing |payload|. The frame should be evenly divided across two
  // fragments.
  Fragmenter fragmenter(kTestHandle, (payload.size() + sizeof(BasicHeader)) / 2);
  PDU pdu = fragmenter.BuildFrame(kTestChannelId, payload, FrameCheckSequenceOption::kNoFcs);
  ASSERT_TRUE(pdu.is_valid());
  EXPECT_EQ(2u, pdu.fragment_count());

  auto fragments = pdu.ReleaseFragments();

  EXPECT_TRUE(ContainersEqual(expected_fragment0, fragments.begin()->view().data()));
  EXPECT_TRUE(ContainersEqual(expected_fragment1, (++fragments.begin())->view().data()));
}

TEST(L2CAP_FragmenterTest, ManyFragmentsOffByOne) {
  constexpr size_t kMaxFragmentPayloadSize = 5;
  constexpr size_t kExpectedFragmentCount = 4;
  constexpr size_t kFrameSize = (kExpectedFragmentCount - 1) * kMaxFragmentPayloadSize + 1;

  StaticByteBuffer<kFrameSize - sizeof(BasicHeader)> payload;
  payload.Fill('X');

  auto expected_fragment0 = CreateStaticByteBuffer(
      // ACL data header
      0x01, 0x00, 0x05, 0x00,

      // Basic L2CAP header contains the complete length but partial payload
      0x0C, 0x00, 0x01, 0x00, 'X');

  auto expected_fragment1 = CreateStaticByteBuffer(
      // ACL data header
      0x01, 0x10, 0x05, 0x00,

      // Continuing payload
      'X', 'X', 'X', 'X', 'X');

  auto expected_fragment2 = CreateStaticByteBuffer(
      // ACL data header
      0x01, 0x10, 0x05, 0x00,

      // Continuing payload
      'X', 'X', 'X', 'X', 'X');

  auto expected_fragment3 = CreateStaticByteBuffer(
      // ACL data header
      0x01, 0x10, 0x01, 0x00,

      // Continuing payload
      'X');

  Fragmenter fragmenter(kTestHandle, kMaxFragmentPayloadSize);
  PDU pdu = fragmenter.BuildFrame(kTestChannelId, payload, FrameCheckSequenceOption::kNoFcs);
  ASSERT_TRUE(pdu.is_valid());
  EXPECT_EQ(kExpectedFragmentCount, pdu.fragment_count());

  auto fragments = pdu.ReleaseFragments();
  auto iter = fragments.begin();
  EXPECT_TRUE(ContainersEqual(expected_fragment0, (iter++)->view().data()));
  EXPECT_TRUE(ContainersEqual(expected_fragment1, (iter++)->view().data()));
  EXPECT_TRUE(ContainersEqual(expected_fragment2, (iter++)->view().data()));
  EXPECT_TRUE(ContainersEqual(expected_fragment3, iter->view().data()));
}

TEST(L2CAP_FragmenterTest, MaximalSizedPayload) {
  DynamicByteBuffer payload(65535);
  Fragmenter fragmenter(kTestHandle, 1024);
  PDU pdu = fragmenter.BuildFrame(kTestChannelId, payload, FrameCheckSequenceOption::kNoFcs);
  ASSERT_TRUE(pdu.is_valid());
  EXPECT_LT(64u, pdu.fragment_count());
}

TEST(L2CAP_FragmenterTest, FragmentsFrameCheckSequence) {
  constexpr size_t kMaxFragmentPayloadSize = 5;
  constexpr size_t kExpectedFragmentCount = 3;
  constexpr size_t kFrameSize = (kExpectedFragmentCount - 1) * kMaxFragmentPayloadSize + 1;

  StaticByteBuffer payload('0', '1', '2', '3', '4');
  EXPECT_EQ(kFrameSize - sizeof(BasicHeader) - sizeof(FrameCheckSequence), payload.size());

  StaticByteBuffer expected_fragment0(
      // ACL data header
      0x01, 0x00, kMaxFragmentPayloadSize, 0x00,

      // Basic L2CAP header contains the length of PDU (including FCS), partial payload
      0x07, 0x00, 0x01, 0x00, '0');

  StaticByteBuffer expected_fragment1(
      // ACL data header
      0x01, 0x10, kMaxFragmentPayloadSize, 0x00,

      // Remaining bytes of payload and first byte of FCS
      '1', '2', '3', '4', 0xcc);

  StaticByteBuffer expected_fragment2(
      // ACL data header
      0x01, 0x10, 0x01, 0x00,

      // Last byte of FCS
      0xe0);

  Fragmenter fragmenter(kTestHandle, kMaxFragmentPayloadSize);
  PDU pdu = fragmenter.BuildFrame(kTestChannelId, payload, FrameCheckSequenceOption::kIncludeFcs);
  ASSERT_TRUE(pdu.is_valid());
  EXPECT_EQ(kExpectedFragmentCount, pdu.fragment_count());

  auto fragments = pdu.ReleaseFragments();
  auto iter = fragments.begin();
  EXPECT_TRUE(ContainersEqual(expected_fragment0, (iter++)->view().data()));
  EXPECT_TRUE(ContainersEqual(expected_fragment1, (iter++)->view().data()));
  EXPECT_TRUE(ContainersEqual(expected_fragment2, (iter++)->view().data()));
}

}  // namespace
}  // namespace bt::l2cap
