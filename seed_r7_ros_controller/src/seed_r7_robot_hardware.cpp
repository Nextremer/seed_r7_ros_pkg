/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2018, Yohei Kakiuchi (JSK lab.)
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Open Source Robotics Foundation
 *     nor the names of its contributors may be
 *     used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/*
 Author: Yohei Kakiuchi
*/

#include "seed_r7_robot_hardware.h"

#include <thread>

namespace robot_hardware
{

  bool RobotHW::init(ros::NodeHandle& root_nh, ros::NodeHandle &robot_hw_nh)// add joint list
  {
    std::string port_upper("/dev/aero_upper");
    std::string port_lower("/dev/aero_lower");

    // reading paramerters
    if (robot_hw_nh.hasParam("port_upper")) robot_hw_nh.getParam("port_upper", port_upper);
    if (robot_hw_nh.hasParam("port_lower")) robot_hw_nh.getParam("port_lower", port_lower);
    if (robot_hw_nh.hasParam("robot_model")) robot_hw_nh.getParam("robot_model", robot_model);
    if (robot_hw_nh.hasParam("/joint_settings/upper")) robot_hw_nh.getParam("/joint_settings/upper/name",joint_names_upper_);
    else ROS_WARN("/joint_settings/upper read error");
    if (robot_hw_nh.hasParam("/joint_settings/lower")) robot_hw_nh.getParam("/joint_settings/lower/name",joint_names_lower_);
    else ROS_WARN("/joint_settings/lower read error");
    if (robot_hw_nh.hasParam("controller_rate")) {
      double rate;
      robot_hw_nh.getParam("controller_rate", rate);
      CONTROL_PERIOD_US_ = (1000*1000)/rate;
    } else {
      CONTROL_PERIOD_US_ = 50*1000; // 50ms
    }
    if (robot_hw_nh.hasParam("overlap_scale")) {
      double scl;
      robot_hw_nh.getParam("overlap_scale", scl);
      OVERLAP_SCALE_    = scl;     //
    }  else {
      OVERLAP_SCALE_    = 2.8;     //
    }

    ROS_INFO("upper_port: %s", port_upper.c_str());
    ROS_INFO("lower_port: %s", port_lower.c_str());
    ROS_INFO("cycle: %f [ms], overlap_scale %f", CONTROL_PERIOD_US_*0.001, OVERLAP_SCALE_);

    // create controllers
    controller_upper_.reset(new UpperController(port_upper));
    controller_lower_.reset(new LowerController(port_lower));
    // stroke converter
    stroke_converter_ = new StrokeConverter(robot_hw_nh, robot_model);

    // joint list
    number_of_angles_ = joint_names_upper_.size() + joint_names_lower_.size();

    joint_list_.resize(number_of_angles_);
    for(int i = 0; i < number_of_angles_; i++) {
      if(i < joint_names_upper_.size() ) joint_list_[i] = joint_names_upper_[i];
      else joint_list_[i] = joint_names_lower_[i - joint_names_upper_.size()];
    }

    prev_ref_positions_.resize(number_of_angles_);
    initialized_flag_ = false;

    std::string model_str;
    if (!root_nh.getParam("robot_description", model_str)) {
      ROS_ERROR("Failed to get model from robot_description");
      return false;
    }
    urdf::Model model;
    if (!model.initString(model_str)) {
      ROS_ERROR("Failed to parse robot_description");
      return false;
    }

    ROS_DEBUG("read %d joints", number_of_angles_);
    for (int i = 0; i < number_of_angles_; i++) {
      ROS_DEBUG("  %d: %s", i, joint_list_[i].c_str());
      if(!model.getJoint(joint_list_[i])) {
        ROS_ERROR("Joint %s does not exist in urdf model", joint_list_[i].c_str());
        return false;
      }
    }

    // joint_names_.resize(number_of_angles_);
    joint_types_.resize(number_of_angles_);
    joint_control_methods_.resize(number_of_angles_);
    joint_position_.resize(number_of_angles_);
    joint_velocity_.resize(number_of_angles_);
    joint_effort_.resize(number_of_angles_);
    joint_position_command_.resize(number_of_angles_);
    joint_velocity_command_.resize(number_of_angles_);
    joint_effort_command_.resize(number_of_angles_);

    readPos(ros::Time::now(), ros::Duration(0.0), true); /// initial

    // Initialize values
    for(unsigned int j = 0; j < number_of_angles_; j++) {
      joint_velocity_[j] = 0.0;
      joint_velocity_command_[j] = 0.0;
      joint_effort_[j]   = 0.0;  // N/m for continuous joints
      joint_effort_command_[j]   = 0.0;

      std::string jointname = joint_list_[j];
      // Create joint state interface for all joints
      js_interface_.registerHandle(hardware_interface::JointStateHandle(jointname, &joint_position_[j], &joint_velocity_[j], &joint_effort_[j]));

      joint_control_methods_[j] = POSITION;
      hardware_interface::JointHandle joint_handle = hardware_interface::JointHandle(js_interface_.getHandle(jointname), &joint_position_command_[j]);
      pj_interface_.registerHandle(joint_handle);

      joint_limits_interface::JointLimits limits;
      const bool urdf_limits_ok = joint_limits_interface::getJointLimits(model.getJoint(jointname), limits);
      if (!urdf_limits_ok) {
        ROS_WARN("urdf limits of joint %s is not defined", jointname.c_str());
      }
      // Register handle in joint limits interface
      joint_limits_interface::PositionJointSaturationHandle limits_handle(joint_handle,limits);       // Limits spec
      pj_sat_interface_.registerHandle(limits_handle);
    }
    // Register interfaces
    registerInterface(&js_interface_);
    registerInterface(&pj_interface_);

    // Battery Voltage Publisher
    bat_vol_pub_ = robot_hw_nh.advertise<std_msgs::Float32>("voltage", 1);
    bat_vol_timer_ = robot_hw_nh.createTimer(ros::Duration(1), &RobotHW::getBatteryVoltage,this);

    // Get Robot Firmware Version
    ROS_INFO("Upper Firmware Ver. is [ %s ]", controller_upper_->getFirmwareVersion().c_str() );
    ROS_INFO("Lower Firmware Ver. is [ %s ]", controller_lower_->getFirmwareVersion().c_str() );

    return true;
  }

