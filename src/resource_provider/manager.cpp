// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "resource_provider/manager.hpp"

#include <string>
#include <utility>

#include <glog/logging.h>

#include <mesos/http.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <mesos/v1/resource_provider/resource_provider.hpp>

#include <process/collect.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/protobuf.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"
#include "common/recordio.hpp"
#include "common/resources_utils.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "resource_provider/validation.hpp"

namespace http = process::http;

using std::list;
using std::string;

using mesos::internal::resource_provider::validation::call::validate;

using mesos::resource_provider::Call;
using mesos::resource_provider::Event;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::ProcessBase;
using process::Promise;
using process::Queue;

using process::collect;
using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::OK;
using process::http::MethodNotAllowed;
using process::http::NotAcceptable;
using process::http::NotImplemented;
using process::http::Pipe;
using process::http::UnsupportedMediaType;

using process::http::authentication::Principal;


namespace mesos {
namespace internal {

// Represents the streaming HTTP connection to a resource provider.
struct HttpConnection
{
  HttpConnection(const http::Pipe::Writer& _writer,
                 ContentType _contentType,
                 UUID _streamId)
    : writer(_writer),
      contentType(_contentType),
      streamId(_streamId),
      encoder(lambda::bind(serialize, contentType, lambda::_1)) {}

  // Converts the message to an Event before sending.
  template <typename Message>
  bool send(const Message& message)
  {
    // We need to evolve the internal 'message' into a
    // 'v1::resource_provider::Event'.
    return writer.write(encoder.encode(evolve(message)));
  }

  bool close()
  {
    return writer.close();
  }

  Future<Nothing> closed() const
  {
    return writer.readerClosed();
  }

  http::Pipe::Writer writer;
  ContentType contentType;
  UUID streamId;
  ::recordio::Encoder<v1::resource_provider::Event> encoder;
};


struct ResourceProvider
{
  ResourceProvider(
      const ResourceProviderInfo& _info,
      const HttpConnection& _http)
    : info(_info),
      http(_http) {}

  ~ResourceProvider()
  {
    LOG(INFO) << "Terminating resource provider " << info.id();

    http.close();

    foreachvalue (const Owned<Promise<Nothing>>& publish, publishes) {
      publish->fail(
          "Failed to publish resources from resource provider " +
          stringify(info.id()) + ": Connection closed");
    }
  }

