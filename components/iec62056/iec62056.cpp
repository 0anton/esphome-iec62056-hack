#include <cstdio>
#include <cstring>
#include <algorithm>  // std::min
#include "iec62056.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "iec62056uart.h"

namespace esphome {
namespace iec62056 {

static const uint8_t ETX = 0x03;
static const uint8_t STX = 0x02;
static const uint8_t ACK = 0x06;
static const uint8_t SOH = 0x01;

static const char *const TAG = "iec62056.component";
const uint32_t BAUDRATES[] = {300, 600, 1200, 2400, 4800, 9600, 19200};
#define MAX_BAUDRATE (BAUDRATES[sizeof(BAUDRATES) / sizeof(uint32_t) - 1])
#define PROTO_B_MIN_BAUDRATE (BAUDRATES[1])

const char *IEC62056Component::obis_codes_[] = {
    "0F0880FF",     // Active energy totals

    // Power total and per phase
    "100700FF",    // Current power total
    "240700FF",    // Current power phase 1
    "380700FF",    // Current power phase 2
    "4C0700FF",    // Current power phase 3

    // Temperature
    "600900FF",

    // Voltage per phase
    "200700FF",   // Voltage phase 1
    "340700FF",   // Voltage phase 2
    "480700FF",   // Voltage phase 3

    // Current per phase
    "1F0700FF",   // Current phase 1
    "330700FF",   // Current phase 2
    "470700FF",   // Current phase 3

    // Active power coefficient
    "0D07FFFF",
};
const size_t IEC62056Component::num_obis_codes_ = sizeof(IEC62056Component::obis_codes_) / sizeof(IEC62056Component::obis_codes_[0]);


IEC62056Component::IEC62056Component() : current_obis_index_(0) {
  state_ = INFINITE_WAIT;
}

void IEC62056Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up iec62056 component...");

  update_last_transmission_from_meter_timestamp_();
#ifdef USE_ESP32_FRAMEWORK_ARDUINO
  iuart_ = make_unique<IEC62056UART>(*static_cast<uart::ESP32ArduinoUARTComponent *>(this->parent_));
#endif

#ifdef USE_ESP_IDF
  iuart_ = make_unique<IEC62056UART>(*static_cast<uart::IDFUARTComponent *>(this->parent_));
#endif

#if USE_ESP8266
  iuart_ = make_unique<IEC62056UART>(*static_cast<uart::ESP8266UartComponent *>(this->parent_));
#endif

  clear_uart_input_buffer_();

