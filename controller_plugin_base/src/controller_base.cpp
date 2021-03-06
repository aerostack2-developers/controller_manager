/********************************************************************************************
 *  \file       controller_base.cpp
 *  \brief      Controller base class implementation
 *  \authors    Miguel Fernández Cortizas
 *              Pedro Arias Pérez
 *              David Pérez Saura
 *              Rafael Pérez Seguí
 *
 *  \copyright  Copyright (c) 2022 Universidad Politécnica de Madrid
 *              All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ********************************************************************************/

#include "controller_plugin_base/controller_base.hpp"

#include <rcl/time.h>

#include <as2_core/control_mode_utils/control_mode_utils.hpp>
#include <chrono>
#include <rclcpp/clock.hpp>
#include <rclcpp/logging.hpp>
#include <rclcpp/rate.hpp>

namespace controller_plugin_base
{

  static inline bool checkMatchWithMask(const uint8_t mode1, const uint8_t mode2,
                                        const uint8_t mask)
  {
    return (mode1 & mask) == (mode2 & mask);
  }

  static uint8_t findBestMatchWithMask(const uint8_t mode, const std::vector<uint8_t> &mode_list,
                                       const uint8_t mask)
  {
    uint8_t best_match = 0;
    for (const auto &candidate : mode_list)
    {
      if (checkMatchWithMask(mode, candidate, mask))
      {
        best_match = candidate;
        if (candidate == mode)
        {
          return candidate;
        }
      }
    }
    return best_match;
  }

  void ControllerBase::initialize(as2::Node *node_ptr)
  {
    node_ptr_ = node_ptr;

    node_ptr_->get_parameter("use_bypass", use_bypass_);

    pose_sub_ = std::make_shared<message_filters::Subscriber<geometry_msgs::msg::PoseStamped>>(node_ptr_, as2_names::topics::self_localization::pose, as2_names::topics::self_localization::qos.get_rmw_qos_profile());
    twist_sub_ = std::make_shared<message_filters::Subscriber<geometry_msgs::msg::TwistStamped>>(node_ptr_, as2_names::topics::self_localization::twist, as2_names::topics::self_localization::qos.get_rmw_qos_profile());
    synchronizer_ = std::make_shared<message_filters::Synchronizer<approximate_policy>>(approximate_policy(5), *(pose_sub_.get()), *(twist_sub_.get()));
    synchronizer_->registerCallback(&ControllerBase::state_callback, this);

    ref_pose_sub_ = node_ptr_->create_subscription<geometry_msgs::msg::PoseStamped>(
        as2_names::topics::motion_reference::pose, as2_names::topics::motion_reference::qos,
        std::bind(&ControllerBase::ref_pose_callback, this, std::placeholders::_1));
    ref_twist_sub_ = node_ptr_->create_subscription<geometry_msgs::msg::TwistStamped>(
        as2_names::topics::motion_reference::twist, as2_names::topics::motion_reference::qos,
        std::bind(&ControllerBase::ref_twist_callback, this, std::placeholders::_1));
    ref_traj_sub_ = node_ptr_->create_subscription<trajectory_msgs::msg::JointTrajectoryPoint>(
        as2_names::topics::motion_reference::trajectory, as2_names::topics::motion_reference::qos,
        std::bind(&ControllerBase::ref_traj_callback, this, std::placeholders::_1));
    platform_info_sub_ = node_ptr_->create_subscription<as2_msgs::msg::PlatformInfo>(
        as2_names::topics::platform::info, as2_names::topics::platform::qos,
        std::bind(&ControllerBase::platform_info_callback, this, std::placeholders::_1));

    set_control_mode_client_ =
        std::make_shared<as2::SynchronousServiceClient<as2_msgs::srv::SetControlMode>>(
            as2_names::services::platform::set_platform_control_mode);

    list_control_modes_client_ =
        std::make_shared<as2::SynchronousServiceClient<as2_msgs::srv::ListControlModes>>(
            as2_names::services::platform::list_control_modes);

    pose_pub_ = node_ptr_->create_publisher<geometry_msgs::msg::PoseStamped>(
        as2_names::topics::actuator_command::pose, as2_names::topics::actuator_command::qos);
    twist_pub_ = node_ptr_->create_publisher<geometry_msgs::msg::TwistStamped>(
        as2_names::topics::actuator_command::twist, as2_names::topics::actuator_command::qos);
    thrust_pub_ = node_ptr_->create_publisher<as2_msgs::msg::Thrust>(
        as2_names::topics::actuator_command::thrust, as2_names::topics::actuator_command::qos);

    using namespace std::chrono_literals;
    // FIXME: Hardcoded timer period
    control_timer_ =
        node_ptr_->create_wall_timer(10ms, std::bind(&ControllerBase::control_timer_callback, this));

    set_control_mode_srv_ = node_ptr->create_service<as2_msgs::srv::SetControlMode>(
        as2_names::services::controller::set_control_mode,
        std::bind(&ControllerBase::setControlModeSrvCall, this,
                  std::placeholders::_1, // Corresponds to the 'request'  input
                  std::placeholders::_2  // Corresponds to the 'response' input
                  ));

    input_mode_.control_mode = as2_msgs::msg::ControlMode::UNSET;
    output_mode_.control_mode = as2_msgs::msg::ControlMode::UNSET;

    ownInitialize();
  }