  ResourceProviderInfo info;
  HttpConnection http;
  hashmap<UUID, Owned<Promise<Nothing>>> publishes;
};


class ResourceProviderManagerProcess
  : public Process<ResourceProviderManagerProcess>
{
public:
  ResourceProviderManagerProcess();

  Future<http::Response> api(
      const http::Request& request,
      const Option<Principal>& principal);

  void applyOfferOperation(const ApplyOfferOperationMessage& message);

  void acknowledgeOfferOperationUpdate(
      const OfferOperationUpdateAcknowledgementMessage& message);

  void reconcileOfferOperations(const ReconcileOfferOperationsMessage& message);

  Future<Nothing> publishResources(const Resources& resources);

  Queue<ResourceProviderMessage> messages;

private:
  void subscribe(
      const HttpConnection& http,
      const Call::Subscribe& subscribe);

  void updateOfferOperationStatus(
      ResourceProvider* resourceProvider,
      const Call::UpdateOfferOperationStatus& update);

  void updateState(
      ResourceProvider* resourceProvider,
      const Call::UpdateState& update);

  void updatePublishResourcesStatus(
      ResourceProvider* resourceProvider,
      const Call::UpdatePublishResourcesStatus& update);

  ResourceProviderID newResourceProviderId();

  struct ResourceProviders
  {
    hashmap<ResourceProviderID, Owned<ResourceProvider>> subscribed;
  } resourceProviders;
};


ResourceProviderManagerProcess::ResourceProviderManagerProcess()
  : ProcessBase(process::ID::generate("resource-provider-manager"))
{
}


Future<http::Response> ResourceProviderManagerProcess::api(
    const http::Request& request,
    const Option<Principal>& principal)
{
  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  v1::resource_provider::Call v1Call;

  // TODO(anand): Content type values are case-insensitive.
  Option<string> contentType = request.headers.get("Content-Type");

  if (contentType.isNone()) {
    return BadRequest("Expecting 'Content-Type' to be present");
  }

  if (contentType.get() == APPLICATION_PROTOBUF) {
    if (!v1Call.ParseFromString(request.body)) {
      return BadRequest("Failed to parse body into Call protobuf");
    }
  } else if (contentType.get() == APPLICATION_JSON) {
    Try<JSON::Value> value = JSON::parse(request.body);
    if (value.isError()) {
      return BadRequest("Failed to parse body into JSON: " + value.error());
    }

    Try<v1::resource_provider::Call> parse =
      ::protobuf::parse<v1::resource_provider::Call>(value.get());

    if (parse.isError()) {
      return BadRequest("Failed to convert JSON into Call protobuf: " +
                        parse.error());
    }

    v1Call = parse.get();
  } else {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' of ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
  }

  Call call = devolve(v1Call);

  Option<Error> error = validate(call);
  if (error.isSome()) {
    return BadRequest(
        "Failed to validate resource_provider::Call: " + error->message);
  }

  if (call.type() == Call::SUBSCRIBE) {
    // We default to JSON 'Content-Type' in the response since an empty
    // 'Accept' header results in all media types considered acceptable.
    ContentType acceptType = ContentType::JSON;

    if (request.acceptsMediaType(APPLICATION_JSON)) {
      acceptType = ContentType::JSON;
    } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
      acceptType = ContentType::PROTOBUF;
    } else {
      return NotAcceptable(
          string("Expecting 'Accept' to allow ") +
          "'" + APPLICATION_PROTOBUF + "' or '" + APPLICATION_JSON + "'");
    }

    if (request.headers.contains("Mesos-Stream-Id")) {
      return BadRequest(
          "Subscribe calls should not include the 'Mesos-Stream-Id' header");
    }

    Pipe pipe;
    OK ok;

    ok.headers["Content-Type"] = stringify(acceptType);
    ok.type = http::Response::PIPE;
    ok.reader = pipe.reader();

    // Generate a stream ID and return it in the response.
    UUID streamId = UUID::random();
    ok.headers["Mesos-Stream-Id"] = streamId.toString();

    HttpConnection http(pipe.writer(), acceptType, streamId);
    subscribe(http, call.subscribe());

    return ok;
  }

  if (!resourceProviders.subscribed.contains(call.resource_provider_id())) {
    return BadRequest("Resource provider is not subscribed");
  }

  ResourceProvider* resourceProvider =
    resourceProviders.subscribed.at(call.resource_provider_id()).get();

  // This isn't a `SUBSCRIBE` call, so the request should include a stream ID.
  if (!request.headers.contains("Mesos-Stream-Id")) {
    return BadRequest(
        "All non-subscribe calls should include to 'Mesos-Stream-Id' header");
  }

  const string& streamId = request.headers.at("Mesos-Stream-Id");
  if (streamId != resourceProvider->http.streamId.toString()) {
    return BadRequest(
        "The stream ID '" + streamId + "' included in this request "
        "didn't match the stream ID currently associated with "
        " resource provider ID " + resourceProvider->info.id().value());
  }

  switch(call.type()) {
    case Call::UNKNOWN: {
      return NotImplemented();
    }

    case Call::SUBSCRIBE: {
      // `SUBSCRIBE` call should have been handled above.
      LOG(FATAL) << "Unexpected 'SUBSCRIBE' call";
    }

    case Call::UPDATE_OFFER_OPERATION_STATUS: {
      updateOfferOperationStatus(
          resourceProvider,
          call.update_offer_operation_status());

      return Accepted();
    }

    case Call::UPDATE_STATE: {
      updateState(resourceProvider, call.update_state());
      return Accepted();
    }

    case Call::UPDATE_PUBLISH_RESOURCES_STATUS: {
      updatePublishResourcesStatus(
          resourceProvider,
          call.update_publish_resources_status());
      return Accepted();
    }
  }

