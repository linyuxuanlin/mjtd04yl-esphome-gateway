#include "mijia_mesh_light.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_random.h>
#include <esp_rom_crc.h>
#include <mbedtls/ccm.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>

#include "esphome/core/log.h"

namespace esphome::mijia_mesh_light {

static const char *const TAG = "mijia_mesh_light";

static int esp_random_callback(void *, unsigned char *output, size_t len) {
  esp_fill_random(output, len);
  return 0;
}

static int hkdf_sha256_64(const uint8_t *input, size_t input_len, const uint8_t *salt, size_t salt_len,
                          const uint8_t *info, size_t info_len, uint8_t *output) {
  const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (sha256 == nullptr || info_len > 64)
    return -1;

  uint8_t pseudo_random_key[32]{};
  int result = mbedtls_md_hmac(sha256, salt, salt_len, input, input_len, pseudo_random_key);
  uint8_t expansion_input[32 + 64 + 1]{};
  for (uint8_t block = 1; result == 0 && block <= 2; block++) {
    size_t offset = 0;
    if (block > 1) {
      std::copy(output + (block - 2) * 32, output + (block - 1) * 32, expansion_input);
      offset = 32;
    }
    std::copy(info, info + info_len, expansion_input + offset);
    offset += info_len;
    expansion_input[offset++] = block;
    result = mbedtls_md_hmac(sha256, pseudo_random_key, sizeof(pseudo_random_key), expansion_input, offset,
                             output + (block - 1) * 32);
  }
  std::fill(std::begin(pseudo_random_key), std::end(pseudo_random_key), 0);
  std::fill(std::begin(expansion_input), std::end(expansion_input), 0);
  return result;
}

MijiaMeshLight::MijiaMeshLight() { this->node_state = esp32_ble_tracker::ClientState::INIT; }

void MijiaMeshLight::set_ltmk(const std::string &ltmk_hex) {
  if (ltmk_hex.size() != this->ltmk_.size() * 2) {
    ESP_LOGE(TAG, "Invalid LTMK length");
    return;
  }
  for (size_t i = 0; i < this->ltmk_.size(); i++) {
    char pair[3] = {ltmk_hex[i * 2], ltmk_hex[i * 2 + 1], 0};
    char *end = nullptr;
    const unsigned long value = strtoul(pair, &end, 16);
    if (end == nullptr || *end != '\0') {
      ESP_LOGE(TAG, "Invalid hexadecimal LTMK");
      return;
    }
    this->ltmk_[i] = static_cast<uint8_t>(value);
  }
  this->ltmk_valid_ = true;
}

light::LightTraits MijiaMeshLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::COLOR_TEMPERATURE});
  traits.set_min_mireds(COLD_WHITE_MIRED);
  traits.set_max_mireds(WARM_WHITE_MIRED);
  return traits;
}

void MijiaMeshLight::write_state(light::LightState *state) {
  const bool power = state->current_values.is_on();
  this->queue_set_(2, 1, power ? 1 : 0, 0, 1);

  if (power) {
    // current_values_as_ct() returns a normalized output-channel balance,
    // not the Home Assistant mired value. Read the model values directly.
    float color_temperature = state->current_values.get_color_temperature();
    const float brightness = state->current_values.get_brightness();
    const uint8_t brightness_percent =
        static_cast<uint8_t>(std::max(1.0f, std::min(100.0f, roundf(brightness * 100.0f))));
    // lamp21 declares both brightness and color-temperature as uint16.
    this->queue_set_(2, 2, brightness_percent, 3, 2);

    color_temperature = std::max(COLD_WHITE_MIRED, std::min(WARM_WHITE_MIRED, color_temperature));
    const uint32_t kelvin = static_cast<uint32_t>(roundf(1000000.0f / color_temperature));
    this->queue_set_(2, 3, std::max<uint32_t>(2700, std::min<uint32_t>(6500, kelvin)), 3, 2);
  }
}

bool MijiaMeshLight::write_char_(uint16_t handle, const uint8_t *data, size_t len) {
  if (handle == 0 || this->parent() == nullptr || this->parent()->get_conn_id() == esp32_ble_client::UNSET_CONN_ID)
    return false;
  const esp_err_t status = esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle, len, const_cast<uint8_t *>(data),
      ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  ESP_LOGD(TAG, "GATT write queued: handle=0x%04X len=%u status=%d", handle, static_cast<unsigned>(len), status);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "GATT write failed on handle 0x%04X: %d", handle, status);
    return false;
  }
  return true;
}

void MijiaMeshLight::reset_connection_state_() {
  this->auth_control_handle_ = 0;
  this->auth_data_handle_ = 0;
  this->command_send_handle_ = 0;
  this->command_receive_handle_ = 0;
  this->notify_registrations_ = 0;
  this->descriptor_writes_ = 0;
  this->auth_state_ = AuthState::DISCONNECTED;
  this->auth_activity_ms_ = millis();
  this->start_auth_pending_ = false;
  this->delayed_dummy_finish_ = false;
  this->command_channel_ready_ = false;
  this->command_phase_ = CommandPhase::IDLE;
  this->set_ack_received_ = false;
  this->active_encrypted_frame_.clear();
  this->command_receive_rx_.clear();
  this->command_receive_expected_frames_ = 0;
  this->command_receive_next_frame_ = 1;
  this->send_counter_ = 0;
  this->last_device_counter_low_ = 0;
  this->device_counter_high_ = 0;
  this->miot_sequence_ = 1;
  this->device_public_key_rx_.clear();
  this->device_public_key_expected_frames_ = 0;
  this->device_public_key_next_frame_ = 1;
  std::fill(this->session_key_.begin(), this->session_key_.end(), 0);
  if (this->authenticated_sensor_ != nullptr)
    this->authenticated_sensor_->publish_state(false);
}