  void RobotHW::readPos(const ros::Time& time, const ros::Duration& period, bool update)
  {

    mutex_lower_.lock();
    mutex_upper_.lock();
    if (update) {
      std::thread t1([&](){
          controller_upper_->getPosition();
        });
      std::thread t2([&](){
          controller_lower_->getPosition();
        });
      t1.join();
      t2.join();
    }
    mutex_upper_.unlock();
    mutex_lower_.unlock();

    // whole body strokes
    std::vector<int16_t> act_strokes(0);
    std::vector<int16_t> act_upper_strokes;
    std::vector<int16_t> act_lower_strokes;

    //remap
    controller_upper_->remapAeroToRos(act_upper_strokes, controller_upper_->raw_data_);
    controller_lower_->remapAeroToRos(act_lower_strokes, controller_lower_->raw_data_);

    act_strokes.insert(act_strokes.end(),act_upper_strokes.begin(),act_upper_strokes.end());
    act_strokes.insert(act_strokes.end(),act_lower_strokes.begin(),act_lower_strokes.end());

    // whole body positions from strokes
    std::vector<double> act_positions;
    act_positions.resize(number_of_angles_);

    // convert from stroke to angle
    stroke_converter_->Stroke2Angle(act_positions, act_strokes);

    double tm = period.toSec();
    for(unsigned int j=0; j < number_of_angles_; j++) {
      float position = act_positions[j];
      float velocity = 0.0;

      joint_position_[j] = position;
      joint_velocity_[j] = velocity; // read velocity from HW
      joint_effort_[j]   = 0;        // read effort   from HW
    }

    if (!initialized_flag_) {
      for(unsigned int j = 0; j < number_of_angles_; j++) {
        joint_position_command_[j] = prev_ref_positions_[j] = joint_position_[j];
        ROS_DEBUG("%d: %s - %f", j, joint_list_[j].c_str(), joint_position_command_[j]);
      }
      initialized_flag_ = true;
    }

    return;
  }

