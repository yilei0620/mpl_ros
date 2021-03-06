#include "bag_reader.hpp"
#include <std_msgs/Bool.h>
#include <std_msgs/Int8.h>
#include <ros_utils/data_ros_utils.h>
#include <ros_utils/primitive_ros_utils.h>
#include <ros_utils/mapping_ros_utils.h>
#include <mapping_utils/voxel_grid.h>
#include <planner/mp_map_util.h>

using namespace MPL;

MPMapUtil planner_(true);
MPMapUtil replan_planner_(true);
std::unique_ptr<VoxelGrid> voxel_mapper_;

std::shared_ptr<MPL::VoxelMapUtil> map_util;

ros::Publisher map_pub;
ros::Publisher sg_pub;
std::vector<ros::Publisher> prs_pub;
std::vector<ros::Publisher> traj_pub;
std::vector<ros::Publisher> linked_cloud_pub;
std::vector<ros::Publisher> close_cloud_pub;
std::vector<ros::Publisher> open_cloud_pub;
std::vector<ros::Publisher> expanded_cloud_pub;

std_msgs::Header header;

Waypoint start, goal;
bool terminated = false;

void visualizeGraph(int id, const MPMapUtil& planner) {
  if(id < 0 || id > 1)
    return;

  /*
  //Publish expanded nodes
  sensor_msgs::PointCloud expanded_ps = vec_to_cloud(planner.getExpandedNodes());
  expanded_ps.header = header;
  expanded_cloud_pub[id].publish(expanded_ps);
  */

  //Publish nodes in closed set
  sensor_msgs::PointCloud close_ps = vec_to_cloud(planner.getCloseSet());
  close_ps.header = header;
  close_cloud_pub[id].publish(close_ps);

  //Publish nodes in open set
  sensor_msgs::PointCloud open_ps = vec_to_cloud(planner.getOpenSet());
  open_ps.header = header;
  open_cloud_pub[id].publish(open_ps);

  //Publish nodes in open set
  sensor_msgs::PointCloud linked_ps = vec_to_cloud(planner.getLinkedNodes());
  linked_ps.header = header;
  linked_cloud_pub[id].publish(linked_ps);

  //Publish primitives
  //planning_ros_msgs::Primitives prs_msg = toPrimitivesROSMsg(planner.getAllPrimitives());
  planning_ros_msgs::Primitives prs_msg = toPrimitivesROSMsg(planner.getValidPrimitives());
  prs_msg.header =  header;
  prs_pub[id].publish(prs_msg);

}

void subtreeCallback(const std_msgs::Int8::ConstPtr& msg) {
  if(replan_planner_.initialized())
    replan_planner_.getSubStateSpace(msg->data);
  else
    return;
  std::vector<Waypoint> ws = replan_planner_.getWs();
  if(ws.size() < 3)
    terminated = true;
  else 
    start = ws[1];

  //replan_planner_.getLinkedNodes();
  visualizeGraph(1, replan_planner_);
}

