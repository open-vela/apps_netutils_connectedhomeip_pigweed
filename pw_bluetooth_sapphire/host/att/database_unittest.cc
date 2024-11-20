// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "database.h"

#include <zircon/assert.h>

#include "gtest/gtest.h"
#include "src/connectivity/bluetooth/core/bt-host/common/test_helpers.h"

namespace bt {
namespace att {
namespace {

constexpr Handle kTestRangeStart = 1;
constexpr Handle kTestRangeEnd = 10;

constexpr UUID kTestType1((uint16_t)1);
constexpr UUID kTestType2((uint16_t)2);
constexpr UUID kTestType3((uint16_t)3);

const AccessRequirements kAllowed(false, false, false);
const sm::SecurityProperties kNoSecurity(sm::SecurityLevel::kNoSecurity, 16,
                                         false);

// Values with different lengths
const auto kTestValue1 = CreateStaticByteBuffer('x', 'x');
const auto kTestValue2 = CreateStaticByteBuffer('x', 'x', 'x');

// Returns the handles of each attribute visited by advancing |iter| until the
// end.
std::vector<Handle> IterHandles(Database::Iterator* iter) {
  ZX_DEBUG_ASSERT(iter);

  std::vector<Handle> handles;
  for (; !iter->AtEnd(); iter->Advance()) {
    handles.push_back(iter->get()->handle());
  }
  return handles;
}

TEST(ATT_DatabaseTest, NewGroupingWhileEmptyError) {
  constexpr size_t kTooLarge = kTestRangeEnd - kTestRangeStart + 1;
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  EXPECT_FALSE(db->NewGrouping(kTestType1, kTooLarge, kTestValue1));
}

TEST(ATT_DatabaseTest, NewGroupingWhileEmptyFill) {
  constexpr size_t kExact = kTestRangeEnd - kTestRangeStart;

  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  auto* grp = db->NewGrouping(kTestType1, kExact, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(kTestType1, grp->group_type());
  EXPECT_EQ(kTestRangeStart, grp->start_handle());
  EXPECT_EQ(kTestRangeEnd, grp->end_handle());

  // Ran out of space.
  EXPECT_FALSE(db->NewGrouping(kTestType1, 0, kTestValue1));
}

// This test case performs multiple insertions and removals on the same
// database.
TEST(ATT_DatabaseTest, NewGroupingMultipleInsertions) {
  // [__________]
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);

  // Insert to empty db
  // [XXX_______] (insert X)
  auto* grp = db->NewGrouping(kTestType1, 2, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(1, grp->start_handle());
  EXPECT_EQ(3, grp->end_handle());

  // Not enough space
  grp = db->NewGrouping(kTestType1, 7, kTestValue1);
  EXPECT_FALSE(grp);

  // Insert back
  // [XXXYYYYY__] (insert Y)
  grp = db->NewGrouping(kTestType1, 4, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(4, grp->start_handle());
  EXPECT_EQ(8, grp->end_handle());

  // Not enough space
  grp = db->NewGrouping(kTestType1, 2, kTestValue1);
  EXPECT_FALSE(grp);

  // Insert back
  // [XXXYYYYYZZ] (insert Z)
  grp = db->NewGrouping(kTestType1, 1, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(9, grp->start_handle());
  EXPECT_EQ(10, grp->end_handle());

  // Out of space
  EXPECT_FALSE(db->NewGrouping(kTestType1, 0, kTestValue1));

  // Remove first grouping. It should be possible to reinsert a smaller group.
  // [___YYYYYZZ]
  EXPECT_TRUE(db->RemoveGrouping(1));

  // Not enough space
  grp = db->NewGrouping(kTestType1, 3, kTestValue1);
  EXPECT_FALSE(grp);

  // Insert front
  // [XX_YYYYYZZ] (insert X)
  grp = db->NewGrouping(kTestType1, 1, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(1, grp->start_handle());
  EXPECT_EQ(2, grp->end_handle());

  // Handle doesn't exist.
  EXPECT_FALSE(db->RemoveGrouping(3));

  // Insert in the middle
  // [XXWYYYYYZZ] (insert W)
  grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(3, grp->start_handle());
  EXPECT_EQ(3, grp->end_handle());

  // [XXW_____ZZ] (remove Y)
  EXPECT_TRUE(db->RemoveGrouping(4));

  // Insert in the middle
  // [XXWAAA__ZZ] (insert A)
  grp = db->NewGrouping(kTestType1, 2, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(4, grp->start_handle());
  EXPECT_EQ(6, grp->end_handle());

  // Insert in the middle
  // [XXWAAABBZZ] (insert B)
  grp = db->NewGrouping(kTestType1, 1, kTestValue1);
  ASSERT_TRUE(grp);
  EXPECT_EQ(7, grp->start_handle());
  EXPECT_EQ(8, grp->end_handle());

  // Out of space
  EXPECT_FALSE(db->NewGrouping(kTestType1, 0, kTestValue1));
}

TEST(ATT_DatabaseTest, RemoveWhileEmpty) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  EXPECT_FALSE(db->RemoveGrouping(kTestRangeStart));
}

TEST(ATT_DatabaseTest, FindAttributeInvalidHandle) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  EXPECT_EQ(nullptr, db->FindAttribute(kInvalidHandle));
}

TEST(ATT_DatabaseTest, FindAttributeGroupingNotFound) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);

  // Create the following layout:
  //
  // handle 0x0001: occupied
  // handle 0x0002: empty
  // handle 0x0003: occupied
  db->NewGrouping(kTestType1, 0, kTestValue1)->set_active(true);
  auto* grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp->set_active(true);
  db->NewGrouping(kTestType1, 0, kTestValue1)->set_active(true);
  db->RemoveGrouping(grp->start_handle());

  EXPECT_EQ(nullptr, db->FindAttribute(0xFFFF));
  EXPECT_EQ(nullptr, db->FindAttribute(0x0002));
  EXPECT_NE(nullptr, db->FindAttribute(0x0001));
  EXPECT_NE(nullptr, db->FindAttribute(0x0003));
}

TEST(ATT_DatabaseTest, FindAttributeIncompleteGrouping) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  auto* grp = db->NewGrouping(kTestType1, 1, kTestValue1);
  EXPECT_EQ(nullptr, db->FindAttribute(grp->start_handle()));
}

TEST(ATT_DatabaseTest, FindAttributeInactiveGrouping) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  auto* grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  EXPECT_EQ(nullptr, db->FindAttribute(grp->start_handle()));
}

TEST(ATT_DatabaseTest, FindAttributeOnePerGrouping) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  auto* grp1 = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp1->set_active(true);
  auto* grp2 = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp2->set_active(true);

