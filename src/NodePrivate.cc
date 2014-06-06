/*
 * Copyright (C) 2014 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <uuid/uuid.h>
#include <zmq.hpp>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "ignition/transport/AddressInfo.hh"
#include "ignition/transport/Discovery.hh"
#include "ignition/transport/NodePrivate.hh"
#include "ignition/transport/SubscriptionHandler.hh"
#include "ignition/transport/TransportTypes.hh"

using namespace ignition;
using namespace transport;

//////////////////////////////////////////////////
NodePrivatePtr NodePrivate::GetInstance(bool _verbose)
{
  static NodePrivatePtr instance(new NodePrivate(_verbose));
  return instance;
}

//////////////////////////////////////////////////
NodePrivate::NodePrivate(bool _verbose)
  : verbose(_verbose),
    context(new zmq::context_t(1)),
    publisher(new zmq::socket_t(*context, ZMQ_PUB)),
    subscriber(new zmq::socket_t(*context, ZMQ_SUB)),
    control(new zmq::socket_t(*context, ZMQ_DEALER)),
    timeout(Timeout)
{
  char bindEndPoint[1024];

  // Initialize random seed.
  srand(time(nullptr));

  // My process UUID.
  uuid_generate(this->pUuid);
  this->pUuidStr = GetGuidStr(this->pUuid);

  // Initialize my discovery service.
  this->discovery.reset(new Discovery(this->pUuid, false));

  // Initialize the 0MQ objects.
  try
  {
    // Set the hostname's ip address.
    this->hostAddr = this->discovery->GetHostAddr();

    // Publisher socket listening in a random port.
    std::string anyTcpEp = "tcp://" + this->hostAddr + ":*";
    this->publisher->bind(anyTcpEp.c_str());
    size_t size = sizeof(bindEndPoint);
    this->publisher->getsockopt(ZMQ_LAST_ENDPOINT, &bindEndPoint, &size);
    this->myAddress = bindEndPoint;

    // Control socket listening in a random port.
    this->control->bind(anyTcpEp.c_str());
    this->control->getsockopt(ZMQ_LAST_ENDPOINT, &bindEndPoint, &size);
    this->myControlAddress = bindEndPoint;
  }
  catch(const zmq::error_t& ze)
  {
     std::cerr << "Error: " << ze.what() << std::endl;
     std::exit(EXIT_FAILURE);
  }

  if (this->verbose)
  {
    std::cout << "Current host address: " << this->hostAddr << std::endl;
    std::cout << "Bind at: [" << this->myAddress << "] for pub/sub\n";
    std::cout << "Bind at: [" << this->myControlAddress << "] for control\n";
    std::cout << "Process UUID: " << this->pUuidStr << std::endl;
  }

  // We don't want to exit yet.
  this->exitMutex.lock();
  this->exit = false;
  this->exitMutex.unlock();

  // Start the service thread.
  this->threadReception = new std::thread(&NodePrivate::RunReceptionTask, this);

  // Set the callback to notify discovery updates (new connections).
  discovery->SetConnectionsCb(&NodePrivate::OnNewConnection, this);

  // Set the callback to notify discovery updates (new disconnections).
  discovery->SetDisconnectionsCb(&NodePrivate::OnNewDisconnection, this);
}

//////////////////////////////////////////////////
NodePrivate::~NodePrivate()
{
  // Tell the service thread to terminate.
  this->exitMutex.lock();
  this->exit = true;
  this->exitMutex.unlock();

  // Wait for the service thread before exit.
  this->threadReception->join();
}

//////////////////////////////////////////////////
void NodePrivate::RunReceptionTask()
{
  while (!this->discovery->Interrupted())
  {
    // Poll socket for a reply, with timeout.
    zmq::pollitem_t items[] =
    {
      {*this->subscriber, 0, ZMQ_POLLIN, 0},
      {*this->control, 0, ZMQ_POLLIN, 0}
    };
    zmq::poll(&items[0], sizeof(items) / sizeof(items[0]), this->timeout);

    //  If we got a reply, process it
    if (items[0].revents & ZMQ_POLLIN)
      this->RecvMsgUpdate();
    if (items[1].revents & ZMQ_POLLIN)
      this->RecvControlUpdate();

    // Is it time to exit?
    {
      std::lock_guard<std::mutex> lock(this->exitMutex);
      if (this->exit)
        break;
    }
  }

  this->exit = true;
}

//////////////////////////////////////////////////
int NodePrivate::Publish(const std::string &_topic, const std::string &_data)
{
  assert(_topic != "");

  zmq::message_t message;
  message.rebuild(_topic.size() + 1);
  memcpy(message.data(), _topic.c_str(), _topic.size() + 1);
  this->publisher->send(message, ZMQ_SNDMORE);

  message.rebuild(this->myAddress.size() + 1);
  memcpy(message.data(), this->myAddress.c_str(), this->myAddress.size() + 1);
  this->publisher->send(message, ZMQ_SNDMORE);

  message.rebuild(_data.size() + 1);
  memcpy(message.data(), _data.c_str(), _data.size() + 1);
  this->publisher->send(message, 0);

  return 0;
}

//////////////////////////////////////////////////
void NodePrivate::RecvMsgUpdate()
{
  std::lock_guard<std::recursive_mutex> lock(this->mutex);

  zmq::message_t message(0);
  std::string topic;
  // std::string sender;
  std::string data;

  try
  {
    if (!this->subscriber->recv(&message, 0))
      return;
    topic = std::string(reinterpret_cast<char *>(message.data()));

    if (!this->subscriber->recv(&message, 0))
      return;
    // sender = std::string(reinterpret_cast<char *>(message.data()));

    if (!this->subscriber->recv(&message, 0))
      return;
    data = std::string(reinterpret_cast<char *>(message.data()));
  }
  catch(const zmq::error_t &_error)
  {
    std::cout << "Error: " << _error.what() << std::endl;
    return;
  }

  if (this->localSubscriptions.Subscribed(topic))
  {
    // Execute the callbacks registered.
    ISubscriptionHandler_M handlers;
    this->localSubscriptions.GetSubscriptionHandlers(topic, handlers);
    for (auto &handler : handlers)
    {
      ISubscriptionHandlerPtr subscriptionHandlerPtr = handler.second;
      if (subscriptionHandlerPtr)
      {
        // ToDo(caguero): Unserialize only once.
        subscriptionHandlerPtr->RunCallback(topic, data);
      }
      else
        std::cerr << "Subscription handler is NULL" << std::endl;
    }
  }
  else
    std::cerr << "I am not subscribed to topic [" << topic << "]\n";
}

//////////////////////////////////////////////////
void NodePrivate::RecvControlUpdate()
{
  std::lock_guard<std::recursive_mutex> lock(this->mutex);

  zmq::message_t message(0);
  std::string topic;
  std::string procUuid;
  std::string nodeUuid;
  std::string data;

  try
  {
    if (!this->control->recv(&message, 0))
      return;
    topic = std::string(reinterpret_cast<char *>(message.data()));

    if (!this->control->recv(&message, 0))
      return;
    procUuid = std::string(reinterpret_cast<char *>(message.data()));

    if (!this->control->recv(&message, 0))
      return;
    nodeUuid = std::string(reinterpret_cast<char *>(message.data()));

    if (!this->control->recv(&message, 0))
      return;
    data = std::string(reinterpret_cast<char *>(message.data()));
  }
  catch(const zmq::error_t &_error)
  {
    std::cerr << "NodePrivate::RecvControlUpdate() error: "
              << _error.what() << std::endl;
    return;
  }

  if (std::stoi(data) == NewConnection)
  {
    if (this->verbose)
    {
      std::cout << "Registering a new remote connection" << std::endl;
      std::cout << "\tProc UUID: [" << procUuid << "]\n";
      std::cout << "\tNode UUID: [" << nodeUuid << "]\n";
    }

    this->remoteSubscribers.AddAddress(topic, "", "", procUuid, nodeUuid);
  }
  else if (std::stoi(data) == EndConnection)
  {
    if (this->verbose)
    {
      std::cout << "Registering the end of a remote connection" << std::endl;
      std::cout << "\tProc UUID: " << procUuid << std::endl;
      std::cout << "\tNode UUID: [" << nodeUuid << "]\n";
    }

    this->remoteSubscribers.DelAddressByNode(topic, procUuid, nodeUuid);
  }
}

//////////////////////////////////////////////////
void NodePrivate::OnNewConnection(const std::string &_topic,
  const std::string &_addr, const std::string &_ctrl,
  const std::string &_pUuid, const std::string &_nUuid,
  const Scope &_scope)
{
  std::lock_guard<std::recursive_mutex> lock(this->mutex);

  if (this->verbose)
  {
    std::cout << "Connection callback" << std::endl;
    std::cout << "Topic: " << _topic << std::endl;
    std::cout << "Addr: " << _addr << std::endl;
    std::cout << "Ctrl Addr: " << _ctrl << std::endl;
    std::cout << "Process UUID: [" << _pUuid << "]" << std::endl;
    std::cout << "Node UUID: [" << _nUuid << "]" << std::endl;
  }

  // ToDo(caguero): Connected before and now want to connect for a different
  // topic. I shouldn't reconnect but I should apply the filter.

  // Check if we are interested in this topic.
  if (this->localSubscriptions.Subscribed(_topic) &&
      this->pUuidStr.compare(_pUuid) != 0)
  {
    if (this->verbose)
      std::cout << "Connecting to a remote publisher" << std::endl;

    try
    {
      // I am not connected to the process.
      if (!this->connections.HasAddress(_addr))
        this->subscriber->connect(_addr.c_str());

      // Add a new filter for the topic.
      this->subscriber->setsockopt(ZMQ_SUBSCRIBE, _topic.data(), _topic.size());

      // Register the new connection with the publisher.
      this->connections.AddAddress(
        _topic, _addr, _ctrl, _pUuid, _nUuid, _scope);

      // Send a message to the publisher's control socket to notify it
      // about all my remoteSubscribers.
      zmq::socket_t socket(*this->context, ZMQ_DEALER);
      socket.connect(_ctrl.c_str());

      if (this->verbose)
      {
        std::cout << "\t* Connected to [" << _addr << "] for data\n";
        std::cout << "\t* Connected to [" << _ctrl << "] for control\n";
      }

      // Set ZMQ_LINGER to 0 means no linger period. Pending messages will
      // be discarded immediately when the socket is closed. That avoids
      // infinite waits if the publisher is disconnected.
      int lingerVal = 200;
      socket.setsockopt(ZMQ_LINGER, &lingerVal, sizeof(lingerVal));

      ISubscriptionHandler_M handlers;
      this->localSubscriptions.GetSubscriptionHandlers(_topic, handlers);
      for (auto handler : handlers)
      {
        std::string nodeUuid = handler.second->GetNodeUuid();

        zmq::message_t message;
        message.rebuild(_topic.size() + 1);
        memcpy(message.data(), _topic.c_str(), _topic.size() + 1);
        socket.send(message, ZMQ_SNDMORE);

        message.rebuild(this->pUuidStr.size() + 1);
        memcpy(message.data(), this->pUuidStr.c_str(),
          this->pUuidStr.size() + 1);
        socket.send(message, ZMQ_SNDMORE);

        message.rebuild(nodeUuid.size() + 1);
        memcpy(message.data(), nodeUuid.c_str(), nodeUuid.size() + 1);
        socket.send(message, ZMQ_SNDMORE);

        std::string data = std::to_string(NewConnection);
        message.rebuild(data.size() + 1);
        memcpy(message.data(), data.c_str(), data.size() + 1);
        socket.send(message, 0);
      }
    }
    catch(const zmq::error_t& ze)
    {
      // std::cerr << "Error connecting [" << ze.what() << "]\n";
    }
  }
}

//////////////////////////////////////////////////
void NodePrivate::OnNewDisconnection(const std::string &_topic,
  const std::string &/*_addr*/, const std::string &/*_ctrlAddr*/,
  const std::string &_pUuid, const std::string &_nUuid,
  const Scope &/*_scope*/)
{
  std::lock_guard<std::recursive_mutex> lock(this->mutex);

  if (this->verbose)
  {
    std::cout << "New disconnection detected " << std::endl;
    std::cout << "\tProcess UUID: " << _pUuid << std::endl;
  }

  // A remote subscriber[s] has been disconnected.
  if (_topic != "" && _nUuid != "")
  {
    this->remoteSubscribers.DelAddressByNode(_topic, _pUuid, _nUuid);

    Address_t connection;
    if (!this->connections.GetAddress(_topic, _pUuid, _nUuid, connection))
      return;

    // Disconnect from a publisher's socket.
    // for (const auto &connection : this->connections[_pUuid])
    //   this->subscriber->disconnect(connection.addr.c_str());
    this->subscriber->disconnect(connection.addr.c_str());

    // I am no longer connected.
    this->connections.DelAddressByNode(_topic, _pUuid, _nUuid);
  }
  else
  {
    this->remoteSubscribers.DelAddressesByProc(_pUuid);

    Addresses_M info;
    if (!this->connections.GetAddresses(_topic, info))
      return;

    // Disconnect from all the connections of that publisher.
    for (auto &connection : info[_pUuid])
      this->subscriber->disconnect(connection.addr.c_str());

    // Remove all the connections from the process disonnected.
    this->connections.DelAddressesByProc(_pUuid);
  }
}
