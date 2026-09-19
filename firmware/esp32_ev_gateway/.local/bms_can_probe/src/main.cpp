#include <Arduino.h>
#include <driver/twai.h>

static constexpr gpio_num_t kTx = GPIO_NUM_9;
static constexpr gpio_num_t kRx = GPIO_NUM_8;
static constexpr uint8_t kFirst = 0x90;
static constexpr uint8_t kLast = 0x98;
static constexpr uint32_t kProbeMs = 15000;
static uint32_t started;
static uint8_t dataId = kFirst;
static uint32_t nextRequest;
static bool finished;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("BMS_CAN_PROBE start bitrate=250000 tx_gpio=9 rx_gpio=8 ext_id=1 read_ids=0x90-0x98");
  const twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(kTx, kRx, TWAI_MODE_NORMAL);
  const twai_timing_config_t timing = TWAI_TIMING_CONFIG_250KBITS();
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  esp_err_t result = twai_driver_install(&general, &timing, &filter);
  if (result == ESP_OK) result = twai_start();
  if (result != ESP_OK) {
    Serial.printf("CAN_START_FAILED err=%d\n", static_cast<int>(result));
    finished = true;
    return;
  }
  started = millis();
  nextRequest = started;
}

void loop() {
  const uint32_t now = millis();
  if (!finished && now - started >= kProbeMs) {
    twai_status_info_t status = {};
    twai_get_status_info(&status);
    Serial.printf("PROBE_DONE tx=%lu rx=%lu tx_error=%lu rx_error=%lu bus_error=%lu bus_off=%u\n",
      static_cast<unsigned long>(status.msgs_to_tx), static_cast<unsigned long>(status.msgs_to_rx),
      static_cast<unsigned long>(status.tx_error_counter), static_cast<unsigned long>(status.rx_error_counter),
      static_cast<unsigned long>(status.bus_error_count), status.state == TWAI_STATE_BUS_OFF ? 1u : 0u);
    twai_stop();
    twai_driver_uninstall();
    finished = true;
  }
  if (!finished && static_cast<int32_t>(now - nextRequest) >= 0) {
    twai_message_t request = {};
    request.identifier = 0x18000000u | (static_cast<uint32_t>(dataId) << 16) | 0x0140u;
    request.extd = 1;
    request.data_length_code = 8;
    const esp_err_t result = twai_transmit(&request, pdMS_TO_TICKS(25));
    Serial.printf("TX id=0x%08lX field=0x%02X result=%d\n",
      static_cast<unsigned long>(request.identifier), dataId, static_cast<int>(result));
    dataId = dataId == kLast ? kFirst : static_cast<uint8_t>(dataId + 1);
    nextRequest += 100;
  }
  twai_message_t frame = {};
  while (twai_receive(&frame, 0) == ESP_OK) {
    Serial.printf("RX id=0x%08lX ext=%u dlc=%u data=", static_cast<unsigned long>(frame.identifier),
      frame.extd ? 1u : 0u, frame.data_length_code);
    for (uint8_t i = 0; i < frame.data_length_code; ++i) Serial.printf("%02X", frame.data[i]);
    Serial.println();
  }
  delay(1);
}