  void ControllerBase::state_callback(const geometry_msgs::msg::PoseStamped::ConstSharedPtr pose_msg,
                                      const geometry_msgs::msg::TwistStamped::ConstSharedPtr twist_msg)
  {
    state_adquired_ = true;
    pose_ = *pose_msg;
    twist_ = *twist_msg;
    if (!bypass_controller_)
      updateState(pose_, twist_);
  }

  void ControllerBase::ref_pose_callback(const geometry_msgs::msg::PoseStamped::SharedPtr msg)
  {
    motion_reference_adquired_ = true;
    ref_pose_ = *msg;
    if (!bypass_controller_)
      updateReference(ref_pose_);
  }

  void ControllerBase::ref_twist_callback(const geometry_msgs::msg::TwistStamped::SharedPtr msg)
  {
    motion_reference_adquired_ = true;
    ref_twist_ = *msg;
    if (!bypass_controller_)
      updateReference(ref_twist_);
  }

  void ControllerBase::ref_traj_callback(
      const trajectory_msgs::msg::JointTrajectoryPoint::SharedPtr msg)
  {
    motion_reference_adquired_ = true;
    ref_traj_ = *msg;
    if (!bypass_controller_)
      updateReference(ref_traj_);
  }

  void ControllerBase::platform_info_callback(const as2_msgs::msg::PlatformInfo::SharedPtr msg)
  {
    platform_info_ = *msg;
  }

  void ControllerBase::control_timer_callback()
  {
    if (!platform_info_.offboard || !platform_info_.armed)
    {
      return;
    }

    if (!control_mode_established_)
    {
      return;
    }

    if (!state_adquired_)
    {
      auto &clock = *node_ptr_->get_clock();
      RCLCPP_INFO_THROTTLE(node_ptr_->get_logger(), clock, 1000, "Waiting for odometry ");

      return;
    }

    sendCommand();
  };

  // TODO: move to ControllerManager?
  bool ControllerBase::setPlatformControlMode(const as2_msgs::msg::ControlMode &mode)
  {
    as2_msgs::srv::SetControlMode::Request set_control_mode_req;
    as2_msgs::srv::SetControlMode::Response set_control_mode_resp;
    set_control_mode_req.control_mode = mode;
    auto out = set_control_mode_client_->sendRequest(set_control_mode_req, set_control_mode_resp);
    if (out && set_control_mode_resp.success)
      return true;
    return false;
  };

