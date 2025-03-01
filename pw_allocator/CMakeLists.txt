# Copyright 2023 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.

include("$ENV{PW_ROOT}/pw_build/pigweed.cmake")
include("$ENV{PW_ROOT}/pw_allocator/allocator.cmake")

if(pw_allocator_POISON_HEAP)
  set(enable_heap_poison "PW_ALLOCATOR_POISON_ENABLE=1")
endif()

pw_add_library(pw_allocator.allocator STATIC
  HEADERS
    public/pw_allocator/allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_status
  PRIVATE_DEPS
    pw_assert
    pw_bytes
  SOURCES
    allocator.cc
)

pw_add_library(pw_allocator.allocator_metric_proxy STATIC
  HEADERS
    public/pw_allocator/allocator_metric_proxy.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_metric
    pw_status
  PRIVATE_DEPS
    pw_assert
  SOURCES
    allocator_metric_proxy.cc
)

pw_add_library(pw_allocator.block STATIC
  PUBLIC_DEFINES
    ${enable_heap_poison}
  HEADERS
    public/pw_allocator/block.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_bytes
    pw_result
    pw_span
    pw_status
    pw_third_party.fuchsia.stdcompat
  PRIVATE_DEPS
    pw_assert
  SOURCES
    block.cc
)

pw_add_library(pw_allocator.fallback_allocator STATIC
  HEADERS
    public/pw_allocator/fallback_allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_assert
    pw_status
  SOURCES
    fallback_allocator.cc
)

pw_add_library(pw_allocator.freelist STATIC
  HEADERS
    public/pw_allocator/freelist.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_containers.vector
    pw_span
    pw_status
  SOURCES
    freelist.cc
)

pw_add_library(pw_allocator.freelist_heap STATIC
  HEADERS
    public/pw_allocator/freelist_heap.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.block
    pw_allocator.freelist
  PRIVATE_DEPS
    pw_assert
    pw_log
    pw_span
  SOURCES
    freelist_heap.cc
)

pw_add_library(pw_allocator.libc_allocator STATIC
  SOURCES
    libc_allocator.cc
  HEADERS
    public/pw_allocator/libc_allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_status
  PRIVATE_DEPS
    pw_assert
    pw_bytes
)

pw_add_library(pw_allocator.null_allocator INTERFACE
  HEADERS
    public/pw_allocator/null_allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.allocator
)

pw_add_library(pw_allocator.simple_allocator INTERFACE
  HEADERS
    public/pw_allocator/simple_allocator.h
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_allocator.block
    pw_bytes
)

pw_add_library(pw_allocator.split_free_list_allocator STATIC
  HEADERS
    public/pw_allocator/split_free_list_allocator.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_allocator.block
    pw_bytes
    pw_result
    pw_status
  PRIVATE_DEPS
    pw_assert
  SOURCES
    split_free_list_allocator.cc
)

pw_add_library(pw_allocator.allocator_testing STATIC
  HEADERS
    public/pw_allocator/allocator_testing.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator.allocator
    pw_allocator.block
    pw_allocator.simple_allocator
    pw_bytes
  PRIVATE_DEPS
    pw_assert
    pw_unit_test
  SOURCES
    allocator_testing.cc
)

pw_add_test(pw_allocator.allocator_test
  SOURCES
    allocator_test.cc
  PRIVATE_DEPS
    pw_allocator.allocator
    pw_allocator.allocator_testing
    pw_bytes
  GROUPS
    modules
    pw_allocator
)

pw_add_test(pw_allocator.allocator_metric_proxy_test
  SOURCES
    allocator_metric_proxy_test.cc
  PRIVATE_DEPS
    pw_allocator.allocator_metric_proxy
    pw_allocator.allocator_testing
  GROUPS
    modules
    pw_allocator
)

pw_add_test(pw_allocator.block_test
  SOURCES
    block_test.cc
  PRIVATE_DEPS
    pw_allocator.block
    pw_span
  GROUPS
    modules
    pw_allocator
)

pw_add_test(pw_allocator.fallback_allocator_test
  PRIVATE_DEPS
    pw_allocator.allocator_testing
    pw_allocator.fallback_allocator
    pw_status
  SOURCES
    fallback_allocator_test.cc
  GROUPS
    modules
    pw_allocator
)

pw_add_test(pw_allocator.freelist_test
  SOURCES
    freelist_test.cc
  PRIVATE_DEPS
    pw_allocator.freelist
    pw_span
    pw_status
  GROUPS
    modules
    pw_allocator
)

pw_add_test(pw_allocator.freelist_heap_test
  SOURCES
    freelist_heap_test.cc
  PRIVATE_DEPS
    pw_allocator.freelist_heap
  GROUPS
    modules
    pw_allocator
)

pw_add_test(pw_allocator.libc_allocator_test
  SOURCES
    libc_allocator_test.cc
  PRIVATE_DEPS
    pw_allocator.libc_allocator
    pw_unit_test
  GROUPS
    modules
    pw_allocator
)

pw_add_test(pw_allocator.null_allocator_test
  SOURCES
    null_allocator_test.cc
  PRIVATE_DEPS
    pw_allocator.null_allocator
    pw_unit_test
  GROUPS
    modules
    pw_allocator
)

pw_add_test(pw_allocator.simple_allocator_test
  SOURCES
    simple_allocator_test.cc
  PRIVATE_DEPS
    pw_allocator.allocator_testing
    pw_allocator.simple_allocator
)

pw_add_test(pw_allocator.split_free_list_allocator_test
  SOURCES
    split_free_list_allocator_test.cc
  PRIVATE_DEPS
    pw_allocator.allocator_testing
    pw_allocator.split_free_list_allocator
    pw_containers.vector
    pw_bytes
    pw_unit_test
  GROUPS
    modules
    pw_allocator
)

pw_add_test(pw_allocator.unique_ptr_test
  SOURCES
    unique_ptr_test.cc
  PRIVATE_DEPS
    pw_allocator.allocator
    pw_allocator.allocator_testing
  GROUPS
    modules
    pw_allocator
)
