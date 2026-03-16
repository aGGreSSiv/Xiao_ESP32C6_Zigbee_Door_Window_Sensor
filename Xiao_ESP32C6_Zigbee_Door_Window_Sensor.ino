#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

#include "Zigbee.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

// --- Pin Definitions ---
// Xiao ESP32-C6 Deep Sleep Requirements: Must use LP GPIOs (0-7) for RTC wakeup
// LP_GPIO1 = D1, LP_GPIO2 = D2, A0/D0 = LP_GPIO0
#define REED_SWITCH_PIN 1 // LP_GPIO1 (D1)

// LED Pins
#define LED_BUILTIN_PIN LED_BUILTIN // Onboard yellow LED

// --- Timing and Thresholds ---
#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */
// Wake up periodically to report battery, e.g., every 1 hour (3600 seconds)
#define TIME_TO_SLEEP 3600

// --- RTC Variables (Retained during deep sleep) ---
RTC_DATA_ATTR uint32_t rtc_time_until_report = TIME_TO_SLEEP;

// --- Zigbee Variables ---
#define CONTACT_SENSOR_ENDPOINT 10

// We use ZigbeeContactSwitch for the door sensor
ZigbeeContactSwitch zbContactSensor =
    ZigbeeContactSwitch(CONTACT_SENSOR_ENDPOINT);

// --- Global State ---
bool keepAwake = false;
uint32_t wakeTime = 0;
uint32_t lastActivityTime = 0;
esp_sleep_wakeup_cause_t global_wakeup_reason;
uint64_t globalWakeupPinMask = 0;

// Helper for LED feedback
void triggerLED(uint8_t ledPin) {
  digitalWrite(ledPin, LOW); // Turn ON (assuming sinking current/active low or
                             // onboard reverse logic)
  delay(100);
  digitalWrite(ledPin, HIGH); // Turn OFF
}

void reportBattery() {
  uint32_t Vbatt = 0;
  // Delay 100ms for ADC capacitor smoothing
  // (Assuming 1MOhm divider + 100nF to 1uF cap requires some settling time)
  delay(100);

  // Read ADC multiple times for averaging
  for (int i = 0; i < 16; i++) {
    Vbatt += analogReadMilliVolts(A0);
  }

  // ADC Calibration Factor: Adjust this to match your multimeter reading.
  float calibration_factor = 1.0145;
  // Factor of 2.0 handles the 1MOhm/1MOhm voltage divider (half voltage at A0)
  float Vbattf = (2.0 * Vbatt / 16.0 / 1000.0) * calibration_factor;

  float max_v = 4.2;
  float min_v = 3.2;
  float percentageReal = ((Vbattf - min_v) / (max_v - min_v)) * 100.0;

  if (percentageReal > 100.0)
    percentageReal = 100.0;
  if (percentageReal < 0.0)
    percentageReal = 0.0;

  // Set the battery attributes
  zbContactSensor.setBatteryVoltage(
      (uint8_t)(Vbattf * 10.0)); // e.g., 41 for 4.1V
  zbContactSensor.setBatteryPercentage(
      (uint8_t)(percentageReal * 2)); // Zigbee percentage is 1 unit = 0.5%

  // Force update to Home Assistant
  zbContactSensor.reportBatteryPercentage();

  Serial.printf("Battery: %.2fV (%d%%)\n", Vbattf, (uint8_t)percentageReal);
}