void MijiaMeshLight::start_authentication_() {
  if (!this->ltmk_valid_) {
    this->fail_authentication_("LTMK is unavailable");
    return;
  }
  if (!this->generate_local_keypair_()) {
    this->fail_authentication_("could not generate P-256 keypair");
    return;
  }
  const uint8_t start = 0xA4;
  ESP_LOGI(TAG, "Starting Xiaomi BLE Mesh local login");
  if (!this->write_char_(this->auth_control_handle_, &start, sizeof(start))) {
    this->fail_authentication_("could not start login");
    return;
  }
  this->auth_state_ = AuthState::WAIT_INIT;
  this->auth_activity_ms_ = millis();
}

void MijiaMeshLight::fail_authentication_(const char *reason) {
  ESP_LOGW(TAG, "Local login failed at %s: %s", auth_state_name_(this->auth_state_), reason);
  this->auth_state_ = AuthState::FAILED;
  this->auth_activity_ms_ = millis();
  if (this->authenticated_sensor_ != nullptr)
    this->authenticated_sensor_->publish_state(false);
}

bool MijiaMeshLight::generate_local_keypair_() {
  mbedtls_ecp_group group;
  mbedtls_mpi private_key;
  mbedtls_ecp_point public_key;
  mbedtls_ecp_group_init(&group);
  mbedtls_mpi_init(&private_key);
  mbedtls_ecp_point_init(&public_key);

  int result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
  if (result == 0)
    result = mbedtls_ecp_gen_keypair(&group, &private_key, &public_key, esp_random_callback, nullptr);
  if (result == 0)
    result = mbedtls_mpi_write_binary(&private_key, this->local_private_key_.data(), this->local_private_key_.size());

  std::array<uint8_t, 65> public_binary{};
  size_t public_length = 0;
  if (result == 0)
    result = mbedtls_ecp_point_write_binary(&group, &public_key, MBEDTLS_ECP_PF_UNCOMPRESSED, &public_length,
                                            public_binary.data(), public_binary.size());
  if (result == 0 && public_length == public_binary.size() && public_binary[0] == 0x04)
    std::copy(public_binary.begin() + 1, public_binary.end(), this->local_public_key_.begin());
  else if (result == 0)
    result = -1;

  mbedtls_ecp_point_free(&public_key);
  mbedtls_mpi_free(&private_key);
  mbedtls_ecp_group_free(&group);
  if (result != 0)
    ESP_LOGW(TAG, "P-256 key generation error: %d", result);
  return result == 0;
}

bool MijiaMeshLight::derive_session_and_auth_blob_(const uint8_t *device_public_key,
                                                    std::array<uint8_t, 8> &auth_blob) {
  mbedtls_ecp_group group;
  mbedtls_mpi private_key;
  mbedtls_mpi shared_secret;
  mbedtls_ecp_point peer_public_key;
  mbedtls_ecp_group_init(&group);
  mbedtls_mpi_init(&private_key);
  mbedtls_mpi_init(&shared_secret);
  mbedtls_ecp_point_init(&peer_public_key);

  int result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
  if (result == 0)
    result = mbedtls_mpi_read_binary(&private_key, this->local_private_key_.data(), this->local_private_key_.size());
  std::array<uint8_t, 65> peer_binary{};
  peer_binary[0] = 0x04;
  std::copy(device_public_key, device_public_key + 64, peer_binary.begin() + 1);
  if (result == 0)
    result = mbedtls_ecp_point_read_binary(&group, &peer_public_key, peer_binary.data(), peer_binary.size());
  if (result == 0)
    result = mbedtls_ecdh_compute_shared(&group, &shared_secret, &peer_public_key, &private_key, esp_random_callback,
                                         nullptr);

  std::array<uint8_t, 64> input_key_material{};
  if (result == 0)
    result = mbedtls_mpi_write_binary(&shared_secret, input_key_material.data(), 32);
  std::copy(this->ltmk_.begin(), this->ltmk_.end(), input_key_material.begin() + 32);

  static constexpr char SALT[] = "miot-mesh-login-salt";
  static constexpr char INFO[] = "miot-mesh-login-info";
  if (result == 0)
    result = hkdf_sha256_64(input_key_material.data(), input_key_material.size(),
                            reinterpret_cast<const uint8_t *>(SALT), sizeof(SALT) - 1,
                            reinterpret_cast<const uint8_t *>(INFO), sizeof(INFO) - 1, this->session_key_.data());

  const uint32_t crc = esp_rom_crc32_le(0, device_public_key, 64);
  uint8_t crc_bytes[4] = {static_cast<uint8_t>(crc), static_cast<uint8_t>(crc >> 8),
                          static_cast<uint8_t>(crc >> 16), static_cast<uint8_t>(crc >> 24)};
  static constexpr uint8_t AUTH_NONCE[12] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                                              0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B};
  mbedtls_ccm_context ccm;
  mbedtls_ccm_init(&ccm);
  if (result == 0)
    result = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, this->session_key_.data() + 16, 128);
  if (result == 0)
    result = mbedtls_ccm_encrypt_and_tag(&ccm, sizeof(crc_bytes), AUTH_NONCE, sizeof(AUTH_NONCE), nullptr, 0,
                                         crc_bytes, auth_blob.data(), auth_blob.data() + 4, 4);
  mbedtls_ccm_free(&ccm);

  mbedtls_ecp_point_free(&peer_public_key);
  mbedtls_mpi_free(&shared_secret);
  mbedtls_mpi_free(&private_key);
  mbedtls_ecp_group_free(&group);
  std::fill(input_key_material.begin(), input_key_material.end(), 0);

  if (result != 0)
    ESP_LOGW(TAG, "Session derivation error: %d", result);
  return result == 0;
}

