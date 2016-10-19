/*-------------------------------------------------
参考プログラム
read_csv.cpp : https://gist.github.com/yoneken/5765597#file-read_csv-cpp

-------------------------------------------------- */


#include <ros/ros.h>
#include <ros/package.h>
#include <sensor_msgs/LaserScan.h>
#include <sensor_msgs/PointCloud.h>
#include <laser_geometry/laser_geometry.h>
#include <tf/transform_listener.h>
#include <move_base_msgs/MoveBaseAction.h>
#include <actionlib/client/simple_action_client.h>
#include <geometry_msgs/Pose.h>
#include <jsk_recognition_msgs/BoundingBox.h>
#include <jsk_recognition_msgs/BoundingBoxArray.h>

#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <boost/tokenizer.hpp>
#include <boost/shared_array.hpp>

#include <ros/package.h>

typedef actionlib::SimpleActionClient<move_base_msgs::MoveBaseAction> MoveBaseClient;

typedef boost::tokenizer<boost::char_separator<char> > tokenizer;

namespace RobotBehaviors
{
enum State
  {
    WAYPOINT_NAV,
    DETECT_TARGET_NAV,
    REACHED_GOAL,
    INIT_NAV,
    PLANNING_ABORTED,
  };
}

class WayPoint
{
public:
  WayPoint();
  WayPoint(move_base_msgs::MoveBaseGoal goal, int area_type, double reach_threshold)
    : goal_(goal), area_type_(area_type), reach_threshold_(reach_threshold)
  {
  }
  ~WayPoint(){}
  bool isSearchArea()
  {
    if (area_type_ == 1) {
      return true;
    }else{
      return false;
    }
  }
  move_base_msgs::MoveBaseGoal goal_;
  int area_type_;
  double reach_threshold_;
};


class WaypointNavigator
{
public:
  WaypointNavigator() : ac_("move_base", true), rate_(100)
  {
    target_waypoint_index_ = 0;
    robot_behavior_state_ = RobotBehaviors::INIT_NAV;
    std::string filename;

    ros::NodeHandle n("~");
    n.param<std::string>("waypointsfile", filename,
                         ros::package::getPath("waypoint_navigator")
                         + "/waypoints/garden_waypoints.csv");

    n.param("dist_thres_to_target_object", dist_thres_to_target_object_, 1.5);
    ROS_INFO("[Waypoints file name] : %s", filename.c_str());

    ROS_INFO("Reading Waypoints.");
    readWaypoint(filename.c_str());
    ROS_INFO("Waiting for action server to start.");
    ac_.waitForServer();
  }

  void sendNewGoal(geometry_msgs::Pose pose)
  {
    move_base_msgs::MoveBaseGoal goal;
    goal.target_pose.pose = pose;
    goal.target_pose.header.frame_id = "map";
    goal.target_pose.header.stamp = ros::Time::now();
    ac_.sendGoal(goal);
    now_goal_ = goal.target_pose.pose;
  }

  void cancelGoal(){
    ROS_INFO("cancelGoal() is called !!");
    ac_.cancelGoal();
  }

  int readWaypoint(std::string filename)
  {
    const int rows_num = 9; // x, y, z, Qx,Qy,Qz,Qw, area_type, reach_threshold
    boost::char_separator<char> sep("," ,"", boost::keep_empty_tokens);
    std::ifstream ifs(filename.c_str());
    std::string line;
    while(ifs.good()){
      getline(ifs, line);
      if(line.empty()){ break; }
      tokenizer tokens(line, sep);
      std::vector<double> data;
      tokenizer::iterator it = tokens.begin();
      for(; it != tokens.end() ; ++it){
        std::stringstream ss;
        double d;
        ss << *it;
        ss >> d;
        data.push_back(d);
      }
      if(data.size() != rows_num){
        ROS_ERROR("Row size is mismatch!!");
        return -1;
      }else{
        move_base_msgs::MoveBaseGoal waypoint;
        waypoint.target_pose.pose.position.x    = data[0];
        waypoint.target_pose.pose.position.y    = data[1];
        waypoint.target_pose.pose.position.z    = data[2];
        waypoint.target_pose.pose.orientation.x = data[3];
        waypoint.target_pose.pose.orientation.y = data[4];
        waypoint.target_pose.pose.orientation.z = data[5];
        waypoint.target_pose.pose.orientation.w = data[6];
        waypoints_.push_back(WayPoint(waypoint, (int)data[7], data[8]/2.0));
      }
    }
    return 0;
  }

  void detectTargetObjectCallback(const jsk_recognition_msgs::BoundingBoxArray::ConstPtr &target_objects_ptr)
  {
    target_objects_ = *target_objects_ptr;
  }

  WayPoint getNextWaypoint()
  {
    WayPoint next_waypoint = waypoints_[target_waypoint_index_];
    target_waypoint_index_++;
    return next_waypoint;
  }