  EXPECT_EQ(&grp1->attributes()[0], db->FindAttribute(grp1->start_handle()));
  EXPECT_EQ(&grp2->attributes()[0], db->FindAttribute(grp2->start_handle()));
}

TEST(ATT_DatabaseTest, FindAttributeIndexIntoGrouping) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);

  auto* grp = db->NewGrouping(kTestType1, 1, kTestValue1);
  auto* attr =
      grp->AddAttribute(kTestType2, AccessRequirements(), AccessRequirements());
  grp->set_active(true);

  EXPECT_EQ(attr, db->FindAttribute(grp->end_handle()));
}

TEST(ATT_DatabaseTest, IteratorEmpty) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  auto iter = db->GetIterator(kTestRangeStart, kTestRangeEnd);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());

  // Advance should have no effect.
  iter.Advance();
  EXPECT_TRUE(iter.AtEnd());
}

TEST(ATT_DatabaseTest, IteratorGroupOnlySingleInactive) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  db->NewGrouping(kTestType1, 0, kTestValue1);

  // |grp| is not active
  auto iter = db->GetIterator(kTestRangeStart, kTestRangeEnd, nullptr,
                              true /* groups_only */);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());
}

TEST(ATT_DatabaseTest, IteratorGroupOnlySingle) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  auto grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp->set_active(true);

  // Not within range.
  auto iter = db->GetIterator(grp->start_handle() + 1, kTestRangeEnd, nullptr,
                              true /* groups_only */);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());

  iter = db->GetIterator(kTestRangeStart, kTestRangeEnd, nullptr,
                         true /* groups_only */);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(grp->start_handle(), handles[0]);
}

