/*
 * Copyright (C) 2016 Open Source Robotics Foundation
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
#include <gz/msgs/int32.pb.h>
#include <gz/msgs/vector3d.pb.h>

#include <chrono>
#include <cstdlib>
#include <string>

#include "gz/transport/Node.hh"
#include "gz/transport/TopicUtils.hh"
#include "gtest/gtest.h"
#include "test_config.hh"

using namespace gz;

static bool g_responseExecuted;
static bool g_wrongResponseExecuted;

static std::string g_partition; // NOLINT(*)
static std::string g_topic = "/foo"; // NOLINT(*)
static int g_data = 5;
static int g_counter = 0;

//////////////////////////////////////////////////
/// \brief Initialize some global variables.
void reset()
{
  g_responseExecuted = false;
  g_wrongResponseExecuted = false;
  g_counter = 0;
}

//////////////////////////////////////////////////
/// \brief Service call response callback.
void response(const gz::msgs::Int32 &_rep, const bool _result)
{
  EXPECT_EQ(_rep.data(), g_data);
  EXPECT_TRUE(_result);

  g_responseExecuted = true;
  ++g_counter;
}

//////////////////////////////////////////////////
/// \brief Service call response callback.
void wrongResponse(const gz::msgs::Vector3d &/*_rep*/, bool /*_result*/)
{
  g_wrongResponseExecuted = true;
}

//////////////////////////////////////////////////
/// \brief Two different nodes running in two different processes. One node
/// advertises a service without input and the other requests a few service
/// calls.
TEST(twoProcSrvCallWithoutInput, SrvTwoProcs)
{
  std::string responser_path = testing::portablePathUnion(
    GZ_TRANSPORT_TEST_DIR,
    "INTEGRATION_twoProcsSrvCallWithoutInputReplier_aux");

  testing::forkHandlerType pi = testing::forkAndRun(responser_path.c_str(),
    g_partition.c_str());

  reset();

  transport::Node node;
  EXPECT_TRUE(node.Request(g_topic, response));

  int i = 0;
  while (i < 300 && !g_responseExecuted)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ++i;
  }

  // Check that the service call response was executed.
  EXPECT_TRUE(g_responseExecuted);
  EXPECT_EQ(g_counter, 1);

  // Make another request.
  reset();

  EXPECT_TRUE(node.Request(g_topic, response));

  i = 0;
  while (i < 300 && !g_responseExecuted)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ++i;
  }

  // Check that the service call response was executed.
  EXPECT_TRUE(g_responseExecuted);
  EXPECT_EQ(g_counter, 1);

  reset();

  // Wait for the child process to return.
  testing::waitAndCleanupFork(pi);
}

//////////////////////////////////////////////////
/// \brief This test spawns a service that doesn't accept input parameters. The
/// service requester uses a wrong type for the response argument. The test
/// should verify that the service call does not succeed.
TEST(twoProcSrvCallWithoutInput, SrvRequestWrongRep)
{
  gz::msgs::Vector3d wrongRep;
  bool result;
  unsigned int timeout = 1000;

  std::string responser_path = testing::portablePathUnion(
     GZ_TRANSPORT_TEST_DIR,
     "INTEGRATION_twoProcsSrvCallWithoutInputReplier_aux");

  testing::forkHandlerType pi = testing::forkAndRun(responser_path.c_str(),
    g_partition.c_str());

  reset();

  transport::Node node;

  // Request an asynchronous service call with wrong type in the response.
  EXPECT_TRUE(node.Request(g_topic, wrongResponse));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_FALSE(g_wrongResponseExecuted);

  // Request a synchronous service call with wrong type in the response.
  EXPECT_FALSE(node.Request(g_topic, timeout, wrongRep, result));

  reset();

  // Wait for the child process to return.
  testing::waitAndCleanupFork(pi);
}