void setup() {
  Serial.begin(115200);

  // --- External Antenna Settings (Optional/Commented out for now) ---
  // If you need the external antenna instead, uncomment these lines:

  pinMode(3, OUTPUT);
  pinMode(14, OUTPUT);
  digitalWrite(3, LOW);
  delay(10);
  digitalWrite(14, HIGH);
  Serial.println("External Antenna Enabled");

  // --- Identify Wake Up Reason ---
  global_wakeup_reason = esp_sleep_get_wakeup_cause();
  if (global_wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    globalWakeupPinMask = esp_sleep_get_ext1_wakeup_status();
    Serial.printf("Woke up from EXT1 with mask: 0x%llX\n", globalWakeupPinMask);
  } else if (global_wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke up from Timer (Battery Report)");
  } else {
    Serial.println("Woke up from regular BOOT/RESET");
  }

  // Calculate how long we were asleep based on the wake-up reason
  uint32_t sleep_duration_s = 0;
  if (global_wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    // If we woke up before the timer, figure out roughly how much we slept.
    // esp_sleep_get_time_since_wakeup() doesn't give time spent in sleep.
    // There isn't a direct way to get "time spent sleeping" accurately without
    // reading an RTC clock, but we can do a simplified approximation or just
    // decrement a fixed amount for simplicity. Let's use a robust approach:
    // Every time we wake up for GPIO, we assume we were asleep for the full
    // elapsed time since we went to sleep. Or we can just read the RTC time.
    // 
    // Wait, let's keep it simple: instead of tracking exact microseconds, we just
    // schedule the *next* wake up timer to the remaining time.
    // For ESP32, wake up from timer resets the internal RTC timer.
    // To measure time properly across sleeps, we could use system time + SNTP,
    // but we don't have WiFi/SNTP.
  }
  
  if (global_wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    // Timer expired! Time to report. Reset the counter.
    rtc_time_until_report = TIME_TO_SLEEP;
  }
  
  // Update last activity time
  wakeTime = millis();
  lastActivityTime = millis();

  // --- Configure Pins ---
  pinMode(LED_BUILTIN_PIN, OUTPUT);
  digitalWrite(LED_BUILTIN_PIN, HIGH); // Wait state is HIGH (Off)

  pinMode(REED_SWITCH_PIN, INPUT_PULLUP);

  pinMode(A0, INPUT); // Configure A0 as ADC input

  // Disable GPIO hold from deep sleep if it was held previously
  gpio_hold_dis((gpio_num_t)REED_SWITCH_PIN);

  // --- Zigbee Setup ---
  zbContactSensor.setManufacturerAndModel("Espressif", "ZBSensor2");

  // Set power source to battery
  zbContactSensor.setPowerSource(ZB_POWER_SOURCE_BATTERY, 100, 33);

  Serial.println("Adding ZigbeeContactSwitch endpoint to Zigbee Core");
  Zigbee.addEndpoint(&zbContactSensor);

  // Fixed to Channel 11 (HA coordinator channel typically)
  Zigbee.setPrimaryChannelMask(1 << 11);
  Zigbee.setScanDuration(4);
  Zigbee.setTimeout(60000);

  // Default Sleepy End Device Config
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = 3000;

  if (!Zigbee.begin(&zigbeeConfig, false)) {
    Serial.println("Zigbee failed to start!");
    Serial.println("Rebooting...");
    ESP.restart();
  }

  Serial.println("Connecting to network...");
  int dots = 0;
  while (!Zigbee.connected() && millis() - wakeTime < 30000) {
    // Timeout connecting after 30 seconds to prevent killing batter
    Serial.print(".");
    dots++;
    if (dots % 50 == 0)
      Serial.println();
    delay(200);
  }
  Serial.println();

  if (Zigbee.connected()) {
    Serial.println("Connected to Zigbee network!");

    // Restore IAS Zone Enrollment if previously paired
    // This is required for the IAS Zone Cluster to properly report state
    // changes to the Coordinator
    if (!zbContactSensor.enrolled()) {
      Serial.println("IAS Zone not currently enrolled. Attempting to restore "
                     "enrollment from flash...");
      bool restored = zbContactSensor.restoreIASZoneEnroll();
      if (restored) {
        Serial.println("IAS Zone enrollment restored successfully.");
      } else {
        Serial.println("Warning: IAS Zone enrollment could not be restored. "
                       "(Normal for first pairing)");
      }
    } else {
      Serial.println("IAS Zone is already enrolled.");
    }

    // --- Application Logic ---
    // Read the current state of the switch right after booting/waking up
    int currentState = digitalRead(REED_SWITCH_PIN);

    // Notify Home Assistant about the current state immediately
    if (currentState == LOW) {
      // Reed switch closed (magnet is near) -> door is closed
      Serial.println("Reporting: CLOSED via setClosed()");
      bool success = zbContactSensor.setClosed();
      Serial.printf("Reporting result: %s\n", success ? "SUCCESS" : "FAILED");
    } else {
      // Reed switch open (magnet is away) -> door is open
      Serial.println("Reporting: OPEN via setOpen()");
      bool success = zbContactSensor.setOpen();
      Serial.printf("Reporting result: %s\n", success ? "SUCCESS" : "FAILED");
    }

    // Blink LED to indicate we connected and sent state
    triggerLED(LED_BUILTIN_PIN);

    // Sadece Timer ile uyandığında veya cihaza ilk güç verildiğinde (UNDEFINED) bataryayı raporla
    if (global_wakeup_reason == ESP_SLEEP_WAKEUP_TIMER || global_wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
      Serial.println("Timer or First Boot Wakeup: Reporting Battery & Heartbeat");
      reportBattery();
    }

    // --- PAIRING INTERVIEW DELAY ---
    // If the device woke up from a normal boot (not deep sleep), it might be
    // pairing for the first time. We must stay awake for a short time so Home
    // Assistant can interview the endpoints properly. Otherwise, the device
    // sleeps before HA finishes.
    if (global_wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
      Serial.println("First boot detected. Staying awake for 15 seconds to "
                     "allow Coordinator interview...");
      uint32_t startWait = millis();
      // Wait 15 seconds, but keep feeding the Zigbee task implicitly via
      // delay()
      while (millis() - startWait < 15000) {
        delay(10);
      }
    } else {
      // Normal deep sleep wake up. Just delay 1000ms to ensure the IAS Zone
      // status packet is sent.
      delay(1000);
    }

  } else {
    Serial.println("Failed to connect to Zigbee Network.");
    delay(500); // Small delay before sleeping anyway
  }

  // --- Configure Deep Sleep ---
  
  // esp_timer_get_time() gives time in microseconds since boot. 
  // However, in deep sleep it resets? Actually, on ESP32-C6, we can use 
  // the RTC timer which is preserved.
  // A simpler and very reliable approach across deep sleep is simply:
  // "How long were we asleep just now?" -> esp_timer_get_time() is NOT preserved.
  // But esp_sleep_get_ext1_wakeup_status() doesn't give time.
  // Wait, `esp_sleep_get_time_since_wakeup()` exists? No.
  
  // Let's use gettimeofday() which is preserved across deep sleep on ESP32 if SNTP is not used
  // (it just counts up using the RTC timer).
  struct timeval now;
  gettimeofday(&now, NULL);
  
  if (global_wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
      // First boot
      rtc_time_until_report = now.tv_sec + TIME_TO_SLEEP;
  } else if (global_wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
      // Woke up from timer, calculate next report time
      rtc_time_until_report = now.tv_sec + TIME_TO_SLEEP;
  }
  
  // Calculate remaining time
  uint64_t remaining_sleep_us = 0;
  if (rtc_time_until_report > now.tv_sec) {
      remaining_sleep_us = (uint64_t)(rtc_time_until_report - now.tv_sec) * uS_TO_S_FACTOR;
  } else {
      // We somehow missed the time, schedule an immediate report on next sleep
      remaining_sleep_us = 1000000ULL; // 1 second
  }
  
  // Enable timer wakeup for the REMAINING time
  esp_sleep_enable_timer_wakeup(remaining_sleep_us);
  Serial.printf("Next battery report in %llu seconds\n", remaining_sleep_us / uS_TO_S_FACTOR);

  // Also keep RTC pins enabled during sleep
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  // Determine the current state of the pin so we can trigger on the OPPOSITE
  // edge
  int pinStateBeforeSleep = digitalRead(REED_SWITCH_PIN);
  uint64_t ext1_bitmask = (1ULL << REED_SWITCH_PIN);

  if (pinStateBeforeSleep == LOW) {
    // Door is currently closed. We want to wake up when it opens (Pin goes HIGH
    // / ANY_HIGH)
    esp_sleep_enable_ext1_wakeup(ext1_bitmask, ESP_EXT1_WAKEUP_ANY_HIGH);
    Serial.println(
        "Setting Wakeup Trigger to ANY_HIGH (Waiting for door to OPEN)");
  } else {
    // Door is currently open. We want to wake up when it closes (Pin goes LOW /
    // ANY_LOW)
    esp_sleep_enable_ext1_wakeup(ext1_bitmask, ESP_EXT1_WAKEUP_ANY_LOW);
    Serial.println(
        "Setting Wakeup Trigger to ANY_LOW (Waiting for door to CLOSE)");
  }

  // To prevent the pin from floating and causing spurious wakeups or draining
  // current when we sleep, we enable PIN Hold
  gpio_hold_en((gpio_num_t)REED_SWITCH_PIN);

  // Wait slightly to ensure Zigbee packets are actually transmitted before MCU
  // dies
  delay(500);

  Serial.println("Sleeping...");
  esp_deep_sleep_start();
}

void loop() {
  // We should never reach here because we go to deep sleep at the end of
  // setup() This approach is much more aggressive for battery savings as the
  // device immediately sleeps after sending the ONE update.
  delay(10);
}