TEST(ATT_DatabaseTest, IteratorGroupOnlyMultiple) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  auto grp1 = db->NewGrouping(kTestType1, 0, kTestValue1);
  auto grp2 = db->NewGrouping(kTestType1, 0, kTestValue1);
  auto grp3 = db->NewGrouping(kTestType1, 0, kTestValue1);
  auto grp4 = db->NewGrouping(kTestType1, 0, kTestValue1);

  // Leave |grp2| as inactive.
  grp1->set_active(true);
  grp3->set_active(true);
  grp4->set_active(true);

  auto iter = db->GetIterator(kTestRangeStart, kTestRangeEnd, nullptr,
                              true /* groups_only */);
  EXPECT_FALSE(iter.AtEnd());

  // |grp2| should be omitted.
  auto handles = IterHandles(&iter);
  ASSERT_EQ(3u, handles.size());
  EXPECT_EQ(grp1->start_handle(), handles[0]);
  EXPECT_EQ(grp3->start_handle(), handles[1]);
  EXPECT_EQ(grp4->start_handle(), handles[2]);

  grp2->set_active(true);

  // Pick a narrow range that excludes |grp1| and |grp4|.
  iter = db->GetIterator(grp2->start_handle(), grp3->end_handle(), nullptr,
                         true /* groups_only */);
  handles = IterHandles(&iter);
  ASSERT_EQ(2u, handles.size());
  EXPECT_EQ(grp2->start_handle(), handles[0]);
  EXPECT_EQ(grp3->start_handle(), handles[1]);
}

TEST(ATT_DatabaseTest, IteratorGroupOnlySingleWithFilter) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);

  auto grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp->set_active(true);

  // No match.
  auto iter = db->GetIterator(kTestRangeStart, kTestRangeEnd, &kTestType2,
                              true /* groups_only */);
  EXPECT_TRUE(iter.AtEnd());

  iter = db->GetIterator(kTestRangeStart, kTestRangeEnd, &kTestType1,
                         true /* groups_only */);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(grp->start_handle(), handles[0]);
}

TEST(ATT_DatabaseTest, IteratorGroupOnlyManyWithFilter) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);

  auto grp1 = db->NewGrouping(kTestType1, 1, kTestValue1);  // match
  grp1->AddAttribute(kTestType1);  // match but skipped - not group decl.
  grp1->set_active(true);

  // Matching but inactive.
  db->NewGrouping(kTestType1, 0, kTestValue1);

  auto grp2 = db->NewGrouping(kTestType2, 0, kTestValue1);
  grp2->set_active(true);
  auto grp3 = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp3->set_active(true);
  auto grp4 = db->NewGrouping(kTestType2, 0, kTestValue1);
  grp4->set_active(true);
  auto grp5 = db->NewGrouping(kTestType2, 0, kTestValue1);
  grp5->set_active(true);
  auto grp6 = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp6->set_active(true);

  // Filter by |kTestType1|
  auto iter = db->GetIterator(kTestRangeStart, kTestRangeEnd, &kTestType1,
                              true /* groups_only */);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);
  ASSERT_EQ(3u, handles.size());
  EXPECT_EQ(grp1->start_handle(), handles[0]);
  EXPECT_EQ(grp3->start_handle(), handles[1]);
  EXPECT_EQ(grp6->start_handle(), handles[2]);

  // Filter by |kTestType2|
  iter = db->GetIterator(kTestRangeStart, kTestRangeEnd, &kTestType2,
                         true /* groups_only */);
  EXPECT_FALSE(iter.AtEnd());

  handles = IterHandles(&iter);
  ASSERT_EQ(3u, handles.size());
  EXPECT_EQ(grp2->start_handle(), handles[0]);
  EXPECT_EQ(grp4->start_handle(), handles[1]);
  EXPECT_EQ(grp5->start_handle(), handles[2]);

  // Search narrower range.
  iter = db->GetIterator(grp1->end_handle(), grp5->end_handle(), &kTestType1,
                         true /* groups_only */);
  EXPECT_FALSE(iter.AtEnd());

  handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(grp3->start_handle(), handles[0]);
}