//////////////////////////////////////////////////
/// \brief This test spawns a service that doesn't accept input parameters. The
/// service requesters use incorrect types in some of the requests. The test
/// should verify that a response is received only when the appropriate types
/// are used.
TEST(twoProcSrvCallWithoutInput, SrvTwoRequestsOneWrong)
{
  gz::msgs::Int32 goodRep;
  gz::msgs::Vector3d badRep;
  bool result;
  unsigned int timeout = 2000;

  std::string responser_path = testing::portablePathUnion(
     GZ_TRANSPORT_TEST_DIR,
     "INTEGRATION_twoProcsSrvCallWithoutInputReplier_aux");

  testing::forkHandlerType pi = testing::forkAndRun(responser_path.c_str(),
    g_partition.c_str());

  reset();

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  transport::Node node;

  // Request service calls with wrong types in the response.
  EXPECT_FALSE(node.Request(g_topic, timeout, badRep, result));
  EXPECT_TRUE(node.Request(g_topic, wrongResponse));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_FALSE(g_wrongResponseExecuted);

  reset();

  // Valid service requests.
  EXPECT_TRUE(node.Request(g_topic, timeout, goodRep, result));
  EXPECT_TRUE(node.Request(g_topic, response));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_TRUE(g_responseExecuted);

  reset();

  // Wait for the child process to return.
  testing::waitAndCleanupFork(pi);
}

//////////////////////////////////////////////////
/// \brief This test spawns two nodes on different processes. One of the nodes
/// advertises a service without input and the other uses ServiceList() for
/// getting the list of available services.
TEST(twoProcSrvCallWithoutInput, ServiceList)
{
  std::string publisherPath = testing::portablePathUnion(
     GZ_TRANSPORT_TEST_DIR,
     "INTEGRATION_twoProcsSrvCallWithoutInputReplier_aux");

  testing::forkHandlerType pi = testing::forkAndRun(publisherPath.c_str(),
    g_partition.c_str());

  reset();

  transport::Node node;

  // We need some time for discovering the other node.
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));

  std::vector<std::string> services;
  auto start1 = std::chrono::steady_clock::now();
  node.ServiceList(services);
  auto end1 = std::chrono::steady_clock::now();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_EQ(services.at(0), g_topic);
  services.clear();

  // Time elapsed to get the first service list
  auto elapsed1 = end1 - start1;

  auto start2 = std::chrono::steady_clock::now();
  node.ServiceList(services);
  auto end2 = std::chrono::steady_clock::now();
  EXPECT_EQ(services.size(), 1u);
  EXPECT_EQ(services.at(0), g_topic);

  // The first ServiceList() call might block if the discovery is still
  // initializing (it may happen if we run this test alone).
  // However, the second call should never block.
  auto elapsed2 = end2 - start2;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>
      (elapsed2).count(), 2);

  EXPECT_LE(elapsed2, elapsed1);

  reset();

  testing::waitAndCleanupFork(pi);
}

//////////////////////////////////////////////////
/// \brief This test spawns two nodes on different processes. One of the nodes
/// advertises a service without input and the other uses ServiceInfo() for
/// getting information about the service.
TEST(twoProcSrvCallWithoutInput, ServiceInfo)
{
  std::string publisherPath = testing::portablePathUnion(
     GZ_TRANSPORT_TEST_DIR,
     "INTEGRATION_twoProcsSrvCallWithoutInputReplier_aux");

  testing::forkHandlerType pi = testing::forkAndRun(publisherPath.c_str(),
    g_partition.c_str());

  reset();

  transport::Node node;
  std::vector<transport::ServicePublisher> publishers;

  // We need some time for discovering the other node.
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));

  EXPECT_FALSE(node.ServiceInfo("@", publishers));
  EXPECT_EQ(publishers.size(), 0u);

  EXPECT_FALSE(node.ServiceInfo("/bogus", publishers));
  EXPECT_EQ(publishers.size(), 0u);

  EXPECT_TRUE(node.ServiceInfo("/foo", publishers));
  EXPECT_EQ(publishers.size(), 1u);
  EXPECT_EQ(publishers.front().ReqTypeName(), "gz.msgs.Empty");
  EXPECT_EQ(publishers.front().RepTypeName(), "gz.msgs.Int32");

  reset();

  testing::waitAndCleanupFork(pi);
}

//////////////////////////////////////////////////
int main(int argc, char **argv)
{
  // Get a random partition name.
  g_partition = testing::getRandomNumber();

  // Set the partition name for this process.
  setenv("GZ_PARTITION", g_partition.c_str(), 1);

  // Enable verbose mode.
  // setenv("GZ_VERBOSE", "1", 1);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
