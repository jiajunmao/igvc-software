/**
Solves for an optimal path using the Field D* incremental search algorithm.

Field D* implementation details can be found in FieldDPlanner.h
*/

#include <nav_msgs/Path.h>
#include <igvc_msgs/map.h>
#include <std_msgs/Int32.h>
#include <sensor_msgs/Image.h>
#include <geometry_msgs/PoseStamped.h>

#include <ros/ros.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_ros/point_cloud.h>
#include <igvc_utils/NodeUtils.hpp>
#include <tf/transform_datatypes.h>

#include <mutex>
#include <tuple>
#include <limits>
#include <vector>
#include "igvc_navigation/Graph.h"
#include "FieldDPlanner.h"

std::mutex planning_mutex;

ros::Publisher path_pub;

bool publish_expanded; // publish a pointcloud of nodes expanded in the search
pcl::PointCloud<pcl::PointXYZRGB> expanded_cloud;
ros::Publisher expanded_pub;

igvc_msgs::mapConstPtr map; // Most up-to-date map
FieldDPlanner planner; // D* Lite path planner
int x_initial, y_initial; // Index for initial x and y location in search space

double maximum_distance; // maximum distance to goal node before warning messages spit out
double CSpace; // configuration space
double goal_range; // distance from goal at which a node is considered the goal
double rateTime; // path planning/replanning rate

bool initialize_search = true; // set to true if the search problem must be initialized
bool initialize_graph = true; // set to true if the graph must be initialized

bool initial_goal_set = false; // true if the first goal has been set
bool goal_changed = false; // the goal node changed and the graph must be re-initialized

//-------------------------- Helper Methods ----------------------------//

/*
 * publishes the nodes that have been expanded for visualization
 */