TEST(ATT_DatabaseTest, IteratorSingleInactive) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  auto grp = db->NewGrouping(kTestType1, 1, kTestValue1);

  auto iter = db->GetIterator(kTestRangeStart, kTestRangeEnd);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());

  // Complete but still inactive.
  grp->AddAttribute(kTestType1);
  iter = db->GetIterator(kTestRangeStart, kTestRangeEnd);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());
}

TEST(ATT_DatabaseTest, IteratorSingle) {
  auto db = Database::Create(kTestRangeStart, kTestRangeEnd);
  auto grp = db->NewGrouping(kTestType1, 0, kTestValue1);
  grp->set_active(true);

  // Not within range.
  auto iter = db->GetIterator(grp->start_handle() + 1, kTestRangeEnd);
  EXPECT_TRUE(iter.AtEnd());
  EXPECT_FALSE(iter.get());

  iter = db->GetIterator(kTestRangeStart, kTestRangeEnd);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(grp->start_handle(), handles[0]);
}

class ATT_DatabaseIteratorManyTest : public ::testing::Test {
 public:
  ATT_DatabaseIteratorManyTest() = default;
  ~ATT_DatabaseIteratorManyTest() override = default;

 protected:
  static constexpr size_t kActiveAttrCount = 8;

  void SetUp() override {
    db_ = Database::Create(kTestRangeStart, kTestRangeEnd);

    auto grp1 = db()->NewGrouping(kTestType1, 3, kTestValue1);  // 1
    grp1->AddAttribute(kTestType2);                             // 2
    grp1->AddAttribute(kTestType2);                             // 3
    grp1->AddAttribute(kTestType1);                             // 4
    grp1->set_active(true);

    auto grp2 = db()->NewGrouping(kTestType2, 2, kTestValue1);  // 5
    grp2->AddAttribute(kTestType1);                             // 6
    grp2->AddAttribute(kTestType2);                             // 7
    grp2->set_active(true);

    auto grp3 = db()->NewGrouping(kTestType1, 1, kTestValue1);  // 8 (inactive)
    grp3->AddAttribute(kTestType2);                             // 9 (inactive)

    auto grp4 = db()->NewGrouping(kTestType1, 0, kTestValue1);  // 10
    grp4->set_active(true);
  }

  Database* db() const { return db_.get(); }

 private:
  fxl::RefPtr<Database> db_;

  DISALLOW_COPY_ASSIGN_AND_MOVE(ATT_DatabaseIteratorManyTest);
};

// static
const size_t ATT_DatabaseIteratorManyTest::kActiveAttrCount;

TEST_F(ATT_DatabaseIteratorManyTest, NoFilter) {
  auto iter = db()->GetIterator(kTestRangeStart, kTestRangeEnd);
  EXPECT_FALSE(iter.AtEnd());

  // Should cover all but the inactive attribute.
  auto handles = IterHandles(&iter);

  // All active attribute handles.
  const std::array<Handle, kActiveAttrCount> kExpected = {1, 2, 3, 4,
                                                          5, 6, 7, 10};
  ASSERT_EQ(kExpected.size(), handles.size());

  for (size_t i = 0; i < handles.size(); i++) {
    EXPECT_EQ(kExpected[i], handles[i]);
  }
}