void replanCallback(const std_msgs::Bool::ConstPtr& msg) {
  if(terminated)
    return;
  //Publish location of start and goal
  sensor_msgs::PointCloud sg_cloud;
  sg_cloud.header = header;
  geometry_msgs::Point32 pt1, pt2;
  pt1.x = start.pos(0), pt1.y = start.pos(1), pt1.z = start.pos(2);
  pt2.x = goal.pos(0), pt2.y = goal.pos(1), pt2.z = goal.pos(2);
  sg_cloud.points.push_back(pt1);
  sg_cloud.points.push_back(pt2); 
  sg_pub.publish(sg_cloud);


  ros::Time t0 = ros::Time::now();
  bool valid = planner_.plan(start, goal, false);
  if(!valid) {
    ROS_ERROR("Failed! Takes %f sec for planning, expand [%zu] nodes", (ros::Time::now() - t0).toSec(), planner_.getCloseSet().size());
    terminated = true;
  }
  else{
    ROS_WARN("Succeed! Takes %f sec for normal planning, openset: [%zu], closeset: [%zu], total: [%zu]", 
        (ros::Time::now() - t0).toSec(), planner_.getOpenSet().size(), planner_.getCloseSet().size(),
        planner_.getOpenSet().size() + planner_.getCloseSet().size());

    //Publish trajectory
    Trajectory traj = planner_.getTraj();
    planning_ros_msgs::Trajectory traj_msg = toTrajectoryROSMsg(traj);
    traj_msg.header = header;
    traj_pub[0].publish(traj_msg);

    printf("================== Traj -- J(0): %f, J(1): %f, J(2): %f, total time: %f\n", traj.J(0), traj.J(1), traj.J(2), traj.getTotalTime());
  }

  visualizeGraph(0, planner_);

  ros::Time t1 = ros::Time::now();
  valid = replan_planner_.plan(start, goal, replan_planner_.initialized());
  if(!valid) {
    ROS_ERROR("Failed! Takes %f sec for planning, expand [%zu] nodes", (ros::Time::now() - t1).toSec(), replan_planner_.getCloseSet().size());
    terminated = true;
  }
  else{
    ROS_WARN("Succeed! Takes %f sec for replan planning, openset: [%zu], closeset: [%zu], total: [%zu]", 
        (ros::Time::now() - t1).toSec(), replan_planner_.getOpenSet().size(), replan_planner_.getCloseSet().size(),
        replan_planner_.getOpenSet().size() + replan_planner_.getCloseSet().size());

    //Publish trajectory
    Trajectory traj = replan_planner_.getTraj();
    planning_ros_msgs::Trajectory traj_msg = toTrajectoryROSMsg(traj);
    traj_msg.header = header;
    traj_pub[1].publish(traj_msg);

    printf("================== Traj -- J(0): %f, J(1): %f, J(2): %f, total time: %f\n", traj.J(0), traj.J(1), traj.J(2), traj.getTotalTime());

 }

  visualizeGraph(1, replan_planner_);

}

void clearCloudCallback(const sensor_msgs::PointCloud::ConstPtr& msg) {
  vec_Vec3f pts = cloud_to_vec(*msg);

  vec_Vec3i pns;
  for(const auto& it: pts) {
    Vec3i pn = map_util->floatToInt(it); 
    voxel_mapper_->clear(pn(0), pn(1));
    pns.push_back(pn);
  }

  planning_ros_msgs::VoxelMap map = voxel_mapper_->getMap();

  setMap(map_util.get(), map);

  map_util->freeUnKnown();

  //Publish the dilated map for visualization
  getMap(map_util.get(), map);
  map.header = header;
  map_pub.publish(map);

  vec_Vec3f affected_pts = replan_planner_.updateClearedNodes(pns);
  sensor_msgs::PointCloud expanded_ps = vec_to_cloud(affected_pts);
  expanded_ps.header = header;
  expanded_cloud_pub[0].publish(expanded_ps);

  //visualizeGraph(1, replan_planner_);
  //visualizeGraph(0, planner_);
  //
  std_msgs::Bool init;
  init.data = true;
  replanCallback(boost::make_shared<std_msgs::Bool>(init));
}


void addCloudCallback(const sensor_msgs::PointCloud::ConstPtr& msg) {
  vec_Vec3f pts = cloud_to_vec(*msg);

  vec_Vec3i pns;
  for(const auto& it: pts) {
    Vec3i pn = map_util->floatToInt(it); 
    voxel_mapper_->fill(pn(0), pn(1));
    pns.push_back(pn);
  }

  planning_ros_msgs::VoxelMap map = voxel_mapper_->getMap();

  setMap(map_util.get(), map);

  map_util->freeUnKnown();

  //Publish the dilated map for visualization
  getMap(map_util.get(), map);
  map.header = header;
  map_pub.publish(map);

  vec_Vec3f affected_pts = replan_planner_.updateBlockedNodes(pns);
  /*
  sensor_msgs::PointCloud expanded_ps = vec_to_cloud(affected_pts);
  expanded_ps.header = header;
  expanded_cloud_pub[0].publish(expanded_ps);
  */

  //visualizeGraph(1, replan_planner_);
  //visualizeGraph(0, planner_);
  std_msgs::Bool init;
  init.data = true;
  replanCallback(boost::make_shared<std_msgs::Bool>(init));
}