void MijiaMeshLight::handle_auth_data_(const uint8_t *data, size_t len) {
  this->auth_activity_ms_ = millis();
  ESP_LOGD(TAG, "Auth-data frame in %s: len=%u header=%02X%02X%02X%02X", auth_state_name_(this->auth_state_),
           static_cast<unsigned>(len), len > 0 ? data[0] : 0, len > 1 ? data[1] : 0, len > 2 ? data[2] : 0,
           len > 3 ? data[3] : 0);
  switch (this->auth_state_) {
    case AuthState::WAIT_INIT: {
      static constexpr uint8_t request[] = {0x00, 0x00, 0x05, 0x00, 0x06, 0x50};
      if (!this->write_char_(this->auth_data_handle_, request, sizeof(request)))
        this->fail_authentication_("dummy exchange request failed");
      else
        this->auth_state_ = AuthState::WAIT_DUMMY;
      break;
    }
    case AuthState::WAIT_DUMMY: {
      std::vector<uint8_t> dummy(68, 0x50);
      dummy[0] = 0x00;
      dummy[1] = 0x00;
      dummy[2] = 0x05;
      dummy[3] = 0x01;
      if (!this->write_char_(this->auth_data_handle_, dummy)) {
        this->fail_authentication_("dummy exchange response failed");
        break;
      }
      this->delayed_dummy_finish_ = true;
      this->delayed_action_ms_ = millis() + 200;
      break;
    }
    case AuthState::WAIT_KEY_READY: {
      if (len < 4 || data[2] != 0x01 || data[3] != 0x01) {
        this->fail_authentication_("unexpected public-key ready frame");
        break;
      }
      std::vector<uint8_t> frame(66);
      frame[0] = 0x01;
      frame[1] = 0x00;
      std::copy(this->local_public_key_.begin(), this->local_public_key_.end(), frame.begin() + 2);
      if (!this->write_char_(this->auth_data_handle_, frame))
        this->fail_authentication_("public key write failed");
      else
        this->auth_state_ = AuthState::WAIT_PUBLIC_KEY_ACK;
      break;
    }
    case AuthState::WAIT_PUBLIC_KEY_ACK:
      if (len < 4 || data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x01 || data[3] != 0x00)
        this->fail_authentication_("unexpected local public-key acknowledgement");
      else
        this->auth_state_ = AuthState::WAIT_DEVICE_PUBLIC_KEY;
      break;
    case AuthState::WAIT_DEVICE_PUBLIC_KEY: {
      // Most devices send a one-frame SingleCtrPacket (00000203 + 64-byte
      // key). MJTD04YL may instead announce a one-frame FlowPacket first
      // (000000030100), then send 0100 + the 64-byte key after our ready ACK.
      if (len == 6 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x03) {
        this->device_public_key_expected_frames_ =
            static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
        if (this->device_public_key_expected_frames_ == 0 || this->device_public_key_expected_frames_ > 8) {
          this->fail_authentication_("invalid device public-key fragment count");
          break;
        }
        this->device_public_key_next_frame_ = 1;
        this->device_public_key_rx_.clear();
        this->device_public_key_rx_.reserve(64);
        static constexpr uint8_t ready[] = {0x00, 0x00, 0x01, 0x01};
        if (!this->write_char_(this->auth_data_handle_, ready, sizeof(ready)))
          this->fail_authentication_("device public key ready acknowledgement failed");
        break;
      }

      const uint8_t *device_public_key = nullptr;
      static constexpr uint8_t single_received[] = {0x00, 0x00, 0x03, 0x00};
      static constexpr uint8_t flow_received[] = {0x00, 0x00, 0x01, 0x00};
      const uint8_t *received_ack = nullptr;
      size_t received_ack_len = 0;
      if (len >= 68 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x02 && data[3] == 0x03) {
        device_public_key = data + 4;
        received_ack = single_received;
        received_ack_len = sizeof(single_received);
      } else if (this->device_public_key_expected_frames_ > 0 && len > 2) {
        const uint16_t sequence = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
        if (sequence != this->device_public_key_next_frame_ || sequence > this->device_public_key_expected_frames_) {
          this->fail_authentication_("out-of-order device public key fragment");
          break;
        }
        this->device_public_key_rx_.insert(this->device_public_key_rx_.end(), data + 2, data + len);
        this->device_public_key_next_frame_++;
        if (sequence < this->device_public_key_expected_frames_)
          break;
        if (this->device_public_key_rx_.size() != 64) {
          this->fail_authentication_("invalid reassembled device public key length");
          break;
        }
        device_public_key = this->device_public_key_rx_.data();
        received_ack = flow_received;
        received_ack_len = sizeof(flow_received);
      }
      if (device_public_key == nullptr) {
        // Xiaomi firmware emits one or more short transfer-status frames
        // before the public-key frame. They are acknowledgements, not failures.
        ESP_LOGD(TAG, "Waiting for complete device public key; ignored %u-byte status frame",
                 static_cast<unsigned>(len));
        break;
      }
      this->write_char_(this->auth_data_handle_, received_ack, received_ack_len);
      std::array<uint8_t, 8> auth_blob{};
      if (!this->derive_session_and_auth_blob_(device_public_key, auth_blob)) {
        this->fail_authentication_("session key derivation failed");
        break;
      }
      this->active_encrypted_frame_.assign(auth_blob.begin(), auth_blob.end());
      static constexpr uint8_t auth_header[] = {0x00, 0x00, 0x00, 0x05, 0x01, 0x00};
      if (!this->write_char_(this->auth_data_handle_, auth_header, sizeof(auth_header)))
        this->fail_authentication_("authentication header failed");
      else
        this->auth_state_ = AuthState::WAIT_AUTH_READY;
      break;
    }
    case AuthState::WAIT_AUTH_READY: {
      if (len < 4 || data[2] != 0x01 || data[3] != 0x01) {
        this->fail_authentication_("unexpected authentication ready frame");
        break;
      }
      // Rebuild the authentication blob from the retained device key is unnecessary:
      // the blob is retained temporarily in active_encrypted_frame_.
      if (this->active_encrypted_frame_.size() != 8) {
        this->fail_authentication_("authentication blob was not retained");
        break;
      }
      std::vector<uint8_t> frame(10);
      frame[0] = 0x01;
      frame[1] = 0x00;
      std::copy(this->active_encrypted_frame_.begin(), this->active_encrypted_frame_.end(), frame.begin() + 2);
      if (!this->write_char_(this->auth_data_handle_, frame))
        this->fail_authentication_("authentication proof failed");
      else
        this->auth_state_ = AuthState::WAIT_AUTH_ACK;
      break;
    }
    case AuthState::WAIT_AUTH_ACK:
      if (len < 4 || data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x01 || data[3] != 0x00)
        this->fail_authentication_("unexpected authentication acknowledgement");
      else
        this->auth_state_ = AuthState::WAIT_LOGIN_RESULT;
      break;
    default:
      ESP_LOGV(TAG, "Ignoring auth-data notification in %s (%u bytes)", auth_state_name_(this->auth_state_),
               static_cast<unsigned>(len));
      break;
  }
}