TEST_F(ATT_DatabaseIteratorManyTest, FilterTestType1) {
  // Filter by |kTestType1|.
  auto iter = db()->GetIterator(kTestRangeStart, kTestRangeEnd, &kTestType1);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);

  // Handles of attributes with type |kTestType1|.
  const std::array<Handle, 4u> kExpected = {1, 4, 6, 10};
  ASSERT_EQ(kExpected.size(), handles.size());

  for (size_t i = 0; i < handles.size(); i++) {
    EXPECT_EQ(kExpected[i], handles[i]);
  }
}

TEST_F(ATT_DatabaseIteratorManyTest, FilterTestType2) {
  // Filter by |kTestType2|.
  auto iter = db()->GetIterator(kTestRangeStart, kTestRangeEnd, &kTestType2);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);

  // Handles of attributes with type |kTestType2|.
  const std::array<Handle, 4u> kExpected = {2, 3, 5, 7};
  ASSERT_EQ(kExpected.size(), handles.size());

  for (size_t i = 0; i < handles.size(); i++) {
    EXPECT_EQ(kExpected[i], handles[i]);
  }
}

TEST_F(ATT_DatabaseIteratorManyTest, FilterTestType3) {
  // Filter by |kTestType3|.
  auto iter = db()->GetIterator(kTestRangeStart, kTestRangeEnd, &kTestType3);
  EXPECT_TRUE(iter.AtEnd());
}

TEST_F(ATT_DatabaseIteratorManyTest, UnaryRange) {
  // Test ranges with a single attribute. Test group begin, middle, and end
  // cases.
  constexpr Handle kBegin = 5;
  constexpr Handle kMiddle = 6;
  constexpr Handle kEnd = 7;

  auto iter = db()->GetIterator(kBegin, kBegin);
  EXPECT_FALSE(iter.AtEnd());
  auto handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(kBegin, handles[0]);

  iter = db()->GetIterator(kMiddle, kMiddle);
  EXPECT_FALSE(iter.AtEnd());
  handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(kMiddle, handles[0]);

  iter = db()->GetIterator(kEnd, kEnd);
  EXPECT_FALSE(iter.AtEnd());
  handles = IterHandles(&iter);
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ(kEnd, handles[0]);
}

TEST_F(ATT_DatabaseIteratorManyTest, Range) {
  auto iter = db()->GetIterator(4, 6);
  EXPECT_FALSE(iter.AtEnd());

  auto handles = IterHandles(&iter);

  // All active attribute handles.
  const std::array<Handle, 3> kExpected = {4, 5, 6};
  ASSERT_EQ(kExpected.size(), handles.size());

  for (size_t i = 0; i < handles.size(); i++) {
    EXPECT_EQ(kExpected[i], handles[i]);
  }
}

class ATT_DatabaseExecuteWriteQueueTest : public ::testing::Test {
 public:
  ATT_DatabaseExecuteWriteQueueTest() = default;
  ~ATT_DatabaseExecuteWriteQueueTest() = default;

 protected:
  struct PendingWrite {
    PeerId peer_id;
    Handle handle;
    uint16_t offset;
    DynamicByteBuffer value;
    Attribute::WriteResultCallback result_callback;
  };

  void SetUp() override {
    db_ = Database::Create(kTestRangeStart, kTestRangeEnd);
  }

  void ExecuteWriteQueue(PeerId peer_id, PrepareWriteQueue wq,
                         const sm::SecurityProperties& security = kNoSecurity) {
    db_->ExecuteWriteQueue(peer_id, std::move(wq), security,
                           [this](Handle h, ErrorCode e) {
                             callback_count_++;
                             ecode_ = e;
                             handle_in_error_ = h;
                           });
  }