int main(int argc, char ** argv){
  ros::init(argc, argv, "test");
  ros::NodeHandle nh("~");

  ros::Subscriber subtree_sub = nh.subscribe("subtree", 1, subtreeCallback);
  ros::Subscriber replan_sub = nh.subscribe("replan", 1, replanCallback);
  ros::Subscriber add_cloud_sub = nh.subscribe("add_cloud", 1, addCloudCallback);
  ros::Subscriber clear_cloud_sub = nh.subscribe("clear_cloud", 1, clearCloudCallback);
  map_pub = nh.advertise<planning_ros_msgs::VoxelMap>("voxel_map", 1, true);
  sg_pub = nh.advertise<sensor_msgs::PointCloud>("start_and_goal", 1, true);

  ros::Publisher prs_pub0 = nh.advertise<planning_ros_msgs::Primitives>("primitives0", 1, true);
  ros::Publisher prs_pub1 = nh.advertise<planning_ros_msgs::Primitives>("primitives1", 1, true);
  prs_pub.push_back(prs_pub0), prs_pub.push_back(prs_pub1);

  ros::Publisher traj_pub0 = nh.advertise<planning_ros_msgs::Trajectory>("trajectory0", 1, true);
  ros::Publisher traj_pub1 = nh.advertise<planning_ros_msgs::Trajectory>("trajectory1", 1, true);
  traj_pub.push_back(traj_pub0), traj_pub.push_back(traj_pub1);

  ros::Publisher close_cloud_pub0 = nh.advertise<sensor_msgs::PointCloud>("close_cloud0", 1, true);
  ros::Publisher close_cloud_pub1 = nh.advertise<sensor_msgs::PointCloud>("close_cloud1", 1, true);
  close_cloud_pub.push_back(close_cloud_pub0), close_cloud_pub.push_back(close_cloud_pub1);

  ros::Publisher open_cloud_pub0 = nh.advertise<sensor_msgs::PointCloud>("open_set0", 1, true);
  ros::Publisher open_cloud_pub1 = nh.advertise<sensor_msgs::PointCloud>("open_set1", 1, true);
  open_cloud_pub.push_back(open_cloud_pub0), open_cloud_pub.push_back(open_cloud_pub1);

  ros::Publisher linked_cloud_pub0 = nh.advertise<sensor_msgs::PointCloud>("linked_pts0", 1, true);
  ros::Publisher linked_cloud_pub1 = nh.advertise<sensor_msgs::PointCloud>("linked_pts1", 1, true);
  linked_cloud_pub.push_back(linked_cloud_pub0), linked_cloud_pub.push_back(linked_cloud_pub1);

  ros::Publisher expanded_cloud_pub0 = nh.advertise<sensor_msgs::PointCloud>("expanded_cloud0", 1, true);
  ros::Publisher expanded_cloud_pub1 = nh.advertise<sensor_msgs::PointCloud>("expanded_cloud1", 1, true);
  expanded_cloud_pub.push_back(expanded_cloud_pub0), expanded_cloud_pub.push_back(expanded_cloud_pub1);

  header.frame_id = std::string("map");
  //Read map from bag file
  std::string file_name, map_name, cloud_name;
  nh.param("file", file_name, std::string("voxel_map"));
  nh.param("map_name", map_name, std::string("voxel_map"));
  nh.param("cloud_name", cloud_name, std::string("cloud"));

  sensor_msgs::PointCloud cloud = read_bag<sensor_msgs::PointCloud>(file_name, cloud_name);
  planning_ros_msgs::VoxelMap map = read_bag<planning_ros_msgs::VoxelMap>(file_name, map_name);

  double res = map.info.resolution;
  Vec3f origin(map.origin.x, map.origin.y, map.origin.z);
  Vec3f dim(map.dim.x * res, map.dim.y * res, map.dim.z * res);

  voxel_mapper_.reset(new VoxelGrid(origin, dim, res));
  voxel_mapper_->addCloud(cloud_to_vec(cloud));
  map = voxel_mapper_->getMap();

  //Initialize map util 
  map_util.reset(new MPL::VoxelMapUtil);
  setMap(map_util.get(), map);

  //Free unknown space and dilate obstacles
  map_util->freeUnKnown();
  //map_util->dilate(0.2, 0.1);
  //map_util->dilating();


  //Publish the dilated map for visualization
  getMap(map_util.get(), map);
  map.header = header;
  map_pub.publish(map);

  bool replan;
  nh.param("replan", replan, false);

  //Set start and goal
  double start_x, start_y, start_z;
  nh.param("start_x", start_x, 12.5);
  nh.param("start_y", start_y, 1.4);
  nh.param("start_z", start_z, 0.0);
  double start_vx, start_vy, start_vz;
  nh.param("start_vx", start_vx, 0.0);
  nh.param("start_vy", start_vy, 0.0);
  nh.param("start_vz", start_vz, 0.0);
  double start_ax, start_ay, start_az;
  nh.param("start_ax", start_ax, 0.0);
  nh.param("start_ay", start_ay, 0.0);
  nh.param("start_az", start_az, 0.0);
  double goal_x, goal_y, goal_z;
  nh.param("goal_x", goal_x, 6.4);
  nh.param("goal_y", goal_y, 16.6);
  nh.param("goal_z", goal_z, 0.0);
 
  start.pos = Vec3f(start_x, start_y, start_z);
  start.vel = Vec3f(start_vx, start_vy, start_vz);
  start.acc = Vec3f(start_ax, start_ay, start_az);
  start.use_pos = true;
  start.use_vel = true;
  start.use_acc = true;

  goal.pos = Vec3f(goal_x, goal_y, goal_z);
  goal.vel = Vec3f(0, 0, 0);
  goal.acc = Vec3f(0, 0, 0);
  goal.use_pos = start.use_pos;
  goal.use_vel = start.use_vel;
  goal.use_acc = start.use_acc;


  //Initialize planner
  double dt, v_max, a_max, u_max;
  int max_num, ndt;
  bool use_3d;
  nh.param("dt", dt, 1.0);
  nh.param("ndt", ndt, -1);
  nh.param("v_max", v_max, 2.0);
  nh.param("a_max", a_max, 1.0);
  nh.param("u_max", u_max, 1.0);
  nh.param("max_num", max_num, -1);
  nh.param("use_3d", use_3d, false);

  planner_.setMapUtil(map_util); // Set collision checking function
  planner_.setEpsilon(1.0); // Set greedy param (default equal to 1)
  planner_.setVmax(v_max); // Set max velocity
  planner_.setAmax(a_max); // Set max acceleration (as control input)
  planner_.setUmax(u_max);// 2D discretization with 1
  planner_.setDt(dt); // Set dt for each primitive
  planner_.setTmax(ndt * dt); // Set dt for each primitive
  planner_.setMaxNum(max_num); // Set maximum allowed expansion, -1 means no limitation
  planner_.setU(1, false);// 2D discretization with 1
  planner_.setMode(start); // use acc as control
  planner_.setTol(1, 1, 1); // Tolerance for goal region
  replan_planner_.setLPAstar(false); // Use Astar

  replan_planner_.setMapUtil(map_util); // Set collision checking function
  replan_planner_.setEpsilon(1.0); // Set greedy param (default equal to 1)
  replan_planner_.setVmax(v_max); // Set max velocity
  replan_planner_.setAmax(a_max); // Set max acceleration (as control input)
  replan_planner_.setUmax(u_max);// 2D discretization with 1
  replan_planner_.setDt(dt); // Set dt for each primitive
  replan_planner_.setTmax(ndt * dt); // Set dt for each primitive
  replan_planner_.setMaxNum(-1); // Set maximum allowed expansion, -1 means no limitation
  replan_planner_.setU(1, false);// 2D discretization with 1
  replan_planner_.setMode(start); // Use acc as control
  replan_planner_.setTol(1, 1, 1); // Tolerance for goal region
  replan_planner_.setLPAstar(true); // Use LPAstar

  //Planning thread!
  std_msgs::Bool init;
  init.data = false;
  replanCallback(boost::make_shared<std_msgs::Bool>(init));

  ros::spin();

  return 0;
}
