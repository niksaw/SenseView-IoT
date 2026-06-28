#include <Wire.h>
#include <math.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <Adafruit_TSL2591.h>
#include <SensirionI2cScd4x.h>
#include "Zigbee.h"

// =====================
// Pin Definitions
// =====================
#define SOIL_MOISTURE_PIN 2
#define BATTERY_ADC_PIN   3
#define I2C_SDA_PIN       5
#define I2C_SCL_PIN       6
#define LED_PIN           LED_BUILTIN
#define VALVE_GPIO_PIN    4

// =====================
// ADC / Battery
// =====================
#define ADC_MAX_VALUE     4095.0
#define ADC_REF_VOLT      3.3

// =====================
// Soil Calibration
// =====================
#define SOIL_AIR_VALUE    2300
#define SOIL_DRY_VALUE    2150
#define SOIL_WET_VALUE    1500

// =====================
// Valve Control
// =====================
#define LOW_THRESHOLD     30
#define HIGH_THRESHOLD    45
#define MAX_ON_TIME       (20UL * 60UL * 1000UL)

// =====================
// BME680 Sanity Limits
// =====================
#define BME_HUM_MAX       99.9f
#define BME_PRES_MIN      80000.0f
#define BME_GAS_MIN       1000.0f

// =====================
// BME680 Staleness Detection
// =====================
#define STALE_COUNT_MAX   15
#define STALE_DELTA       0.05f
#define BME_READ_INTERVAL 2000UL

// =====================
// State Variables
// =====================
bool valveState = false;
bool faultState = false;
bool bmeOK = false;
bool tslOK = false;
bool scdOK = false;

unsigned long valveOnStartTime = 0;

float lastTemp = -999.0f;
float lastHum  = -999.0f;
float lastPres = -999.0f;

uint8_t staleCount = 0;
unsigned long lastBmeRead = 0;

// =====================
// Sensors
// =====================
Adafruit_BME680   bme;
Adafruit_TSL2591  tsl = Adafruit_TSL2591(2591);
SensirionI2cScd4x scd4x;

// =====================
// Zigbee Endpoints
// =====================
ZigbeeLight               zbLED(1);
ZigbeeTempSensor          zbTempHum(2);
ZigbeePressureSensor      zbPres(3);
ZigbeeIlluminanceSensor   zbLux(4);
ZigbeeTempSensor          zbSoil(5);
ZigbeeCarbonDioxideSensor zbCO2(6);

// Custom battery endpoint only
ZigbeeAnalog              zbBattery(7);

tsl2591Gain_t tslGain = TSL2591_GAIN_MED;

// =====================
// LED Callback
// =====================
void setLED(bool state) {
  digitalWrite(LED_PIN, state);
}

// =====================
// TSL2591 Auto-range
// =====================
void tslAutoRange(uint16_t full) {

  if (full < 100 && tslGain != TSL2591_GAIN_MAX) {
    tslGain = (tsl2591Gain_t)(tslGain + 1);
    tsl.setGain(tslGain);
    delay(50);
  }

  if (full > 30000 && tslGain != TSL2591_GAIN_LOW) {
    tslGain = (tsl2591Gain_t)(tslGain - 1);
    tsl.setGain(tslGain);
    delay(50);
  }
}

// =====================
// BME680 Garbage Detector
// =====================
bool isGarbage() {

  if (bme.humidity >= BME_HUM_MAX)
    return true;

  if (bme.pressure < BME_PRES_MIN)
    return true;

  if (bme.gas_resistance < BME_GAS_MIN)
    return true;

  return false;
}

// =====================
// BME680 Staleness Detector
// =====================
bool isStale() {

  bool tempSame =
      fabsf(bme.temperature - lastTemp) < STALE_DELTA;

  bool humSame =
      fabsf(bme.humidity - lastHum) < STALE_DELTA;

  bool presSame =
      fabsf((bme.pressure / 100.0f) - lastPres) < STALE_DELTA;

  if (tempSame && humSame && presSame) {

    staleCount++;

  } else {

    staleCount = 0;

    lastTemp = bme.temperature;
    lastHum  = bme.humidity;
    lastPres = bme.pressure / 100.0f;
  }

  return (staleCount >= STALE_COUNT_MAX);
}