  // Sets up an attribute grouping with 4 attributes.
  void SetUpAttributes() {
    auto* grp = db()->NewGrouping(kTestType1, 3, kTestValue1);  // handle: 1
    group_decl_handle_ = grp->start_handle();

    auto* attr =
        grp->AddAttribute(kTestType2, kAllowed, kAllowed);  // handle: 2
    attr->set_write_handler(fit::bind_member(
        this, &ATT_DatabaseExecuteWriteQueueTest::WriteHandler));
    test_handle1_ = attr->handle();

    attr = grp->AddAttribute(kTestType2, kAllowed, kAllowed);  // handle: 3
    attr->set_write_handler(fit::bind_member(
        this, &ATT_DatabaseExecuteWriteQueueTest::WriteHandler));
    test_handle2_ = attr->handle();

    attr = grp->AddAttribute(kTestType2, kAllowed, kAllowed);  // handle: 4
    attr->set_write_handler(fit::bind_member(
        this, &ATT_DatabaseExecuteWriteQueueTest::WriteHandler));
    test_handle3_ = attr->handle();

    grp->set_active(true);
  }

  void ResolveNextPendingWrite(ErrorCode ecode) {
    ASSERT_FALSE(pending_writes_.empty());
    auto pw = std::move(pending_writes_.front());
    pending_writes_.pop();

    pw.result_callback(ecode);
  }

  ErrorCode ecode() const { return ecode_; }
  Handle handle_in_error() const { return handle_in_error_; }
  int callback_count() const { return callback_count_; }

  Handle group_decl_handle() const { return group_decl_handle_; }
  Handle test_handle1() const { return test_handle1_; }
  Handle test_handle2() const { return test_handle2_; }
  Handle test_handle3() const { return test_handle3_; }

  const std::queue<PendingWrite>& pending_writes() const {
    return pending_writes_;
  }

  Database* db() { return db_.get(); }

 private:
  void WriteHandler(PeerId peer_id, Handle handle, uint16_t offset,
                    const ByteBuffer& value,
                    Attribute::WriteResultCallback result_callback) {
    PendingWrite pw;
    pw.peer_id = peer_id;
    pw.handle = handle;
    pw.offset = offset;
    pw.value = DynamicByteBuffer(value);
    pw.result_callback = std::move(result_callback);

    pending_writes_.push(std::move(pw));
  }

  ErrorCode ecode_ = ErrorCode::kNoError;
  Handle handle_in_error_ = kInvalidHandle;
  int callback_count_ = 0;
  fxl::RefPtr<Database> db_;
  std::queue<PendingWrite> pending_writes_;

  // Handles of the test attributes.
  Handle group_decl_handle_ = kInvalidHandle;
  Handle test_handle1_ = kInvalidHandle;
  Handle test_handle2_ = kInvalidHandle;
  Handle test_handle3_ = kInvalidHandle;

  DISALLOW_COPY_ASSIGN_AND_MOVE(ATT_DatabaseExecuteWriteQueueTest);
};

constexpr PeerId kPeerId(1);

TEST_F(ATT_DatabaseExecuteWriteQueueTest, EmptyQueueSucceedsImmediately) {
  ExecuteWriteQueue(kPeerId, {});
  EXPECT_EQ(1, callback_count());
  EXPECT_EQ(ErrorCode::kNoError, ecode());
  EXPECT_EQ(kInvalidHandle, handle_in_error());
}

TEST_F(ATT_DatabaseExecuteWriteQueueTest, InvalidHandle) {
  constexpr Handle kHandle = 1;
  PrepareWriteQueue wq;
  wq.push(QueuedWrite(kHandle, 0, BufferView()));

  ExecuteWriteQueue(kPeerId, std::move(wq));
  EXPECT_EQ(1, callback_count());
  EXPECT_EQ(ErrorCode::kInvalidHandle, ecode());
  EXPECT_EQ(kHandle, handle_in_error());
}