void expanded_callback(const std::vector<std::tuple<int,int>>& inds)
{
    expanded_cloud.clear();
    expanded_cloud.header.frame_id = "odom";

    for (std::tuple<int,int> ind : inds)
    {
        pcl::PointXYZRGB p;

        p.x = static_cast<float>(std::get<0>(ind) - x_initial) * map->resolution;
        p.y = static_cast<float>(std::get<1>(ind) - y_initial) * map->resolution;
        p.z = -0.05f;

        if (planner.getG(Node(std::get<0>(ind), std::get<1>(ind))) == std::numeric_limits<float>::infinity())
        {
            uint8_t r = 255, g = 0, b = 0;    // Example: Red color
            uint32_t rgb = ((uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b);
            p.rgb = *reinterpret_cast<float*>(&rgb);
        }
        else
        {
            uint8_t r = 0, g = 125, b = 125;    // Example: Red color
            uint32_t rgb = ((uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b);
            p.rgb = *reinterpret_cast<float*>(&rgb);
        }

        expanded_cloud.points.push_back(p);
    }

    expanded_pub.publish(expanded_cloud);
}

//--------------------------- ROS Callbacks ----------------------------//

/**
    Set the current map to be used by the D* Lite search problem. The initial
    map is used to perform the first search through the occupancy grid (equivalent
    to A*). All maps thereafter are used to update edge costs for the search problem.
*/
void map_callback(const igvc_msgs::mapConstPtr& msg)
{
      std::lock_guard<std::mutex> planning_lock(planning_mutex);
      map = msg; // update current map

      if (initialize_graph)
      {
          x_initial = static_cast<int>(msg->x_initial); // initial x coord of graph search problem
          y_initial = static_cast<int>(msg->y_initial); // initial y coord of graph search problem
          planner.graph.initializeGraph(msg);
          initialize_graph = false;
      }
}

/**
    Assigns a valid goal to the graph search problem. Goal index obtained by
    converting from the /map frame goal coordinate to the graph index.
*/
void waypoint_callback(const geometry_msgs::PointStampedConstPtr& msg)
{
    std::lock_guard<std::mutex> lock(planning_mutex);

    int goal_x, goal_y;
    goal_x = static_cast<int>(std::round(msg->point.x / planner.graph.Resolution)) + x_initial;
    goal_y = static_cast<int>(std::round(msg->point.y / planner.graph.Resolution)) + y_initial;

    std::tuple<int,int> newGoal = std::make_tuple(goal_x, goal_y);

    if (planner.graph.Goal.getIndex() != newGoal)
        goal_changed = true; // re-initialize graph search problem

    planner.graph.setGoal(newGoal);

    float distance_to_goal = planner.graph.euclidian_heuristic(newGoal) * planner.graph.Resolution;

    ROS_INFO_STREAM((goal_changed ? "New" : "Same") << " waypoint received. Search Problem Goal = " << goal_x
                    << ", " << goal_y << ". Distance: "
                    << distance_to_goal << "m.");

    if(distance_to_goal > maximum_distance)
      ROS_WARN_STREAM("Planning to waypoint more than " << maximum_distance
                      << "m. away - distance = " << distance_to_goal);

    initial_goal_set = true;
}

//----------------------------- main ----------------------------------//

int main(int argc, char** argv)
{
  ros::init(argc, argv, "field_d_planner");

  ros::NodeHandle nh;
  ros::NodeHandle pNh("~");

  // subscribe to map for occupancy grid and waypoint for goal node
  ros::Subscriber map_sub = nh.subscribe("/map", 1, map_callback);
  ros::Subscriber waypoint_sub = nh.subscribe("/waypoint", 1, waypoint_callback);

  // publish path for path_follower
  path_pub = nh.advertise<nav_msgs::Path>("/path", 1);

  igvc::getParam(pNh, "c_space", CSpace);
  igvc::getParam(pNh, "maximum_distance", maximum_distance);
  igvc::getParam(pNh, "rate", rateTime);
  igvc::getParam(pNh, "goal_range", goal_range);
  igvc::getParam(pNh, "publish_expanded", publish_expanded);

  planner.graph.setCSpace(static_cast<float>(CSpace));
  planner.GOAL_DIST = static_cast<float>(goal_range);

  // publish a 2D pointcloud of expanded nodes for visualization
  expanded_pub = nh.advertise<pcl::PointCloud<pcl::PointXYZRGB>>("/expanded", 1);
  expanded_cloud.header.frame_id = "odom";
  // expanded_size_pub = nh.advertise<std_msgs::Int32>("/expanded_size", 1);

  int numNodesUpdated;
  int numNodesExpanded;

  ros::Rate rate(rateTime);

  while (ros::ok())
  {
      ros::spinOnce(); // handle subscriber callbacks

      // don't plan unless the map has been initialized and there's a goal node
      if (initialize_graph || !initial_goal_set)
          continue;

      if (initialize_search)
      {
           planner.initialize();
           initialize_search = false;
      }

      if (goal_changed)
      {
          ROS_INFO_STREAM("New Goal Received. Initializing Search...");
          planner.reinitialize();
          initialize_search = true;
          goal_changed = false;
      }

      // update the underlying graph with new edge costs
      planner.graph.updateGraph(map);
      // update nodes with changed edge costs
      numNodesUpdated = planner.updateNodesAroundUpdatedCells();
      if (numNodesUpdated > 0) ROS_INFO_STREAM(numNodesUpdated << " nodes updated");

      // compute the shortest path by expanding inconsistent nodes
      ros::Time begin = ros::Time::now();
      numNodesExpanded = planner.computeShortestPath();
      double elapsed = (ros::Time::now() - begin).toSec();
      if (numNodesExpanded > 0) ROS_INFO_STREAM(numNodesExpanded << " nodes expanded in " << elapsed << "s.");

      if (publish_expanded)
        expanded_callback(planner.getExplored());

      planner.constructOptimalPath(); // construct the optimal path for traversal

      nav_msgs::Path path_msg;
      path_msg.header.stamp = ros::Time::now();
      path_msg.header.frame_id = "odom";

      for (std::tuple<float,float> point : planner.path)
      {
          geometry_msgs::PoseStamped pose;
          pose.header.stamp = path_msg.header.stamp;
          pose.header.frame_id = path_msg.header.frame_id;
          pose.pose.position.x = (std::get<0>(point) - x_initial) * planner.graph.Resolution;
          pose.pose.position.y = (std::get<1>(point) - y_initial) * planner.graph.Resolution;
          path_msg.poses.push_back(pose);
      }

      path_pub.publish(path_msg);
      rate.sleep();
  }

  return 0;
}