// =====================
// BME680 Init / Recovery
// =====================
bool initBME() {

  staleCount  = 0;
  lastTemp    = -999.0f;
  lastHum     = -999.0f;
  lastPres    = -999.0f;
  lastBmeRead = 0;

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  delay(100);

  if (!(bme.begin(0x76) || bme.begin(0x77))) {
    return false;
  }

  bme.setTemperatureOversampling(BME680_OS_4X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(250, 100);

  return true;
}

// =====================
// Setup
// =====================
void setup() {

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(VALVE_GPIO_PIN, OUTPUT);
  digitalWrite(VALVE_GPIO_PIN, LOW);

  pinMode(SOIL_MOISTURE_PIN, INPUT);
  pinMode(BATTERY_ADC_PIN, INPUT);

  // =====================
  // Zigbee Endpoints
  // =====================
  zbLED.onLightChange(setLED);
  Zigbee.addEndpoint(&zbLED);

  // Temperature + Humidity
  zbTempHum.addHumiditySensor(0, 100, 1);

  Zigbee.addEndpoint(&zbTempHum);

  // Pressure
  Zigbee.addEndpoint(&zbPres);

  // Lux
  Zigbee.addEndpoint(&zbLux);

  // Soil Moisture
  zbSoil.addHumiditySensor(0, 100, 1);
  Zigbee.addEndpoint(&zbSoil);

  // CO2
  Zigbee.addEndpoint(&zbCO2);

  // Battery endpoint
  zbBattery.addAnalogInput();
  Zigbee.addEndpoint(&zbBattery);

  Zigbee.setPrimaryChannelMask(0x00100000);

  // =====================
  // Start Zigbee
  // =====================
  if (!Zigbee.begin(ZIGBEE_END_DEVICE)) {
    while (1);
  }

  while (!Zigbee.connected()) {
    delay(300);
  }

  // =====================
  // Init Sensors
  // =====================
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  delay(100);

  // ---------- BME680 ----------
  bmeOK = initBME();

  // ---------- TSL2591 ----------
  if (tsl.begin()) {

    tsl.setGain(tslGain);
    tsl.setTiming(TSL2591_INTEGRATIONTIME_200MS);

    delay(300);

    tslOK = true;

    // Force Lux entity creation
    zbLux.setIlluminance(0);
    zbLux.report();
  }

  // ---------- SCD4x ----------
  scd4x.begin(Wire, 0x62);

  scd4x.stopPeriodicMeasurement();

  delay(500);

  if (scd4x.startPeriodicMeasurement() == 0) {
    scdOK = true;
  }
}

// =====================
// Main Loop
// =====================
void loop() {

  delay(10);

  // =====================
  // BME680
  // =====================
  if (bmeOK) {

    unsigned long now = millis();

    if (now - lastBmeRead >= BME_READ_INTERVAL) {

      lastBmeRead = now;

      if (!bme.performReading() ||
          isGarbage() ||
          isStale()) {

        bmeOK = initBME();

      } else {

        // Temperature + Humidity
        zbTempHum.setTemperature(bme.temperature);
        zbTempHum.setHumidity(bme.humidity);
        zbTempHum.report();

        // Pressure in hPa
        zbPres.setPressure(bme.pressure / 100.0f);
        zbPres.report();
      }
    }

  } else {

    static unsigned long lastRetry = 0;

    unsigned long now = millis();

    if (now - lastRetry >= 5000UL) {

      lastRetry = now;

      bmeOK = initBME();
    }
  }

  // =====================
  // Soil Moisture
  // =====================
  uint16_t soilRaw =
      analogRead(SOIL_MOISTURE_PIN);

  float soilPercent;

  if (soilRaw >= SOIL_AIR_VALUE) {

    soilPercent = 0.0f;

  } else {

    soilPercent =
        map(
            soilRaw,
            SOIL_DRY_VALUE,
            SOIL_WET_VALUE,
            0,
            100);

    soilPercent =
        constrain(soilPercent, 0, 100);
  }

  // Dummy temperature to avoid NaN
  zbSoil.setTemperature(0);

  // Soil moisture as humidity
  zbSoil.setHumidity(soilPercent);

  zbSoil.report();

  // =====================
  // TSL2591 Lux
  // =====================
  if (tslOK) {

    uint32_t lum =
        tsl.getFullLuminosity();

    uint16_t ir =
        lum >> 16;

    uint16_t full =
        lum & 0xFFFF;

    tslAutoRange(full);

    float lux =
        tsl.calculateLux(full, ir);

    if (isnan(lux) || lux < 0) {
      lux = 0;
    }

    // Convert lux to Zigbee illuminance format
    uint16_t zigbeeLux;

    if (lux <= 0.1f) {
      zigbeeLux = 0;
    } else {
      zigbeeLux =
          (uint16_t)(10000.0f * log10f(lux) + 1);
    }

    zbLux.setIlluminance(zigbeeLux);
    zbLux.report();
  }

// =====================
// Battery
// =====================
uint16_t adcRaw =
    analogRead(BATTERY_ADC_PIN);

// Calibrated voltage calculation
float battVoltage =
    ((float)adcRaw / ADC_MAX_VALUE) *
    ADC_REF_VOLT *
    1.262f *   // ADC calibration factor
    2.0f;      // 100k/100k divider

// Linear Li-ion estimation
float battPercent =
    (battVoltage - 3.0f) /
    (4.2f - 3.0f) *
    100.0f;

battPercent =
    constrain(
        battPercent,
        0.0f,
        100.0f);

zbBattery.setAnalogInput(battPercent);
zbBattery.reportAnalogInput();

  // =====================
  // CO2
  // =====================
  if (scdOK) {

    bool dataReady = false;

    scd4x.getDataReadyStatus(dataReady);

    if (dataReady) {

      uint16_t co2Raw = 0;
      float scdTemp   = 0.0f;
      float scdHum    = 0.0f;

      if (scd4x.readMeasurement(
              co2Raw,
              scdTemp,
              scdHum) == 0 &&
          co2Raw != 0) {

        zbCO2.setCarbonDioxide((float)co2Raw);
        zbCO2.report();
      }
    }
  }

  // =====================
  // Valve Logic
  // =====================
  if (!faultState) {

    if (!valveState &&
        soilPercent < LOW_THRESHOLD) {

      valveState = true;

      valveOnStartTime = millis();

      digitalWrite(VALVE_GPIO_PIN, HIGH);
    }

    else if (valveState &&
             soilPercent > HIGH_THRESHOLD) {

      valveState = false;

      digitalWrite(VALVE_GPIO_PIN, LOW);
    }

    if (valveState &&
        (millis() - valveOnStartTime >
         MAX_ON_TIME)) {

      valveState = false;
      faultState = true;

      digitalWrite(VALVE_GPIO_PIN, LOW);
    }
  }

  delay(1000);
}