void MijiaMeshLight::handle_auth_control_(const uint8_t *data, size_t len) {
  this->auth_activity_ms_ = millis();
  ESP_LOGD(TAG, "Auth-control frame in %s: len=%u first=%02X", auth_state_name_(this->auth_state_),
           static_cast<unsigned>(len), len > 0 ? data[0] : 0);
  if ((this->auth_state_ == AuthState::WAIT_AUTH_ACK || this->auth_state_ == AuthState::WAIT_LOGIN_RESULT) && len > 0 &&
      (data[0] == 0x51 || data[0] == 0x21 || data[0] == 0x11)) {
    this->mark_authenticated_();
  }
}

void MijiaMeshLight::mark_authenticated_() {
  this->auth_state_ = AuthState::AUTHENTICATED;
  this->auth_activity_ms_ = millis();
  this->active_encrypted_frame_.clear();
  this->send_counter_ = 0;
  this->last_device_counter_low_ = 0;
  this->device_counter_high_ = 0;
  this->command_phase_ = CommandPhase::IDLE;
  this->set_ack_received_ = false;
  ESP_LOGI(TAG, "Xiaomi BLE Mesh local login succeeded; enabling command channel");
  if (this->parent()->register_for_notify(this->command_send_handle_) != ESP_OK) {
    this->fail_authentication_("command notification registration failed");
  }
}

void MijiaMeshLight::queue_set_(uint8_t siid, uint16_t piid, uint32_t value, uint8_t type_id, uint8_t value_len) {
  for (auto it = this->command_queue_.begin(); it != this->command_queue_.end();) {
    if (it->siid == siid && it->piid == piid)
      it = this->command_queue_.erase(it);
    else
      ++it;
  }
  this->command_queue_.push_back({siid, piid, value, type_id, value_len});
}

std::vector<uint8_t> MijiaMeshLight::build_miot_tlv_(const MiotCommand &command) {
  const uint8_t value_len = command.value_len;
  const uint8_t type_id = command.type_id;
  const uint16_t type_length = (static_cast<uint16_t>(type_id) << 12) | value_len;
  std::vector<uint8_t> result(11 + value_len, 0);
  result[0] = result.size();
  result[1] = 0x20;
  result[2] = this->miot_sequence_++;
  result[3] = 0x00;
  result[4] = 0x00;
  result[5] = 0x01;
  result[6] = command.siid;
  result[7] = command.piid & 0xFF;
  result[8] = command.piid >> 8;
  result[9] = type_length & 0xFF;
  result[10] = type_length >> 8;
  for (uint8_t i = 0; i < value_len; i++)
    result[11 + i] = static_cast<uint8_t>(command.value >> (i * 8));
  return result;
}