  bool ControllerBase::listPlatformAvailableControlModes()
  {
    if (platform_available_modes_in_.empty())
    {
      RCLCPP_DEBUG(node_ptr_->get_logger(), "LISTING AVAILABLE MODES");
      // if the list is empty, send a request to the platform to get the list of available modes
      as2_msgs::srv::ListControlModes::Request list_control_modes_req;
      as2_msgs::srv::ListControlModes::Response list_control_modes_resp;

      bool out =
          list_control_modes_client_->sendRequest(list_control_modes_req, list_control_modes_resp);
      if (!out)
      {
        RCLCPP_ERROR(node_ptr_->get_logger(), "Error listing control_modes");
        return false;
      }
      if (list_control_modes_resp.control_modes.empty())
      {
        RCLCPP_ERROR(node_ptr_->get_logger(), "No available control modes");
        return false;
      }

      // log the available modes
      for (auto &mode : list_control_modes_resp.control_modes)
      {
        RCLCPP_DEBUG(node_ptr_->get_logger(), "Available mode: %s",
                     as2::controlModeToString(mode).c_str());
      }

      platform_available_modes_in_ = list_control_modes_resp.control_modes;
    }
    return true;
  }

  bool ControllerBase::tryToBypassController(const uint8_t input_mode, uint8_t &output_mode)
  {
    // check if platform available modes are set
    if ((input_mode & MATCH_MODE) == UNSET_MODE_MASK ||
        (input_mode & MATCH_MODE) == HOVER_MODE_MASK)
    {
      return false;
    }

    uint8_t candidate_mode =
        findBestMatchWithMask(input_mode, platform_available_modes_in_, MATCH_ALL);
    if (candidate_mode)
    {
      output_mode = candidate_mode;
      return true;
    }
    return false;
  }

  bool ControllerBase::checkSuitabilityInputMode(const uint8_t input_mode,
                                                 const uint8_t output_mode)
  {

    // check if input_conversion is in the list of available modes
    bool mode_found = false;
    for (auto &mode : controller_available_modes_in_)
    {
      if ((input_mode & MATCH_MODE) == HOVER_MODE_MASK && (input_mode & MATCH_MODE) == mode)
      {
        mode_found = true;
        return true;
      }
      else if (mode == input_mode)
      {
        mode_found = true;
        break;
      }
    }

    // check if the input mode is compatible with the output mode
    if ((input_mode & MATCH_MODE) < (output_mode & 0b1111000))
    {
      RCLCPP_ERROR(node_ptr_->get_logger(),
                   "Input control mode has lower level than output control mode");
      return false;
    }

    return mode_found;
  }

  bool ControllerBase::findSuitableOutputControlModeForPlatformInputMode(uint8_t &output_mode,
                                                                         const uint8_t input_mode)
  {
    //  check if the prefered mode is available
    if (prefered_output_mode_)
    {
      auto match =
          findBestMatchWithMask(prefered_output_mode_, platform_available_modes_in_, MATCH_ALL);
      if (match)
      {
        output_mode = match;
        return true;
      }
    }

    // if the prefered mode is not available, search for the first common mode

    uint8_t common_mode = 0;
    bool same_yaw = false;

    for (auto &mode_out : controller_available_modes_out_)
    {
      // skip unset modes and hover
      if ((mode_out & MATCH_MODE) == UNSET_MODE_MASK || (mode_out & MATCH_MODE) == HOVER_MODE_MASK)
      {
        continue;
      }
      common_mode = findBestMatchWithMask(mode_out, platform_available_modes_in_, MATCH_ALL);
    }

    // check if the common mode exist
    if (common_mode == 0)
    {
      return false;
    }
    output_mode = common_mode;
    return true;
  }

