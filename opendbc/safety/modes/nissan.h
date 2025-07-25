#pragma once

#include "opendbc/safety/safety_declarations.h"

#define NISSAN_COMMON_RX_CHECKS                                                                                                                               \
  {.msg = {{0x2, 0, 5, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 100U},                                      \
           {0x2, 1, 5, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 100U}, { 0 }}},  /* STEER_ANGLE_SENSOR */   \
  {.msg = {{0x285, 0, 8, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 50U},                                     \
           {0x285, 1, 8, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 50U}, { 0 }}}, /* WHEEL_SPEEDS_REAR */    \
  {.msg = {{0x30f, 2, 3, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 10U},                                     \
           {0x30f, 1, 3, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 10U}, { 0 }}}, /* CRUISE_STATE */         \
  {.msg = {{0x15c, 0, 8, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 50U},                                     \
           {0x15c, 1, 8, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 50U},                                     \
           {0x239, 0, 8, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 50U}}},        /* GAS_PEDAL */            \
  {.msg = {{0x454, 0, 8, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 10U},                                     \
           {0x454, 1, 8, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 10U},                                     \
           {0x1cc, 0, 4, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 100U}}},       /* DOORS_LIGHTS / BRAKE */ \

#define NISSAN_PRO_PILOT_RX_CHECKS(alt_eps_bus)                                                                                                      \
  {.msg = {{0x1B6, alt_eps_bus, 8, .ignore_checksum = true, .ignore_counter = true, .ignore_quality_flag = true, .frequency = 10U}, { 0 }, { 0 }}},  \

static bool nissan_alt_eps = false;

static void nissan_rx_hook(const CANPacket_t *to_push) {
  int bus = GET_BUS(to_push);
  int addr = GET_ADDR(to_push);

  if (bus == (nissan_alt_eps ? 1 : 0)) {
    if (addr == 0x2) {
      // Current steering angle
      // Factor -0.1, little endian
      int angle_meas_new = (GET_BYTES(to_push, 0, 4) & 0xFFFFU);
      // Multiply by -10 to match scale of LKAS angle
      angle_meas_new = to_signed(angle_meas_new, 16) * -10;

      // update array of samples
      update_sample(&angle_meas, angle_meas_new);
    }

    if (addr == 0x285) {
      // Get current speed and standstill
      uint16_t right_rear = (GET_BYTE(to_push, 0) << 8) | (GET_BYTE(to_push, 1));
      uint16_t left_rear = (GET_BYTE(to_push, 2) << 8) | (GET_BYTE(to_push, 3));
      vehicle_moving = (right_rear | left_rear) != 0U;
      UPDATE_VEHICLE_SPEED((right_rear + left_rear) / 2.0 * 0.005 * KPH_TO_MS);
    }

    // X-Trail 0x15c, Leaf 0x239
    if ((addr == 0x15c) || (addr == 0x239)) {
      if (addr == 0x15c){
        gas_pressed = ((GET_BYTE(to_push, 5) << 2) | ((GET_BYTE(to_push, 6) >> 6) & 0x3U)) > 3U;
      } else {
        gas_pressed = GET_BYTE(to_push, 0) > 3U;
      }
    }

    // X-trail 0x454, Leaf 0x239
    if ((addr == 0x454) || (addr == 0x239)) {
      if (addr == 0x454){
        brake_pressed = (GET_BYTE(to_push, 2) & 0x80U) != 0U;
      } else {
        brake_pressed = ((GET_BYTE(to_push, 4) >> 5) & 1U) != 0U;
      }
    }
  }

  // Handle cruise enabled
  if ((addr == 0x30f) && (bus == (nissan_alt_eps ? 1 : 2))) {
    bool cruise_engaged = (GET_BYTE(to_push, 0) >> 3) & 1U;
    pcm_cruise_check(cruise_engaged);
  }

  if ((addr == 0x239) && (bus == 0)) {
    acc_main_on = GET_BIT(to_push, 17U);
  }

  if ((addr == 0x1B6) && (bus == (nissan_alt_eps ? 2 : 1))) {
    acc_main_on = GET_BIT(to_push, 36U);
  }
}