bool MijiaMeshLight::encrypt_command_(const std::vector<uint8_t> &plain, std::vector<uint8_t> &encrypted) {
  uint8_t nonce[12]{};
  std::copy(this->session_key_.begin() + 36, this->session_key_.begin() + 40, nonce);
  nonce[8] = this->send_counter_;
  nonce[9] = this->send_counter_ >> 8;
  nonce[10] = this->send_counter_ >> 16;
  nonce[11] = this->send_counter_ >> 24;

  encrypted.assign(2 + plain.size() + 4, 0);
  encrypted[0] = this->send_counter_;
  encrypted[1] = this->send_counter_ >> 8;
  mbedtls_ccm_context ccm;
  mbedtls_ccm_init(&ccm);
  int result = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, this->session_key_.data() + 16, 128);
  if (result == 0)
    result = mbedtls_ccm_encrypt_and_tag(&ccm, plain.size(), nonce, sizeof(nonce), nullptr, 0, plain.data(),
                                         encrypted.data() + 2, encrypted.data() + 2 + plain.size(), 4);
  mbedtls_ccm_free(&ccm);
  if (result == 0)
    this->send_counter_++;
  else
    ESP_LOGW(TAG, "Command encryption error: %d", result);
  return result == 0;
}

bool MijiaMeshLight::decrypt_command_(const uint8_t *data, size_t len, std::vector<uint8_t> &plain) {
  if (len < 6)
    return false;
  const uint16_t counter_low = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  if (counter_low < this->last_device_counter_low_ &&
      static_cast<uint16_t>(this->last_device_counter_low_ - counter_low) > 32768)
    this->device_counter_high_++;
  this->last_device_counter_low_ = counter_low;
  const uint32_t counter = (static_cast<uint32_t>(this->device_counter_high_) << 16) | counter_low;
  uint8_t nonce[12]{};
  std::copy(this->session_key_.begin() + 32, this->session_key_.begin() + 36, nonce);
  nonce[8] = counter;
  nonce[9] = counter >> 8;
  nonce[10] = counter >> 16;
  nonce[11] = counter >> 24;

  const size_t cipher_len = len - 2 - 4;
  plain.assign(cipher_len, 0);
  mbedtls_ccm_context ccm;
  mbedtls_ccm_init(&ccm);
  int result = mbedtls_ccm_setkey(&ccm, MBEDTLS_CIPHER_ID_AES, this->session_key_.data(), 128);
  if (result == 0)
    result = mbedtls_ccm_auth_decrypt(&ccm, cipher_len, nonce, sizeof(nonce), nullptr, 0, data + 2, plain.data(),
                                      data + 2 + cipher_len, 4);
  mbedtls_ccm_free(&ccm);
  if (result != 0)
    ESP_LOGW(TAG, "Command decryption error: %d", result);
  return result == 0;
}

