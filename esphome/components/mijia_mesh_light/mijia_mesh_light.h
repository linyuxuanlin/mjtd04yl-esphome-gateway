#pragma once

#ifdef USE_ESP32

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome::mijia_mesh_light {

class MijiaMeshLight final : public Component, public ble_client::BLEClientNode, public light::LightOutput {
 public:
  MijiaMeshLight();
  ~MijiaMeshLight() = default;

  void set_ltmk(const std::string &ltmk_hex);
  void set_authenticated_sensor(binary_sensor::BinarySensor *sensor) { this->authenticated_sensor_ = sensor; }

  light::LightTraits get_traits() override;
  void setup_state(light::LightState *state) override { this->light_state_ = state; }
  void write_state(light::LightState *state) override;

  void loop() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

 protected:
  enum class AuthState : uint8_t {
    DISCONNECTED,
    REGISTERING,
    WAIT_INIT,
    WAIT_DUMMY,
    WAIT_KEY_READY,
    WAIT_PUBLIC_KEY_ACK,
    WAIT_DEVICE_PUBLIC_KEY,
    WAIT_AUTH_READY,
    WAIT_AUTH_ACK,
    WAIT_LOGIN_RESULT,
    AUTHENTICATED,
    FAILED,
  };

  enum class CommandPhase : uint8_t { IDLE, WAIT_READY, WAIT_ACCEPT, WAIT_RESPONSE };

  struct MiotCommand {
    uint8_t siid;
    uint16_t piid;
    uint32_t value;
    uint8_t type_id;
    uint8_t value_len;
  };

  static constexpr uint16_t SERVICE_UUID = 0xFE95;
  static constexpr uint16_t AUTH_CONTROL_UUID = 0x0010;
  static constexpr uint16_t AUTH_DATA_UUID = 0x0016;
  static constexpr uint16_t COMMAND_SEND_UUID = 0x001A;
  static constexpr uint16_t COMMAND_RECEIVE_UUID = 0x001B;

  static constexpr float COLD_WHITE_MIRED = 1000000.0f / 6500.0f;
  static constexpr float WARM_WHITE_MIRED = 1000000.0f / 2700.0f;

  bool write_char_(uint16_t handle, const uint8_t *data, size_t len);
  bool write_char_(uint16_t handle, const std::vector<uint8_t> &data) {
    return this->write_char_(handle, data.data(), data.size());
  }
  void reset_connection_state_();
  void start_authentication_();
  void fail_authentication_(const char *reason);
  void handle_auth_control_(const uint8_t *data, size_t len);
  void handle_auth_data_(const uint8_t *data, size_t len);
  bool generate_local_keypair_();
  bool derive_session_and_auth_blob_(const uint8_t *device_public_key, std::array<uint8_t, 8> &auth_blob);
  void mark_authenticated_();

  void queue_set_(uint8_t siid, uint16_t piid, uint32_t value, uint8_t type_id, uint8_t value_len);
  std::vector<uint8_t> build_miot_tlv_(const MiotCommand &command);
  bool encrypt_command_(const std::vector<uint8_t> &plain, std::vector<uint8_t> &encrypted);
  bool decrypt_command_(const uint8_t *data, size_t len, std::vector<uint8_t> &plain);
  void begin_next_command_();
  void handle_command_send_(const uint8_t *data, size_t len);
  void handle_command_receive_(const uint8_t *data, size_t len);
  void finish_command_response_(const std::vector<uint8_t> &plain);
  void complete_active_command_();
  void process_miot_plaintext_(const std::vector<uint8_t> &plain);
  void publish_property_(uint8_t siid, uint16_t piid, uint32_t value);

  static const char *auth_state_name_(AuthState state);

  std::array<uint8_t, 32> ltmk_{};
  bool ltmk_valid_{false};
  std::array<uint8_t, 32> local_private_key_{};
  std::array<uint8_t, 64> local_public_key_{};
  std::array<uint8_t, 64> session_key_{};
  std::vector<uint8_t> device_public_key_rx_;
  uint16_t device_public_key_expected_frames_{0};
  uint16_t device_public_key_next_frame_{1};

  uint16_t auth_control_handle_{0};
  uint16_t auth_data_handle_{0};
  uint16_t command_send_handle_{0};
  uint16_t command_receive_handle_{0};
  uint8_t notify_registrations_{0};
  uint8_t descriptor_writes_{0};

  AuthState auth_state_{AuthState::DISCONNECTED};
  uint32_t auth_activity_ms_{0};
  uint32_t delayed_action_ms_{0};
  uint32_t auth_start_not_before_ms_{0};
  bool delayed_dummy_finish_{false};
  bool start_auth_pending_{false};
  bool command_channel_ready_{false};

  std::deque<MiotCommand> command_queue_;
  MiotCommand active_command_{};
  std::vector<uint8_t> active_encrypted_frame_;
  std::vector<uint8_t> command_receive_rx_;
  uint16_t command_receive_expected_frames_{0};
  uint16_t command_receive_next_frame_{1};
  CommandPhase command_phase_{CommandPhase::IDLE};
  bool set_ack_received_{false};
  uint32_t command_activity_ms_{0};
  uint32_t send_counter_{0};
  uint16_t last_device_counter_low_{0};
  uint16_t device_counter_high_{0};
  uint8_t miot_sequence_{1};

  light::LightState *light_state_{nullptr};
  binary_sensor::BinarySensor *authenticated_sensor_{nullptr};
};

}  // namespace esphome::mijia_mesh_light

#endif  // USE_ESP32
