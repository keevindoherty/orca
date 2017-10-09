#include <std_msgs/Bool.h>

#include "orca_base/orca_util.h"
#include "orca_base/orca_base.h"
#include "orca_base/Camera.h"
#include "orca_base/Lights.h"
#include "orca_base/Thruster.h"

// Joy message axes:
// TODO move to yaml
#define JOY_AXIS_YAW            0   // Left stick left/right; 1.0 is left and -1.0 is right
#define JOY_AXIS_FORWARD        1   // Left stick up/down; 1.0 is forward and -1.0 is backward
#define JOY_AXIS_STRAFE         3   // Right stick left/right; 1.0 is left and -1.0 is right
#define JOY_AXIS_VERTICAL       4   // Right stick up/down; 1.0 is ascend and -1.0 is descend
#define JOY_AXIS_YAW_TRIM       6   // Trim left/right; acts like 2 buttons; 1.0 for left and -1.0 for right
#define JOY_AXIS_VERTICAL_TRIM  7   // Trim up/down; acts like 2 buttons; 1.0 for up and -1.0 for down

// Unused axes:
// 2 Left trigger; starts from 1.0 and moves to -1.0
// 5 Right trigger; starts from 1.0 and moves to -1.0

// Joy message buttons:
// TODO move to yaml
#define JOY_BUTTON_DISARM       6   // View
#define JOY_BUTTON_ARM          7   // Menu
#define JOY_BUTTON_MANUAL       0   // A
#define JOY_BUTTON_STABILIZE    2   // X
#define JOY_BUTTON_DEPTH_HOLD   3   // Y
#define JOY_BUTTON_SURFACE      1   // B
#define JOY_CAMERA_TILT_DOWN    4   // Left bumper
#define JOY_CAMERA_TILT_UP      5   // Right bumper
#define JOY_LIGHTS_BRIGHT       9   // Left stick
#define JOY_LIGHTS_DIM          10  // Right stick

// Unused buttons:
// 8 Logo

// Trim increments
// TODO move to yaml
#define PI          3.14159
#define INC_YAW     PI/36
#define INC_DEPTH   0.1
#define INC_TILT    0.2   // Values range -1.0 to 1.0
#define INC_LIGHTS  0.2   // Values range 0.0 to 1.0

// Don't respond to tiny joystick movements
// TODO move to yaml
#define INPUT_DEAD_BAND 0.05

// Don't publish tiny thruster efforts
// TODO move to yaml
#define EFFORT_DEAD_BAND 0.01

// Publish messages at 100Hz
#define SPIN_RATE 100

// Covariance for /odom messages. We assume that roll and pitch are fixed.
// TODO covariance for depth might also be MAX_DBL???
static const double MAX_DBL = std::numeric_limits<double>::max();
static const double COVARIANCE[36] =
{
  1e-5,    1e-5,    1e-5,    0.0,     0.0,     1e-5,
  1e-5,    1e-5,    1e-5,    0.0,     0.0,     1e-5,
  1e-5,    1e-5,    1e-5,    0.0,     0.0,     1e-5,
  0.0,     0.0,     0.0,     MAX_DBL, 0.0,     0.0,
  0.0,     0.0,     0.0,     0.0,     MAX_DBL, 0.0,
  1e-5,    1e-5,    1e-5,    0.0,     0.0,     1e-5
};