TEST_F(ATT_DatabaseExecuteWriteQueueTest, ValueLength) {
  auto* grp = db()->NewGrouping(kTestType1, 1, kTestValue1);
  auto* attr = grp->AddAttribute(kTestType2);
  grp->set_active(true);

  PrepareWriteQueue wq;
  wq.push(QueuedWrite(attr->handle(), 0,
                      DynamicByteBuffer(kMaxAttributeValueLength + 1)));

  ExecuteWriteQueue(kPeerId, std::move(wq));
  EXPECT_EQ(1, callback_count());
  EXPECT_EQ(ErrorCode::kInvalidAttributeValueLength, ecode());
  EXPECT_EQ(attr->handle(), handle_in_error());
}

TEST_F(ATT_DatabaseExecuteWriteQueueTest, WritingStaticValueNotPermitted) {
  auto* grp = db()->NewGrouping(kTestType1, 1, kTestValue1);
  auto* attr = grp->AddAttribute(kTestType2);  // read/write not permitted
  grp->set_active(true);

  PrepareWriteQueue wq;
  wq.push(QueuedWrite(attr->handle(), 0, kTestValue1));

  ExecuteWriteQueue(kPeerId, std::move(wq));
  EXPECT_EQ(1, callback_count());
  EXPECT_EQ(ErrorCode::kWriteNotPermitted, ecode());
  EXPECT_EQ(attr->handle(), handle_in_error());
}

TEST_F(ATT_DatabaseExecuteWriteQueueTest, SecurityChecks) {
  auto* grp = db()->NewGrouping(kTestType1, 1, kTestValue1);
  auto* attr =
      grp->AddAttribute(kTestType2, att::AccessRequirements(),
                        att::AccessRequirements(
                            true, false, false));  // write requires encryption
  grp->set_active(true);

  PrepareWriteQueue wq;
  wq.push(QueuedWrite(attr->handle(), 0, kTestValue1));

  ExecuteWriteQueue(kPeerId, std::move(wq), kNoSecurity);
  EXPECT_EQ(1, callback_count());
  EXPECT_EQ(ErrorCode::kInsufficientAuthentication, ecode());
  EXPECT_EQ(attr->handle(), handle_in_error());

  // Request should succeed with an encrypted link.
  ExecuteWriteQueue(
      kPeerId, std::move(wq),
      sm::SecurityProperties(sm::SecurityLevel::kEncrypted, 16, false));
  EXPECT_EQ(2, callback_count());
  EXPECT_EQ(ErrorCode::kNoError, ecode());
}

// If an error is caught before delivering the request to the delegate then we
// expect subsequent entries to not be delivered.
TEST_F(ATT_DatabaseExecuteWriteQueueTest, UndelegatedWriteErrorAborts) {
  SetUpAttributes();
  PrepareWriteQueue wq;

  // Queue a request to one of the delegated handles. This won't generate a
  // result until we explicitly send a reply via ResolveNextPendingWrite().
  wq.push(QueuedWrite(test_handle1(), 0, kTestValue1));

  // Queue a write to the group declaration handle. This should get rejected
  // right away since it's not writable. Since the database catches this error
  // internally, we expect the following queued writes to get aborted.
  wq.push(QueuedWrite(group_decl_handle(), 0, kTestValue1));

  // Queue more writes.
  wq.push(QueuedWrite(test_handle2(), 1, kTestValue2));
  wq.push(QueuedWrite(test_handle3(), 2, kTestValue1));
  wq.push(QueuedWrite(test_handle1(), 3, kTestValue2));

  ExecuteWriteQueue(kPeerId, std::move(wq));

  // The database should have generated an error response.
  EXPECT_EQ(1, callback_count());
  EXPECT_EQ(ErrorCode::kWriteNotPermitted, ecode());
  EXPECT_EQ(group_decl_handle(), handle_in_error());

  // Only the first write should have been delivered.
  ASSERT_EQ(1u, pending_writes().size());
  EXPECT_EQ(test_handle1(), pending_writes().front().handle);
  EXPECT_EQ(0, pending_writes().front().offset);
  EXPECT_TRUE(ContainersEqual(kTestValue1, pending_writes().front().value));

  // Resolving the request should have no effect.
  ResolveNextPendingWrite(ErrorCode::kUnlikelyError);
  EXPECT_EQ(1, callback_count());  // still 1
}