  void RobotHW::read(const ros::Time& time, const ros::Duration& period)
  {
    return;
  }

  void RobotHW::write(const ros::Time& time, const ros::Duration& period)
  {
    pj_sat_interface_.enforceLimits(period);

    ////// convert poitions to strokes and write strokes
    std::vector<double > ref_positions(number_of_angles_);
    for(unsigned int j=0; j < number_of_angles_; j++) {
      switch (joint_control_methods_[j]) {
      case POSITION:
        {
          ref_positions[j] = joint_position_command_[j];
        }
        break;
      case VELOCITY:
        {
        }
        break;
      case EFFORT:
        {
        }
        break;
      case POSITION_PID:
        {
        }
        break;
      case VELOCITY_PID:
        {
        }
        break;
      } // switch
    } // for

    std::vector<bool > mask_positions(number_of_angles_);
    std::fill(mask_positions.begin(), mask_positions.end(), true); // send if true

    for(int i = 0; i < number_of_angles_; i++) {
      double tmp = ref_positions[i];
      if (tmp == prev_ref_positions_[i]) {
        mask_positions[i] = false;
      }
      prev_ref_positions_[i] = tmp;
    }

    //convert from angle to stroke
    std::vector<int16_t> ref_strokes(ref_positions.size());
    stroke_converter_->Angle2Stroke(ref_strokes, ref_positions);
    
    // masking
    std::vector<int16_t> snt_strokes(ref_strokes);
    for(size_t i = 0; i < ref_strokes.size() ; ++i){
      if(!mask_positions[i]) snt_strokes[i] = 0x7FFF;
    }

    // split strokes into upper and lower
    std::vector<int16_t> upper_strokes;
    std::vector<int16_t> lower_strokes;

    //remap
    if(controller_upper_->is_open_) controller_upper_->remapRosToAero(upper_strokes,snt_strokes);
    else controller_upper_->remapRosToAero(upper_strokes,ref_strokes);
    if(controller_lower_->is_open_) controller_lower_->remapRosToAero(lower_strokes,snt_strokes);
    else controller_lower_->remapRosToAero(lower_strokes,ref_strokes);

    uint16_t time_csec = static_cast<uint16_t>((OVERLAP_SCALE_ * CONTROL_PERIOD_US_)/(1000*10));

    mutex_lower_.lock();
    mutex_upper_.lock();
    {
      std::thread t1([&](){
          controller_upper_->sendPosition(time_csec, upper_strokes);
        });
      std::thread t2([&](){
          controller_lower_->sendPosition(time_csec, lower_strokes);
        });
      t1.join();
      t2.join();
    }
    mutex_upper_.unlock();
    mutex_lower_.unlock();

    // read
    readPos(time, period, false);
    return;
  }

/////////////////////////////////////
// specific functions are below:  ///
/////////////////////////////////////
  void RobotHW::runHandScript(uint8_t _number, uint16_t _script, uint8_t _current)
  {
    mutex_upper_.lock();
    if(_script == 2){
      controller_upper_->runScript(_number, 4);
      usleep(20 * 1000);    //magic number
      controller_upper_->setCurrent(_number, _current, _current);
    }
    controller_upper_->runScript(_number, _script);
    ROS_INFO("sendnum : %d, script : %d", _number, _script);
    mutex_upper_.unlock();
  }

  void RobotHW::turnWheel(std::vector<int16_t> &_vel)
  {
    mutex_lower_.lock();
    controller_lower_->sendVelocity(_vel);
    mutex_lower_.unlock();
  }

   void RobotHW::onWheelServo(bool _value)
  {
    mutex_lower_.lock();
    controller_lower_->onServo(_value);
    mutex_lower_.unlock();
  }

  void RobotHW::getBatteryVoltage(const ros::TimerEvent& _event)
  {
    //max voltage is 26[V]
    //min voltage is 22.2[V]
    std_msgs::Float32 voltage;
    mutex_lower_.lock();
    voltage.data = controller_lower_->getBatteryVoltage();
    mutex_lower_.unlock();
    bat_vol_pub_.publish(voltage);
  }

}