namespace orca_base {

OrcaBase::OrcaBase(ros::NodeHandle &nh, tf::TransformListener &tf):
  nh_{nh},
  tf_{tf},
  mode_{Mode::disarmed},
  forward_effort_{0},
  yaw_effort_{0},
  strafe_effort_{0},
  vertical_effort_{0},
  tilt_{0},
  tilt_trim_button_previous_{false},
  lights_{0},
  lights_trim_button_previous_{false}
{
  // Set up all subscriptions
  baro_sub_ = nh_.subscribe<orca_base::Depth>("/depth", 10, &OrcaBase::baroCallback, this);
  imu_sub_ = nh_.subscribe<sensor_msgs::Imu>("/imu", 10, &OrcaBase::imuCallback, this);
  joy_sub_ = nh_.subscribe<sensor_msgs::Joy>("/joy", 10, &OrcaBase::joyCallback, this);
  yaw_control_effort_sub_ = nh_.subscribe<std_msgs::Float64>("/yaw_control_effort", 10, &OrcaBase::yawControlEffortCallback, this);
  depth_control_effort_sub_ = nh_.subscribe<std_msgs::Float64>("/depth_control_effort", 10, &OrcaBase::depthControlEffortCallback, this);

  // Advertise all topics that we'll publish on
  thruster_pub_ = nh_.advertise<orca_base::Thruster>("/thruster", 1);
  camera_tilt_pub_ = nh_.advertise<orca_base::Camera>("/camera_tilt", 1);
  lights_pub_ = nh_.advertise<orca_base::Lights>("/lights", 1);
  yaw_pid_enable_pub_ = nh_.advertise<std_msgs::Bool>("/yaw_pid_enable", 1);
  yaw_state_pub_ = nh_.advertise<std_msgs::Float64>("/yaw_state", 1);
  yaw_setpoint_pub_ = nh_.advertise<std_msgs::Float64>("/yaw_setpoint", 1);
  depth_pid_enable_pub_ = nh_.advertise<std_msgs::Bool>("/depth_pid_enable", 1);
  depth_state_pub_ = nh_.advertise<std_msgs::Float64>("/depth_state", 1);
  depth_setpoint_pub_ = nh_.advertise<std_msgs::Float64>("/depth_setpoint", 1);
  odom_pub_ = nh_.advertise<nav_msgs::Odometry>("/odom", 1);

  // Prepopulate odom message
  odom_msg_.header.frame_id = "odom";
  odom_msg_.child_frame_id = "base_link";
  for (int i = 0; i < 36; i++)
  {
    odom_msg_.pose.covariance[i] = COVARIANCE[i];
    odom_msg_.twist.covariance[i] = COVARIANCE[i];
  }
}

// New depth reading
void OrcaBase::baroCallback(const orca_base::Depth::ConstPtr& baro_msg)
{
  depth_state_ = baro_msg->depth;
}

// New imu reading
void OrcaBase::imuCallback(const sensor_msgs::ImuConstPtr &msg)
{
  tf::Quaternion q;
  double roll, pitch, yaw;

  tf::quaternionMsgToTF(msg->orientation, q);
  tf::Matrix3x3(q).getRPY(roll, pitch, yaw);

  yaw_state_ = yaw;
}

// Result of yaw pid controller
void OrcaBase::yawControlEffortCallback(const std_msgs::Float64::ConstPtr& msg)
{
  if (mode_ == Mode::stabilize || mode_ == Mode::depth_hold)
  {
    yaw_effort_ = dead_band(msg->data, EFFORT_DEAD_BAND);
  }
}

// Result of depth pid controller
void OrcaBase::depthControlEffortCallback(const std_msgs::Float64::ConstPtr& msg)
{
  if (mode_ == Mode::depth_hold)
  {
    vertical_effort_ = dead_band(msg->data, EFFORT_DEAD_BAND);  
  }
}

void OrcaBase::publishYawSetpoint()
{
  std_msgs::Float64 setpoint;
  setpoint.data = yaw_setpoint_;
  yaw_setpoint_pub_.publish(setpoint);
}

void OrcaBase::publishDepthSetpoint()
{
  std_msgs::Float64 setpoint;
  setpoint.data = depth_setpoint_;
  depth_setpoint_pub_.publish(setpoint);
}

void OrcaBase::publishCameraTilt()
{
  orca_base::Camera msg;
  msg.tilt = tilt_;
  camera_tilt_pub_.publish(msg);
}

void OrcaBase::publishLights()
{
  orca_base::Lights msg;
  msg.brightness = lights_;
  lights_pub_.publish(msg);
}

void OrcaBase::publishOdom()
{
  odom_msg_.header.stamp = ros::Time::now();

  // Publish a niave odom based on thrust effort and depth.
  //
  // BlueROV2 specs: http://docs.bluerobotics.com/brov2/
  // T200 specs: http://docs.bluerobotics.com/thrusters/t200/
  //
  // Drag equation (The ROV Manual, Christ and Wernli, pp82-84):
  //    drag = 1/2 * sigma * area * velocity^2 * drag_coefficient
  //    sigma = density of seawater / gravitional_accel = 1035 kg/m^3 / 9.8 m/s^2 = 105.6
  //    area = cross-sectional area facing direction of motion
  //    drag_coefficient = 0.9 for the ROV, and 1.2 for the unfaired tether
  //
  // Simplifying assumptions:
  //    thrust force ~= PWM ~= effort
  //    tether drag in xy ~= depth
  //    ignore acceleration, so thrust force == drag force
  //    cross-sectional area is the same for forward and lateral thrust, and constant during rotations
  //    voltage = 16V
  //
  // TODO monitor voltage
  // TODO modify drag equation for rotation

  #define SIGMA 105.6
  #define ROV_AREA 0.0859
  #define ROV_DRAG_COEF 0.9
  #define TETHER_DIAM 0.008
  #define TETHER_DRAG_COEF 1.2
  #define MAX_THRUST_XY 14 // Forward and lateral bollard thrust

  // Thrust force
  // TODO calc yaw force
  double thrust_force_x = MAX_THRUST_XY * strafe_effort_;
  double thrust_force_y = MAX_THRUST_XY * forward_effort_;
  // double thrust_force_yaw = MAX_THRUST_YAW * yaw_effort_;
  
  // Velocity
  // TODO handle tethered case
  // TODO calc depth velocity
  odom_msg_.twist.twist.linear.x = sqrt(thrust_force_x * 2 / SIGMA / ROV_AREA / ROV_DRAG_COEF);
  odom_msg_.twist.twist.linear.y = sqrt(thrust_force_y * 2 / SIGMA / ROV_AREA / ROV_DRAG_COEF);
  //odom_msg_.twist.twist.linear.z = sqrt(thrust_force_z * 2 / SIGMA / ROV_AREA / ROV_DRAG_COEF);
  //odom_msg_.twist.twist.angular.z = thrust_force_yaw / drag_coef_yaw;

  // Populate position info
  // TODO calc xy distance moved since last time
  // geometry_msgs::Quaternion quat = tf::createQuaternionMsgFromRollPitchYaw(0, 0, pose.yaw);
  // odom_msg_.pose.pose.position.x = pose.x;
  // odom_msg_.pose.pose.position.y = pose.y;
  odom_msg_.pose.pose.position.z = depth_state_;
  // odom_msg_.pose.pose.orientation = quat;

  odom_pub_.publish(odom_msg_);
}

// Change operation mode
void OrcaBase::setMode(Mode mode, double depth_setpoint = 0.0)
{
  // TODO mutex critical state
  mode_ = mode;  

  if (mode == Mode::depth_hold)
  {
    // Turn on depth pid controller
    std_msgs::Bool enable;
    enable.data = true;
    depth_pid_enable_pub_.publish(enable);
    
    // Set target depth
    depth_setpoint_ = depth_setpoint;
    publishDepthSetpoint();

    // Clear button state
    depth_trim_button_previous_ = false;    
  }
  else
  {
    // Turn off depth pid controller
    std_msgs::Bool enable;
    enable.data = false;
    depth_pid_enable_pub_.publish(enable);
  }

  if (mode == Mode::stabilize || mode == Mode::depth_hold)
  {
    // Turn on yaw pid controller
    std_msgs::Bool enable;
    enable.data = true;
    yaw_pid_enable_pub_.publish(enable);
    
    // Set target angle
    yaw_setpoint_ = yaw_state_;
    publishYawSetpoint();

    // Clear button state
    yaw_trim_button_previous_ = false;
  }
  else
  {
    // Turn off yaw pid controller
    std_msgs::Bool enable;
    enable.data = false;
    yaw_pid_enable_pub_.publish(enable);
  }

  if (mode == Mode::disarmed)
  {
    forward_effort_ = yaw_effort_ = strafe_effort_ = vertical_effort_ = 0;
  }
}

// New input from the gamepad
void OrcaBase::joyCallback(const sensor_msgs::Joy::ConstPtr& joy_msg)
{
  // Arm/disarm
  if (joy_msg->buttons[JOY_BUTTON_DISARM])
  {
    ROS_INFO("Disarmed");
    setMode(Mode::disarmed);
  }
  else if (joy_msg->buttons[JOY_BUTTON_ARM])
  {
    ROS_INFO("Armed, manual");
    setMode(Mode::manual);
  }

  // If we're disarmed, ignore everything else
  if (mode_ == Mode::disarmed)
  {
    ROS_INFO("Disarmed, ignoring further input");    
    return;
  }

  // Select a mode
  if (joy_msg->buttons[JOY_BUTTON_MANUAL])
  {
    ROS_INFO("Manual");
    setMode(Mode::manual);
  }
  else if (joy_msg->buttons[JOY_BUTTON_STABILIZE])
  {
    ROS_INFO("Stabilize");
    setMode(Mode::stabilize);
  }
  else if (joy_msg->buttons[JOY_BUTTON_DEPTH_HOLD])
  {
    ROS_INFO("Depth hold");
    setMode(Mode::depth_hold, depth_state_);
  }
  else if (joy_msg->buttons[JOY_BUTTON_SURFACE])
  {
    ROS_INFO("Surface");
    setMode(Mode::depth_hold, 10.0); // TODO create notion of 'underwater' in gazebo, and set target depth to 0
  }

  // Yaw trim
  // TODO support faster trim method?
  if (joy_msg->axes[JOY_AXIS_YAW_TRIM] && !yaw_trim_button_previous_)
  {
    // Rising edge
    if ((mode_ == Mode::stabilize || mode_ == Mode::depth_hold))
    {
      // TODO deal w/ wraparound
      yaw_setpoint_ = joy_msg->axes[JOY_AXIS_YAW_TRIM] > 0 ? yaw_setpoint_ + INC_YAW : yaw_setpoint_ - INC_YAW;
      publishYawSetpoint();
    }

    yaw_trim_button_previous_ = true;
  }
  else
  {
    // Falling edge
    yaw_trim_button_previous_ = false;
  }

  // Depth trim
  // TODO support faster trim method?
  if (joy_msg->axes[JOY_AXIS_VERTICAL_TRIM] && !depth_trim_button_previous_)
  {
    // Rising edge
    if (mode_ == Mode::depth_hold)
    {
      // TODO clamp this to the surface
      depth_setpoint_ = joy_msg->axes[JOY_AXIS_VERTICAL_TRIM] > 0 ? depth_setpoint_ + INC_DEPTH : depth_setpoint_ - INC_DEPTH;
      publishDepthSetpoint();
    }

    depth_trim_button_previous_ = true;
  }
  else
  {
    // Falling edge
    depth_trim_button_previous_ = false;
  }

  // Camera tilt
  if (!tilt_trim_button_previous_)
  {
    if (joy_msg->buttons[JOY_CAMERA_TILT_UP])
    {
      tilt_ = clamp(tilt_ + INC_TILT, -1.0, 1.0);
      publishCameraTilt();
      tilt_trim_button_previous_ = true;
    }
    else if (joy_msg->buttons[JOY_CAMERA_TILT_DOWN])
    {
      tilt_ = clamp(tilt_ - INC_TILT, -1.0, 1.0);
      publishCameraTilt();
      tilt_trim_button_previous_ = true;    
    }
    else
    {
      tilt_trim_button_previous_ = false;
    }
  }

  // Lights
  if (!lights_trim_button_previous_)
  {
    if (joy_msg->buttons[JOY_LIGHTS_BRIGHT])
    {
      lights_ = clamp(lights_ + INC_LIGHTS, 0.0, 1.0);
      publishLights();
      lights_trim_button_previous_ = true;
    }
    else if (joy_msg->buttons[JOY_LIGHTS_DIM])
    {
      lights_ = clamp(lights_ - INC_LIGHTS, 0.0, 1.0);
      publishLights();
      lights_trim_button_previous_ = true;
    }
    else
    {
      lights_trim_button_previous_ = false;      
    }
  }

  // Respond to thruster controls
  forward_effort_ = dead_band((double)joy_msg->axes[JOY_AXIS_FORWARD], INPUT_DEAD_BAND);
  if (mode_ == Mode::manual)
  {
    yaw_effort_ = dead_band((double)joy_msg->axes[JOY_AXIS_YAW], INPUT_DEAD_BAND);
  }
  strafe_effort_ = dead_band((double)joy_msg->axes[JOY_AXIS_STRAFE], INPUT_DEAD_BAND);
  if (mode_ == Mode::manual || mode_ == Mode::stabilize)
  {
    vertical_effort_ = dead_band((double)joy_msg->axes[JOY_AXIS_VERTICAL], INPUT_DEAD_BAND);
  }
}

// Called at 100Hz; publish various messages
void OrcaBase::SpinOnce(const ros::TimerEvent &event)
{
  // Set target yaw
  if (mode_ == Mode::stabilize || mode_ == Mode::depth_hold)
  {
    std_msgs::Float64 yaw_state;
    yaw_state.data = yaw_state_;
    yaw_state_pub_.publish(yaw_state);
  }

  // Set target depth
  if (mode_ == Mode::depth_hold)
  {
    std_msgs::Float64 depth_state;
    depth_state.data = depth_state_;
    depth_state_pub_.publish(depth_state);
  }

  // Set thruster efforts. Note that strafe and yaw are 1.0 for left, -1.0 for right.
  // Order must match the order of the <thruster> tags in the URDF.
  // 3 of the thrusters spin cw, and 3 spin ccw; see URDF for details.
  orca_base::Thruster thruster_msg;
  thruster_msg.effort.push_back(clamp(forward_effort_ + strafe_effort_ + yaw_effort_, -1.0, 1.0));
  thruster_msg.effort.push_back(clamp(forward_effort_ - strafe_effort_ - yaw_effort_, -1.0, 1.0));
  thruster_msg.effort.push_back(clamp(forward_effort_ - strafe_effort_ + yaw_effort_, -1.0, 1.0));
  thruster_msg.effort.push_back(clamp(forward_effort_ + strafe_effort_ - yaw_effort_, -1.0, 1.0));
  thruster_msg.effort.push_back(clamp(vertical_effort_, -1.0, 1.0));
  thruster_msg.effort.push_back(clamp(-vertical_effort_, -1.0, 1.0));
  thruster_pub_.publish(thruster_msg);

  // TODO publish odometry
}

} // namespace orca_base

int main(int argc, char **argv)
{
  ros::init(argc, argv, "orca_base");
  ros::NodeHandle nh{"~"};
  tf::TransformListener tf{nh};
  orca_base::OrcaBase orca_base{nh, tf};

  ros::Timer t = nh.createTimer(ros::Duration(1.0 / SPIN_RATE), &orca_base::OrcaBase::SpinOnce, &orca_base);

  ros::spin();

  return 0;
}