  void ControllerBase::setControlModeSrvCall(
      const as2_msgs::srv::SetControlMode::Request::SharedPtr request,
      as2_msgs::srv::SetControlMode::Response::SharedPtr response)
  {
    control_mode_established_ = false;
    // input_control_mode_desired
    uint8_t input_control_mode_desired = 0;
    if (request->control_mode.control_mode == as2_msgs::msg::ControlMode::HOVER)
    {
      input_control_mode_desired = HOVER_MODE_MASK;
    }
    else
    {
      input_control_mode_desired = as2::convertAS2ControlModeToUint8t(request->control_mode);
    }

    // check if platform_available_modes is set
    if (!listPlatformAvailableControlModes())
    {
      response->success = false;
      return;
    }

    // 1st: check if a bypass is possible for the input_control_mode_desired ( DISCARDING YAW
    // COMPONENT)

    uint8_t output_control_mode_candidate = 0;

    if (use_bypass_)
    {
      bypass_controller_ =
        tryToBypassController(input_control_mode_desired, output_control_mode_candidate);
    }
    else
    {
      bypass_controller_ = false;
    }

    if (!bypass_controller_)
    {
      bool success = findSuitableOutputControlModeForPlatformInputMode(output_control_mode_candidate,
                                                                       input_control_mode_desired);
      if (!success)
      {
        RCLCPP_WARN(node_ptr_->get_logger(), "No suitable output control mode found");
        response->success = false;
        return;
      }

      success = checkSuitabilityInputMode(input_control_mode_desired, output_control_mode_candidate);
      if (!success)
      {
        RCLCPP_ERROR(node_ptr_->get_logger(),
                     "Input control mode is not suitable for this controller");
        response->success = false;
        return;
      }
    }

    // request the common mode to the platform
    auto mode_to_request = as2::convertUint8tToAS2ControlMode(output_control_mode_candidate);
    if (!setPlatformControlMode(mode_to_request))
    {
      RCLCPP_ERROR(node_ptr_->get_logger(), "Failed to set platform control mode");
      response->success = false;
      return;
    }

    input_mode_ = request->control_mode;
    output_mode_ = mode_to_request;

    if (bypass_controller_)
    {
      RCLCPP_INFO(node_ptr_->get_logger(), "Bypassing controller:");
      RCLCPP_INFO(node_ptr_->get_logger(), "input_mode:[%s]",
                  as2::controlModeToString(input_mode_).c_str());
      RCLCPP_INFO(node_ptr_->get_logger(), "output_mode:[%s]",
                  as2::controlModeToString(output_mode_).c_str());
      as2::printControlMode(output_mode_);
      auto unset_mode = as2::convertUint8tToAS2ControlMode(UNSET_MODE_MASK);
      response->success = setMode(unset_mode, unset_mode);
      control_mode_established_ = response->success;
      return;
    }

    RCLCPP_INFO(node_ptr_->get_logger(), "input_mode:[%s]",
                as2::controlModeToString(input_mode_).c_str());
    RCLCPP_INFO(node_ptr_->get_logger(), "output_mode:[%s]",
                as2::controlModeToString(output_mode_).c_str());

    response->success = setMode(input_mode_, output_mode_);
    control_mode_established_ = response->success;

    if (!response->success)
    {
      RCLCPP_ERROR(node_ptr_->get_logger(), "Failed to set control mode in the controller");
    }
    state_adquired_ = false;
    motion_reference_adquired_ = false;
    return;
  }

  void ControllerBase::sendCommand()
  {
    if (bypass_controller_)
    {
      if (!motion_reference_adquired_)
      {
        auto &clock = *node_ptr_->get_clock();
        RCLCPP_INFO_THROTTLE(node_ptr_->get_logger(), clock, 1000, "Waiting for motion reference");

        return;
      }
      pose_pub_->publish(ref_pose_);
      twist_pub_->publish(ref_twist_);
      return;
    }
    geometry_msgs::msg::PoseStamped pose;
    geometry_msgs::msg::TwistStamped twist;
    as2_msgs::msg::Thrust thrust;
    computeOutput(pose, twist, thrust);

    // set time stamp
    pose.header.stamp = node_ptr_->now();
    twist.header.stamp = pose.header.stamp;
    thrust.header = pose.header;

    pose_pub_->publish(pose);
    twist_pub_->publish(twist);
    thrust_pub_->publish(thrust);
  };

} // namespace controller_plugin_base
