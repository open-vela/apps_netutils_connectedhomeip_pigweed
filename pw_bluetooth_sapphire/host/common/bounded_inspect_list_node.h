// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_BOUNDED_INSPECT_LIST_NODE_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_BOUNDED_INSPECT_LIST_NODE_H_

#include <lib/sys/inspect/cpp/component.h>

#include <cstddef>
#include <queue>

namespace bt {

// This class is intended to represent a list node in Inspect, which doesn't support lists natively.
// Furthermore, it makes sure that the number of list items doesn't exceed |capacity|.
//
// Each item in BoundedInspectListNode is represented as a child node with name as index. This
// index is always increasing and does not wrap around. For example, if capacity is 3,
// then the children names are `[0, 1, 2]` on the first three additions. When a new node is
// added, `0` is popped, and the children names are `[1, 2, 3]`.
//
// Example Usage:
//    BoundedInspectListNode list(/*capacity=*/2);
//    list.AttachInspect(parent_node);
//
//    auto& item_0 = list.CreateItem();
//    item_0.node.CreateInt("property_0", 0, &item_0.values);
//
//    auto& item_1 = list.CreateItem();
//    item_1.node.CreateInt("property_A", 1, &item_1.values);
//    item_1.node.CreateInt("property_B", 2, &item_1.values);
//
// Inspect Tree:
//     example_list:
//         0:
//             property_0: 0
//         1:
//             property_A: 1
//             property_B: 2
class BoundedInspectListNode {
 public:
  struct Item {
    // The list child node with the index as it's name (0, 1, 2...).
    inspect::Node node;
    // Properties and child nodes should be added to |values| to tie their lifetime to the life
    // of the list item.
    inspect::ValueList values;
  };

  explicit BoundedInspectListNode(size_t capacity) : capacity_(capacity) {
    ZX_ASSERT(capacity_ > 0u);
  }
  ~BoundedInspectListNode() = default;

  // Attach this node as a child of |parent| with the name |name|.
  void AttachInspect(inspect::Node& parent, std::string name);

  // Add an item to the list, removing a previous item if the list is at capacity.
  // The returned item reference is valid until the next item is added (or this list node is
  // destroyed).
  Item& CreateItem();

 private:
  inspect::Node list_node_;
  size_t next_index_ = 0;
  const size_t capacity_;
  std::queue<Item> items_;
};

}  // namespace bt

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_COMMON_BOUNDED_INSPECT_LIST_NODE_H_