static bool nissan_tx_hook(const CANPacket_t *to_send) {
  const AngleSteeringLimits NISSAN_STEERING_LIMITS = {
    .max_angle = 60000,  // 600 deg, reasonable limit
    .angle_deg_to_can = 100,
    .angle_rate_up_lookup = {
      {0., 5., 15.},
      {5., .8, .15}
    },
    .angle_rate_down_lookup = {
      {0., 5., 15.},
      {5., 3.5, .4}
    },
  };

  bool tx = true;
  int addr = GET_ADDR(to_send);
  bool violation = false;

  // steer cmd checks
  if (addr == 0x169) {
    int desired_angle = ((GET_BYTE(to_send, 0) << 10) | (GET_BYTE(to_send, 1) << 2) | ((GET_BYTE(to_send, 2) >> 6) & 0x3U));
    bool lka_active = (GET_BYTE(to_send, 6) >> 4) & 1U;

    // Factor is -0.01, offset is 1310. Flip to correct sign, but keep units in CAN scale
    desired_angle = -desired_angle + (1310.0f * NISSAN_STEERING_LIMITS.angle_deg_to_can);

    if (steer_angle_cmd_checks(desired_angle, lka_active, NISSAN_STEERING_LIMITS)) {
      violation = true;
    }
  }

  // acc button check, only allow cancel button to be sent
  if (addr == 0x20b) {
    // Violation of any button other than cancel is pressed
    violation |= ((GET_BYTE(to_send, 1) & 0x3dU) > 0U);
  }

  if (violation) {
    tx = false;
  }

  return tx;
}


static safety_config nissan_init(uint16_t param) {
  static const CanMsg NISSAN_TX_MSGS[] = {
    {0x169, 0, 8, .check_relay = true},   // LKAS
    {0x2b1, 0, 8, .check_relay = true},   // PROPILOT_HUD
    {0x4cc, 0, 8, .check_relay = true},   // PROPILOT_HUD_INFO_MSG
    {0x20b, 2, 6, .check_relay = false},  // CRUISE_THROTTLE (X-Trail)
    {0x20b, 1, 6, .check_relay = false},  // CRUISE_THROTTLE (Altima)
    {0x280, 2, 8, .check_relay = true}    // CANCEL_MSG (Leaf)
  };

  // Signals duplicated below due to the fact that these messages can come in on either CAN bus, depending on car model.
  static RxCheck nissan_rx_checks[] = {
    NISSAN_COMMON_RX_CHECKS
    NISSAN_PRO_PILOT_RX_CHECKS(1)
  };

  static RxCheck nissan_alt_eps_rx_checks[] = {
    NISSAN_COMMON_RX_CHECKS
    NISSAN_PRO_PILOT_RX_CHECKS(2)
  };

  static RxCheck nissan_leaf_rx_checks[] = {
    NISSAN_COMMON_RX_CHECKS
  };

  // EPS Location. false = V-CAN, true = C-CAN
  const int NISSAN_PARAM_ALT_EPS_BUS = 1;

  const int NISSAN_PARAM_SP_LEAF = 1;

  nissan_alt_eps = GET_FLAG(param, NISSAN_PARAM_ALT_EPS_BUS);
  const bool nissan_leaf = GET_FLAG(current_safety_param_sp, NISSAN_PARAM_SP_LEAF);

  safety_config ret;
  SET_TX_MSGS(NISSAN_TX_MSGS, ret);
  if (nissan_leaf) {
    SET_RX_CHECKS(nissan_leaf_rx_checks, ret);
  } else if (nissan_alt_eps) {
    SET_RX_CHECKS(nissan_alt_eps_rx_checks, ret);
  } else {
    SET_RX_CHECKS(nissan_rx_checks, ret);
  }

  return ret;
}

const safety_hooks nissan_hooks = {
  .init = nissan_init,
  .rx = nissan_rx_hook,
  .tx = nissan_tx_hook,
};