  if (force_mode_d_) {
    ESP_LOGI(TAG, "Mode D. Continuously reading data");
    set_next_state_(MODE_D_WAIT);
  } else if (is_periodic_readout_enabled_()) {
    wait_(15000, BEGIN);  // Start the first readout 15s from now
  } else {
    ESP_LOGI(TAG, "No periodic readouts (update_interval=never). Only switch can trigger readout.");
    set_next_state_(INFINITE_WAIT);
  }
}

void IEC62056Component::dump_config() {
  ESP_LOGCONFIG(TAG, "IEC62056:");
  LOG_UPDATE_INTERVAL(this);
  ESP_LOGCONFIG(TAG, "  Connection timeout: %.3fs", this->connection_timeout_ms_ / 1000.0f);
  if (!force_mode_d_) {
    // These settings are not used in Mode D
    ESP_LOGCONFIG(TAG, "  Battery meter: %s", YESNO(this->battery_meter_));
    if (this->config_baud_rate_max_bps_ > 0) {
      ESP_LOGCONFIG(TAG, "  Max baud rate: %u bps", this->config_baud_rate_max_bps_);
    } else {
      ESP_LOGCONFIG(TAG, "  Max baud rate: not limited");
    }
    ESP_LOGCONFIG(TAG, "  Max retries: %u", this->max_retries_);
    ESP_LOGCONFIG(TAG, "  Retry delay: %.3fs", this->retry_delay_ / 1000.0f);
  }
  ESP_LOGCONFIG(TAG, "  Mode D: %s", YESNO(this->force_mode_d_));

  ESP_LOGCONFIG(TAG, "  Sensors:");
  for (const auto &item : sensors_) {
    IEC62056SensorBase *s = item.second;
    ESP_LOGCONFIG(TAG, "    OBIS: %s", s->get_obis().c_str());
  }
}

std::string format_ascii_pretty(const uint8_t *data, size_t length) {
  std::string ascii_str;
  for (size_t i = 0; i < length; ++i) {
    char c = static_cast<char>(data[i]);
    if (std::isprint(static_cast<unsigned char>(c))) {
      ascii_str += c;
    } else {
      ascii_str += '.';
    }
  }
  return ascii_str;
}

std::string format_hex_ascii_pretty(const uint8_t *data, size_t length) {
  std::string hex_str;
  std::string ascii_str;

  for (size_t i = 0; i < length; ++i) {
    // Append hex representation
    if (i > 0)
      hex_str += '.';
    char buf[4]; // Enough for "FF\0"
    snprintf(buf, sizeof(buf), "%02X", data[i]);
    hex_str += buf;

    // Build ASCII representation
    char c = static_cast<char>(data[i]);
    if (std::isprint(static_cast<unsigned char>(c))) {
      ascii_str += c;
    } else {
      ascii_str += '.';
    }
  }

  // Build the final string
  char final_buf[512]; // Adjust size as needed
  snprintf(final_buf, sizeof(final_buf), "%s (%u) |%s|", hex_str.c_str(), (unsigned int)length, ascii_str.c_str());
  return std::string(final_buf);
}

void IEC62056Component::send_frame_() {
  this->write_array(out_buf_, data_out_size_);
  std::string hex_str = format_hex_pretty(out_buf_, data_out_size_);
  std::string ascii_str = format_ascii_pretty(out_buf_, data_out_size_);
  ESP_LOGVV(TAG, "TX: %s |%s|", hex_str.c_str(), ascii_str.c_str());
}


size_t IEC62056Component::receive_frame_() {
  const uint32_t max_while_ms = 15;
  size_t ret_val;
  auto count = this->available();
  if (count <= 0)
    return 0;

  uint32_t while_start = millis();
  uint8_t *p;
  while (count > 0) {
    // Make sure loop() is <30 ms
    if (millis() - while_start > max_while_ms) {
      return 0;
    }

    if (data_in_size_ < MAX_IN_BUF_SIZE) {
      p = &in_buf_[data_in_size_];
      if (!iuart_->read_one_byte(p)) {
        return 0;
      }
      data_in_size_++;
    } else {
      memmove(in_buf_, in_buf_ + 1, data_in_size_ - 1);
      p = &in_buf_[data_in_size_ - 1];
      if (!iuart_->read_one_byte(p)) {
        return 0;
      }
    }

    // Check for ACK
    if (in_buf_[data_in_size_ - 1] == ACK) {
      ESP_LOGVV(TAG, "RX: %s", format_hex_ascii_pretty(in_buf_, data_in_size_).c_str());
      ESP_LOGV(TAG, "Detected ACK");
      update_last_transmission_from_meter_timestamp_();
      ret_val = data_in_size_;
      data_in_size_ = 0;
      return ret_val;
    }

    // it is not possible to have \r\n and ETX in buffer at one time
    if (data_in_size_ >= 2 && ETX == in_buf_[data_in_size_ - 2]) {
      std::string hex_str = format_hex_pretty(in_buf_, data_in_size_);
      std::string ascii_str = format_ascii_pretty(in_buf_, data_in_size_);
      ESP_LOGVV(TAG, "RX: %s |%s|", hex_str.c_str(), ascii_str.c_str());
      ESP_LOGV(TAG, "Detected ETX");

      readout_lrc_ = in_buf_[data_in_size_ - 1];
      ESP_LOGV(TAG, "BCC: 0x%02x", readout_lrc_);

      update_last_transmission_from_meter_timestamp_();
      ret_val = data_in_size_;
      data_in_size_ = 0;
      return ret_val;
    }

    if (STX == in_buf_[data_in_size_ - 1]) {
      ESP_LOGVV(TAG, "RX: %s", format_hex_ascii_pretty(in_buf_, data_in_size_).c_str());
      ESP_LOGV(TAG, "Detected STX");
      reset_lrc_();
      update_last_transmission_from_meter_timestamp_();
      ret_val = data_in_size_;
      data_in_size_ = 0;
      return ret_val;
    }

    if (data_in_size_ >= 2 && '\r' == in_buf_[data_in_size_ - 2] && '\n' == in_buf_[data_in_size_ - 1]) {
      ESP_LOGVV(TAG, "RX: %s", format_hex_ascii_pretty(in_buf_, data_in_size_).c_str());

      // check echo
      if (data_in_size_ == data_out_size_ && 0 == memcmp(out_buf_, in_buf_, data_out_size_)) {
        data_out_size_ = data_in_size_ = 0;
        ESP_LOGVV(TAG, "Echo. Ignore frame.");
        return 0;
      }

      update_last_transmission_from_meter_timestamp_();
      ret_val = data_in_size_;
      data_in_size_ = 0;
      return ret_val;
    }
  }
  return 0;
}

void IEC62056Component::send_battery_wakeup_sequence_() {
  const size_t n = 84;  //~2.24s
  static_assert(n <= MAX_OUT_BUF_SIZE, "Out buffer too small");
  memset(out_buf_, 0, n);
  data_out_size_ = n;
  send_frame_();
}

char *IEC62056Component::get_id_(size_t frame_size) {
  uint8_t *p = &in_buf_[frame_size - 1 - 2 /*\r\n*/];
  size_t min_id_data_size = 7;  // min packet is '/XXXZ\r\n'

  while (p >= in_buf_ && frame_size >= min_id_data_size) {
    if ('/' == *p) {
      if ((size_t)(&in_buf_[MAX_IN_BUF_SIZE - 1] - p) < min_id_data_size) {
        ESP_LOGVV(TAG, "Invalid ID packet.");

        // garbage, ignore
        break;
      }
      in_buf_[frame_size - 2] = '\0';  // terminate string and remove \r\n
      ESP_LOGD(TAG, "Meter identification: '%s'", p);

      return (char *) p;
    }

    p--;
  }

  return nullptr;
}

void IEC62056Component::set_protocol_(char z) {
  if (force_mode_d_) {
    mode_ = PROTOCOL_MODE_D;
  } else if (z >= PROTO_B_RANGE_BEGIN && z <= PROTO_B_RANGE_END) {
    mode_ = PROTOCOL_MODE_B;
  } else if (z >= PROTO_C_RANGE_BEGIN && z <= PROTO_C_RANGE_END) {
    mode_ = PROTOCOL_MODE_C;
  } else {
    mode_ = PROTOCOL_MODE_A;
  }
}

uint32_t IEC62056Component::identification_to_baud_rate_(char z) {
  uint32_t rate;

  if (z >= PROTO_B_RANGE_BEGIN && z <= PROTO_B_RANGE_END) {
    rate = BAUDRATES[1 + z - PROTO_B_RANGE_BEGIN];
  } else if (z >= PROTO_C_RANGE_BEGIN && z <= PROTO_C_RANGE_END) {
    rate = BAUDRATES[z - PROTO_C_RANGE_BEGIN];
  } else {
    rate = 0;
  }

  return rate;
}

char IEC62056Component::baud_rate_to_identification_(uint32_t baud_rate) {
  if (PROTOCOL_MODE_B == mode_) {
    for (size_t i = 1; i < (sizeof(BAUDRATES) / sizeof(uint32_t)); i++) {
      if (baud_rate == BAUDRATES[i]) {
        return PROTO_B_RANGE_BEGIN + i - 1;
      }
    }
  } else {  // PROTOCOL_MODE_C == mode_
    for (size_t i = 0; i < (sizeof(BAUDRATES) / sizeof(uint32_t)); i++) {
      if (baud_rate == BAUDRATES[i]) {
        return PROTO_C_RANGE_BEGIN + i;
      }
    }
  }

  // return lowest baudrate for unknown char
  return PROTOCOL_MODE_B == mode_ ? PROTO_B_RANGE_BEGIN : PROTO_C_RANGE_BEGIN;
}

void IEC62056Component::connection_status_(bool connected) {
  if (connected) {
    ESP_LOGD(TAG, "Connection start");
  } else {
    ESP_LOGD(TAG, "Connection end");
  }
#ifdef USE_BINARY_SENSOR
  if (readout_status_sensor_) {
    readout_status_sensor_->publish_state(connected);
  }
#endif
}

void IEC62056Component::parse_id_(const char *packet) {
  auto len = strlen(packet);
  meter_identification_.assign(packet);
  baud_rate_identification_ = len >= 5 ? packet[4] : 0 /*set proto A, baud rate=0*/;
  ESP_LOGVV(TAG, "Baudrate char: '%c'", baud_rate_identification_);
  set_protocol_(baud_rate_identification_);
  if (len >= 7 && packet[5] == '\\' && packet[6] == '2') // /XXXZ\2Ident
  {                                    
      ESP_LOGD(TAG, "The meter is indicating mode E, which is unsupported. Attempting mode C. " 
                    "This will work for meters supporting both mode E and C.");
  }
}

void IEC62056Component::build_readout_command_(const char *obis_code) {
  // Build the readout command for the given OBIS code
  // Format: SOH R1 STX [OBIS code] ( ) ETX BCC

  // Start with SOH, 'R', '1', STX
  data_out_size_ = 0;
  out_buf_[data_out_size_++] = SOH;
  out_buf_[data_out_size_++] = 'R';
  out_buf_[data_out_size_++] = '1';
  out_buf_[data_out_size_++] = STX;

  // Copy OBIS code characters
  size_t obis_len = strlen(obis_code);
  for (size_t i = 0; i < obis_len; ++i) {
    out_buf_[data_out_size_++] = obis_code[i];
  }

  // Add '(' and ')'
  out_buf_[data_out_size_++] = '(';
  out_buf_[data_out_size_++] = ')';

  // Add ETX
  out_buf_[data_out_size_++] = ETX;

  // Calculate BCC (Block Check Character)
  uint8_t bcc = 0x00;
  for (size_t i = 1; i < data_out_size_; ++i) {
    bcc ^= out_buf_[i];
  }
  // Append BCC
  out_buf_[data_out_size_++] = bcc;
}


void IEC62056Component::update_baudrate_(uint32_t baudrate) {
  ESP_LOGV(TAG, "Baudrate set to: %u bps", baudrate);
  iuart_->update_baudrate(baudrate);
}

void IEC62056Component::loop() {
  static char baud_rate_char;
  static bool mode_d_empty_frame_received;
  static uint32_t new_baudrate;

  const uint8_t id_request[5] = {'/', '?', '!', '\r', '\n'};
  const uint8_t set_baud_and_programm[6] = {ACK, 0x30, 0x30, 0x31, 0x0d, 0x0a};
  // const uint8_t set_baud[6] = {ACK, 0x30, 0x30, 0x30, 0x0d, 0x0a};
  const uint8_t set_password[16] = {SOH, 'P', '1', STX, '(', '0', '0', '0', '0', '0', '0', '0', '0', ')', ETX, 0x61};
  const uint8_t readout_energy[16] = {SOH, 'R', '1', STX, '0', 'F', '0', '8', '8', '0', 'F', 'F', '(', ')', ETX, 0x15};
  const uint32_t now = millis();

  size_t frame_size;

  if (!is_wait_state_() && now - last_transmission_from_meter_timestamp_ >= connection_timeout_ms_) {
    ESP_LOGE(TAG, "No transmission from meter.");
    connection_status_(false);
    retry_or_sleep_();
    return;
  }

  switch (state_) {
    case INFINITE_WAIT:
      // only switch can set another state
      report_state_();
      update_last_transmission_from_meter_timestamp_();
      break;

    case WAIT:
      report_state_();
      if (check_wait_period_()) {
        state_ = wait_next_state_;
      }
      update_last_transmission_from_meter_timestamp_();
      break;

    case MODE_D_WAIT:
      report_state_();

      if ((frame_size = receive_frame_())) {
        char *packet = get_id_(frame_size);
        if (packet) {
          parse_id_(packet);
          set_next_state_(MODE_D_READOUT);
          update_last_transmission_from_meter_timestamp_();
          retry_connection_start_timestamp_ = millis();
          connection_status_(true);
          mode_d_empty_frame_received = false;
        }
      }
      break;

    case MODE_D_READOUT:
      report_state_();

      if ((frame_size = receive_frame_())) {
        if (in_buf_[0] == '!') {
          connection_status_(false);

          // end of data
          ESP_LOGD(TAG, "Total connection time: %u ms", millis() - retry_connection_start_timestamp_);

          verify_all_sensors_got_value_();
          ESP_LOGD(TAG, "Start of sensor update");
          set_next_state_(UPDATE_STATES);
          sensors_iterator_ = sensors_.begin();
        } else {
          // parse data frame
          in_buf_[frame_size - 2] = 0;

          ESP_LOGD(TAG, "Data: '%s'", in_buf_);

          // in mode D an empty line is sent after identification packet
          // ignore only one
          if (!mode_d_empty_frame_received && '\0' == in_buf_[0]) {
            ESP_LOGV(TAG, "Ignore empty frame");
            mode_d_empty_frame_received = true;
            break;
          }

          std::string obis;
          std::string val1;
          std::string val2;

          if (!parse_line_((const char *) in_buf_, obis, val1, val2)) {
            ESP_LOGE(TAG, "Invalid frame format: '%s'", in_buf_);
            break;
          }

          // Update all matching sensors
          auto range = sensors_.equal_range(obis);
          for (auto it = range.first; it != range.second; ++it) {
            set_sensor_value_(it, val1.c_str(), val2.c_str());
          }
        }
      }
      break;

    case BEGIN:
      report_state_();
      current_obis_index_ = 0;  // Reset index at the beginning

      update_connection_start_timestamp_();
      connection_status_(true);

      if (battery_meter_) {
        set_next_state_(BATTERY_WAKEUP);
      } else {
        set_next_state_(SEND_REQUEST);
      }
      update_baudrate_(300);  // make sure we start with 300 bps

      update_last_transmission_from_meter_timestamp_();
      break;

    case BATTERY_WAKEUP:
      // Special sequence to wake up ("normal" is used here)
      // 1. send NULL chars for 2.1-2.3s
      // 2. wait 1.5-1.7
      // 3. send standard identification message

      // if we send 84 NULLs at 300 baud it will take ~2.24s
      report_state_();

      ESP_LOGD(TAG, "Battery meter wakeup sequence");

      this->send_battery_wakeup_sequence_();
      wait_(1600 + 2240, SEND_REQUEST);  // wait for ~1.6s + 2.24s for all NULLs transmitted
      break;

    case SEND_REQUEST:
      report_state_();

      clear_uart_input_buffer_();  // remove garbage including NULLs from battery meter wakeup sequence

      static_assert(MAX_OUT_BUF_SIZE >= sizeof(id_request), "Out buffer too small");
      memcpy(out_buf_, id_request, sizeof(id_request));
      data_out_size_ = sizeof(id_request);
      send_frame_();
      set_next_state_(GET_IDENTIFICATION);
      break;

    case GET_IDENTIFICATION:
      report_state_();

      if ((frame_size = receive_frame_())) {
        char *packet = get_id_(frame_size);
        if (packet) {
          parse_id_(packet);
        } else {
          ESP_LOGE(TAG, "Invalid identification frame");
          retry_or_sleep_();
          break;
        }

        ESP_LOGD(TAG, "Meter reported protocol: %c", (char) mode_);
        if (mode_ != PROTOCOL_MODE_A) {
          ESP_LOGD(TAG, "Meter reported max baud rate: %u bps ('%c')",
                   identification_to_baud_rate_(baud_rate_identification_), baud_rate_identification_);
        }
        set_next_state_(PREPARE_ACK);
      }
      break;

    case PREPARE_ACK:
      report_state_();

      if (mode_ == PROTOCOL_MODE_A) {
        ESP_LOGVV(TAG, "Using PROTOCOL_MODE_A");
        // switching baud rate not supported, start reading data
        set_next_state_(WAIT_FOR_STX);
        break;
      }

      // protocol B, C
      if (config_baud_rate_max_bps_ != 0 && config_baud_rate_max_bps_ != MAX_BAUDRATE) {
        auto negotiated_bps = identification_to_baud_rate_(baud_rate_identification_);
        if (negotiated_bps > config_baud_rate_max_bps_) {
          negotiated_bps = config_baud_rate_max_bps_;

          if (mode_ == PROTOCOL_MODE_B && negotiated_bps < PROTO_B_MIN_BAUDRATE) {
            negotiated_bps = PROTO_B_MIN_BAUDRATE;
          }
        }

        baud_rate_char = baud_rate_to_identification_(negotiated_bps);
        ESP_LOGD(TAG, "Using negotiated baud rate %d bps.", negotiated_bps);
      } else {
        ESP_LOGD(TAG, "Using meter maximum baud rate %d bps ('%c').",
                 identification_to_baud_rate_(baud_rate_identification_), baud_rate_identification_);
        baud_rate_char = baud_rate_identification_;
      }

      // if (retry_counter_ > 0) {  // decrease baud rate for retry
      //   baud_rate_char -= retry_counter_;
      //   if (mode_ == PROTOCOL_MODE_B && baud_rate_char < PROTO_B_RANGE_BEGIN) {
      //     baud_rate_char = PROTO_B_RANGE_BEGIN;
      //   } else if (baud_rate_char < PROTO_C_RANGE_BEGIN) {
      //     baud_rate_char = PROTO_C_RANGE_BEGIN;
      //   }
      //   ESP_LOGD(TAG, "Decreased baud rate for retry %u to: %d bps ('%c').", retry_counter_,
      //            identification_to_baud_rate_(baud_rate_char), baud_rate_char);
      // }

      data_out_size_ = sizeof(set_baud_and_programm);
      memcpy(out_buf_, set_baud_and_programm, data_out_size_);
      out_buf_[2] = baud_rate_char;
      send_frame_();

      new_baudrate = identification_to_baud_rate_(baud_rate_char);

      // wait for the frame to be fully transmitted before changing baud rate,
      // otherwise port get stuck and no packet can be received (ESP32)

      wait_(250, SET_BAUD_RATE);
      break;

    case SET_BAUD_RATE:
      ESP_LOGD(TAG, "Switching to new baud rate %u bps ('%c')", new_baudrate, baud_rate_char);
      update_baudrate_(new_baudrate);
      set_next_state_(WAIT_FOR_PPP);
      break;

    case WAIT_FOR_PPP:
      report_state_();
      if (receive_frame_() >= 1) {
        if (SOH == in_buf_[0]) {  // RX: 01.50.30.02 (4) |.P0.|
          ESP_LOGD(TAG, "Meter asks for password");
          set_next_state_(WAIT_FOR_PPP_READ_DATA);
        } else {
          ESP_LOGD(TAG, "No PPP. Got 0x%02x", in_buf_[0]);
          retry_or_sleep_();
        }
      }
      break;

    case WAIT_FOR_PPP_READ_DATA:
      report_state_();
      if (receive_frame_() >= 1) {
        if ('(' == in_buf_[0]) { // RX: .P1.(00000000).a
        ESP_LOGD(TAG, "Meter asks for password (data)");
        set_next_state_(SEND_PASSWORD);
      } else {
        ESP_LOGD(TAG, "No data. Got 0x%02x", in_buf_[0]);
        retry_or_sleep_();
      }
    }
    break;

    case SEND_PASSWORD:
       report_state_();
       data_out_size_ = sizeof(set_password);
       memcpy(out_buf_, set_password, data_out_size_);
       send_frame_();
       set_next_state_(WAIT_FOR_ACK);
    break;


    case WAIT_FOR_ACK:
      report_state_();

      if (receive_frame_() >= 1) {
        if (ACK == in_buf_[0]) {
          ESP_LOGD(TAG, "Meter accepted password");
          set_next_state_(ASK_FOR_ENERGY);
        } else {
          ESP_LOGD(TAG, "Meter rejected password. Got 0x%02x", in_buf_[0]);
        }
      }
      break;

    case WAIT_FOR_STX2:  // wait for STX
      report_state_();

      // If the loop is called not very often, data can be overwritten.
      // In that case just increase UART buffer size
      if (receive_frame_() >= 1) {
        if (STX == in_buf_[0]) {
          ESP_LOGD(TAG, "Meter started readout transmission");
          set_next_state_(READOUT2);
        } else {
          ESP_LOGD(TAG, "No STX. Got 0x%02x", in_buf_[0]);
          retry_or_sleep_();
        }
      }
      break;

    case READOUT2:
      report_state_();
      if (receive_frame_() >= 1) {
          set_next_state_(ASK_FOR_ENERGY);
      }
      break;

    case ASK_FOR_ENERGY:
      report_state_();
      build_readout_command_(obis_codes_[current_obis_index_]);  // Build command for current OBIS code
      send_frame_();
      set_next_state_(WAIT_FOR_STX);
      break;

    case WAIT_FOR_STX:  // wait for STX
      report_state_();

      // If the loop is called not very often, data can be overwritten.
      // In that case just increase UART buffer size
      if (receive_frame_() >= 1) {
        if (STX == in_buf_[0]) {
          ESP_LOGD(TAG, "Meter started readout transmission");
          set_next_state_(READOUT);
        } else {
          ESP_LOGD(TAG, "No STX. Got 0x%02x", in_buf_[0]);
          retry_or_sleep_();
        }
      }
      break;


    case READOUT:
      report_state_();

      if ((frame_size = receive_frame_())) {
        // Check if ETX is at the end of the frame
        if (frame_size >= 2 && in_buf_[frame_size - 2] == ETX) {
          ESP_LOGD(TAG, "Detected ETX at the end of data");
          ESP_LOGD(TAG, "Total connection time: %u ms", millis() - retry_connection_start_timestamp_);

          // Reset lrc_
          lrc_ = 0x00;
          // Update lrc_ over data bytes including ETX, exclude BCC
          update_lrc_(in_buf_, frame_size - 1);  // Exclude BCC byte

          // Verify BCC
          bool bcc_failed = false;
          if (lrc_ == readout_lrc_) {
            ESP_LOGD(TAG, "BCC verification is OK");
          } else {
            ESP_LOGE(TAG, "BCC verification failed. Expected 0x%02x, got 0x%02x", lrc_, readout_lrc_);
            bcc_failed = true;
          }

          // Process the data before proceeding
          in_buf_[frame_size - 2] = 0;  // Null-terminate before ETX
          ESP_LOGD(TAG, "Data: %s", in_buf_);
          std::string obis;
          std::string val1;
          std::string val2;

          if (!parse_line_((const char *) in_buf_, obis, val1, val2)) {
            ESP_LOGE(TAG, "Invalid frame format: '%s'", in_buf_);
          } else {
            // Update all matching sensors
            auto range = sensors_.equal_range(obis);
            for (auto it = range.first; it != range.second; ++it) {
              set_sensor_value_(it, val1.c_str(), val2.c_str());
            }
          }

          connection_status_(false);

          if (bcc_failed) {
            ESP_LOGD(TAG, "BCC check has failed, but will carry on with the sensors update for now...");
            // Handle BCC failure if necessary
          }

          // Move to the next OBIS code or proceed to updating sensors
          if (++current_obis_index_ < num_obis_codes_) {
            // There are more OBIS codes to read
            set_next_state_(ASK_FOR_ENERGY);
          } else {
            // All OBIS codes have been read
            verify_all_sensors_got_value_();
            ESP_LOGD(TAG, "Start of sensor update");
            set_next_state_(UPDATE_STATES);
            sensors_iterator_ = sensors_.begin();
          }
        } else {
          // Handle data frames without ETX (if applicable)
          update_lrc_(in_buf_, frame_size);

          in_buf_[frame_size - 2] = 0;  // Null-terminate the data string
          ESP_LOGD(TAG, "Data: %s", in_buf_);
          std::string obis;
          std::string val1;
          std::string val2;

          if (!parse_line_((const char *) in_buf_, obis, val1, val2)) {
            ESP_LOGE(TAG, "Invalid frame format: '%s'", in_buf_);
            break;
          }

          // Update all matching sensors
          auto range = sensors_.equal_range(obis);
          for (auto it = range.first; it != range.second; ++it) {
            set_sensor_value_(it, val1.c_str(), val2.c_str());
          }
        }
      }
      break;

    case UPDATE_STATES:
      report_state_();
      if (sensors_iterator_ != sensors_.end()) {
        IEC62056SensorBase *s = (*sensors_iterator_).second;
        sensors_iterator_++;
        if (s->has_value()) {
          s->publish();
        }
      } else {
        ESP_LOGD(TAG, "End of sensor update");

        wait_next_readout_();  // wait for the next cycle
        break;
      }
  }
}

void IEC62056Component::update_lrc_(const uint8_t *data, size_t size) {
  for (size_t i = 0; i < size; i++) {
    lrc_ ^= data[i];
  }
}

float IEC62056Component::get_setup_priority() const { return setup_priority::DATA; }

void IEC62056Component::register_sensor(IEC62056SensorBase *sensor) {
  this->sensors_.insert({sensor->get_obis(), sensor});
}

bool IEC62056Component::validate_float_(const char *value) {
  // safe value, in reality this number is related to the number of digits on meter's display
  const size_t max_len = 20;
  size_t count = 0;
  const char *p = value;
  while (*p && *p != '*') {  // ignore unit at the end
    if (!(isdigit(*p) || *p == '.' || *p == '-')) {
      return false;
    }
    p++;
    count++;
  }

  return count > 0 && count <= max_len;
}

bool IEC62056Component::set_sensor_value_(SENSOR_MAP::iterator &i, const char *value1, const char *value2) {
  const char *value = value1;
  IEC62056SensorBase *sensor = i->second;
  SensorType type = sensor->get_type();
  if (type == TEXT_SENSOR) {
    IEC62056TextSensor *txt = static_cast<IEC62056TextSensor *>(sensor);
    switch (txt->get_group()) {
      case 0:  // set entire frame
        value = (const char *) this->in_buf_;
        break;

      case 2:
        value = value2;
        break;

        // defaults to value1
    }
    txt->set_value(value);

    ESP_LOGD(TAG, "Set text sensor '%s' for OBIS '%s' group %d. Value: '%s'", txt->get_name().c_str(),
             txt->get_obis().c_str(), txt->get_group(), value);
  } else {  // SENSOR
    // convert to float
    if (validate_float_(value)) {
      IEC62056Sensor *sen = static_cast<IEC62056Sensor *>(sensor);
      float f = strtof(value, nullptr);
      sen->set_value(f);
      ESP_LOGD(TAG, "Set sensor '%s' for OBIS '%s'. Value: %f", sen->get_name().c_str(), sen->get_obis().c_str(), f);
    } else {
      ESP_LOGE(TAG, "Cannot convert data to number. Consider using text sensor. Invalid data: '%s'", value);
      return false;
    }
  }

  return true;
}

void IEC62056Component::reset_all_sensors_() {
  for (const auto &item : sensors_) {
    IEC62056SensorBase *s = item.second;
    s->reset();
  }
}

void IEC62056Component::verify_all_sensors_got_value_() {
  for (const auto &item : sensors_) {
    IEC62056SensorBase *s = item.second;
    if (!s->has_value()) {
      ESP_LOGE(TAG,
               "Not all sensors received data from the meter. The first one: OBIS '%s'. Verify sensor is defined with "
               "valid OBIS code.",
               s->get_obis().c_str());
      break;  // Display just one error. If more displayed, component could take a long time for an operation
    }
  }
}

// Valid OBIS codes may be empty or may contain digits and uppercase letters
bool IEC62056Component::validate_obis_(const std::string &obis) {
  const size_t max_obis_len = 25;  // Arbitrary chosen max length

  // Allow empty OBIS codes
  if (obis.empty()) {
    ESP_LOGVV(TAG, "OBIS code is empty");
    return true;
  }

  // Check if the OBIS code exceeds the maximum allowed length
  if (obis.size() > max_obis_len) {
    ESP_LOGVV(TAG, "OBIS code is too long");
    return false;
  }

  // Validate each character in the OBIS code
  for (const char &c : obis) {
    if (!(c == ':' || c == '.' || c == '-' || c == '*' ||
          (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
      ESP_LOGVV(TAG, "OBIS code has invalid characters");
      return false;
    }
  }
  return true;
}

bool IEC62056Component::parse_line_(const char *line, std::string &out_obis, std::string &out_value1,
                                    std::string &out_value2) {
  const char *open_bracket = nullptr;
  const char *close_bracket = nullptr;
  const char *open_bracket2 = nullptr;
  const char *close_bracket2 = nullptr;

  const char *p = line;
  while (*p) {
    if ('(' == *p && !open_bracket) {
      open_bracket = p;
    } else if (')' == *p && !close_bracket) {
      close_bracket = p;
    } else if ('(' == *p && !open_bracket2) {
      open_bracket2 = p;
    } else if (')' == *p && !close_bracket2) {
      close_bracket2 = p;
    }
    p++;  // Increment pointer after processing the character
  }

  if (!open_bracket || !close_bracket || close_bracket < open_bracket) {
    ESP_LOGVV(TAG, "Missing expected open and closing bracket");
    return false;
  }

  out_obis.assign(line, open_bracket - line);
  out_value1.assign(open_bracket + 1, close_bracket - open_bracket - 1);

  if (open_bracket2 && close_bracket2 && close_bracket2 > open_bracket2) {
    out_value2.assign(open_bracket2 + 1, close_bracket2 - open_bracket2 - 1);
  } else {
    out_value2.erase();
  }

  return validate_obis_(out_obis);
}


void IEC62056Component::clear_uart_input_buffer_() {
  int available = this->available();
  int len;

  if (available > 0) {
    ESP_LOGVV(TAG, "Garbage data in UART input buffer: %d bytes", available);
  }

  while (available > 0) {
    len = std::min(available, (int) MAX_IN_BUF_SIZE);

    this->read_array(in_buf_, len);
    available -= len;
  }
  data_in_size_ = 0;
}

void IEC62056Component::wait_(uint32_t ms, CommState state) {
  ESP_LOGVV(TAG, "Start WAIT for %u ms", ms);
  set_next_state_(WAIT);
  wait_start_timestamp_ = millis();
  wait_period_ms_ = ms;
  wait_next_state_ = state;
}

const char *IEC62056Component::state2txt_(CommState state) {
  switch (state) {
    case BATTERY_WAKEUP:
      return "BATTERY_WAKEUP";

    case BEGIN:
      return "BEGIN";

    case WAIT:
      return "WAIT";

    case SEND_REQUEST:
      return "SEND_REQUEST";

    case GET_IDENTIFICATION:
      return "GET_IDENTIFICATION";

    case PREPARE_ACK:
      return "SET_BAUD_RATE";

    case WAIT_FOR_ACK:
      return "WAIT_FOR_ACK";

    case SEND_PASSWORD:
      return "SEND_PASSWORD";

    case WAIT_FOR_STX:
      return "WAIT_FOR_STX";

    case WAIT_FOR_STX2:
      return "WAIT_FOR_STX2";

    case ASK_FOR_ENERGY:
      return "ASK_FOR_ENERGY";

    case WAIT_FOR_PPP:
      return "WAIT_FOR_PPP";
    
    case READOUT:
      return "READOUT";

    case READOUT2:
      return "READOUT2";

    case UPDATE_STATES:
      return "UPDATE_STATES";

    case INFINITE_WAIT:
      return "INFINITE_WAIT";

    case MODE_D_WAIT:
      return "MODE_D_WAIT";

    case MODE_D_READOUT:
      return "MODE_D_READOUT";

    case WAIT_FOR_PPP_READ_DATA:
      return "WAIT_FOR_PPP_READ_DATA";

    default:
      return "UNKNOWN";
  }
}

void IEC62056Component::report_state_() {
  if (state_ != reported_state_) {
    ESP_LOGV(TAG, "%s", state2txt_(state_));
    reported_state_ = state_;
  }
}

void IEC62056Component::retry_or_sleep_() {
  if (force_mode_d_) {
    set_next_state_(MODE_D_WAIT);
  } else if (retry_counter_ >= max_retries_) {
    ESP_LOGD(TAG, "Exceeded retry counter.");
    wait_next_readout_();
  } else {
    retry_counter_inc_();
    ESP_LOGD(TAG, "Retry %d of %d. Waiting %u ms before the next try", retry_counter_, max_retries_, retry_delay_);
    wait_(retry_delay_, BEGIN);
  }
}

void IEC62056Component::trigger_readout() {
  if (force_mode_d_) {
    ESP_LOGD(TAG, "Triggering readout in Mode D is not possible.");
    return;
  }

  if (!is_wait_state_()) {
    ESP_LOGD(TAG, "Readout in progress. Ignoring trigger.");
    return;
  }

  ESP_LOGD(TAG, "Triggering readout");
  set_next_state_(BEGIN);
}

void IEC62056Component::wait_next_readout_() {
  if (force_mode_d_) {
    set_next_state_(MODE_D_WAIT);
    return;
  }

  uint32_t now = millis();
  uint32_t delta = now - scheduled_connection_start_timestamp_;
  uint32_t actual_wait_time = update_interval_ms_ - delta;

  retry_counter_reset_();
  if (delta > update_interval_ms_ && is_periodic_readout_enabled_()) {
    ESP_LOGD(TAG, "Total connection time greater than configured update interval. Working continuously.");
    actual_wait_time = 0;  // continuously read data
  }

  scheduled_timestamp_set_ = false;
  if (is_periodic_readout_enabled_()) {
    ESP_LOGD(TAG, "Waiting %u ms for the next scheduled readout (every %u ms).", actual_wait_time, update_interval_ms_);
    wait_(actual_wait_time, BEGIN);
  } else {
    // UINT32_MAX means no update, use switch to trigger readout
    // just wait
    ESP_LOGD(TAG, "No scheduled readout. Use switch to trigger readout.");
    set_next_state_(INFINITE_WAIT);
  }
}

void IEC62056Component::update_connection_start_timestamp_() {
  retry_connection_start_timestamp_ = millis();

  // Update only for the first time
  if (!scheduled_timestamp_set_) {
    scheduled_connection_start_timestamp_ = retry_connection_start_timestamp_;
    scheduled_timestamp_set_ = true;
    ESP_LOGV(TAG, "Begin scheduled readout");
  } else {
    ESP_LOGV(TAG, "Begin retry");
  }
}

}  // namespace iec62056
}  // namespace esphome