  bool isFinalGoal()
  {
    if ((target_waypoint_index_) == ((int)waypoints_.size())) {
      return true;
    }else{
      return false;
    }
  }

  bool isAlreadyApproachedToTargetObject(jsk_recognition_msgs::BoundingBox target_object)
  {
    for (int i = 0; i < approached_target_objects_.boxes.size(); ++i) {
      double dist = calculateDistance(target_object.pose,
                                      approached_target_objects_.boxes[i].pose);
      if (dist < 3.0) { // しきい値はパラメータサーバで設定できるようにする
        return true;
      }
    }
    return false;
  }

  double calculateDistance(geometry_msgs::Pose a,geometry_msgs::Pose b)
  {
     sqrt(pow((a.position.x - b.position.x), 2.0) +
          pow((a.position.y - b.position.y), 2.0));
  }

  // 探索対象へのアプローチの場合
  void setNextGoal(jsk_recognition_msgs::BoundingBox target_object,
                   double threshold)
  {
    reach_threshold_ = threshold;
    // 探索対象へのアプローチの場合はアプローチ時のロボットの姿勢を計算する必要がある
    // まだ出来てない。
    approached_target_objects_.boxes.push_back(target_object);//探索済みに追加
    this->sendNewGoal(target_object.pose);
  }

  // 通常のwaypointの場合
  void setNextGoal(WayPoint waypoint)
  {
    reach_threshold_ = waypoint.reach_threshold_;
    this->sendNewGoal(waypoint.goal_.target_pose.pose);
  }

  double getReachThreshold()
  {
    return reach_threshold_;
  }

  geometry_msgs::Pose getRobotCurrentPosition()
  {
    // tfを使ってロボットの現在位置を取得する
    tf::StampedTransform transform;
    geometry_msgs::Pose pose;
    try {
      listener_.lookupTransform("/map", "/base_link", ros::Time(0), transform);
    } catch (tf::TransformException ex) {
      ROS_ERROR("%s", ex.what());
    }
    pose.position.x = transform.getOrigin().x();
    pose.position.y = transform.getOrigin().y();
    
    return pose;
  }

  geometry_msgs::Pose getNowGoalPosition()
  {
    return now_goal_;
  }

  void tryBackRecovery()
  {
    ROS_INFO_STREAM("Start tryBackRecovery()");
    laser_scan_sub_ = nh_.subscribe("scan_multi", 1, &WaypointNavigator::laserCallback, this);
    cmd_vel_pub_ = nh_.advertise<geometry_msgs::Twist>("/cmd_vel", 1);
    geometry_msgs::Twist msg;
    geometry_msgs::Pose start_recovery_position = this->getRobotCurrentPosition(); // 現在座標
    ros::Time start_recovery_time = ros::Time::now();
    while(ros::ok())
    {
      // 1m 下がる
      int obstacle_counter = 0;
      for (size_t i = 0; i < cloud_.points.size(); ++i) {
        if(0.2 < cloud_.points[i].x && cloud_.points[i].x < 0.7)
        {
          if (0.3 < cloud_.points[i].y && cloud_.points[i].y < 0.5) {
            obstacle_counter++;
          }
        }
      }
      if(obstacle_counter > 5)
      {
        msg.linear.x = 0;
        msg.angular.z = 0;
        cmd_vel_pub_.publish(msg);
      }
      else
      {
        msg.linear.x = -0.5;
        msg.angular.z = 0;
        cmd_vel_pub_.publish(msg);
      }
      geometry_msgs::Pose robot_current_position = this->getRobotCurrentPosition(); // 現在のロボットの座標
      double delta_dist = this->calculateDistance(robot_current_position,
                                                  start_recovery_position); // 現在位置までの距離を計算
      if(delta_dist > 1.0) // 1[m]後退したら終わり
      {
        ROS_INFO_STREAM("!! Success to back 1[m] !!");
        break;
      }
      ros::Duration delta_time = ros::Time::now() - start_recovery_time;
      if(delta_time.toSec() > 30) // 30[s]経過したら終わり
      {
        ROS_WARN_STREAM("Fail to back 1[m], but passed 30[s].");
        break;
      }
      ros::spinOnce();
      rate_.sleep();
    }
    laser_scan_sub_.shutdown();
    cmd_vel_pub_.shutdown();
  }

  void laserCallback(const sensor_msgs::LaserScan::ConstPtr& scan)
  {
    projector_.projectLaser(*scan, cloud_);
  }
  
