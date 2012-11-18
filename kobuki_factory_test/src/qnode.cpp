/**
 * @file /src/qnode.cpp
 *
 * @brief Ros communication central!
 *
 * @date February 2011
 **/

/*****************************************************************************
** Includes
*****************************************************************************/

#include <math.h>
#include <sstream>
#include <cstdarg>

#include <tf/tf.h>
#include <ros/ros.h>

#include <geometry_msgs/Twist.h>
#include <kobuki_msgs/Led.h>
#include <kobuki_msgs/Sound.h>
#include <kobuki_msgs/DigitalOutput.h>

#include "../include/kobuki_factory_test/qnode.hpp"
#include "../include/kobuki_factory_test/test_imu.hpp"

/*****************************************************************************
** Namespaces
*****************************************************************************/

namespace kobuki_factory_test {

/*****************************************************************************
** Constants
*****************************************************************************/

#define TEST_MOTORS_V    0.2        // lin. speed, in m/s
#define TEST_MOTORS_W   (M_PI/2.0)  // ang. speed, in rad/s
#define TEST_MOTORS_D    0.4        // distance, in m
#define TEST_MOTORS_A   (1.0*M_PI)  // turning, in rad
#define TEST_BUMPERS_V   0.1
#define TEST_BUMPERS_W  (M_PI/5.0)
#define TEST_GYRO_W     (M_PI/3.0)
#define TEST_GYRO_A     (2.0*M_PI)  // 360 deg, clockwise + counter cw.

/*****************************************************************************
** Implementation
*****************************************************************************/

// Define a postfix increment operator for QNode::EvalStep
inline QNode::EvalStep operator++(QNode::EvalStep& es, int)
{
  return es = (QNode::EvalStep)(es + 1);
}


QNode::QNode(int argc, char** argv ) :
  init_argc(argc),
  init_argv(argv),
  frequency(20.0),
  under_eval(NULL),
  answer_req(false)
  {}

QNode::~QNode() {
  if(ros::isStarted()) {
    ros::shutdown(); // explicitly needed since we use ros::start();
    ros::waitForShutdown();
  }
  wait();
}

void QNode::versionInfoCB(const kobuki_msgs::VersionInfo::ConstPtr& msg) {
  if (under_eval == NULL)
    return;

  // Check if we already have version info for the current robot
  if (under_eval->device_ok[Robot::V_INFO] == true) {
    if (std::equal(msg->udid.begin(), msg->udid.end(), under_eval->u_dev_id.begin()) == true) {
      // This is ok but... a bit weird; why should robot resend the version info?
      log(Debug, "Version info received more that once for %s", under_eval->serial.c_str());
      return;
    }
    else {
      // This could happen if the driver takes long time to republish the version info after a new
      // robot comes online; but is weird; we should avoid this possibility in the driver itself
      std::string old_sn = under_eval->serial;
      under_eval->setSerial(msg->udid);
      log(Warn, "Overwriting version info: old SN: %s / new SN: %s",
          old_sn.c_str(), under_eval->serial.c_str());
    }
  }
  else
    under_eval->setSerial(msg->udid);

  // Check if this robot has been previously evaluated; we don't allow reevaluation
  // TODO/WARN: ONLY IN CURRENT SESION; WE DON'T RELOAD RESULT FILES
  if (evaluated.get(under_eval->serial) != NULL) {
    showUserMsg(Error, "Known robot",
                "Robot %s has been previously evaluated. Proceed with a new robot",
                under_eval->serial.c_str());
    delete under_eval;
    under_eval = NULL;
    return;
  }

  under_eval->device_val[Robot::V_INFO] |= msg->firmware; under_eval->device_val[Robot::V_INFO] <<= 16;
  under_eval->device_val[Robot::V_INFO] |= msg->hardware; under_eval->device_val[Robot::V_INFO] <<= 32;
  under_eval->device_val[Robot::V_INFO] |= msg->software;
  under_eval->device_ok[Robot::V_INFO] = true;

  log(Info, "UDID: %s. Hardware/firmware/software version: %s",
      under_eval->serial.c_str(), under_eval->version_nb().c_str());
}

void QNode::sensorsCoreCB(const kobuki_msgs::SensorState::ConstPtr& msg) {
  if (under_eval == NULL)
    return;

  if ((current_step >= TEST_MOTORS_FORWARD) && (current_step <= TEST_MOTORS_COUNTERCW)) {
    under_eval->device_val[Robot::MOTOR_L] =
        std::max((uint8_t)under_eval->device_val[Robot::MOTOR_L], msg->current[0]);
    under_eval->device_val[Robot::MOTOR_R] =
        std::max((uint8_t)under_eval->device_val[Robot::MOTOR_R], msg->current[1]);
    return;
  }

  if (current_step == MEASURE_CHARGE_AND_PSD) {
    if (msg->charger)
      under_eval->device_val[Robot::CHARGING] = msg->battery;

    for (unsigned int i = 0; i < msg->bottom.size(); i++)
      under_eval->psd_data[i] = (under_eval->psd_data[i] == 0)?msg->bottom[i]
                               :(under_eval->psd_data[i] + msg->bottom[i])/2.0;

    return;
  }

  if (current_step == TEST_ANALOG_INPUT_PORTS) {
    for (unsigned int i = 0; i < msg->analog_input.size(); i++) {
      if (under_eval->analog_in[i][AI_VAL] != -1)
        under_eval->analog_in[i][AI_INC] = msg->analog_input[i] - under_eval->analog_in[i][AI_VAL];
      under_eval->analog_in[i][AI_VAL] = msg->analog_input[i];

      under_eval->analog_in[i][AI_MIN] =
          std::min(under_eval->analog_in[i][AI_MIN], (int16)msg->analog_input[i]);
      under_eval->analog_in[i][AI_MAX] =
          std::max(under_eval->analog_in[i][AI_MAX], (int16)msg->analog_input[i]);
    }
  }
}

void QNode::dockBeaconCB(const kobuki_msgs::DockInfraRed::ConstPtr& msg) {
  if ((under_eval == NULL) || (under_eval->ir_dock_ok() == true))
    return;

  // Collect ir dock readings at any moment;
  // TODO should we restrict it to an evaluation step? and be more exigent?
  for (unsigned int i = 0; i < msg->data.size(); i++) {
    if (msg->data[i] > 0) {
      under_eval->device_ok[Robot::IR_DOCK_L + i]  = true;
      under_eval->device_val[Robot::IR_DOCK_L + i] = msg->data[i];
      under_eval->device_cmp[Robot::IR_DOCK_L + i] = 1.0f;
    }
  }

  if (under_eval->ir_dock_ok() == true)
    log(Info, "Docking ir sensor evaluation completed: %d/%d/%d",
                under_eval->device_val[Robot::IR_DOCK_L],
                under_eval->device_val[Robot::IR_DOCK_C],
                under_eval->device_val[Robot::IR_DOCK_R]);
}

void QNode::gyroscopeCB(const sensor_msgs::Imu::ConstPtr& msg) {
  if (under_eval == NULL)
    return;

/////////  std::cout << tf::getYaw(msg->orientation) << "\n";
  under_eval->imu_data[4] = tf::getYaw(msg->orientation);
}

void QNode::buttonEventCB(const kobuki_msgs::ButtonEvent::ConstPtr& msg) {
  if (under_eval == NULL)
    return;

  if (((current_step == TEST_LEDS)   ||
       (current_step == TEST_SOUNDS) ||
       (current_step == TEST_DIGITAL_IO_PORTS)) &&
      (answer_req == true) && (msg->state  == kobuki_msgs::ButtonEvent::RELEASED)) {
    // We are currently evaluating a device that requires tester feedback
    // he must press the left button if test is ok or the right otherwise
    if ((msg->button == kobuki_msgs::ButtonEvent::Button0) ||
        (msg->button == kobuki_msgs::ButtonEvent::Button2)) {
      if (current_step == TEST_LEDS) {
        under_eval->device_ok[Robot::LED_1]  = msg->button == kobuki_msgs::ButtonEvent::Button0;
        under_eval->device_ok[Robot::LED_2]  = msg->button == kobuki_msgs::ButtonEvent::Button0;
        under_eval->device_cmp[Robot::LED_1] = under_eval->device_ok[Robot::LED_1]?1.0f:0.0f;
        under_eval->device_cmp[Robot::LED_2] = under_eval->device_ok[Robot::LED_2]?1.0f:0.0f;
      }
      else if (current_step == TEST_SOUNDS) {
        under_eval->device_ok[Robot::SOUNDS]  = msg->button == kobuki_msgs::ButtonEvent::Button0;
        under_eval->device_cmp[Robot::SOUNDS] = under_eval->device_ok[Robot::SOUNDS]?1.0f:0.0f;
      }
      else if (current_step == TEST_DIGITAL_IO_PORTS) {
        under_eval->device_ok[Robot::D_INPUT]   = msg->button == kobuki_msgs::ButtonEvent::Button0;
        under_eval->device_ok[Robot::D_OUTPUT]  = msg->button == kobuki_msgs::ButtonEvent::Button0;
        under_eval->device_cmp[Robot::D_INPUT]  = under_eval->device_ok[Robot::D_INPUT]?1.0f:0.0f;
        under_eval->device_cmp[Robot::D_OUTPUT] = under_eval->device_ok[Robot::D_OUTPUT]?1.0f:0.0f;
      }

      if (msg->button == kobuki_msgs::ButtonEvent::Button0)
        log(Info, "%s evaluation completed", (current_step == TEST_LEDS)?"LEDs":(current_step == TEST_SOUNDS)?"Sounds":"Digital I/O");
      else if (msg->button == kobuki_msgs::ButtonEvent::Button2)
        log(Warn, "%s didn't pass the test", (current_step == TEST_LEDS)?"LEDs":(current_step == TEST_SOUNDS)?"Sounds":"Digital I/O");

      answer_req = false;  // disable user input to avoid "accumulating" answers
      hideUserMsg();
      current_step++;
    }

    return;
  }

  // Buttons evaluation is completed
  if (under_eval->buttons_ok() == true)
    return;

  if ((current_step < BUTTON_0_PRESSED) || (current_step > BUTTON_2_RELEASED)) {
    // It's not time to evaluate buttons; assume it's an accidental hit
    log(Debug, "Button %d %s; ignoring", msg->button, msg->state?"pressed":"released");
    return;
  }

  // Note that we assume that buttons are 0, 1 and 2, and states are 1 (pressed) and 0 (released)
  uint8_t expected_button = (current_step - BUTTON_0_PRESSED)/2;
  uint8_t expected_action = (current_step - BUTTON_0_PRESSED)%2 ^ 1;

  if ((msg->button == expected_button) && (msg->state == expected_action)) {
    Robot::Device dev = (Robot::Device)(Robot::BUTTON_0 + msg->button);

    log(Info, "Button %d %s, as expected", msg->button, msg->state?"pressed":"released");

    if (msg->state == kobuki_msgs::ButtonEvent::RELEASED)
      under_eval->device_ok[dev] = true;

    if (current_step == BUTTON_2_RELEASED)
      log(Info, "Buttons evaluation completed");

    under_eval->device_cmp[dev] += 0.5f;
    current_step++;
  }
  else
    log(Warn, "Unexpected button event: %d %s", msg->button, msg->state?"pressed":"released");
}

void QNode::bumperEventCB(const kobuki_msgs::BumperEvent::ConstPtr& msg) {
  if ((under_eval == NULL) || (under_eval->bumpers_ok() == true))
    return;

  if ((current_step < CENTER_BUMPER_PRESSED) || (current_step > LEFT_BUMPER_RELEASED)) {
    // it's not time to evaluate bumpers; assume it's an accidental hit
    log(Debug, "Bumper %d accidental hit; ignoring", msg->bumper);
    return;
  }

  // Note that we assume that bumpers are 0, 1 and 2, and states are 1 (pressed) and 0 (released)
  int expected_bumper = ((current_step - CENTER_BUMPER_PRESSED)/3 + 1) % 3;
  int expected_action = ((current_step - CENTER_BUMPER_PRESSED)%3 ^ 1);

  if ((msg->bumper == expected_bumper) && (msg->state == expected_action)) {
    log(Info, "Bumper %d %s, as expected", msg->bumper, msg->state?"pressed":"released");
    under_eval->device_val[Robot::BUMPER_L + msg->bumper]++;

    if (msg->state == kobuki_msgs::BumperEvent::PRESSED) {
      under_eval->device_cmp[Robot::BUMPER_L + msg->bumper] = 0.5f;
      move(-TEST_BUMPERS_V, 0.0, 1.5);
      current_step++;
    }
    else {
      under_eval->device_ok[Robot::BUMPER_L + msg->bumper] = true;
      under_eval->device_cmp[Robot::BUMPER_L + msg->bumper] = 1.0f;
      hideUserMsg();
    }

    if (current_step == LEFT_BUMPER_RELEASED)
      log(Info, "Bumper evaluation completed");
  }
  else
    log(Warn, "Unexpected bumper event: %d %s", msg->bumper, msg->state?"pressed":"released");
}

void QNode::wDropEventCB(const kobuki_msgs::WheelDropEvent::ConstPtr& msg) {
  if ((under_eval == NULL) || (current_step != TEST_WHEEL_DROP_SENSORS))
    return;

  Robot::Device dev =
      (msg->wheel == kobuki_msgs::WheelDropEvent::LEFT)?Robot::W_DROP_L:Robot::W_DROP_R;
  if (under_eval->device_ok[dev] == true)
    return;

  if (((msg->state == kobuki_msgs::WheelDropEvent::DROPPED) &&
       (under_eval->device_val[dev] % 2 == 0)) ||
      ((msg->state == kobuki_msgs::WheelDropEvent::RAISED) &&
       (under_eval->device_val[dev] % 2 == 1))) {
    log(Info, "%s wheel %s, as expected", msg->wheel?"Right":"Left", msg->state?"dropped":"raised");
    under_eval->device_val[dev]++;
    under_eval->device_cmp[dev] += under_eval->device_val[dev]/(wheel_drop_tests*2.0);

    if (under_eval->device_val[dev] >= wheel_drop_tests*2) {
      log(Info, "%s wheel drop evaluation completed", msg->wheel?"Right":"Left");
      under_eval->device_ok[dev] = true;

      // Wheel drop sensors evaluation complete
      if (under_eval->w_drop_ok() == true)
        current_step++;
    }
  }
  else
    log(Warn, "Unexpected wheel drop event: %d, %d", msg->wheel, msg->state);
}

void QNode::cliffEventCB(const kobuki_msgs::CliffEvent::ConstPtr& msg) {
  if ((under_eval == NULL) || (current_step != TEST_CLIFF_SENSORS))
    return;

  Robot::Device dev = (msg->sensor == kobuki_msgs::CliffEvent::LEFT) ?Robot::CLIFF_L:
                      (msg->sensor == kobuki_msgs::CliffEvent::RIGHT)?Robot::CLIFF_R:
                                                                      Robot::CLIFF_C;
  if (under_eval->device_ok[dev] == true)
    return;

  if (((msg->state == kobuki_msgs::CliffEvent::CLIFF) &&
       (under_eval->device_val[dev] % 2 == 0)) ||
      ((msg->state == kobuki_msgs::CliffEvent::FLOOR) &&
       (under_eval->device_val[dev] % 2 == 1))) {
    log(Info, "%s cliff sensor reports %s, as expected",
         dev == Robot::CLIFF_R?"Right":dev == Robot::CLIFF_C?"Center":"Left",
        msg->state?"cliff":"no cliff");
    under_eval->device_val[dev]++;
    under_eval->device_cmp[dev] += under_eval->device_val[dev]/(cliff_sensor_tests*2.0);

    if (under_eval->device_val[dev] >= cliff_sensor_tests*2) {
      log(Info, "%s cliff sensor evaluation completed",
           dev == Robot::CLIFF_R?"Right":dev == Robot::CLIFF_C?"Center":"Left");
      under_eval->device_ok[dev] = true;

      // Cliff sensors evaluation complete
      if (under_eval->cliffs_ok() == true)
        current_step++;
    }
  }
  else
    log(Warn, "Unexpected cliff sensor event: %d, %d", msg->sensor, msg->state);
}

void QNode::powerEventCB(const kobuki_msgs::PowerSystemEvent::ConstPtr& msg) {
  if ((under_eval == NULL) || (under_eval->pwr_src_ok() == true))
    return;

  if ((current_step != TEST_DC_ADAPTER) && (current_step != TEST_DOCKING_BASE)) {
    // It's not time to evaluate power sources; it can be just an irrelevant event but
    // also the tester can not be following the protocol or there's an error in the code
    if ((msg->event != kobuki_msgs::PowerSystemEvent::CHARGE_COMPLETED) &&
        (msg->event != kobuki_msgs::PowerSystemEvent::BATTERY_LOW)      &&
        (msg->event != kobuki_msgs::PowerSystemEvent::BATTERY_CRITICAL))
      log(Warn, "Power event %d while current step is %d", msg->event, current_step);

    return;
  }

  Robot::Device dev = (current_step == TEST_DC_ADAPTER)?Robot::PWR_JACK:Robot::PWR_DOCK;
  if (under_eval->device_ok[dev] == true)
    return;

#define PTAE kobuki_msgs::PowerSystemEvent::PLUGGED_TO_ADAPTER
#define PTDE kobuki_msgs::PowerSystemEvent::PLUGGED_TO_DOCKBASE

  // A bit tricky if... but not that much; gets true if:
  // the robot is plugged to currently evaluated power source and we expect so (device_val is even)
  // or the robot is unplugged (we don't know from which device) and we expect so (odd device_val)
  if (((((msg->event == PTAE) && (current_step == TEST_DC_ADAPTER)) ||
        ((msg->event == PTDE) && (current_step == TEST_DOCKING_BASE))) &&
        (under_eval->device_val[dev] % 2 == 0)) ||
      ((msg->event == kobuki_msgs::PowerSystemEvent::UNPLUGGED) &&
       (under_eval->device_val[dev] % 2 == 1))) {
    log(Info, "%s %s, as expected",
        (dev == Robot::PWR_JACK)?"Adapter":"Docking base", msg->event?"plugged":"unplugged");
    under_eval->device_val[dev]++;
    under_eval->device_cmp[dev] = under_eval->device_val[dev]/(power_plug_tests*2.0);

    if (under_eval->device_val[dev] >= power_plug_tests*2) {
      log(Info, "%s plugging evaluation completed",
          (dev == Robot::PWR_JACK)?"Adapter":"Docking base");
      under_eval->device_ok[dev] = true;
        current_step++;
    }
  }
  else
    log(Warn, "Unexpected power event: %d", msg->event);
}

void QNode::inputEventCB(const kobuki_msgs::DigitalInputEvent::ConstPtr& msg) {
  if ((under_eval == NULL) || (current_step != TEST_DIGITAL_IO_PORTS) ||
      (under_eval->device_ok[Robot::D_INPUT] == true))
    return;

  // Set the bit corresponding to pressed DI button; ask user when the 4-bit mask gets completed
  // We switch on/off I/O test board's LEDs together with the digital input events, so we evaluate
  // input and output simultaneously, thanks to tester's confirmation/rejection
  for (unsigned int i = 0; i < msg->values.size(); i++) {
    if (msg->values[i] == false) {
      under_eval->device_val[Robot::D_INPUT] |= (int)pow(2, i);
      kobuki_msgs::DigitalOutput cmd;
      cmd.values[i] = cmd.mask[i] = true;
      output_pub.publish(cmd);
      return;
    }
  }

  kobuki_msgs::DigitalOutput cmd;
  cmd.mask[0] = cmd.mask[1] = cmd.mask[2] = cmd.mask[3] = true;
  output_pub.publish(cmd);

  under_eval->device_cmp[Robot::D_INPUT]  = std::min(0.8f, under_eval->device_cmp[Robot::D_INPUT] + 0.2f);
  under_eval->device_cmp[Robot::D_OUTPUT] = std::min(0.8f, under_eval->device_cmp[Robot::D_OUTPUT] + 0.2f);

  if (under_eval->device_val[Robot::D_INPUT] == 0b00001111) {
    // All I/O tested; request tester confirmation
    showUserMsg(Info, "Digital I/O test",
              "Press left function button if LEDs blinked as expected or right otherwise");
    answer_req = true;
  }
}

void QNode::diagnosticsCB(const diagnostic_msgs::DiagnosticArray::ConstPtr& msg) {
  if (under_eval == NULL)
    return;

  std::stringstream diagnostics;

  for (unsigned int i = 0; i < msg->status.size(); i++) {
    diagnostics << "Device: "  <<      msg->status[i].name << std::endl;
    diagnostics << "Level: "   << (int)msg->status[i].level << std::endl;  // it's a char!
    diagnostics << "Message: " <<      msg->status[i].message << std::endl;
    for (unsigned int j = 0; j < msg->status[i].values.size(); j++) {
      diagnostics << "   " << msg->status[i].values[j].key
                  << ": "  << msg->status[i].values[j].value << std::endl;
    }
  }

  under_eval->diagnostics = diagnostics.str();
}

void QNode::robotStatusCB(const diagnostic_msgs::DiagnosticStatus::ConstPtr& msg) {
  if ((under_eval == NULL) || (under_eval->state == Robot::OK))
    return;

  under_eval->state = (Robot::State)msg->level;

  if (msg->level == diagnostic_msgs::DiagnosticStatus::OK) {
    log(Info, "Robot %s diagnostics received with OK status", under_eval->serial.c_str());
  }
  else {
    log(Warn, "Robot %s diagnostics received with %s status", under_eval->serial.c_str(),
         msg->level == diagnostic_msgs::DiagnosticStatus::WARN?"WARN":"ERROR");
    if (under_eval->diagnostics.size() > 0)
      log(Warn, "Full diagnostics:\n%s",  under_eval->diagnostics.c_str());
  }
}

void QNode::robotEventCB(const kobuki_msgs::RobotStateEvent::ConstPtr& msg) {
  if (msg->state == kobuki_msgs::RobotStateEvent::ONLINE) {
    if (under_eval != NULL) {
      log(Warn, "New robot connected while %s is still under evaluation; saving...",
           under_eval->serial.c_str());
      saveResults();  // TODO + WARN: if it's the same robot comming back after a cut in the serial, GET_SERIAL_NUMBER will prevent it to be re-tested...  can be problematic if such cuts are common
    }
    else {
      log(Info, "New robot connected");
    }

    // Go to the beginning of the test process and create a new robot object
    current_step = INITIALIZATION;
    under_eval = new Robot(evaluated.size());

    // Resubscribe to version_info to get robot version number (it's a latched topic)
    ros::NodeHandle nh;
    v_info_sub.shutdown();
    v_info_sub = nh.subscribe("mobile_base/version_info", 1, &QNode::versionInfoCB, this);
  }
  else if (msg->state == kobuki_msgs::RobotStateEvent::OFFLINE) {
    if (under_eval != NULL) {
      if (under_eval->all_ok() == false) {
        // Robot disconnected without finishing the evaluation; assume it failed to pass all tests
        log(Info, "Robot %s disconnected without finishing the evaluation",
             under_eval->serial.c_str());
      }
      else {
        log(Info, "Robot %s evaluation successfully completed", under_eval->serial.c_str());
      }

      saveResults();
    }
    else {
      // This should not happen
      log(Warn, "Robot offline event received, but no robot is under evaluation");
    }
  }
  else {
    // This should not happen
    log(Warn, "Unrecognized robot event received; ignoring");
  }
}

void QNode::timerEventCB(const ros::TimerEvent& event) {
  // jump to next step and stop robot
  current_step++;
  timer_active = false;
  move(0.0, 0.0);
}

void QNode::move(double v, double w, double t, bool blocking) {
  geometry_msgs::Twist vel;

  vel.linear.x  = v;
  vel.angular.z = w;
  cmd_vel_pub.publish(vel);
  if (t > 0.0) {
    if (blocking == true) {
      nbSleep(t);  // block this function but not the whole node!
      vel.linear.x  = 0.0;
      vel.angular.z = 0.0;
      cmd_vel_pub.publish(vel);
    }
    else {
      timer.stop();
      timer.setPeriod(ros::Duration(t));
      timer.start();
      timer_active = true;
    }
  }
}

void QNode::testLeds(bool first_call) {
  answer_req = ! first_call;  // require user input after the first iteration

  const char* COLOR[] = { "GREEN", "ORANGE", "RED" };

  kobuki_msgs::Led led;

  for (uint8_t c = kobuki_msgs::Led::GREEN; c <= kobuki_msgs::Led::RED &&
                                            current_step == TEST_LEDS; c++) {
    showUserMsg(Info, "LEDs test",
               "You should see both LEDs blinking in green, orange and red alternatively\n%s%s",
                first_call?"":"Press left function button if so or right otherwise\n",
                COLOR[c - kobuki_msgs::Led::GREEN]);
    led.value = c;
    led_1_pub.publish(led);
    led_2_pub.publish(led);

    nbSleep(1.0);

    led.value = kobuki_msgs::Led::BLACK;
    led_1_pub.publish(led);
    led_2_pub.publish(led);

    nbSleep(0.5);
  }
}

void QNode::testSounds(bool first_call) {
  answer_req = ! first_call;  // require user input after the first iteration

  const char* SOUND[] =
      { "ON", "OFF", "RECHARGE", "BUTTON", "ERROR", "CLEANING START", "CLEANING END" };

  kobuki_msgs::Sound sound;

  for (uint8_t s = kobuki_msgs::Sound::ON; s <= kobuki_msgs::Sound::CLEANINGEND &&
                                           current_step == TEST_SOUNDS; s++) {
    showUserMsg(Info, "Sounds test",
              "You should hear sounds for 'On', 'Off', 'Recharge', 'Button', " \
              "'Error', 'Cleaning Start' and 'Cleaning End' continuously\n%s%s",
              first_call?"":"Press left function button if so or right otherwise\n",
              SOUND[s - kobuki_msgs::Sound::ON]);
    sound.value = s;
    sound_pub.publish(sound);

    nbSleep(1.2);
  }
}

bool QNode::testIMU(bool first_call) {
  if (first_call == true) {
    // This should be executed only once
    showUserMsg(Info, "Gyroscope test",
                "Place the robot with the check board right below the camera");
  }

  std::string path;
  unsigned int dev = 0;    // Use the first video input by default
  ros::NodeHandle nh("~");
  nh.getParam("camera_device_index", (int&)dev);  // TODO control return = true!
  nh.getParam("camera_calibration_file", path);

  TestIMU imuTester;
  if (imuTester.init(path, dev) == false) {
    log(Error, "Gyroscope test initialization failed; aborting test");
    hideUserMsg();
    return false;
  }

  double vo_yaw[] = { std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::quiet_NaN() };

  for (unsigned int i = 0; i < 2; i++) {
    for (unsigned int j = 0; j < 30 && ros::ok(); j++) { // around 30 seconds before timeout
      nbSleep(0.2);
      vo_yaw[i] = - imuTester.getYaw();  // We invert, as the camera is looking AT the robot
      if (isnan(vo_yaw[i]) == false) {
        hideUserMsg();
        break;
      }

      showUserMsg(Warn, "Gyroscope test",
           "Cannot recognize the check board; please place the robot right below the camera");
    }

    if (isnan(vo_yaw[i]) == true) {
      log(Error, "Cannot recognize the check board after 80 attempts; gyroscope test aborted");
      hideUserMsg();
      return false;
    }

    double diff = under_eval->imu_data[4] - vo_yaw[i];
    if (diff > +M_PI) diff -= 2.0*M_PI; else if (diff < -M_PI) diff += 2.0*M_PI;
    log(Info, "Gyroscope test %d result: imu yaw = %.3f / vo yaw = %.3f / diff = %.3f", i + 1,
               under_eval->imu_data[4],  vo_yaw[i], diff);

    under_eval->imu_data[i*2] = under_eval->imu_data[4];
    under_eval->imu_data[i*2 + 1] = diff;

    under_eval->device_cmp[Robot::IMU_DEV] += 0.33f;

    if (i == 0) {
      move(0.0, +TEST_GYRO_W, TEST_GYRO_A/TEST_GYRO_W, true);  // +360 deg, blocking call
      move(0.0, -TEST_GYRO_W, TEST_GYRO_A/TEST_GYRO_W, true);  // -360 deg, blocking call
    }

    under_eval->device_val[Robot::IMU_DEV]++;
    ros::spinOnce();
  }

  if (abs(under_eval->imu_data[1] - under_eval->imu_data[3]) <= gyro_camera_max_diff) {
    log(Info, "Gyroscope testing successful: diff 1 = %.3f / diff 2 = %.3f",
        under_eval->imu_data[1], under_eval->imu_data[3]);
    under_eval->device_ok[Robot::IMU_DEV] = true;
  }
  else
    log(Warn, "Gyroscope testing failed: diff 1 = %.3f / diff 2 = %.3f",
        under_eval->imu_data[1], under_eval->imu_data[3]);

  under_eval->device_cmp[Robot::IMU_DEV] = under_eval->device_ok[Robot::IMU_DEV]?1.0f:0.0f;

  hideUserMsg();
  return true;
}

bool QNode::measureCharge(bool first_call) {
  if (first_call == true) {
    // This should be executed only once
    showUserMsg(Info, "Charge measurement", "Plug the adaptor to the robot and wait %d seconds\n" \
                "Do not move it, as we will measure the PSD sensors readings at the same time",
                (int)ceil(measure_charge_time));
  }

  // Wait until charging starts (and a bit more) to take first measure...
  for (int i = 0; i < 40*frequency && under_eval->device_val[Robot::CHARGING] == 0; i++)
    nbSleep(1.0/frequency);

  hideUserMsg();

  if (under_eval->device_val[Robot::CHARGING] == 0) {
    log(Error, "Adaptor not plugged after 40 seconds; aborting charge measurement");
    return false;
  }

  nbSleep(2.0);
  uint8_t v1 = under_eval->device_val[Robot::CHARGING];
  under_eval->device_cmp[Robot::CHARGING] = 0.3f;

  // ...and (if the 40 seconds timeout didn't happen) take the second
  nbSleep(measure_charge_time);
  uint8_t v2 = under_eval->device_val[Robot::CHARGING];

  under_eval->device_val[Robot::CHARGING]  = v1;      // initial
  under_eval->device_val[Robot::CHARGING] <<= 8;
  under_eval->device_val[Robot::CHARGING] |= v2;      // final
  under_eval->device_val[Robot::CHARGING] <<= 8;
  under_eval->device_val[Robot::CHARGING] |= v2 - v1; // difference

  if ((v2 - v1) >= min_power_charged) {
    log(Info, "Charge measurement: %.1f V in %d seconds",
        (v2 - v1)/10.0, (int)round(measure_charge_time));
    under_eval->device_ok[Robot::CHARGING] = true;
  }
  else
    log(Warn, "Charge measurement: %.1f V in %d seconds",
        (v2 - v1)/10.0, (int)round(measure_charge_time));

  under_eval->device_cmp[Robot::CHARGING] = under_eval->device_ok[Robot::CHARGING]?1.0f:0.0f;

  return true;
}

bool QNode::testAnalogIn(bool first_call) {
  // We need to keep track of witch input is currently changing and reuse
  // the same command to incrementally illuminate I/O test board's LEDs
  static kobuki_msgs::DigitalOutput last_do_cmd;
  static unsigned short active = std::numeric_limits<unsigned int>::max();

  if (first_call == true) {
    // This should be executed only once
    showUserMsg(Info, "Test analogue input",
       "Turn analogue input screws clockwise and counterclockwise until reaching the limits\n" \
       "The four LEDs below should get illuminated when completed");

    // Ensure that all I/O test board's LEDs are off
    for (unsigned int i = 0; i < last_do_cmd.values.size(); i++) {
      last_do_cmd.values[i] = false;
      last_do_cmd.mask[i]   = true;
    }
    output_pub.publish(last_do_cmd);

    under_eval->device_val[Robot::A_INPUT] = 0;
  }

  // Update countdown; we use it to switch off LEDs after finishing testing every input
  if ((under_eval->device_val[Robot::A_INPUT] & 0xFFFF) > 0) {
    under_eval->device_val[Robot::A_INPUT]--;

    if ((under_eval->device_val[Robot::A_INPUT] & 0xFFFF) == 0) { // countdown finished
      for (unsigned int i = 0; i < last_do_cmd.values.size(); i++)
        last_do_cmd.values[i] = false;

      output_pub.publish(last_do_cmd);
    }
  }

  // Verify whether inputs get validated (minimum and maximum values surpass the thresholds)
  for (unsigned int i = 0; i < under_eval->analog_in.size(); i++) {
    int MIN_MASK = (int)pow(2, i) << 16;
    int MAX_MASK = (int)pow(2, i) << 24;

    if ((! (under_eval->device_val[Robot::A_INPUT] & MIN_MASK)) &&
        (under_eval->analog_in[i][AI_MIN] <= ainput_min_threshold)) {
      under_eval->device_val[Robot::A_INPUT] |= MIN_MASK;
    }
    if ((! (under_eval->device_val[Robot::A_INPUT] & MAX_MASK)) &&
        (under_eval->analog_in[i][AI_MAX] >= ainput_max_threshold)) {
      under_eval->device_val[Robot::A_INPUT] |= MAX_MASK;
    }

    // The next 30 lines incrementally illuminate I/O test board's LEDs
    if (abs(under_eval->analog_in[i][AI_INC]) > 60) {
      if (active != i) {
        active = i;
        // Active input changed; switch off LEDs (strange behavior on tester)
        for (unsigned int i = 0; i < last_do_cmd.values.size(); i++)
          last_do_cmd.values[i] = false;
      }
    }

    if (i == active) {
      if ((under_eval->device_val[Robot::A_INPUT] & MIN_MASK) &&
          (under_eval->device_val[Robot::A_INPUT] & MAX_MASK)) {
        under_eval->device_val[Robot::A_INPUT] |= int(frequency); // switch on for 1 second
        under_eval->device_cmp[Robot::A_INPUT] += 0.25f;
        active = -1;
      }

      int led = (int)rint((4.0*(ainput_max_threshold - under_eval->analog_in[i][AI_VAL]))
                          / (double)ainput_max_threshold);

      if ((led == 1) || (led == 2))
        last_do_cmd.values[led] = true;
      if (under_eval->device_val[Robot::A_INPUT] & MIN_MASK)
        last_do_cmd.values[3] = true;
      if (under_eval->device_val[Robot::A_INPUT] & MAX_MASK)
        last_do_cmd.values[0] = true;

      output_pub.publish(last_do_cmd);
    }
  }

  if (under_eval->device_val[Robot::A_INPUT] == 0x0F0F0000) {
    // Min/max verified for all ports and last countdown finished
    log(Info, "Analogue input evaluation completed");
    under_eval->device_ok[Robot::A_INPUT] = true;
    under_eval->device_cmp[Robot::A_INPUT] = under_eval->device_ok[Robot::A_INPUT]?1.0f:0.0f;
    hideUserMsg();
    current_step++;
    return true;
  }

  return false;
}

void QNode::evalMotorsCurrent(bool first_call) {
  under_eval->device_ok[Robot::MOTOR_L] = under_eval->device_val[Robot::MOTOR_L] <= motor_max_current;
  under_eval->device_ok[Robot::MOTOR_R] = under_eval->device_val[Robot::MOTOR_R] <= motor_max_current;

  under_eval->device_cmp[Robot::MOTOR_L] = under_eval->device_ok[Robot::MOTOR_L]?1.0f:0.0f;
  under_eval->device_cmp[Robot::MOTOR_R] = under_eval->device_ok[Robot::MOTOR_R]?1.0f:0.0f;

  if (under_eval->motors_ok() == true)
    log(Info, "Motors current evaluation completed (%d, %d)",
        under_eval->device_val[Robot::MOTOR_L], under_eval->device_val[Robot::MOTOR_R]);
  else
    log(Warn, "Motors current too high! (%d, %d)",
        under_eval->device_val[Robot::MOTOR_L], under_eval->device_val[Robot::MOTOR_R]);
}

bool QNode::saveResults() {
  log(Info, "Saving results for %s", under_eval->serial.c_str());
  under_eval->saveToCSVFile(out_file);

  evaluated.push_back(under_eval);
  under_eval = NULL;

  return true;
}

bool QNode::init() {
  ros::init(init_argc, init_argv, "kobuki_factory_test");
  if (! ros::master::check()) {
    return false;
  }

  ros::start(); // explicitly needed since our nodehandle is going out of scope.
  ros::NodeHandle nh;

  nh.param("kobuki_factory_test/motor_max_current",    motor_max_current,    24);
  nh.param("kobuki_factory_test/cliff_sensor_tests",   cliff_sensor_tests,   3);
  nh.param("kobuki_factory_test/wheel_drop_tests",     wheel_drop_tests,     3);
  nh.param("kobuki_factory_test/power_plug_tests",     power_plug_tests,     3);
  nh.param("kobuki_factory_test/min_power_charged",    min_power_charged,    5);
  nh.param("kobuki_factory_test/measure_charge_time",  measure_charge_time,  10.0);
  nh.param("kobuki_factory_test/gyro_camera_max_diff", gyro_camera_max_diff, 0.05);
  nh.param("kobuki_factory_test/ainput_min_threshold", ainput_min_threshold, 2);
  nh.param("kobuki_factory_test/ainput_max_threshold", ainput_max_threshold, 4090);

  nh.getParam("kobuki_factory_test/test_result_output_file", out_file);

  // Subscribe to kobuki sensors and publish to its actuators
  v_info_sub  = nh.subscribe("mobile_base/version_info",         10, &QNode::versionInfoCB, this);
  s_core_sub  = nh.subscribe("mobile_base/sensors/core",         10, &QNode::sensorsCoreCB, this);
  beacon_sub  = nh.subscribe("mobile_base/sensors/dock_ir",      10, &QNode::dockBeaconCB,  this);
  gyro_sub    = nh.subscribe("mobile_base/sensors/imu_data",     10, &QNode::gyroscopeCB,   this);
  button_sub  = nh.subscribe("mobile_base/events/button",        10, &QNode::buttonEventCB, this);
  bumper_sub  = nh.subscribe("mobile_base/events/bumper",        10, &QNode::bumperEventCB, this);
  w_drop_sub  = nh.subscribe("mobile_base/events/wheel_drop",    10, &QNode::wDropEventCB,  this);
  cliff_sub   = nh.subscribe("mobile_base/events/cliff",         10, &QNode::cliffEventCB,  this);
  power_sub   = nh.subscribe("mobile_base/events/power_system",  10, &QNode::powerEventCB,  this);
  input_sub   = nh.subscribe("mobile_base/events/digital_input", 10, &QNode::inputEventCB,  this);
  robot_sub   = nh.subscribe("mobile_base/events/robot_state",   10, &QNode::robotEventCB,  this);
  state_sub   = nh.subscribe("diagnostics_toplevel_state",       10, &QNode::robotStatusCB, this);
  diags_sub   = nh.subscribe("diagnostics",                      10, &QNode::diagnosticsCB, this);

  cmd_vel_pub = nh.advertise <geometry_msgs::Twist>       ("cmd_vel", 1);
  led_1_pub   = nh.advertise <kobuki_msgs::Led>           ("mobile_base/commands/led1", 1);
  led_2_pub   = nh.advertise <kobuki_msgs::Led>           ("mobile_base/commands/led2", 1);
  sound_pub   = nh.advertise <kobuki_msgs::Sound>         ("mobile_base/commands/sound", 1);
  output_pub  = nh.advertise <kobuki_msgs::DigitalOutput> ("mobile_base/commands/digital_output", 1);
  ext_pwr_pub = nh.advertise <kobuki_msgs::DigitalOutput> ("mobile_base/commands/external_power", 1);

  start();

  current_step = INITIALIZATION;

  timer_active = false;

  // Create a one-shot timer
  timer = nh.createTimer(ros::Duration(1.0), &QNode::timerEventCB, this, true);
  timer.stop();

  return true;
}

void QNode::run() {
  ros::Rate loop_rate(frequency);
  int count = 0;
  EvalStep previous_step = current_step;

  while (ros::ok() == true) {
    ros::spinOnce();
    loop_rate.sleep();
    ++count;

    // No robot under evaluation
    if (under_eval == NULL)
      continue;

    // A motion is still on course
    if (timer_active == true)
      continue;

    // Did evaluation step changed due to incoming messages? reset previous_step if so; we must
    // keep track of step changes because many actions (for example popup dialogs) are executed
    // just the first iteration within a given step
    bool step_changed = (previous_step != current_step);
    previous_step = current_step;

    // Here we perform the evaluation actions that cannot be driven through events
    switch (current_step) {
      case INITIALIZATION:
        current_step++;
        break;
      case GET_SERIAL_NUMBER:
        if (under_eval->device_ok[Robot::V_INFO] == true)
          current_step++;
        else if (count%int(frequency*2) == 0)
          log(Debug, "Waiting for serial number...");
        break;
      case TEST_DC_ADAPTER:
        if (step_changed == true) {
          showUserMsg(Info, "DC adapter plug test", "Plug and unplug adapter to robot %d time(s)",
                      power_plug_tests);
        }
        break;
      case TEST_DOCKING_BASE:
        if (step_changed == true) {
          showUserMsg(Info, "Docking base plug test", "Plug and unplug robot to its base %d time(s)",
                      power_plug_tests);
        }
        break;
      case BUTTON_0_PRESSED:
        if (step_changed == true) {
          showUserMsg(Info, "Function buttons test",
                      "Press the three function buttons sequentially from left to right");
        }
        break;
      case TEST_LEDS:
        testLeds(step_changed);
        break;
      case TEST_SOUNDS:
        testSounds(step_changed);
        break;
      case TEST_CLIFF_SENSORS:
        if (step_changed == true)
          showUserMsg(Info, "Cliff sensors test", "Raise and lower robot %d time(s)", cliff_sensor_tests);
        break;
      case TEST_WHEEL_DROP_SENSORS:
        if (step_changed == true)
          showUserMsg(Info, "Wheel drop sensors test", "Raise and lower robot %d time(s)", wheel_drop_tests);
        break;
      case CENTER_BUMPER_PRESSED:
        if (step_changed == true) {
          showUserMsg(Info, "Bumper sensors test", "Place the robot facing a wall; " \
                            "after a while, the robot will move forward");

          // After a while, launch the robot to bump frontally
          ros::Duration(1.5).sleep();
          move(+TEST_BUMPERS_V, 0.0);
        }
        break;
      case POINT_RIGHT_BUMPER:
        move(0.0, +TEST_BUMPERS_W, (M_PI/4.0)/TEST_BUMPERS_W);  // +45 degrees
        break;
      case RIGHT_BUMPER_PRESSED:
        move(+TEST_BUMPERS_V, 0.0);
        break;
      case POINT_LEFT_BUMPER:
        move(0.0, -TEST_BUMPERS_W, (M_PI/2.0)/TEST_BUMPERS_W);  // -90 degrees
        break;
      case LEFT_BUMPER_PRESSED:
        move(+TEST_BUMPERS_V, 0.0);
        break;
      case PREPARE_MOTORS_TEST:
        if (step_changed == true) {
          showUserMsg(Info, "Motors current test", "Now the robot will move forward...");
        }
        move(0.0, -TEST_BUMPERS_W, (M_PI/4.0)/TEST_BUMPERS_W);  // -45 deg (parallel to wall)
        break;
      case TEST_MOTORS_FORWARD:
        move(+TEST_MOTORS_V, 0.0, TEST_MOTORS_D/TEST_MOTORS_V);
        break;
      case TEST_MOTORS_BACKWARD:
        under_eval->device_cmp[Robot::MOTOR_L] = under_eval->device_cmp[Robot::MOTOR_R] = 0.25f;
        move(-TEST_MOTORS_V, 0.0, TEST_MOTORS_D/TEST_MOTORS_V);
        showUserMsg(Info, "Motors current test", "Now the robot will move backward...");
        break;
      case TEST_MOTORS_CLOCKWISE:
        under_eval->device_cmp[Robot::MOTOR_L] = under_eval->device_cmp[Robot::MOTOR_R] = 0.50f;
        move(0.0, -TEST_MOTORS_W, TEST_MOTORS_A/TEST_MOTORS_W);
        showUserMsg(Info, "Motors current test", "...and spin to evaluate motors");
        break;
      case TEST_MOTORS_COUNTERCW:
        under_eval->device_cmp[Robot::MOTOR_L] = under_eval->device_cmp[Robot::MOTOR_R] = 0.75f;
        move(0.0, +TEST_MOTORS_W, TEST_MOTORS_A/TEST_MOTORS_W);
        break;
      case EVAL_MOTORS_CURRENT:
        under_eval->device_cmp[Robot::MOTOR_L] = under_eval->device_cmp[Robot::MOTOR_R] = 1.00f;
        hideUserMsg();
        evalMotorsCurrent(step_changed);
        current_step++;
        break;
      case MEASURE_GYRO_ERROR:
        testIMU(step_changed);
        current_step++;
        break;
      case MEASURE_CHARGE_AND_PSD:
        measureCharge(step_changed);
        current_step++;  // important: if we not change state, next call
        break;           // to spinOnce will overwrite the measured value!
      case TEST_DIGITAL_IO_PORTS:
        if (step_changed) {
          showUserMsg(Info, "Digital I/O test",
                    "Press the four digital input buttons, DI-1 to DI-4\n" \
                    "The digital output LED below should switch on and off as the result");
          under_eval->device_val[Robot::D_INPUT] = 0;

          // Ensure that all I/O test board's LEDs are off
          kobuki_msgs::DigitalOutput cmd;
          cmd.mask[0] = cmd.mask[1] = cmd.mask[2] = cmd.mask[3] = true;
          output_pub.publish(cmd);
        }
        break;
      case TEST_ANALOG_INPUT_PORTS:
        testAnalogIn(step_changed);
        break;
      case EVALUATION_COMPLETED:
        showUserMsg(Info, "Evaluation result", "Evaluation completed. Overall result: %s",
                    under_eval->all_ok()?"PASS":"FAILED");
        saveResults();
        current_step = INITIALIZATION;
        break;
      default:
        // Nothing special at this point; we must be evaluating a multi-step devicet!
        break;
    }

    Q_EMIT evalUpdated(under_eval);
  }
  std::cout << "Ros shutdown, proceeding to close the gui." << std::endl;
  Q_EMIT rosShutdown(); // used to signal the gui for a shutdown (useful to roslaunch)
}

void QNode::log(const LogLevel &level, const std::string &format, ...) {
  va_list arguments;
  va_start(arguments, format);

  char str[2048];
  vsnprintf(str, 2048, format.c_str(), arguments);

  std::string msg(str);

  logging_model.insertRows(logging_model.rowCount(),1);
  std::stringstream logging_model_msg;
  switch (level) {
    case(Debug) : {
      ROS_DEBUG_STREAM(msg);
      logging_model_msg << "[DEBUG] [" << ros::Time::now() << "]: " << msg;
      break;
    }
    case(Info) : {
      ROS_INFO_STREAM(msg);
      logging_model_msg << "[INFO] [" << ros::Time::now() << "]: " << msg;
      break;
    }
    case(Warn) : {
      ROS_WARN_STREAM(msg);
      logging_model_msg << "[WARN] [" << ros::Time::now() << "]: " << msg;
      break;
    }
    case(Error) : {
      ROS_ERROR_STREAM(msg);
      logging_model_msg << "[ERROR] [" << ros::Time::now() << "]: " << msg;
      break;
    }
    case(Fatal) : {
      ROS_FATAL_STREAM(msg);
      logging_model_msg << "[FATAL] [" << ros::Time::now() << "]: " << msg;
      break;
    }
  }
  QVariant new_row(QString(logging_model_msg.str().c_str()));
  logging_model.setData(logging_model.index(logging_model.rowCount()-1),new_row);
  //Q_EMIT loggingUpdated(); // used to readjust the scrollbar
  Q_EMIT addLogLine(QString(logging_model_msg.str().c_str()));
}

}  // namespace kobuki_factory_test
