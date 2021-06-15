// Copyright 2020 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
#pragma once

#include <tuple>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_containers/vector.h"
#include "pw_preprocessor/arguments.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/internal/hash.h"
#include "pw_rpc/internal/method_lookup.h"
#include "pw_rpc/internal/nanopb_method.h"
#include "pw_rpc/internal/packet.h"
#include "pw_rpc/internal/server.h"
#include "pw_rpc_private/fake_channel_output.h"

namespace pw::rpc {

// Declares a context object that may be used to invoke an RPC. The context is
// declared with the name of the implemented service and the method to invoke.
// The RPC can then be invoked with the call method.
//
// For a unary RPC, context.call(request) returns the status, and the response
// struct can be accessed via context.response().
//
//   PW_NANOPB_TEST_METHOD_CONTEXT(my::CoolService, TheMethod) context;
//   EXPECT_EQ(OkStatus(), context.call({.some_arg = 123}));
//   EXPECT_EQ(500, context.response().some_response_value);
//
// For a server streaming RPC, context.call(request) invokes the method. As in a
// normal RPC, the method completes when the ServerWriter's Finish method is
// called (or it goes out of scope).
//
//   PW_NANOPB_TEST_METHOD_CONTEXT(my::CoolService, TheStreamingMethod) context;
//   context.call({.some_arg = 123});
//
//   EXPECT_TRUE(context.done());  // Check that the RPC completed
//   EXPECT_EQ(OkStatus(), context.status());  // Check the status
//
//   EXPECT_EQ(3u, context.responses().size());
//   EXPECT_EQ(123, context.responses()[0].value); // check individual responses
//
//   for (const MyResponse& response : context.responses()) {
//     // iterate over the responses
//   }
//
// PW_NANOPB_TEST_METHOD_CONTEXT forwards its constructor arguments to the
// underlying serivce. For example:
//
//   PW_NANOPB_TEST_METHOD_CONTEXT(MyService, Go) context(service, args);
//
// PW_NANOPB_TEST_METHOD_CONTEXT takes two optional arguments:
//
//   size_t kMaxResponse: maximum responses to store; ignored unless streaming
//   size_t kOutputSizeBytes: buffer size; must be large enough for a packet
//
// Example:
//
//   PW_NANOPB_TEST_METHOD_CONTEXT(MyService, BestMethod, 3, 256) context;
//   ASSERT_EQ(3u, context.responses().max_size());
//
#define PW_NANOPB_TEST_METHOD_CONTEXT(service, method, ...)              \
  ::pw::rpc::NanopbTestMethodContext<service,                            \
                                     &service::method,                   \
                                     ::pw::rpc::internal::Hash(#method), \
                                     ##__VA_ARGS__>
template <typename Service,
          auto kMethod,
          uint32_t kMethodId,
          size_t kMaxResponse = 4,
          size_t kOutputSizeBytes = 128>
class NanopbTestMethodContext;

// Internal classes that implement NanopbTestMethodContext.
namespace internal::test::nanopb {

// A ChannelOutput implementation that stores the outgoing payloads and status.
template <typename Response>
class MessageOutput final : public FakeChannelOutput {
 public:
  MessageOutput(const internal::NanopbMethod& kMethod,
                Vector<Response>& responses,
                ByteSpan packet_buffer,
                bool server_streaming)
      : FakeChannelOutput(packet_buffer, server_streaming),
        method_(kMethod),
        responses_(responses) {}

 private:
  void AppendResponse(ConstByteSpan response) override {
    // If we run out of space, the back message is always the most recent.
    responses_.emplace_back();
    responses_.back() = {};
    PW_ASSERT(method_.serde().DecodeResponse(response, &responses_.back()));
  }

  void ClearResponses() override { responses_.clear(); }

  const internal::NanopbMethod& method_;
  Vector<Response>& responses_;
};

// Collects everything needed to invoke a particular RPC.
template <typename Service,
          auto kMethod,
          uint32_t kMethodId,
          size_t kMaxResponse,
          size_t kOutputSize>
struct InvocationContext {
  using Request = internal::Request<kMethod>;
  using Response = internal::Response<kMethod>;

  template <typename... Args>
  InvocationContext(Args&&... args)
      : output(MethodLookup::GetNanopbMethod<Service, kMethodId>(),
               responses,
               buffer,
               MethodTraits<decltype(kMethod)>::kServerStreaming),
        channel(Channel::Create<123>(&output)),
        server(std::span(&channel, 1)),
        service(std::forward<Args>(args)...),
        call(static_cast<internal::Server&>(server),
             static_cast<internal::Channel&>(channel),
             service,
             MethodLookup::GetNanopbMethod<Service, kMethodId>()) {}

  MessageOutput<Response> output;

  rpc::Channel channel;
  rpc::Server server;
  Service service;
  Vector<Response, kMaxResponse> responses;
  std::array<std::byte, kOutputSize> buffer = {};

  internal::ServerCall call;
};

// Method invocation context for a unary RPC. Returns the status in call() and
// provides the response through the response() method.
template <typename Service,
          auto kMethod,
          uint32_t kMethodId,
          size_t kOutputSize>
class UnaryContext {
 private:
  InvocationContext<Service, kMethod, kMethodId, 1, kOutputSize> ctx_;

 public:
  using Request = typename decltype(ctx_)::Request;
  using Response = typename decltype(ctx_)::Response;

  template <typename... Args>
  UnaryContext(Args&&... args) : ctx_(std::forward<Args>(args)...) {}

  Service& service() { return ctx_.service; }

  // Invokes the RPC with the provided request. Returns the status.
  Status call(const Request& request) {
    ctx_.output.clear();
    ctx_.responses.emplace_back();
    ctx_.responses.back() = {};
    return CallMethodImplFunction<kMethod>(
        ctx_.call, request, ctx_.responses.back());
  }

  // Gives access to the RPC's response.
  const Response& response() const {
    PW_ASSERT(ctx_.responses.size() > 0u);
    return ctx_.responses.back();
  }
};

// Method invocation context for a server streaming RPC.
template <typename Service,
          auto kMethod,
          uint32_t kMethodId,
          size_t kMaxResponse,
          size_t kOutputSize>
class ServerStreamingContext {
 private:
  InvocationContext<Service, kMethod, kMethodId, kMaxResponse, kOutputSize>
      ctx_;

 public:
  using Request = typename decltype(ctx_)::Request;
  using Response = typename decltype(ctx_)::Response;

  template <typename... Args>
  ServerStreamingContext(Args&&... args) : ctx_(std::forward<Args>(args)...) {}

  Service& service() { return ctx_.service; }

  // Invokes the RPC with the provided request.
  void call(const Request& request) {
    ctx_.output.clear();
    NanopbServerWriter<Response> writer(ctx_.call);
    return CallMethodImplFunction<kMethod>(ctx_.call, request, writer);
  }

  // Returns a server writer which writes responses into the context's buffer.
  // This should not be called alongside call(); use one or the other.
  NanopbServerWriter<Response> writer() {
    ctx_.output.clear();
    return NanopbServerWriter<Response>(ctx_.call);
  }

  // Returns the responses that have been recorded. The maximum number of
  // responses is responses().max_size(). responses().back() is always the most
  // recent response, even if total_responses() > responses().max_size().
  const Vector<Response>& responses() const { return ctx_.responses; }

  // The total number of responses sent, which may be larger than
  // responses.max_size().
  size_t total_responses() const { return ctx_.output.total_responses(); }

  // True if the stream has terminated.
  bool done() const { return ctx_.output.done(); }

  // The status of the stream. Only valid if done() is true.
  Status status() const {
    PW_ASSERT(done());
    return ctx_.output.last_status();
  }
};

// Alias to select the type of the context object to use based on which type of
// RPC it is for.
template <typename Service,
          auto kMethod,
          uint32_t kMethodId,
          size_t kMaxResponse,
          size_t kOutputSize>
using Context = std::tuple_element_t<
    static_cast<size_t>(internal::MethodTraits<decltype(kMethod)>::kType),
    std::tuple<UnaryContext<Service, kMethod, kMethodId, kOutputSize>,
               ServerStreamingContext<Service,
                                      kMethod,
                                      kMethodId,
                                      kMaxResponse,
                                      kOutputSize>
               // TODO(hepler): Support client and bidi streaming
               >>;

}  // namespace internal::test::nanopb

template <typename Service,
          auto kMethod,
          uint32_t kMethodId,
          size_t kMaxResponse,
          size_t kOutputSizeBytes>
class NanopbTestMethodContext
    : public internal::test::nanopb::
          Context<Service, kMethod, kMethodId, kMaxResponse, kOutputSizeBytes> {
 public:
  // Forwards constructor arguments to the service class.
  template <typename... ServiceArgs>
  NanopbTestMethodContext(ServiceArgs&&... service_args)
      : internal::test::nanopb::Context<Service,
                                        kMethod,
                                        kMethodId,
                                        kMaxResponse,
                                        kOutputSizeBytes>(
            std::forward<ServiceArgs>(service_args)...) {}
};

}  // namespace pw::rpc