  void run()
  {
    robot_behavior_state_ = RobotBehaviors::INIT_NAV;
    while(ros::ok()){
      WayPoint next_waypoint = this->getNextWaypoint();
      ROS_INFO("Next WayPoint is got");
      if (next_waypoint.isSearchArea()) { // 次のwaypointが探索エリアがどうか判定
        if(target_objects_.boxes.size() > 0){ // 探索対象が見つかっているか
          for (int i = 0; i < target_objects_.boxes.size(); ++i) {
            if (! this->isAlreadyApproachedToTargetObject(target_objects_.boxes[i])) { // 探索対象にまだアプローチしていなかったら
              this->setNextGoal(target_objects_.boxes[i], dist_thres_to_target_object_); // 探索対象を次のゴールに設定
              robot_behavior_state_ = RobotBehaviors::DETECT_TARGET_NAV;
              break;
            }
          }
        }else // 探索エリアだが探索対象がいない
        {
          ROS_INFO("Searching area but there are not target objects.");
          this->setNextGoal(next_waypoint);
          robot_behavior_state_ = RobotBehaviors::WAYPOINT_NAV;
        }
      }else // 探索エリアではない
      {
        ROS_INFO("Go next_waypoint.");
        this->setNextGoal(next_waypoint);
        robot_behavior_state_ = RobotBehaviors::WAYPOINT_NAV;
      }
      ros::Time begin_navigation = ros::Time::now(); // 新しいナビゲーションを設定した時間
      double last_distance_to_goal = 0;
      double delta_distance_to_goal = 1.0; // 0.1[m]より大きければよい
      while(ros::ok())
      {
        geometry_msgs::Pose robot_current_position = this->getRobotCurrentPosition(); // 現在のロボットの座標
        geometry_msgs::Pose now_goal_position = this->getNowGoalPosition(); // 現在目指している座標
        double distance_to_goal = this->calculateDistance(robot_current_position,
                                                          now_goal_position); // 現在位置とwaypointまでの距離を計算
        // ここからスタック(Abort)判定。
        delta_distance_to_goal = last_distance_to_goal - distance_to_goal; // どれだけ進んだか
        if(delta_distance_to_goal < 0.1){ // 進んだ距離が0.1[m]より小さくて
          ros::Duration how_long_stay_time = ros::Time::now() - begin_navigation;
          if (how_long_stay_time.toSec() > 60.0 ) { // 60秒間経過していたら
            robot_behavior_state_ = RobotBehaviors::PLANNING_ABORTED; // プランニング失敗とする
            break;
          }
        }else{ // 0.1[m]以上進んでいればOK
          last_distance_to_goal = distance_to_goal;
          begin_navigation = ros::Time::now();
        }
        // waypointの更新判定
        if(distance_to_goal < this->getReachThreshold()) // 目標座標までの距離がしきい値になれば
        {
          robot_behavior_state_ = RobotBehaviors::REACHED_GOAL;
          break;
        }
        rate_.sleep();
        ros::spinOnce();
      }
      switch (robot_behavior_state_) {
        case RobotBehaviors::REACHED_GOAL: {
          ROS_INFO("REACHED_GOAL");
          if (this->isFinalGoal()) { // そのwaypointが最後だったら
            this->cancelGoal(); // ゴールをキャンセルして終了
            return;
          }
          break;
        }
        case RobotBehaviors::PLANNING_ABORTED: {
          ROS_INFO("!! PLANNING_ABORTED !!");
          this->cancelGoal(); // 今のゴールをキャンセルして
          this->tryBackRecovery(); // 1mくらい戻ってみて
          target_waypoint_index_ -= 1; // waypoint indexを１つ戻す
          break;
        }  
        default:
          break;
      }
      rate_.sleep();
      ros::spinOnce();
    }// while(ros::ok())
  }
private:
  MoveBaseClient ac_;
  RobotBehaviors::State robot_behavior_state_;
  ros::Rate rate_;
  std::vector<WayPoint> waypoints_;
  ros::NodeHandle nh_;
  tf::TransformListener listener_;
  int target_waypoint_index_; // 次に目指すウェイポイントのインデックス
  jsk_recognition_msgs::BoundingBoxArray target_objects_; //探索対象
  jsk_recognition_msgs::BoundingBoxArray approached_target_objects_; //アプローチ済みの探索対象
  double dist_thres_to_target_object_; // 探索対象にどれだけ近づいたらゴールとするか
  double reach_threshold_; // 今セットされてるゴール（waypointもしくは探索対象）へのしきい値
  geometry_msgs::Pose now_goal_; // 現在目指しているゴールの座標
  ros::Subscriber laser_scan_sub_;
  sensor_msgs::LaserScan scan_;
  laser_geometry::LaserProjection projector_;
  sensor_msgs::PointCloud cloud_;
  ros::Publisher cmd_vel_pub_;
};


int main(int argc, char** argv){
  ros::init(argc, argv, "waypoint_navigator");

  WaypointNavigator waypoint_navigator;
  waypoint_navigator.run();
 
  return 0;
}