  UNREACHABLE();
}


void ResourceProviderManagerProcess::applyOfferOperation(
    const ApplyOfferOperationMessage& message)
{
  const Offer::Operation& operation = message.operation_info();
  const FrameworkID& frameworkId = message.framework_id();

  Try<UUID> uuid = UUID::fromBytes(message.operation_uuid());
  if (uuid.isError()) {
    LOG(ERROR) << "Failed to parse offer operation UUID for operation "
               << "'" << operation.id() << "' from framework "
               << frameworkId << ": " << uuid.error();
    return;
  }

  Result<ResourceProviderID> resourceProviderId =
    getResourceProviderId(operation);

  if (!resourceProviderId.isSome()) {
    LOG(ERROR) << "Failed to get the resource provider ID of operation "
               << "'" << operation.id() << "' (uuid: " << uuid->toString()
               << ") from framework " << frameworkId << ": "
               << (resourceProviderId.isError() ? resourceProviderId.error()
                                                : "Not found");
    return;
  }

  if (!resourceProviders.subscribed.contains(resourceProviderId.get())) {
    LOG(WARNING) << "Dropping operation '" << operation.id() << "' (uuid: "
                 << uuid.get() << ") from framework " << frameworkId
                 << " because resource provider " << resourceProviderId.get()
                 << " is not subscribed";
    return;
  }

  ResourceProvider* resourceProvider =
    resourceProviders.subscribed.at(resourceProviderId.get()).get();

  CHECK(message.resource_version_uuid().has_resource_provider_id());

  CHECK_EQ(message.resource_version_uuid().resource_provider_id(),
           resourceProviderId.get())
    << "Resource provider ID "
    << message.resource_version_uuid().resource_provider_id()
    << " in resource version UUID does not match that in the operation "
    << resourceProviderId.get();

  Event event;
  event.set_type(Event::APPLY_OFFER_OPERATION);
  event.mutable_apply_offer_operation()
    ->mutable_framework_id()->CopyFrom(frameworkId);
  event.mutable_apply_offer_operation()->mutable_info()->CopyFrom(operation);
  event.mutable_apply_offer_operation()
    ->set_operation_uuid(message.operation_uuid());
  event.mutable_apply_offer_operation()->set_resource_version_uuid(
      message.resource_version_uuid().uuid());

  if (!resourceProvider->http.send(event)) {
    LOG(WARNING) << "Failed to send operation '" << operation.id() << "' "
                 << "(uuid: " << uuid.get() << ") from framework "
                 << frameworkId << " to resource provider "
                 << resourceProviderId.get() << ": connection closed";
  }
}


void ResourceProviderManagerProcess::acknowledgeOfferOperationUpdate(
    const OfferOperationUpdateAcknowledgementMessage& message)
{
  CHECK(message.has_resource_provider_id());

  if (!resourceProviders.subscribed.contains(message.resource_provider_id())) {
    LOG(WARNING) << "Dropping offer operation update acknowledgement with"
                 << " status_uuid " << message.status_uuid() << " and"
                 << " operation_uuid " << message.operation_uuid() << " because"
                 << " resource provider " << message.resource_provider_id()
                 << " is not subscribed";
    return;
  }

  ResourceProvider& resourceProvider =
    *resourceProviders.subscribed.at(message.resource_provider_id());

  Event event;
  event.set_type(Event::ACKNOWLEDGE_OFFER_OPERATION);
  event.mutable_acknowledge_offer_operation()
    ->set_status_uuid(message.status_uuid());
  event.mutable_acknowledge_offer_operation()
    ->set_operation_uuid(message.operation_uuid());

  if (!resourceProvider.http.send(event)) {
    LOG(WARNING) << "Failed to send offer operation update acknowledgement with"
                 << " status_uuid " << message.status_uuid() << " and"
                 << " operation_uuid " << message.operation_uuid() << " to"
                 << " resource provider " << message.resource_provider_id()
                 << ": connection closed";
  }
}


void ResourceProviderManagerProcess::reconcileOfferOperations(
    const ReconcileOfferOperationsMessage& message)
{
  hashmap<ResourceProviderID, Event> events;

  auto addOperation =
    [&events](const ReconcileOfferOperationsMessage::Operation& operation) {
      const ResourceProviderID resourceProviderId =
        operation.resource_provider_id();

      if (events.contains(resourceProviderId)) {
        events.at(resourceProviderId).mutable_reconcile_offer_operations()
          ->add_operation_uuids(operation.operation_uuid());
      } else {
        Event event;
        event.set_type(Event::RECONCILE_OFFER_OPERATIONS);
        event.mutable_reconcile_offer_operations()
          ->add_operation_uuids(operation.operation_uuid());

        events[resourceProviderId] = event;
      }
  };

  // Construct events for individual resource providers.
  foreach (
      const ReconcileOfferOperationsMessage::Operation& operation,
      message.operations()) {
    if (operation.has_resource_provider_id()) {
      if (!resourceProviders.subscribed.contains(
              operation.resource_provider_id())) {
        LOG(WARNING) << "Dropping offer operation reconciliation message with"
                     << " operation_uuid " << operation.operation_uuid()
                     << " because resource provider "
                     << operation.resource_provider_id()
                     << " is not subscribed";
        continue;
      }

      addOperation(operation);
    }
  }

  foreachpair (
      const ResourceProviderID& resourceProviderId,
      const Event& event,
      events) {
    CHECK(resourceProviders.subscribed.contains(resourceProviderId));
    ResourceProvider& resourceProvider =
      *resourceProviders.subscribed.at(resourceProviderId);

    if (!resourceProvider.http.send(event)) {
      LOG(WARNING) << "Failed to send offer operation reconciliation event"
                   << " to resource provider " << resourceProviderId
                   << ": connection closed";
    }
  }
}


Future<Nothing> ResourceProviderManagerProcess::publishResources(
    const Resources& resources)
{
  hashmap<ResourceProviderID, Resources> providedResources;

  foreach (const Resource& resource, resources) {
    // NOTE: We ignore agent default resources here because those
    // resources do not need publish, and shouldn't be handled by the
    // resource provider manager.
    if (!resource.has_provider_id()) {
      continue;
    }

    const ResourceProviderID& resourceProviderId = resource.provider_id();

    if (!resourceProviders.subscribed.contains(resourceProviderId)) {
      // TODO(chhsiao): If the manager is running on an agent and the
      // resource comes from an external resource provider, we may want
      // to load the provider's agent component.
      return Failure(
          "Resource provider " + stringify(resourceProviderId) +
          " is not subscribed");
    }

    providedResources[resourceProviderId] += resource;
  }

  list<Future<Nothing>> futures;

  foreachpair (const ResourceProviderID& resourceProviderId,
               const Resources& resources,
               providedResources) {
    UUID uuid = UUID::random();

    Event event;
    event.set_type(Event::PUBLISH_RESOURCES);
    event.mutable_publish_resources()->set_uuid(uuid.toBytes());
    event.mutable_publish_resources()->mutable_resources()->CopyFrom(resources);

    ResourceProvider* resourceProvider =
      resourceProviders.subscribed.at(resourceProviderId).get();

    LOG(INFO)
      << "Sending PUBLISH event " << uuid << " with resources '" << resources
      << "' to resource provider " << resourceProviderId;

    if (!resourceProvider->http.send(event)) {
      return Failure(
          "Failed to send PUBLISH_RESOURCES event to resource provider " +
          stringify(resourceProviderId) + ": connection closed");
    }

    Owned<Promise<Nothing>> promise(new Promise<Nothing>());
    futures.push_back(promise->future());
    resourceProvider->publishes.put(uuid, std::move(promise));
  }

  return collect(futures).then([] { return Nothing(); });
}


void ResourceProviderManagerProcess::subscribe(
    const HttpConnection& http,
    const Call::Subscribe& subscribe)
{
  ResourceProviderInfo resourceProviderInfo =
    subscribe.resource_provider_info();

  LOG(INFO) << "Subscribing resource provider " << resourceProviderInfo;

  // We always create a new `ResourceProvider` struct when a
  // resource provider subscribes or resubscribes, and replace the
  // existing `ResourceProvider` if needed.
  Owned<ResourceProvider> resourceProvider(
      new ResourceProvider(resourceProviderInfo, http));

  if (!resourceProviderInfo.has_id()) {
    // The resource provider is subscribing for the first time.
    resourceProvider->info.mutable_id()->CopyFrom(newResourceProviderId());
  } else {
    // TODO(chhsiao): The resource provider is resubscribing after being
    // restarted or an agent failover. The 'ResourceProviderInfo' might
    // have been updated, but its type and name should remain the same.
    // We should checkpoint its 'type', 'name' and ID, then check if the
    // resubscribption is consistent with the checkpointed record.
  }

  const ResourceProviderID& resourceProviderId = resourceProvider->info.id();

  Event event;
  event.set_type(Event::SUBSCRIBED);
  event.mutable_subscribed()->mutable_provider_id()
    ->CopyFrom(resourceProviderId);

  if (!resourceProvider->http.send(event)) {
    LOG(WARNING) << "Failed to send SUBSCRIBED event to resource provider "
                 << resourceProviderId << ": connection closed";
    return;
  }

  http.closed()
    .onAny(defer(self(), [=](const Future<Nothing>&) {
      CHECK(resourceProviders.subscribed.contains(resourceProviderId));

      // NOTE: All pending futures of publish requests for the resource
      // provider will become failed.
      resourceProviders.subscribed.erase(resourceProviderId);
    }));

  // TODO(jieyu): Start heartbeat for the resource provider.
  resourceProviders.subscribed.put(
      resourceProviderId,
      std::move(resourceProvider));
}


void ResourceProviderManagerProcess::updateOfferOperationStatus(
    ResourceProvider* resourceProvider,
    const Call::UpdateOfferOperationStatus& update)
{
  ResourceProviderMessage::UpdateOfferOperationStatus body;
  body.update.mutable_framework_id()->CopyFrom(update.framework_id());
  body.update.mutable_status()->CopyFrom(update.status());
  body.update.set_operation_uuid(update.operation_uuid());
  if (update.has_latest_status()) {
    body.update.mutable_latest_status()->CopyFrom(update.latest_status());
  }

  ResourceProviderMessage message;
  message.type = ResourceProviderMessage::Type::UPDATE_OFFER_OPERATION_STATUS;
  message.updateOfferOperationStatus = std::move(body);

  messages.put(std::move(message));
}


void ResourceProviderManagerProcess::updateState(
    ResourceProvider* resourceProvider,
    const Call::UpdateState& update)
{
  foreach (const Resource& resource, update.resources()) {
    CHECK_EQ(resource.provider_id(), resourceProvider->info.id());
  }

  // TODO(chhsiao): Report pending operations.

  Try<UUID> resourceVersion =
    UUID::fromBytes(update.resource_version_uuid());

  CHECK_SOME(resourceVersion)
    << "Could not deserialize version of resource provider "
    << resourceProvider->info.id() << ": " << resourceVersion.error();

  hashmap<UUID, OfferOperation> offerOperations;
  foreach (const OfferOperation &operation, update.operations()) {
    Try<UUID> uuid = UUID::fromBytes(operation.operation_uuid());
    CHECK_SOME(uuid);

    offerOperations.put(uuid.get(), operation);
  }

  LOG(INFO)
    << "Received UPDATE_STATE call with resources '" << update.resources()
    << "' from resource provider " << resourceProvider->info.id();

  ResourceProviderMessage::UpdateState updateState{
      resourceProvider->info,
      resourceVersion.get(),
      update.resources(),
      std::move(offerOperations)};

  ResourceProviderMessage message;
  message.type = ResourceProviderMessage::Type::UPDATE_STATE;
  message.updateState = std::move(updateState);

  messages.put(std::move(message));
}


void ResourceProviderManagerProcess::updatePublishResourcesStatus(
    ResourceProvider* resourceProvider,
    const Call::UpdatePublishResourcesStatus& update)
{
  Try<UUID> uuid = UUID::fromBytes(update.uuid());
  if (uuid.isError()) {
    LOG(ERROR) << "Invalid UUID in UpdatePublishResourcesStatus from resource"
               << " provider " << resourceProvider->info.id()
               << ": " << uuid.error();
    return;
  }

  if (!resourceProvider->publishes.contains(uuid.get())) {
    LOG(ERROR) << "Ignoring UpdatePublishResourcesStatus from resource"
               << " provider " << resourceProvider->info.id()
               << " because UUID " << uuid->toString() << " is unknown";
    return;
  }

  LOG(INFO)
    << "Received UPDATE_PUBLISH_RESOURCES_STATUS call for PUBLISH_RESOURCES"
    << " event " << uuid.get() << " with " << update.status()
    << " status from resource provider " << resourceProvider->info.id();

  if (update.status() == Call::UpdatePublishResourcesStatus::OK) {
    resourceProvider->publishes.at(uuid.get())->set(Nothing());
  } else {
    // TODO(jieyu): Consider to include an error message in
    // 'UpdatePublishResourcesStatus' and surface that to the caller.
    resourceProvider->publishes.at(uuid.get())->fail(
        "Failed to publish resources for resource provider " +
        stringify(resourceProvider->info.id()) + ": Received " +
        stringify(update.status()) + " status");
  }

  resourceProvider->publishes.erase(uuid.get());
}


ResourceProviderID ResourceProviderManagerProcess::newResourceProviderId()
{
  ResourceProviderID resourceProviderId;
  resourceProviderId.set_value(UUID::random().toString());
  return resourceProviderId;
}


ResourceProviderManager::ResourceProviderManager()
  : process(new ResourceProviderManagerProcess())
{
  spawn(CHECK_NOTNULL(process.get()));
}


ResourceProviderManager::~ResourceProviderManager()
{
  terminate(process.get());
  wait(process.get());
}


Future<http::Response> ResourceProviderManager::api(
    const http::Request& request,
    const Option<Principal>& principal) const
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::api,
      request,
      principal);
}


void ResourceProviderManager::applyOfferOperation(
    const ApplyOfferOperationMessage& message) const
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::applyOfferOperation,
      message);
}


void ResourceProviderManager::acknowledgeOfferOperationUpdate(
    const OfferOperationUpdateAcknowledgementMessage& message) const
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::acknowledgeOfferOperationUpdate,
      message);
}


void ResourceProviderManager::reconcileOfferOperations(
    const ReconcileOfferOperationsMessage& message) const
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::reconcileOfferOperations,
      message);
}


Future<Nothing> ResourceProviderManager::publishResources(
    const Resources& resources)
{
  return dispatch(
      process.get(),
      &ResourceProviderManagerProcess::publishResources,
      resources);
}


Queue<ResourceProviderMessage> ResourceProviderManager::messages() const
{
  return process->messages;
}

} // namespace internal {
} // namespace mesos {
