/*
 * Copyright (C) 2020 Open Source Robotics Foundation
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
#include <gz/msgs/statistic.pb.h>
#include <gz/msgs/stringmsg.pb.h>

#include <chrono>
#include <string>

#include "gtest/gtest.h"
#include "gz/transport/Node.hh"
#include "gz/transport/TransportTypes.hh"
#include "test_config.hh"

using namespace gz;

static int statisticsCount = 0;

void cb(const gz::msgs::StringMsg & /*_msg*/)
{
  // no-op
}

void statsCb(const gz::msgs::Metric & /*_msg*/)
{
  statisticsCount++;
}

TEST(topicStatistics, SingleProcessPublishStatistics)
{
  statisticsCount = 0;
  std::string topic = "/foo";
  transport::Node node;
  auto pub = node.Advertise<gz::msgs::StringMsg>(topic);
  EXPECT_TRUE(pub);

  gz::msgs::StringMsg msg;
  msg.set_data("Hello");

  EXPECT_TRUE(node.Subscribe(topic, cb));
  EXPECT_TRUE(node.Subscribe("/statistics", statsCb));

  // Now, we should have subscribers.
  EXPECT_TRUE(pub.HasConnections());

  EXPECT_EQ(0, statisticsCount);

  EXPECT_TRUE(node.EnableStats(topic, true, "/statistics", 1000));

  for (auto i = 0; i < 10; ++i)
  {
    EXPECT_TRUE(pub.Publish(msg));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  // Currently, within process subscribers and publishers won't have statistics.
  EXPECT_EQ(0, statisticsCount);
  EXPECT_EQ(std::nullopt,  node.TopicStats(topic));
}

//////////////////////////////////////////////////
int main(int argc, char **argv)
{
  // Get a random partition name.
  std::string partition = testing::getRandomNumber();

  // Set the partition name for this process.
  setenv("GZ_PARTITION", partition.c_str(), 1);
  setenv("GZ_TRANSPORT_TOPIC_STATISTICS", "1", 1);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