void MijiaMeshLight::begin_next_command_() {
  if (this->auth_state_ != AuthState::AUTHENTICATED || !this->command_channel_ready_ ||
      this->command_phase_ != CommandPhase::IDLE ||
      this->command_queue_.empty())
    return;
  this->active_command_ = this->command_queue_.front();
  this->command_queue_.pop_front();
  const auto plain = this->build_miot_tlv_(this->active_command_);
  if (!this->encrypt_command_(plain, this->active_encrypted_frame_))
    return;
  static constexpr uint8_t header[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
  if (!this->write_char_(this->command_send_handle_, header, sizeof(header)))
    return;
  this->command_phase_ = CommandPhase::WAIT_READY;
  this->set_ack_received_ = false;
  this->command_activity_ms_ = millis();
  ESP_LOGD(TAG, "SET siid=%u piid=%u value=%lu", this->active_command_.siid, this->active_command_.piid,
           static_cast<unsigned long>(this->active_command_.value));
}

void MijiaMeshLight::handle_command_send_(const uint8_t *data, size_t len) {
  this->command_activity_ms_ = millis();
  if (this->command_phase_ == CommandPhase::WAIT_READY && len >= 4 && data[2] == 0x01 && data[3] == 0x01) {
    std::vector<uint8_t> frame(2 + this->active_encrypted_frame_.size());
    frame[0] = 0x01;
    frame[1] = 0x00;
    std::copy(this->active_encrypted_frame_.begin(), this->active_encrypted_frame_.end(), frame.begin() + 2);
    if (this->write_char_(this->command_send_handle_, frame))
      this->command_phase_ = CommandPhase::WAIT_ACCEPT;
  } else if (this->command_phase_ == CommandPhase::WAIT_ACCEPT && len >= 4 && data[2] == 0x01 && data[3] == 0x00) {
    // Keep one MIoT request in flight until its encrypted response arrives.
    // MJTD04YL otherwise drops responses when command-send outruns
    // command-receive.
    this->command_phase_ = CommandPhase::WAIT_RESPONSE;
    this->active_encrypted_frame_.clear();
  }
}

void MijiaMeshLight::complete_active_command_() {
  if (this->command_phase_ != CommandPhase::WAIT_RESPONSE)
    return;
  // lamp21 confirms direct BLE SETs with an authenticated 0x01 ACK and may
  // follow with a 0x04 result packet. Direct property GETs are not supported.
  this->command_phase_ = CommandPhase::IDLE;
  this->set_ack_received_ = false;
  this->active_encrypted_frame_.clear();
}

void MijiaMeshLight::finish_command_response_(const std::vector<uint8_t> &plain) {
  if (this->command_phase_ != CommandPhase::WAIT_RESPONSE || plain.size() < 9)
    return;
  const uint8_t opcode = plain[4];
  const uint8_t siid = plain[6];
  const uint16_t piid = static_cast<uint16_t>(plain[7]) | (static_cast<uint16_t>(plain[8]) << 8);
  if (siid != this->active_command_.siid || piid != this->active_command_.piid)
    return;

  if (opcode == 0x01) {
    // A SET first receives an ACK and may receive its 0x04 result shortly
    // afterwards. Keep the receive window open instead of racing the next
    // request into the command channel.
    this->set_ack_received_ = true;
    this->command_activity_ms_ = millis();
    ESP_LOGD(TAG, "SET acknowledged: siid=%u piid=%u", siid, piid);
  } else if (opcode == 0x04) {
    this->complete_active_command_();
  }
}

void MijiaMeshLight::handle_command_receive_(const uint8_t *data, size_t len) {
  // Once a FlowPacket has been announced, every following notification is a
  // sequence-prefixed fragment. The final fragment may contain only one byte
  // of payload (three ATT value bytes total), so handle it before the generic
  // minimum-length and inline-frame checks.
  if (this->command_receive_expected_frames_ > 0 && len > 2) {
    const uint16_t sequence = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
    if (sequence != this->command_receive_next_frame_ || sequence > this->command_receive_expected_frames_) {
      ESP_LOGW(TAG, "Out-of-order MIoT response fragment: got %u expected %u", sequence,
               this->command_receive_next_frame_);
      this->command_receive_rx_.clear();
      this->command_receive_expected_frames_ = 0;
      this->command_receive_next_frame_ = 1;
      return;
    }
    this->command_receive_rx_.insert(this->command_receive_rx_.end(), data + 2, data + len);
    this->command_receive_next_frame_++;
    if (sequence < this->command_receive_expected_frames_)
      return;

    static constexpr uint8_t flow_received[] = {0x00, 0x00, 0x01, 0x00};
    this->write_char_(this->command_receive_handle_, flow_received, sizeof(flow_received));
    std::vector<uint8_t> plain;
    if (this->decrypt_command_(this->command_receive_rx_.data(), this->command_receive_rx_.size(), plain)) {
      this->process_miot_plaintext_(plain);
      this->finish_command_response_(plain);
    }
    this->command_receive_rx_.clear();
    this->command_receive_expected_frames_ = 0;
    this->command_receive_next_frame_ = 1;
    return;
  }

  if (len < 4)
    return;
  if (data[2] == 0x02) {
    static constexpr uint8_t acknowledged[] = {0x00, 0x00, 0x03, 0x00};
    this->write_char_(this->command_receive_handle_, acknowledged, sizeof(acknowledged));
    std::vector<uint8_t> plain;
    if (len > 4 && this->decrypt_command_(data + 4, len - 4, plain)) {
      this->process_miot_plaintext_(plain);
      this->finish_command_response_(plain);
    }
  } else if (len == 6 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00) {
    // MJTD04YL sends property responses as a FlowPacket announcement followed
    // by one or more sequence-prefixed fragments.
    this->command_receive_expected_frames_ =
        static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
    if (this->command_receive_expected_frames_ == 0 || this->command_receive_expected_frames_ > 16) {
      ESP_LOGW(TAG, "Invalid MIoT response fragment count: %u", this->command_receive_expected_frames_);
      this->command_receive_expected_frames_ = 0;
      return;
    }
    this->command_receive_next_frame_ = 1;
    this->command_receive_rx_.clear();
    this->command_receive_rx_.reserve(this->command_receive_expected_frames_ * 18);
    static constexpr uint8_t ready[] = {0x00, 0x00, 0x01, 0x01};
    this->write_char_(this->command_receive_handle_, ready, sizeof(ready));
  }
}

void MijiaMeshLight::process_miot_plaintext_(const std::vector<uint8_t> &plain) {
  ESP_LOGV(TAG, "Received decrypted MIoT packet (%u bytes)", static_cast<unsigned>(plain.size()));
  if (plain.size() >= 9) {
    ESP_LOGD(TAG, "MIoT response: opcode=0x%02X siid=%u piid=%u len=%u", plain[4], plain[6],
             static_cast<unsigned>(static_cast<uint16_t>(plain[7]) | (static_cast<uint16_t>(plain[8]) << 8)),
             static_cast<unsigned>(plain.size()));
  }
  if (plain.size() < 12 || plain[6] != 2)
    return;
  const uint16_t piid = static_cast<uint16_t>(plain[7]) | (static_cast<uint16_t>(plain[8]) << 8);
  const uint8_t opcode = plain[4];
  uint32_t value = 0;
  bool has_value = false;
  if (opcode == 0x03 && plain.size() >= 14) {
    const uint8_t value_len = plain[11];
    if (value_len > 0 && value_len <= 4 && plain.size() >= static_cast<size_t>(13 + value_len)) {
      for (uint8_t i = 0; i < value_len; i++)
        value |= static_cast<uint32_t>(plain[13 + i]) << (i * 8);
      has_value = true;
    }
  } else if ((opcode == 0x02 || opcode == 0x04) && plain.size() >= 12) {
    const size_t value_index = 11;
    const size_t remaining = plain.size() - value_index;
    const size_t value_len = std::min<size_t>(remaining, 4);
    for (size_t i = 0; i < value_len; i++)
      value |= static_cast<uint32_t>(plain[value_index + i]) << (i * 8);
    has_value = value_len > 0;
  }
  if (has_value)
    this->publish_property_(2, piid, value);
}

void MijiaMeshLight::publish_property_(uint8_t siid, uint16_t piid, uint32_t value) {
  if (siid != 2 || this->light_state_ == nullptr)
    return;
  auto values = this->light_state_->remote_values;
  values.set_color_mode(light::ColorMode::COLOR_TEMPERATURE);
  if (piid == 1) {
    values.set_state(value != 0);
  } else if (piid == 2 && value >= 1 && value <= 100) {
    values.set_brightness(static_cast<float>(value) / 100.0f);
  } else if (piid == 3 && value >= 2700 && value <= 6500) {
    values.set_color_temperature(1000000.0f / static_cast<float>(value));
  } else {
    return;
  }
  this->light_state_->remote_values = values;
  this->light_state_->current_values = values;
  this->light_state_->publish_state();
  ESP_LOGD(TAG, "Lamp property updated: siid=%u piid=%u value=%lu", siid, piid,
           static_cast<unsigned long>(value));
}

void MijiaMeshLight::loop() {
  uint32_t now = millis();
  if (this->start_auth_pending_ && static_cast<int32_t>(now - this->auth_start_not_before_ms_) >= 0) {
    this->start_auth_pending_ = false;
    this->start_authentication_();
    // P-256 key generation is intentionally done immediately before login and
    // can take over 100 ms on an ESP32-C3. Refresh `now` so subtracting the
    // newer auth_activity_ms_ below cannot underflow and look like a timeout.
    now = millis();
  }
  if (this->delayed_dummy_finish_ && static_cast<int32_t>(now - this->delayed_action_ms_) >= 0) {
    this->delayed_dummy_finish_ = false;
    static constexpr uint8_t prepare[] = {0x50, 0x00, 0x00, 0x00};
    static constexpr uint8_t key_header[] = {0x00, 0x00, 0x00, 0x03, 0x01, 0x00};
    if (!this->write_char_(this->auth_control_handle_, prepare, sizeof(prepare)) ||
        !this->write_char_(this->auth_data_handle_, key_header, sizeof(key_header))) {
      this->fail_authentication_("public key exchange setup failed");
    } else {
      this->auth_state_ = AuthState::WAIT_KEY_READY;
      this->auth_activity_ms_ = now;
    }
  }
  if (this->auth_state_ != AuthState::DISCONNECTED && this->auth_state_ != AuthState::REGISTERING &&
      this->auth_state_ != AuthState::AUTHENTICATED && this->auth_state_ != AuthState::FAILED &&
      now - this->auth_activity_ms_ > 8000) {
    this->fail_authentication_("timeout");
  }
  if (this->auth_state_ == AuthState::FAILED && now - this->auth_activity_ms_ > 1000 && this->parent() != nullptr)
    this->parent()->disconnect();

  if (this->auth_state_ == AuthState::AUTHENTICATED) {
    if (this->command_phase_ == CommandPhase::WAIT_RESPONSE && this->set_ack_received_ &&
        now - this->command_activity_ms_ > 1000) {
      ESP_LOGD(TAG, "SET completed with authenticated device ACK");
      this->complete_active_command_();
    }
    if (this->command_phase_ != CommandPhase::IDLE && now - this->command_activity_ms_ > 4000) {
      ESP_LOGW(TAG, "MIoT command channel timeout; continuing with next command");
      this->command_phase_ = CommandPhase::IDLE;
      this->set_ack_received_ = false;
      this->active_encrypted_frame_.clear();
    }
    this->begin_next_command_();
  }
}

void MijiaMeshLight::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status == ESP_GATT_OK) {
        this->reset_connection_state_();
        this->auth_state_ = AuthState::REGISTERING;
        ESP_LOGI(TAG, "Connected to MJTD04YL");
      }
      break;
    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGW(TAG, "MJTD04YL disconnected (reason 0x%02X)", param->disconnect.reason);
      this->reset_connection_state_();
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *service = this->parent()->get_service(SERVICE_UUID);
      if (service == nullptr) {
        this->fail_authentication_("FE95 service not found");
        break;
      }
      auto *auth_control = this->parent()->get_characteristic(SERVICE_UUID, AUTH_CONTROL_UUID);
      auto *auth_data = this->parent()->get_characteristic(SERVICE_UUID, AUTH_DATA_UUID);
      auto *command_send = this->parent()->get_characteristic(SERVICE_UUID, COMMAND_SEND_UUID);
      auto *command_receive = this->parent()->get_characteristic(SERVICE_UUID, COMMAND_RECEIVE_UUID);
      if (auth_control == nullptr || auth_data == nullptr || command_send == nullptr || command_receive == nullptr) {
        this->fail_authentication_("required FE95 characteristics not found");
        break;
      }
      this->auth_control_handle_ = auth_control->handle;
      this->auth_data_handle_ = auth_data->handle;
      this->command_send_handle_ = command_send->handle;
      this->command_receive_handle_ = command_receive->handle;
      ESP_LOGI(TAG,
               "FE95 handles: auth-control=0x%04X auth-data=0x%04X command-send=0x%04X command-receive=0x%04X",
               this->auth_control_handle_, this->auth_data_handle_, this->command_send_handle_,
               this->command_receive_handle_);
      // BLE ATT permits only one descriptor procedure at a time. Subscribe in
      // the same strictly sequential order as the reference Xiaomi client.
      if (this->parent()->register_for_notify(this->auth_control_handle_) != ESP_OK) {
        this->fail_authentication_("authentication notification registration failed");
        return;
      }
      this->auth_state_ = AuthState::REGISTERING;
      this->auth_activity_ms_ = millis();
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      ESP_LOGD(TAG, "Notify registration: handle=0x%04X status=%d", param->reg_for_notify.handle,
               param->reg_for_notify.status);
      if (param->reg_for_notify.status == ESP_GATT_OK &&
          (param->reg_for_notify.handle == this->auth_control_handle_ ||
           param->reg_for_notify.handle == this->auth_data_handle_ ||
           param->reg_for_notify.handle == this->command_send_handle_ ||
           param->reg_for_notify.handle == this->command_receive_handle_)) {
        this->notify_registrations_++;
        if (this->notify_registrations_ >= 4)
          this->node_state = esp32_ble_tracker::ClientState::ESTABLISHED;
      }
      break;
    case ESP_GATTC_WRITE_DESCR_EVT:
      ESP_LOGD(TAG, "CCCD write complete: handle=0x%04X status=%d auth-state=%s", param->write.handle,
               param->write.status, auth_state_name_(this->auth_state_));
      if (param->write.status == ESP_GATT_OK && this->auth_state_ == AuthState::REGISTERING) {
        this->descriptor_writes_++;
        if (this->descriptor_writes_ == 1) {
          if (this->parent()->register_for_notify(this->auth_data_handle_) != ESP_OK)
            this->fail_authentication_("authentication data notification registration failed");
        } else if (this->descriptor_writes_ == 2) {
          this->start_auth_pending_ = true;
          this->auth_start_not_before_ms_ = millis() + 300;
        }
      } else if (param->write.status == ESP_GATT_OK && this->auth_state_ == AuthState::AUTHENTICATED &&
                 !this->command_channel_ready_) {
        this->descriptor_writes_++;
        if (this->descriptor_writes_ == 3) {
          if (this->parent()->register_for_notify(this->command_receive_handle_) != ESP_OK)
            this->fail_authentication_("command receive notification registration failed");
        } else if (this->descriptor_writes_ >= 4) {
          this->command_channel_ready_ = true;
          ESP_LOGI(TAG, "MJTD04YL command channel is ready");
          if (this->authenticated_sensor_ != nullptr)
            this->authenticated_sensor_->publish_state(true);
        }
      }
      break;
    case ESP_GATTC_NOTIFY_EVT:
      ESP_LOGD(TAG, "GATT notification: handle=0x%04X len=%u", param->notify.handle,
               static_cast<unsigned>(param->notify.value_len));
      if (param->notify.handle == this->auth_control_handle_)
        this->handle_auth_control_(param->notify.value, param->notify.value_len);
      else if (param->notify.handle == this->auth_data_handle_)
        this->handle_auth_data_(param->notify.value, param->notify.value_len);
      else if (param->notify.handle == this->command_send_handle_)
        this->handle_command_send_(param->notify.value, param->notify.value_len);
      else if (param->notify.handle == this->command_receive_handle_)
        this->handle_command_receive_(param->notify.value, param->notify.value_len);
      break;
    default:
      break;
  }
}

const char *MijiaMeshLight::auth_state_name_(AuthState state) {
  switch (state) {
    case AuthState::DISCONNECTED:
      return "disconnected";
    case AuthState::REGISTERING:
      return "notification setup";
    case AuthState::WAIT_INIT:
      return "login init";
    case AuthState::WAIT_DUMMY:
      return "dummy exchange";
    case AuthState::WAIT_KEY_READY:
      return "key ready";
    case AuthState::WAIT_PUBLIC_KEY_ACK:
      return "local public key ack";
    case AuthState::WAIT_DEVICE_PUBLIC_KEY:
      return "device public key";
    case AuthState::WAIT_AUTH_READY:
      return "authentication ready";
    case AuthState::WAIT_AUTH_ACK:
      return "authentication ack";
    case AuthState::WAIT_LOGIN_RESULT:
      return "login result";
    case AuthState::AUTHENTICATED:
      return "authenticated";
    case AuthState::FAILED:
      return "failed";
  }
  return "unknown";
}

}  // namespace esphome::mijia_mesh_light

#endif  // USE_ESP32