TEST_F(ATT_DatabaseExecuteWriteQueueTest, ErrorInMultipleQueuedWrites) {
  SetUpAttributes();
  PrepareWriteQueue wq;

  // Queue writes to the writable handles in arbitrary order. We expect all of
  // them to be delivered to the delegate.
  wq.push(QueuedWrite(test_handle1(), 0, kTestValue1));
  wq.push(QueuedWrite(test_handle2(), 1, kTestValue2));
  wq.push(QueuedWrite(test_handle3(), 2, kTestValue1));
  wq.push(QueuedWrite(test_handle1(), 3, kTestValue2));

  ExecuteWriteQueue(kPeerId, std::move(wq));

  // The execute write request should be pending.
  EXPECT_EQ(0, callback_count());

  // All 4 requests should have been delivered.
  ASSERT_EQ(4u, pending_writes().size());

  // Resolve the first request with success. The execute write request should
  // remain pending.
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle1(), next.handle);
    EXPECT_EQ(0, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue1, next.value));

    ResolveNextPendingWrite(ErrorCode::kNoError);
    EXPECT_EQ(0, callback_count());
  }

  // Resolve the second request with an error.
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle2(), next.handle);
    EXPECT_EQ(1, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue2, next.value));

    ResolveNextPendingWrite(ErrorCode::kUnlikelyError);
    EXPECT_EQ(1, callback_count());
    EXPECT_EQ(ErrorCode::kUnlikelyError, ecode());
    EXPECT_EQ(test_handle2(), handle_in_error());
  }

  // Resolving the remaining writes should have no effect.
  ResolveNextPendingWrite(ErrorCode::kNoError);
  ResolveNextPendingWrite(ErrorCode::kUnlikelyError);
  EXPECT_EQ(1, callback_count());
}

TEST_F(ATT_DatabaseExecuteWriteQueueTest, MultipleQueuedWritesSucceed) {
  SetUpAttributes();
  PrepareWriteQueue wq;

  // Queue writes to the writable handles in arbitrary order. We expect all of
  // them to be delivered to the delegate.
  wq.push(QueuedWrite(test_handle1(), 0, kTestValue1));
  wq.push(QueuedWrite(test_handle2(), 1, kTestValue2));
  wq.push(QueuedWrite(test_handle3(), 2, kTestValue1));
  wq.push(QueuedWrite(test_handle1(), 3, kTestValue2));

  ExecuteWriteQueue(kPeerId, std::move(wq));

  // The execute write request should be pending.
  EXPECT_EQ(0, callback_count());

  // All 4 requests should have been delivered.
  ASSERT_EQ(4u, pending_writes().size());

  // Resolve all requests with success.
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle1(), next.handle);
    EXPECT_EQ(0, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue1, next.value));

    ResolveNextPendingWrite(ErrorCode::kNoError);
    EXPECT_EQ(0, callback_count());
  }
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle2(), next.handle);
    EXPECT_EQ(1, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue2, next.value));

    ResolveNextPendingWrite(ErrorCode::kNoError);
    EXPECT_EQ(0, callback_count());
  }
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle3(), next.handle);
    EXPECT_EQ(2, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue1, next.value));

    ResolveNextPendingWrite(ErrorCode::kNoError);
    EXPECT_EQ(0, callback_count());
  }

  // Resolving the last request should complete the execute write request.
  {
    const auto& next = pending_writes().front();
    EXPECT_EQ(kPeerId, next.peer_id);
    EXPECT_EQ(test_handle1(), next.handle);
    EXPECT_EQ(3, next.offset);
    EXPECT_TRUE(ContainersEqual(kTestValue2, next.value));

    ResolveNextPendingWrite(ErrorCode::kNoError);
    EXPECT_EQ(1, callback_count());
    EXPECT_EQ(ErrorCode::kNoError, ecode());
  }
}

}  // namespace
}  // namespace att
}  // namespace bt
