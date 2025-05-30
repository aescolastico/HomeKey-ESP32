#include <cstdint>
#include <memory>
#define JSON_NOEXCEPTION 1
#include <sodium/crypto_sign.h>
#include <sodium/crypto_box.h>
#include "HAP.h"
#include "hkAuthContext.h"
#include "HomeKey.h"
#include "array"
#include "logging.h"
#include "HomeSpan.h"
#include "PN532_SPI.h"
#include "PN532.h"
#include "chrono"
#include "ESPAsyncWebServer.h"
#include "LittleFS.h"
#include "HK_HomeKit.h"
#include "config.h"
#include "esp_app_desc.h"
#include "pins_arduino.h"
#include "NFC_SERV_CHARS.h"
#include <mbedtls/sha256.h>
#include <esp_mac.h>
#include "mqtt_stub.h"

const char* TAG = "MAIN";

AsyncWebServer webServer(80);
PN532_SPI *pn532spi;
PN532 *nfc;
QueueHandle_t gpio_led_handle = nullptr;
QueueHandle_t neopixel_handle = nullptr;
QueueHandle_t gpio_lock_handle = nullptr;
TaskHandle_t gpio_led_task_handle = nullptr;
TaskHandle_t neopixel_task_handle = nullptr;
TaskHandle_t gpio_lock_task_handle = nullptr;
TaskHandle_t alt_action_task_handle = nullptr;
TaskHandle_t nfc_reconnect_task = nullptr;
TaskHandle_t nfc_poll_task = nullptr;

nvs_handle savedData;
readerData_t readerData;
uint8_t ecpData[18] = { 0x6A, 0x2, 0xCB, 0x2, 0x6, 0x2, 0x11, 0x0 };
const std::array<std::array<uint8_t, 6>, 4> hk_color_vals = { {{0x01,0x04,0xce,0xd5,0xda,0x00}, {0x01,0x04,0xaa,0xd6,0xec,0x00}, {0x01,0x04,0xe3,0xe3,0xe3,0x00}, {0x01,0x04,0x00,0x00,0x00,0x00}} };
const std::array<const char*, 6> pixelTypeMap = { "RGB", "RBG", "BRG", "BGR", "GBR", "GRB" };
struct gpioLockAction
{
  enum
  {
    HOMEKIT = 1,
    HOMEKEY = 2,
    OTHER = 3
  };
  uint8_t source;
  uint8_t action;
};

std::string platform_create_id_string(void) {
  uint8_t mac[6];
  char id_string[13];
  esp_read_mac(mac, ESP_MAC_BT);
  sprintf(id_string, "ESP32_%02x%02X%02X", mac[3], mac[4], mac[5]);
  return std::string(id_string);
}

struct eth_chip_desc_t {
  std::string name;
  bool emac;
  eth_phy_type_t phy_type;
  NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(eth_chip_desc_t, name, emac, phy_type)
};

struct eth_board_presets_t {
  std::string name;
  eth_chip_desc_t ethChip;
  #if CONFIG_ETH_USE_ESP32_EMAC
  struct rmii_conf_t {
    int32_t phy_addr = 1;
    uint8_t pin_mcd = 23;
    uint8_t pin_mdio = 18;
    int8_t pin_power = -1;
    eth_clock_mode_t pin_rmii_clock = ETH_CLOCK_GPIO0_IN;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(rmii_conf_t, phy_addr, pin_mcd, pin_mdio, pin_power, pin_rmii_clock)
  } rmii_conf;
  #endif
  struct spi_conf_t {
    uint8_t spi_freq_mhz = 20;
    uint8_t pin_cs = SS;
    uint8_t pin_irq = A4;
    uint8_t pin_rst = A5;
    uint8_t pin_sck = SCK;
    uint8_t pin_miso = MISO;
    uint8_t pin_mosi = MOSI;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(spi_conf_t, spi_freq_mhz, pin_cs, pin_irq, pin_rst, pin_sck, pin_miso, pin_mosi)
  } spi_conf;
  friend void to_json(nlohmann ::json &nlohmann_json_j, const eth_board_presets_t &nlohmann_json_t) {
    nlohmann_json_j["name"] = nlohmann_json_t.name;
    nlohmann_json_j["ethChip"] = nlohmann_json_t.ethChip;
    if (nlohmann_json_t.ethChip.emac) {
    #if CONFIG_ETH_USE_ESP32_EMAC
      nlohmann_json_j["rmii_conf"] = nlohmann_json_t.rmii_conf;
    #endif
    } else {
      nlohmann_json_j["spi_conf"] = nlohmann_json_t.spi_conf;
    }
  }
};

namespace eth_config_ns {
  std::map<eth_phy_type_t, eth_chip_desc_t> supportedChips = {
  #if CONFIG_ETH_USE_ESP32_EMAC
      {ETH_PHY_LAN8720, eth_chip_desc_t{"LAN8720", true, ETH_PHY_LAN8720}},
      {ETH_PHY_TLK110, eth_chip_desc_t{"TLK110", true, ETH_PHY_TLK110}},
      {ETH_PHY_RTL8201, eth_chip_desc_t{"RTL8201", true, ETH_PHY_RTL8201}},
      {ETH_PHY_DP83848, eth_chip_desc_t{"DP83848", true, ETH_PHY_DP83848}},
      {ETH_PHY_KSZ8041, eth_chip_desc_t{"KSZ8041", true, ETH_PHY_KSZ8041}},
      {ETH_PHY_KSZ8081, eth_chip_desc_t{"KSZ8081", true, ETH_PHY_KSZ8081}},
  #endif
  #if CONFIG_ETH_SPI_ETHERNET_DM9051
      {ETH_PHY_DM9051, eth_chip_desc_t{"DM9051", false, ETH_PHY_DM9051}},
  #endif
  #if CONFIG_ETH_SPI_ETHERNET_W5500
      {ETH_PHY_W5500, eth_chip_desc_t{"W5500", false, ETH_PHY_W5500}},
  #endif
  #if CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL
      {ETH_PHY_KSZ8851, eth_chip_desc_t{"KSZ8851", false, ETH_PHY_KSZ8851}},
  #endif
  };
  std::vector<eth_board_presets_t> boardPresets = {
      eth_board_presets_t{.name = "Generic W5500",
                          .ethChip = supportedChips[ETH_PHY_W5500],
                          .spi_conf{20, SS, A3, A4, SCK, MISO, MOSI}},
      eth_board_presets_t{.name = "T-ETH-Lite-ESP32S3",
                          .ethChip = supportedChips[ETH_PHY_W5500],
                          .spi_conf{20, 9, 13, 14, 10, 11, 12}},
  #if CONFIG_ETH_USE_ESP32_EMAC
      eth_board_presets_t{.name = "WT32-ETH01",
                          .ethChip = supportedChips[ETH_PHY_LAN8720],
                          .rmii_conf{1, 23, 18, 16, ETH_CLOCK_GPIO0_IN}},
      eth_board_presets_t{.name = "Olimex ESP32-POE",
                          .ethChip = supportedChips[ETH_PHY_LAN8720],
                          .rmii_conf{0, 23, 18, 12, ETH_CLOCK_GPIO17_OUT}},
      eth_board_presets_t{.name = "EST-PoE-32",
                          .ethChip = supportedChips[ETH_PHY_LAN8720],
                          .rmii_conf{0, 23, 18, 12, ETH_CLOCK_GPIO17_OUT}},
      eth_board_presets_t{.name = "T-ETH-Lite-ESP32",
                          .ethChip = supportedChips[ETH_PHY_RTL8201],
                          .rmii_conf{0, 23, 18, 12, ETH_CLOCK_GPIO0_IN}}
  #endif
  };
};

namespace espConfig
{
  struct misc_config_t
  {
    enum colorMap
    {
      R,
      G,
      B
    };
    std::string deviceName = DEVICE_NAME;
    std::string otaPasswd = OTA_PWD;
    uint8_t hk_key_color = HOMEKEY_COLOR;
    std::string setupCode = SETUP_CODE;
    bool lockAlwaysUnlock = HOMEKEY_ALWAYS_UNLOCK;
    bool lockAlwaysLock = HOMEKEY_ALWAYS_LOCK;
    uint8_t controlPin = HS_PIN;
    uint8_t hsStatusPin = HS_STATUS_LED;
    uint8_t nfcNeopixelPin = NFC_NEOPIXEL_PIN;
    uint8_t neoPixelType = 5;
    std::map<colorMap, int> neopixelSuccessColor = { {R, NEOPIXEL_SUCCESS_R}, {G, NEOPIXEL_SUCCESS_G}, {B, NEOPIXEL_SUCCESS_B} };
    std::map<colorMap, int> neopixelFailureColor = { {R, NEOPIXEL_FAIL_R}, {G, NEOPIXEL_FAIL_G}, {B, NEOPIXEL_FAIL_B} };
    uint16_t neopixelSuccessTime = NEOPIXEL_SUCCESS_TIME;
    uint16_t neopixelFailTime = NEOPIXEL_FAIL_TIME;
    uint8_t nfcSuccessPin = NFC_SUCCESS_PIN;
    uint16_t nfcSuccessTime = NFC_SUCCESS_TIME;
    bool nfcSuccessHL = NFC_SUCCESS_HL;
    uint8_t nfcFailPin = NFC_FAIL_PIN;
    uint16_t nfcFailTime = NFC_FAIL_TIME;
    bool nfcFailHL = NFC_FAIL_HL;
    uint8_t gpioActionPin = GPIO_ACTION_PIN;
    bool gpioActionLockState = GPIO_ACTION_LOCK_STATE;
    bool gpioActionUnlockState = GPIO_ACTION_UNLOCK_STATE;
    uint8_t gpioActionMomentaryEnabled = GPIO_ACTION_MOMENTARY_STATE;
    bool hkGpioControlledState = true;
    uint16_t gpioActionMomentaryTimeout = GPIO_ACTION_MOMENTARY_TIMEOUT;
    bool webAuthEnabled = WEB_AUTH_ENABLED;
    std::string webUsername = WEB_AUTH_USERNAME;
    std::string webPassword = WEB_AUTH_PASSWORD;
    std::array<uint8_t, 4> nfcGpioPins{SS, SCK, MISO, MOSI};
    uint8_t btrLowStatusThreshold = 10;
    bool proxBatEnabled = false;
    bool hkDumbSwitchMode = false;
    uint8_t hkAltActionInitPin = GPIO_HK_ALT_ACTION_INIT_PIN;
    uint8_t hkAltActionInitLedPin = GPIO_HK_ALT_ACTION_INIT_LED_PIN;
    uint16_t hkAltActionInitTimeout = GPIO_HK_ALT_ACTION_INIT_TIMEOUT;
    uint8_t hkAltActionPin = GPIO_HK_ALT_ACTION_PIN;
    uint16_t hkAltActionTimeout = GPIO_HK_ALT_ACTION_TIMEOUT;
    uint8_t hkAltActionGpioState = GPIO_HK_ALT_ACTION_GPIO_STATE;
    bool ethernetEnabled = false;
    uint8_t ethActivePreset = 255; // 255 for custom pins
    uint8_t ethPhyType = 0;
    #if CONFIG_ETH_USE_ESP32_EMAC
    std::array<int8_t, 5> ethRmiiConfig = {0, -1, -1, -1, 0};
    #endif
    std::array<int8_t, 7> ethSpiConfig = {20, -1, -1, -1, -1, -1, -1};
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        misc_config_t, deviceName, otaPasswd, hk_key_color, setupCode,
        lockAlwaysUnlock, lockAlwaysLock, controlPin, hsStatusPin,
        nfcSuccessPin, nfcSuccessTime, nfcNeopixelPin, neoPixelType,
        neopixelSuccessColor, neopixelFailureColor, neopixelSuccessTime,
        neopixelFailTime, nfcSuccessHL, nfcFailPin, nfcFailTime, nfcFailHL,
        gpioActionPin, gpioActionLockState, gpioActionUnlockState,
        gpioActionMomentaryEnabled, gpioActionMomentaryTimeout, webAuthEnabled,
        webUsername, webPassword, nfcGpioPins, btrLowStatusThreshold,
        proxBatEnabled, hkDumbSwitchMode, hkAltActionInitPin,
        hkAltActionInitLedPin, hkAltActionInitTimeout, hkAltActionPin,
        hkAltActionTimeout, hkAltActionGpioState, hkGpioControlledState,
        ethernetEnabled, ethActivePreset, ethPhyType,
#if CONFIG_ETH_USE_ESP32_EMAC
        ethRmiiConfig,
#endif
        ethSpiConfig
    )
  } miscConfig;
}; // namespace espConfig

KeyFlow hkFlow = KeyFlow::kFlowFAST;
bool hkAltActionActive = false;
SpanCharacteristic* lockCurrentState;
SpanCharacteristic* lockTargetState;
SpanCharacteristic* statusLowBtr;
SpanCharacteristic* btrLevel;

std::shared_ptr<Pixel> pixel;

bool save_to_nvs() {
  std::vector<uint8_t> serialized = nlohmann::json::to_msgpack(readerData);
  esp_err_t set_nvs = nvs_set_blob(savedData, "READERDATA", serialized.data(), serialized.size());
  esp_err_t commit_nvs = nvs_commit(savedData);
  LOG(D, "NVS SET STATUS: %s", esp_err_to_name(set_nvs));
  LOG(D, "NVS COMMIT STATUS: %s", esp_err_to_name(commit_nvs));
  return !set_nvs && !commit_nvs;
}

struct PhysicalLockBattery : Service::BatteryService
{
  PhysicalLockBattery() {
    LOG(I, "Configuring PhysicalLockBattery");
    statusLowBtr = new Characteristic::StatusLowBattery(0, true);
    btrLevel = new Characteristic::BatteryLevel(100, true);
  }
};

struct LockManagement : Service::LockManagement
{
  SpanCharacteristic* lockControlPoint;
  SpanCharacteristic* version;
  const char* TAG = "LockManagement";

  LockManagement() : Service::LockManagement() {

    LOG(I, "Configuring LockManagement"); // initialization message

    lockControlPoint = new Characteristic::LockControlPoint();
    version = new Characteristic::Version();

  } // end constructor

}; // end LockManagement

struct NFCAccessoryInformation : Service::AccessoryInformation
{
  const char* TAG = "NFCAccessoryInformation";

  NFCAccessoryInformation() : Service::AccessoryInformation() {

    LOG(I, "Configuring NFCAccessoryInformation"); // initialization message

    opt.push_back(&_CUSTOM_HardwareFinish);
    new Characteristic::Identify();
    new Characteristic::Manufacturer("rednblkx");
    new Characteristic::Model("HomeKey-ESP32");
    new Characteristic::Name(DEVICE_NAME);
    const esp_app_desc_t* app_desc = esp_app_get_description();
    std::string app_version = app_desc->version;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char macStr[9] = { 0 };
    sprintf(macStr, "%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3]);
    std::string serialNumber = "HK-";
    serialNumber.append(macStr);
    new Characteristic::SerialNumber(serialNumber.c_str());
    new Characteristic::FirmwareRevision(app_version.c_str());
    std::array<uint8_t, 6> decB64 = hk_color_vals[HK_COLOR(espConfig::miscConfig.hk_key_color)];
    TLV8 hwfinish(NULL, 0);
    hwfinish.unpack(decB64.data(), decB64.size());
    new Characteristic::HardwareFinish(hwfinish);

  } // end constructor
};

// Function to calculate CRC16
void crc16a(unsigned char* data, unsigned int size, unsigned char* result) {
  unsigned short w_crc = 0x6363;

  for (unsigned int i = 0; i < size; ++i) {
    unsigned char byte = data[i];
    byte = (byte ^ (w_crc & 0x00FF));
    byte = ((byte ^ (byte << 4)) & 0xFF);
    w_crc = ((w_crc >> 8) ^ (byte << 8) ^ (byte << 3) ^ (byte >> 4)) & 0xFFFF;
  }

  result[0] = static_cast<unsigned char>(w_crc & 0xFF);
  result[1] = static_cast<unsigned char>((w_crc >> 8) & 0xFF);
}

// Function to append CRC16 to data
void with_crc16(unsigned char* data, unsigned int size, unsigned char* result) {
  crc16a(data, size, result);
}

void alt_action_task(void* arg) {
  uint8_t buttonState = 0;
  hkAltActionActive = false;
  LOG(I, "Starting Alt Action button task");
  while (true)
  {
    buttonState = digitalRead(espConfig::miscConfig.hkAltActionInitPin);
    if (buttonState == HIGH) {
      LOG(D, "BUTTON HIGH");
      hkAltActionActive = true;
      if(espConfig::miscConfig.hkAltActionInitLedPin != 255) {
        digitalWrite(espConfig::miscConfig.hkAltActionInitLedPin, HIGH);
      }
      vTaskDelay(espConfig::miscConfig.hkAltActionInitTimeout / portTICK_PERIOD_MS);
      if (espConfig::miscConfig.hkAltActionInitLedPin != 255) {
        digitalWrite(espConfig::miscConfig.hkAltActionInitLedPin, LOW);
      }
      LOG(D, "TIMEOUT");
      hkAltActionActive = false;
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}

void gpio_task(void* arg) {
  gpioLockAction status;
  while (1) {
    if (gpio_lock_handle != nullptr) {
      status = {};
      if (uxQueueMessagesWaiting(gpio_lock_handle) > 0) {
        xQueueReceive(gpio_lock_handle, &status, 0);
        LOG(D, "Got something in queue - source = %d action = %d", status.source, status.action);
        if (status.action == 0) {
          LOG(D, "%d - %d - %d -%d", espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionMomentaryEnabled, espConfig::miscConfig.lockAlwaysUnlock, espConfig::miscConfig.lockAlwaysLock);
          if (espConfig::miscConfig.lockAlwaysUnlock && status.source != gpioLockAction::HOMEKIT) {
            lockTargetState->setVal(lockStates::UNLOCKED);
            if(espConfig::miscConfig.gpioActionPin != 255){
              digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionUnlockState);
            }
            lockCurrentState->setVal(lockStates::UNLOCKED);

            if (static_cast<uint8_t>(espConfig::miscConfig.gpioActionMomentaryEnabled) & status.source) {
              delay(espConfig::miscConfig.gpioActionMomentaryTimeout);
              lockTargetState->setVal(lockStates::LOCKED);
              if(espConfig::miscConfig.gpioActionPin != 255){
                digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionLockState);
              }
              lockCurrentState->setVal(lockStates::LOCKED);
            }
          } else if (espConfig::miscConfig.lockAlwaysLock && status.source != gpioLockAction::HOMEKIT) {
            lockTargetState->setVal(lockStates::LOCKED);
            if(espConfig::miscConfig.gpioActionPin != 255){
              digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionLockState);
            }
            lockCurrentState->setVal(lockStates::LOCKED);
          } else {
            int currentState = lockCurrentState->getVal();
            if (status.source != gpioLockAction::HOMEKIT) {
              lockTargetState->setVal(!currentState);
            }
            if(espConfig::miscConfig.gpioActionPin != 255){
              digitalWrite(espConfig::miscConfig.gpioActionPin, currentState == lockStates::UNLOCKED ? espConfig::miscConfig.gpioActionLockState : espConfig::miscConfig.gpioActionUnlockState);
            }
            lockCurrentState->setVal(!currentState);
            if ((static_cast<uint8_t>(espConfig::miscConfig.gpioActionMomentaryEnabled) & status.source) && currentState == lockStates::LOCKED) {
              delay(espConfig::miscConfig.gpioActionMomentaryTimeout);
              lockTargetState->setVal(currentState);
              if(espConfig::miscConfig.gpioActionPin != 255){
                digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionLockState);
              }
              lockCurrentState->setVal(currentState);
            }
          }
        } else if (status.action == 2) {
          vTaskDelete(NULL);
          return;
        }
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void neopixel_task(void* arg) {
  uint8_t status = 0;
  while (1) {
    if (neopixel_handle != nullptr) {
      status = 0;
      if (uxQueueMessagesWaiting(neopixel_handle) > 0) {
        xQueueReceive(neopixel_handle, &status, 0);
        LOG(D, "Got something in queue %d", status);
        switch (status) {
        case 0:
          if (espConfig::miscConfig.nfcNeopixelPin && espConfig::miscConfig.nfcNeopixelPin != 255) {
            LOG(D, "SUCCESS PIXEL %d:%d,%d,%d", espConfig::miscConfig.nfcNeopixelPin, espConfig::miscConfig.neopixelFailureColor[espConfig::misc_config_t::colorMap::R], espConfig::miscConfig.neopixelFailureColor[espConfig::misc_config_t::colorMap::G], espConfig::miscConfig.neopixelFailureColor[espConfig::misc_config_t::colorMap::B]);
            pixel->set(pixel->RGB(espConfig::miscConfig.neopixelFailureColor[espConfig::misc_config_t::colorMap::R], espConfig::miscConfig.neopixelFailureColor[espConfig::misc_config_t::colorMap::G], espConfig::miscConfig.neopixelFailureColor[espConfig::misc_config_t::colorMap::B]));
            delay(espConfig::miscConfig.neopixelFailTime);
            pixel->off();
          }
          break;
        case 1:
          if (espConfig::miscConfig.nfcNeopixelPin && espConfig::miscConfig.nfcNeopixelPin != 255) {
            LOG(D, "FAIL PIXEL %d:%d,%d,%d", espConfig::miscConfig.nfcNeopixelPin, espConfig::miscConfig.neopixelSuccessColor[espConfig::misc_config_t::colorMap::R], espConfig::miscConfig.neopixelSuccessColor[espConfig::misc_config_t::colorMap::G], espConfig::miscConfig.neopixelSuccessColor[espConfig::misc_config_t::colorMap::B]);
            pixel->set(pixel->RGB(espConfig::miscConfig.neopixelSuccessColor[espConfig::misc_config_t::colorMap::R], espConfig::miscConfig.neopixelSuccessColor[espConfig::misc_config_t::colorMap::G], espConfig::miscConfig.neopixelSuccessColor[espConfig::misc_config_t::colorMap::B]));
            delay(espConfig::miscConfig.neopixelSuccessTime);
            pixel->off();
          }
          break;
        default:
          vTaskDelete(NULL);
          return;
          break;
        }
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}
void nfc_gpio_task(void* arg) {
  uint8_t status = 0;
  while (1) {
    if (gpio_led_handle != nullptr) {
      status = 0;
      if (uxQueueMessagesWaiting(gpio_led_handle) > 0) {
        xQueueReceive(gpio_led_handle, &status, 0);
        LOG(D, "Got something in queue %d", status);
        switch (status) {
        case 0:
          if (espConfig::miscConfig.nfcFailPin && espConfig::miscConfig.nfcFailPin != 255) {
            LOG(D, "FAIL LED %d:%d", espConfig::miscConfig.nfcFailPin, espConfig::miscConfig.nfcFailHL);
            digitalWrite(espConfig::miscConfig.nfcFailPin, espConfig::miscConfig.nfcFailHL);
            delay(espConfig::miscConfig.nfcFailTime);
            digitalWrite(espConfig::miscConfig.nfcFailPin, !espConfig::miscConfig.nfcFailHL);
          }
          break;
        case 1:
          if (espConfig::miscConfig.nfcSuccessPin && espConfig::miscConfig.nfcSuccessPin != 255) {
            LOG(D, "SUCCESS LED %d:%d", espConfig::miscConfig.nfcSuccessPin, espConfig::miscConfig.nfcSuccessHL);
            digitalWrite(espConfig::miscConfig.nfcSuccessPin, espConfig::miscConfig.nfcSuccessHL);
            delay(espConfig::miscConfig.nfcSuccessTime);
            digitalWrite(espConfig::miscConfig.nfcSuccessPin, !espConfig::miscConfig.nfcSuccessHL);
          }
          break;
        case 2:
          if(hkAltActionActive){
            digitalWrite(espConfig::miscConfig.hkAltActionPin, espConfig::miscConfig.hkAltActionGpioState);
            delay(espConfig::miscConfig.hkAltActionTimeout);
            digitalWrite(espConfig::miscConfig.hkAltActionPin, !espConfig::miscConfig.hkAltActionGpioState);
          }
          break;
        default:
          LOG(I, "STOP");
          vTaskDelete(NULL);
          return;
          break;
        }
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

struct LockMechanism : Service::LockMechanism
{
  const char* TAG = "LockMechanism";

  LockMechanism() : Service::LockMechanism() {
    LOG(I, "Configuring LockMechanism");
    lockCurrentState = new Characteristic::LockCurrentState(1, true);
    lockTargetState = new Characteristic::LockTargetState(1, true);
    memcpy(ecpData + 8, readerData.reader_gid.data(), readerData.reader_gid.size());
    with_crc16(ecpData, 16, ecpData + 16);
    if (espConfig::miscConfig.gpioActionPin != 255) {
      if (lockCurrentState->getVal() == lockStates::LOCKED) {
        digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionLockState);
      } else if (lockCurrentState->getVal() == lockStates::UNLOCKED) {
        digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionUnlockState);
      }
    }
  }

  boolean update() {
    int targetState = lockTargetState->getNewVal();
    LOG(I, "New LockState=%d, Current LockState=%d", targetState, lockCurrentState->getVal());
    if (espConfig::miscConfig.gpioActionPin != 255) {
      const gpioLockAction gpioAction{ .source = gpioLockAction::HOMEKIT, .action = 0 };
      xQueueSend(gpio_lock_handle, &gpioAction, 0);
    } else if (espConfig::miscConfig.hkDumbSwitchMode) {
      const gpioLockAction gpioAction{ .source = gpioLockAction::HOMEKIT, .action = 0 };
      xQueueSend(gpio_lock_handle, &gpioAction, 0);
    }
    return (true);
  }
};

struct NFCAccess : Service::NFCAccess
{
  SpanCharacteristic* configurationState;
  SpanCharacteristic* nfcControlPoint;
  SpanCharacteristic* nfcSupportedConfiguration;
  const char* TAG = "NFCAccess";

  NFCAccess() : Service::NFCAccess() {
    LOG(I, "Configuring NFCAccess");
    configurationState = new Characteristic::ConfigurationState();
    nfcControlPoint = new Characteristic::NFCAccessControlPoint();
    TLV8 conf(NULL, 0);
    conf.add(0x01, 0x10);
    conf.add(0x02, 0x10);
    nfcSupportedConfiguration = new Characteristic::NFCAccessSupportedConfiguration(conf);
  }

  boolean update() {
    LOG(D, "PROVISIONED READER KEY: %s", red_log::bufToHexString(readerData.reader_pk.data(), readerData.reader_pk.size()).c_str());
    LOG(D, "READER GROUP IDENTIFIER: %s", red_log::bufToHexString(readerData.reader_gid.data(), readerData.reader_gid.size()).c_str());
    LOG(D, "READER UNIQUE IDENTIFIER: %s", red_log::bufToHexString(readerData.reader_id.data(), readerData.reader_id.size()).c_str());

    TLV8 ctrlData(NULL, 0);
    nfcControlPoint->getNewTLV(ctrlData);
    std::vector<uint8_t> tlvData(ctrlData.pack_size());
    ctrlData.pack(tlvData.data());
    if (tlvData.size() == 0)
      return false;
    LOG(D, "Decoded data: %s", red_log::bufToHexString(tlvData.data(), tlvData.size()).c_str());
    LOG(D, "Decoded data length: %d", tlvData.size());
    HK_HomeKit hkCtx(readerData, savedData, "READERDATA", tlvData);
    std::vector<uint8_t> result = hkCtx.processResult();
    if (readerData.reader_gid.size() > 0) {
      memcpy(ecpData + 8, readerData.reader_gid.data(), readerData.reader_gid.size());
      with_crc16(ecpData, 16, ecpData + 16);
    }
    TLV8 res(NULL, 0);
    res.unpack(result.data(), result.size());
    nfcControlPoint->setTLV(res, false);
    return true;
  }

};

void deleteReaderData(const char* buf = "") {
  esp_err_t erase_nvs = nvs_erase_key(savedData, "READERDATA");
  esp_err_t commit_nvs = nvs_commit(savedData);
  readerData.issuers.clear();
  readerData.reader_gid.clear();
  readerData.reader_id.clear();
  readerData.reader_pk.clear();
  readerData.reader_pk_x.clear();
  readerData.reader_sk.clear();
  LOG(D, "*** NVS W STATUS");
  LOG(D, "ERASE: %s", esp_err_to_name(erase_nvs));
  LOG(D, "COMMIT: %s", esp_err_to_name(commit_nvs));
  LOG(D, "*** NVS W STATUS");
}

std::vector<uint8_t> getHashIdentifier(const uint8_t* key, size_t len) {
  const char* TAG = "getHashIdentifier";
  LOG(V, "Key: %s, Length: %d", red_log::bufToHexString(key, len).c_str(), len);
  std::vector<unsigned char> hashable;
  std::string string = "key-identifier";
  hashable.insert(hashable.begin(), string.begin(), string.end());
  hashable.insert(hashable.end(), key, key + len);
  LOG(V, "Hashable: %s", red_log::bufToHexString(&hashable.front(), hashable.size()).c_str());
  uint8_t hash[32];
  mbedtls_sha256(&hashable.front(), hashable.size(), hash, 0);
  LOG(V, "HashIdentifier: %s", red_log::bufToHexString(hash, 8).c_str());
  return std::vector<uint8_t>{hash, hash + 8};
}

void pairCallback() {
  if (HAPClient::nAdminControllers() == 0) {
    deleteReaderData(NULL);
    return;
  }
  for (auto it = homeSpan.controllerListBegin(); it != homeSpan.controllerListEnd(); ++it) {
    std::vector<uint8_t> id = getHashIdentifier(it->getLTPK(), 32);
    LOG(D, "Found allocated controller - Hash: %s", red_log::bufToHexString(id.data(), 8).c_str());
    hkIssuer_t* foundIssuer = nullptr;
    for (auto&& issuer : readerData.issuers) {
      if (std::equal(issuer.issuer_id.begin(), issuer.issuer_id.end(), id.begin())) {
        LOG(D, "Issuer %s already added, skipping", red_log::bufToHexString(issuer.issuer_id.data(), issuer.issuer_id.size()).c_str());
        foundIssuer = &issuer;
        break;
      }
    }
    if (foundIssuer == nullptr) {
      LOG(D, "Adding new issuer - ID: %s", red_log::bufToHexString(id.data(), 8).c_str());
      hkIssuer_t newIssuer;
      newIssuer.issuer_id = std::vector<uint8_t>{ id.begin(), id.begin() + 8 };
      newIssuer.issuer_pk.insert(newIssuer.issuer_pk.begin(), it->getLTPK(), it->getLTPK() + 32);
      readerData.issuers.emplace_back(newIssuer);
    }
  }
  save_to_nvs();
}

void setFlow(const char* buf) {
  switch (buf[1]) {
  case '0':
    hkFlow = KeyFlow::kFlowFAST;
    LOG(I, "FAST Flow");
    break;

  case '1':
    hkFlow = KeyFlow::kFlowSTANDARD;
    LOG(I, "STANDARD Flow");
    break;
  case '2':
    hkFlow = KeyFlow::kFlowATTESTATION;
    LOG(I, "ATTESTATION Flow");
    break;

  default:
    LOG(I, "0 = FAST flow, 1 = STANDARD Flow, 2 = ATTESTATION Flow");
    break;
  }
}

void setLogLevel(const char* buf) {
  esp_log_level_t level = esp_log_level_get("*");
  if (strncmp(buf + 1, "E", 1) == 0) {
    level = ESP_LOG_ERROR;
    LOG(I, "ERROR");
  } else if (strncmp(buf + 1, "W", 1) == 0) {
    level = ESP_LOG_WARN;
    LOG(I, "WARNING");
  } else if (strncmp(buf + 1, "I", 1) == 0) {
    level = ESP_LOG_INFO;
    LOG(I, "INFO");
  } else if (strncmp(buf + 1, "D", 1) == 0) {
    level = ESP_LOG_DEBUG;
    LOG(I, "DEBUG");
  } else if (strncmp(buf + 1, "V", 1) == 0) {
    level = ESP_LOG_VERBOSE;
    LOG(I, "VERBOSE");
  } else if (strncmp(buf + 1, "N", 1) == 0) {
    level = ESP_LOG_NONE;
    LOG(I, "NONE");
  }

  esp_log_level_set(TAG, level);
  esp_log_level_set("HK_HomeKit", level);
  esp_log_level_set("HKAuthCtx", level);
  esp_log_level_set("HKFastAuth", level);
  esp_log_level_set("HKStdAuth", level);
  esp_log_level_set("HKAttestAuth", level);
  esp_log_level_set("PN532", level);
  esp_log_level_set("PN532_SPI", level);
  esp_log_level_set("ISO18013_SC", level);
  esp_log_level_set("LockMechanism", level);
  esp_log_level_set("NFCAccess", level);
  esp_log_level_set("actions-config", level);
  esp_log_level_set("misc-config", level);
  esp_log_level_set("mqttconfig", level);
}

void print_issuers(const char* buf) {
  for (auto&& issuer : readerData.issuers) {
    LOG(I, "Issuer ID: %s, Public Key: %s", red_log::bufToHexString(issuer.issuer_id.data(), issuer.issuer_id.size()).c_str(), red_log::bufToHexString(issuer.issuer_pk.data(), issuer.issuer_pk.size()).c_str());
    for (auto&& endpoint : issuer.endpoints) {
      LOG(I, "Endpoint ID: %s, Public Key: %s", red_log::bufToHexString(endpoint.endpoint_id.data(), endpoint.endpoint_id.size()).c_str(), red_log::bufToHexString(endpoint.endpoint_pk.data(), endpoint.endpoint_pk.size()).c_str());
    }
  }
}

void notFound(AsyncWebServerRequest* request) {
  request->send(404, "text/plain", "Not found");
}

void listDir(fs::FS& fs, const char* dirname, uint8_t levels) {
  LOG(I, "Listing directory: %s\r\n", dirname);

  File root = fs.open(dirname);
  if (!root) {
    LOG(I, "- failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    LOG(I, " - not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  DIR : ");
      LOG(I, "%s", file.name());
      if (levels) {
        listDir(fs, file.name(), levels - 1);
      }
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("\tSIZE: ");
      LOG(I, "%d", file.size());
    }
    file = root.openNextFile();
  }
}

String indexProcess(const String& var) {
  if (var == "VERSION") {
    const esp_app_desc_t* app_desc = esp_app_get_description();
    std::string app_version = app_desc->version;
    return String(app_version.c_str());
  }
  return "";
}

bool headersFix(AsyncWebServerRequest* request) { request->addInterestingHeader("ANY"); return true; };
void setupWeb() {
  auto assetsHandle = new AsyncStaticWebHandler("/assets", LittleFS, "/assets/", NULL);
  assetsHandle->setFilter(headersFix);
  webServer.addHandler(assetsHandle);
  auto routesHandle = new AsyncStaticWebHandler("/fragment", LittleFS, "/routes", NULL);
  routesHandle->setFilter(headersFix);
  webServer.addHandler(routesHandle);
  AsyncCallbackWebHandler* dataProvision = new AsyncCallbackWebHandler();
  webServer.addHandler(dataProvision);
  dataProvision->setUri("/config");
  dataProvision->setMethod(HTTP_GET);
  dataProvision->onRequest([](AsyncWebServerRequest* req) {
    if (req->hasParam("type")) {
      json serializedData;
      AsyncWebParameter* data = req->getParam(0);
      std::array<std::string, 3> pages = {"actions", "misc", "hkinfo"};
      if (std::equal(data->value().begin(), data->value().end(),pages[0].begin(), pages[0].end()) || std::equal(data->value().begin(), data->value().end(),pages[1].begin(), pages[1].end())) {
        LOG(D, "ACTIONS CONFIG REQ");
        serializedData = espConfig::miscConfig;
      } else if (std::equal(data->value().begin(), data->value().end(),pages[2].begin(), pages[2].end())) {
        LOG(D, "HK DATA REQ");
        json inputData = readerData;
        if (inputData.contains("group_identifier")) {
          serializedData["group_identifier"] = red_log::bufToHexString(readerData.reader_gid.data(), readerData.reader_gid.size(), true);
        }
        if (inputData.contains("unique_identifier")) {
          serializedData["unique_identifier"] = red_log::bufToHexString(readerData.reader_id.data(), readerData.reader_id.size(), true);
        }
        if (inputData.contains("issuers")) {
          serializedData["issuers"] = json::array();
          for (auto it = inputData.at("issuers").begin(); it != inputData.at("issuers").end(); ++it)
          {
            json issuer;
            if (it.value().contains("issuerId")) {
              std::vector<uint8_t> id = it.value().at("issuerId").get<std::vector<uint8_t>>();
              issuer["issuerId"] = red_log::bufToHexString(id.data(), id.size(), true);
            }
            if (it.value().contains("endpoints") && it.value().at("endpoints").size() > 0) {
              issuer["endpoints"] = json::array();
              for (auto it2 = it.value().at("endpoints").begin(); it2 != it.value().at("endpoints").end(); ++it2) {
                json endpoint;
                if (it2.value().contains("endpointId")) {
                  std::vector<uint8_t> id = it2.value().at("endpointId").get<std::vector<uint8_t>>();
                  endpoint["endpointId"] = red_log::bufToHexString(id.data(), id.size(), true);
                }
                issuer["endpoints"].push_back(endpoint);
              }
            }
            serializedData["issuers"].push_back(issuer);
          }
        }
      } else {
        req->send(400);
        return;
      }
      if (!serializedData.empty()) {
        req->send(200, "application/json", serializedData.dump().c_str());
      } else {
        req->send(500);
      }
    } else req->send(500);
  });
  AsyncCallbackWebHandler* ethSuppportConfig = new AsyncCallbackWebHandler();
  webServer.addHandler(ethSuppportConfig);
  ethSuppportConfig->setUri("/eth_get_config");
  ethSuppportConfig->setMethod(HTTP_GET);
  ethSuppportConfig->onRequest([](AsyncWebServerRequest *req) {
    json eth_config;
    eth_config["supportedChips"] = json::array();
    for (auto &&v : eth_config_ns::supportedChips) {
      eth_config.at("supportedChips").push_back(v.second);
    }
    eth_config["boardPresets"] = eth_config_ns::boardPresets;
    eth_config["ethEnabled"] = espConfig::miscConfig.ethernetEnabled;
    req->send(200, "application/json", eth_config.dump().c_str());
  });
  AsyncCallbackWebHandler* dataClear = new AsyncCallbackWebHandler();
  webServer.addHandler(dataClear);
  dataClear->setUri("/config/clear");
  dataClear->setMethod(HTTP_POST);
  dataClear->onRequest([](AsyncWebServerRequest* req) {
    if (req->hasParam("type")) {
      AsyncWebParameter* data = req->getParam(0);
      std::array<std::string, 2> pages = { "actions", "misc" };  // Remove "mqtt" from pages array
      if (std::equal(data->value().begin(), data->value().end(), pages[0].begin(), pages[0].end())) {
        LOG(D, "ACTIONS CONFIG SEL");
        nvs_erase_key(savedData, "MISCDATA");
        espConfig::miscConfig = {};
        req->send(200, "text/plain", "200 Success");
      } else if (std::equal(data->value().begin(), data->value().end(), pages[1].begin(), pages[1].end())) {
        LOG(D, "MISC CONFIG SEL");
        nvs_erase_key(savedData, "MISCDATA");
        espConfig::miscConfig = {};
        req->send(200, "text/plain", "200 Success");
      } else {
        req->send(400);
        return;
      }
    } else {
        req->send(400);
        return;
      }
  });
  AsyncCallbackWebHandler* dataLoad = new AsyncCallbackWebHandler();
  webServer.addHandler(dataLoad);
  dataLoad->setUri("/config/save");
  dataLoad->setMethod(HTTP_POST);
  dataLoad->onBody([](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    json *dataJson = new json(json::parse(data, data + len));
    if (!dataJson->is_discarded()) {
      LOG(I, "%s", dataJson->dump().c_str());
      request->_tempObject = dataJson;
    }
  });
  dataLoad->onRequest([=](AsyncWebServerRequest* req) {
    json *serializedData = static_cast<json*>(req->_tempObject);
    if (req->hasParam("type") && serializedData) {
      AsyncWebParameter* data = req->getParam(0);
      json configData;
      std::array<std::string, 2> pages = { "actions", "misc" };  // Remove mqtt
      uint8_t selConfig;
      if (std::equal(data->value().begin(), data->value().end(), pages[0].begin(), pages[0].end())) {
        LOG(D, "ACTIONS CONFIG SEL");
        configData = espConfig::miscConfig;
        selConfig = 0;
      } else if (std::equal(data->value().begin(), data->value().end(), pages[1].begin(), pages[1].end())) {
        LOG(D, "MISC CONFIG SEL");
        configData = espConfig::miscConfig;
        selConfig = 1;
      } else {
        req->send(400);
        return;
      }
      uint8_t propertiesProcessed = 0;
      for (auto it = serializedData->begin(); it != serializedData->end(); ++it) {
        if (configData.contains(it.key()) && ((configData.at(it.key()).type() == it.value().type()) || configData.at(it.key()).is_boolean())) {
          if (it.key() == std::string("setupCode")) {
            std::string code = it.value().template get<std::string>();
            if (it.value().is_string() && (!code.empty() && std::find_if(code.begin(), code.end(), [](unsigned char c) { return !std::isdigit(c); }) == code.end()) && it.value().template get<std::string>().length() == 8) {
              if (homeSpan.controllerListBegin() != homeSpan.controllerListEnd() && code.compare(configData.at(it.key()).template get<std::string>())) {
                LOG(E, "The Setup Code can only be set if no devices are paired, reset if any issues!");
                req->send(400, "text/plain", "The Setup Code can only be set if no devices are paired, reset if any issues!");
                break;
              }
            } else {
              LOG(E, "\"%s\" could not validate!", it.key().c_str());
              std::string msg = "\"\" is not a valid value for \"\"";
              msg.insert(1, it.value().dump().c_str()).insert(msg.length() - 1, it.key());
              req->send(400, "text/plain", msg.c_str());
              break;
            }
          } else if (!(std::char_traits<char>::compare(it.key().data() + (it.key().length() - 3), "Pin", 3))) {
            if (it.value().is_number() && it.value() > 0 && it.value() < 256) {
              if (!GPIO_IS_VALID_GPIO(it.value().template get<uint8_t>()) && !GPIO_IS_VALID_OUTPUT_GPIO(it.value().template get<uint8_t>()) && it.value() != 255) {
                LOG(E, "\"%s\" could not validate!", it.key().c_str());
                std::string msg = "\"\" is not a valid GPIO Pin for \"\"";
                msg.insert(1, it.value().dump().c_str()).insert(msg.length() - 1, it.key());
                req->send(400, "text/plain", msg.c_str());
                break;
              }
            } else {
                LOG(E, "\"%s\" could not validate!", it.key().c_str());
                std::string msg = "\"\" is not a valid value for \"\"";
                msg.insert(1, it.value().dump().c_str()).insert(msg.length() - 1, it.key());
                req->send(400, "text/plain", msg.c_str());
                break;
            }
          }
          if (configData.at(it.key()).is_boolean() && it.value().is_number()) {
            it.value() = static_cast<bool>(it.value().template get<uint8_t>());
          } else if(configData.at(it.key()).is_boolean() && !it.value().is_number()) {
            LOG(E, "\"%s\" could not validate!", it.key().c_str());
            std::string msg = "\"\" is not a valid value for \"\"";
            msg.insert(1, it.value().dump().c_str()).insert(msg.length() - 1, it.key());
            req->send(400, "text/plain", msg.c_str());
            break;
          }
          propertiesProcessed++;
        } else {
          LOG(E, "\"%s\" could not validate!", it.key().c_str());
          std::string msg = "\"\" not of correct type or does not exist in config";
          msg.insert(1, it.key());
          req->send(400, "text/plain", msg.c_str());
          break;
        }
      }
      if (propertiesProcessed != serializedData->size()) {
        LOG(E, "Not all properties could be validated, cannot continue!");
        if(!req->client()->disconnected() || !req->client()->disconnecting()) {
          req->send(500, "text/plain", "Something went wrong!");
        }
        return;
      }
      bool rebootNeeded = false;
      std::string rebootMsg;
      for (auto it = serializedData->begin(); it != serializedData->end(); ++it) {
        if (it.key() == std::string("nfcTagNoPublish") && (it.value() != 0)) {
          std::string clientId;
          if (serializedData->contains("mqttClientId")) {
            clientId = serializedData->at("mqttClientId");
          }
          std::string rfidTopic;
          rfidTopic.append("homeassistant/tag/").append(!clientId.empty() ? clientId : espConfig::mqttData.mqttClientId).append("/rfid/config");
          esp_mqtt_client_publish(client, rfidTopic.c_str(), "", 0, 0, false);
        } else if (it.key() == std::string("setupCode")) {
          std::string code = it.value().template get<std::string>();
          if (espConfig::miscConfig.setupCode.c_str() != it.value() && code.length() == 8) {
            if (homeSpan.controllerListBegin() == homeSpan.controllerListEnd()) {
              homeSpan.setPairingCode(code.c_str());
            }
          }
        } else if (it.key() == std::string("nfcNeopixelPin")) {
          if (espConfig::miscConfig.nfcNeopixelPin == 255 && it.value() != 255 && neopixel_task_handle == nullptr) {
            xTaskCreate(neopixel_task, "neopixel_task", 4096, NULL, 2, &neopixel_task_handle);
            if (!pixel) {
              pixel = std::make_shared<Pixel>(it.value(), PixelType::GRB);
            }
          } else if (espConfig::miscConfig.nfcNeopixelPin != 255 && it.value() == 255 && neopixel_task_handle != nullptr) {
            uint8_t status = 2;
            xQueueSend(neopixel_handle, &status, 0);
            neopixel_task_handle = nullptr;
          }
        } else if (it.key() == std::string("nfcSuccessPin")) {
          if (espConfig::miscConfig.nfcSuccessPin == 255 && it.value() != 255 && gpio_led_task_handle == nullptr) {
            pinMode(it.value(), OUTPUT);
            xTaskCreate(nfc_gpio_task, "nfc_gpio_task", 4096, NULL, 2, &gpio_led_task_handle);
          } else if (espConfig::miscConfig.nfcSuccessPin != 255 && it.value() == 255 && gpio_led_task_handle != nullptr) {
            if (serializedData->contains("nfcFailPin") && serializedData->at("nfcFailPin") == 255) {
              uint8_t status = 2;
              xQueueSend(gpio_led_handle, &status, 0);
              gpio_led_task_handle = nullptr;
            }
          } else if (it.value() != 255) {
            pinMode(it.value(), OUTPUT);
          }
        } else if (it.key() == std::string("nfcFailPin")) {
          if (espConfig::miscConfig.nfcFailPin == 255 && it.value() != 255 && gpio_led_task_handle == nullptr) {
            pinMode(it.value(), OUTPUT);
            xTaskCreate(nfc_gpio_task, "nfc_gpio_task", 4096, NULL, 2, &gpio_led_task_handle);
          } else if (espConfig::miscConfig.nfcFailPin != 255 && it.value() == 255 && gpio_led_task_handle != nullptr) {
            if (serializedData->contains("nfcSuccessPin") && serializedData->at("nfcSuccessPin") == 255) {
              uint8_t status = 2;
              xQueueSend(gpio_led_handle, &status, 0);
              gpio_led_task_handle = nullptr;
            }
          } else if (it.value() != 255) {
            pinMode(it.value(), OUTPUT);
          }
        } else if (it.key() == std::string("btrLowStatusThreshold")) {
          if (statusLowBtr && btrLevel) {
            if (btrLevel->getVal() <= it.value()) {
              statusLowBtr->setVal(1);
            } else {
              statusLowBtr->setVal(0);
            }
          }
        } else if (it.key() == std::string("neoPixelType")) {
          uint8_t pixelType = it.value().template get<uint8_t>();
          if (pixelType != configData.at(it.key()).template get<uint8_t>()) {
            rebootNeeded = true;
            rebootMsg = "Pixel Type was changed, reboot needed! Rebooting...";
          }
        } else if (it.key() == std::string("gpioActionPin")) {
          if (espConfig::miscConfig.gpioActionPin == 255 && it.value() != 255 ) {
            LOG(D, "ENABLING HomeKit Trigger - Simple GPIO");
            pinMode(it.value(), OUTPUT);
            if(gpio_lock_task_handle == nullptr){
              xTaskCreate(gpio_task, "gpio_task", 4096, NULL, 2, &gpio_lock_task_handle);
            }
            if(espConfig::miscConfig.hkDumbSwitchMode){
              serializedData->at("hkDumbSwitchMode") = false;
            }
          } else if (espConfig::miscConfig.gpioActionPin != 255 && it.value() == 255) {
            LOG(D, "DISABLING HomeKit Trigger - Simple GPIO");
            if( gpio_lock_task_handle != nullptr){
              gpioLockAction status{ .source = gpioLockAction::OTHER, .action = 2 };
              xQueueSend(gpio_lock_handle, &status, 0);
              gpio_lock_task_handle = nullptr;
            }
            gpio_reset_pin(gpio_num_t(espConfig::miscConfig.gpioActionPin));
          }
        } else if (it.key() == std::string("hkDumbSwitchMode") && gpio_lock_task_handle == nullptr) {
          xTaskCreate(gpio_task, "gpio_task", 4096, NULL, 2, &gpio_lock_task_handle);
        }
        configData.at(it.key()) = it.value();
      }
      std::vector<uint8_t> vectorData = json::to_msgpack(configData);
      esp_err_t set_nvs = nvs_set_blob(savedData, selConfig == 0 ? "MQTTDATA" : "MISCDATA", vectorData.data(), vectorData.size());
      esp_err_t commit_nvs = nvs_commit(savedData);
      LOG(D, "SET_STATUS: %s", esp_err_to_name(set_nvs));
      LOG(D, "COMMIT_STATUS: %s", esp_err_to_name(commit_nvs));
      if (set_nvs == ESP_OK && commit_nvs == ESP_OK) {
        LOG(I, "Config successfully saved to NVS");
        if (selConfig == 0) {
          configData.get_to<espConfig::mqttConfig_t>(espConfig::mqttData);
        } else {
          configData.get_to<espConfig::misc_config_t>(espConfig::miscConfig);
        }
      } else {
        LOG(E, "Something went wrong, could not save to NVS");
      }
      if (selConfig == 0 || selConfig == 2) {
        req->send(200, "text/plain", "Saved! Restarting...");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        ESP.restart();
      } else {
        if(rebootNeeded){
          req->send(200, "text/plain", rebootMsg.c_str());
        } else {
          req->send(200, "text/plain", "Saved and applied!");
        }
      }
    }
  });
  auto rebootDeviceHandle = new AsyncCallbackWebHandler();
  rebootDeviceHandle->setUri("/reboot_device");
  rebootDeviceHandle->setMethod(HTTP_GET);
  rebootDeviceHandle->onRequest([](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "Rebooting the device...");
    delay(1000);
    ESP.restart();
    });
  webServer.addHandler(rebootDeviceHandle);
  auto startConfigAP = new AsyncCallbackWebHandler();
  startConfigAP->setUri("/start_config_ap");
  startConfigAP->setMethod(HTTP_GET);
  startConfigAP->onRequest([](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "Starting the AP...");
    delay(1000);
    webServer.end();
    homeSpan.processSerialCommand("A");
    });
  webServer.addHandler(startConfigAP);
  auto resetHkHandle = new AsyncCallbackWebHandler();
  resetHkHandle->setUri("/reset_hk_pair");
  resetHkHandle->setMethod(HTTP_GET);
  resetHkHandle->onRequest([](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "Erasing HomeKit pairings and restarting...");
    delay(1000);
    deleteReaderData();
    homeSpan.processSerialCommand("H");
    });
  webServer.addHandler(resetHkHandle);
  auto resetWifiHandle = new AsyncCallbackWebHandler();
  resetWifiHandle->setUri("/reset_wifi_cred");
  resetWifiHandle->setMethod(HTTP_GET);
  resetWifiHandle->onRequest([](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "Erasing WiFi credentials and restarting, AP will start on boot...");
    delay(1000);
    homeSpan.processSerialCommand("X");
    });
  webServer.addHandler(resetWifiHandle);
  auto getWifiRssi = new AsyncCallbackWebHandler();
  getWifiRssi->setUri("/get_wifi_rssi");
  getWifiRssi->setMethod(HTTP_GET);
  getWifiRssi->onRequest([](AsyncWebServerRequest* request) {
    std::string rssi_val = std::to_string(WiFi.RSSI());
    request->send(200, "text/plain", rssi_val.c_str());
    });
  webServer.addHandler(getWifiRssi);
  AsyncCallbackWebHandler* rootHandle = new AsyncCallbackWebHandler();
  webServer.addHandler(rootHandle);
  rootHandle->setUri("/");
  rootHandle->setMethod(HTTP_GET);
  rootHandle->onRequest([](AsyncWebServerRequest* req) {
    req->send(LittleFS, "/index.html", "text/html", false, indexProcess);
  });
  AsyncCallbackWebHandler* hashPage = new AsyncCallbackWebHandler();
  webServer.addHandler(hashPage);
  hashPage->setUri("/#*");
  hashPage->setMethod(HTTP_GET);
  hashPage->onRequest([](AsyncWebServerRequest* req) {
    req->send(LittleFS, "/index.html", "text/html", false, indexProcess);
  });
  if (espConfig::miscConfig.webAuthEnabled) {
    LOG(I, "Web Authentication Enabled");
    routesHandle->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    dataProvision->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    dataLoad->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    dataClear->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    rootHandle->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    hashPage->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    resetHkHandle->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    resetWifiHandle->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    getWifiRssi->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    startConfigAP->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    ethSuppportConfig->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
  }
  webServer.onNotFound(notFound);
  webServer.begin();
}

void wifiCallback(int status) {
  if (status == 1) {
    setupWeb();
  }
}

void nfc_retry(void* arg) {
  ESP_LOGI(TAG, "Starting reconnecting PN532");
  while (1) {
    nfc->begin();
    uint32_t versiondata = nfc->getFirmwareVersion();
    if (!versiondata) {
      ESP_LOGE("NFC_SETUP", "Error establishing PN532 connection");
    } else {
      unsigned int model = (versiondata >> 24) & 0xFF;
      ESP_LOGI("NFC_SETUP", "Found chip PN5%x", model);
      int maj = (versiondata >> 16) & 0xFF;
      int min = (versiondata >> 8) & 0xFF;
      ESP_LOGI("NFC_SETUP", "Firmware ver. %d.%d", maj, min);
      nfc->SAMConfig();
      nfc->setRFField(0x02, 0x01);
      nfc->setPassiveActivationRetries(0);
      ESP_LOGI("NFC_SETUP", "Waiting for an ISO14443A card");
      vTaskResume(nfc_poll_task);
      vTaskDelete(NULL);
      return;
    }
    nfc->stop();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void nfc_thread_entry(void* arg) {
  uint32_t versiondata = nfc->getFirmwareVersion();
  if (!versiondata) {
    ESP_LOGE("NFC_SETUP", "Error establishing PN532 connection");
    nfc->stop();
    xTaskCreate(nfc_retry, "nfc_reconnect_task", 8192, NULL, 1, &nfc_reconnect_task);
    vTaskSuspend(NULL);
  } else {
    unsigned int model = (versiondata >> 24) & 0xFF;
    ESP_LOGI("NFC_SETUP", "Found chip PN5%x", model);
    int maj = (versiondata >> 16) & 0xFF;
    int min = (versiondata >> 8) & 0xFF;
    ESP_LOGI("NFC_SETUP", "Firmware ver. %d.%d", maj, min);
    nfc->SAMConfig();
    nfc->setRFField(0x02, 0x01);
    nfc->setPassiveActivationRetries(0);
    ESP_LOGI("NFC_SETUP", "Waiting for an ISO14443A card");
  }
  memcpy(ecpData + 8, readerData.reader_gid.data(), readerData.reader_gid.size());
  with_crc16(ecpData, 16, ecpData + 16);
  while (1) {
    uint8_t res[4];
    uint16_t resLen = 4;
    bool writeStatus = nfc->writeRegister(0x633d, 0, true);
    if (!writeStatus) {
      LOG(W, "writeRegister has failed, abandoning ship !!");
      nfc->stop();
      xTaskCreate(nfc_retry, "nfc_reconnect_task", 8192, NULL, 1, &nfc_reconnect_task);
      vTaskSuspend(NULL);
    }
    nfc->inCommunicateThru(ecpData, sizeof(ecpData), res, &resLen, 100, true);
    uint8_t uid[16];
    uint8_t uidLen = 0;
    uint8_t atqa[2];
    uint8_t sak[1];
    bool passiveTarget = nfc->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, atqa, sak, 500, true, true);
    if (passiveTarget) {
      nfc->setPassiveActivationRetries(5);
      LOG(D, "ATQA: %02x", atqa[0]);
      LOG(D, "SAK: %02x", sak[0]);
      ESP_LOG_BUFFER_HEX_LEVEL(TAG, uid, (size_t)uidLen, ESP_LOG_VERBOSE);
      LOG(I, "*** PASSIVE TARGET DETECTED ***");
      auto startTime = std::chrono::high_resolution_clock::now();
      uint8_t data[13] = { 0x00, 0xA4, 0x04, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x08, 0x58, 0x01, 0x01, 0x0 };
      uint8_t selectCmdRes[9];
      uint16_t selectCmdResLength = 9;
      LOG(I, "Requesting supported HomeKey versions");
      LOG(D, "SELECT HomeKey Applet, APDU: ");
      ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, sizeof(data), ESP_LOG_VERBOSE);
      bool status = nfc->inDataExchange(data, sizeof(data), selectCmdRes, &selectCmdResLength);
      LOG(D, "SELECT HomeKey Applet, Response");
      ESP_LOG_BUFFER_HEX_LEVEL(TAG, selectCmdRes, selectCmdResLength, ESP_LOG_VERBOSE);
      if (status && selectCmdRes[selectCmdResLength - 2] == 0x90 && selectCmdRes[selectCmdResLength - 1] == 0x00) {
        LOG(D, "*** SELECT HOMEKEY APPLET SUCCESSFUL ***");
        LOG(D, "Reader Private Key: %s", red_log::bufToHexString(readerData.reader_pk.data(), readerData.reader_pk.size()).c_str());
        HKAuthenticationContext authCtx([](uint8_t* s, uint8_t l, uint8_t* r, uint16_t* rl, bool il) -> bool {return nfc->inDataExchange(s, l, r, rl, il);}, readerData, savedData);
        auto authResult = authCtx.authenticate(hkFlow);
        if (std::get<2>(authResult) != kFlowFailed) {
          bool status = true;
          if (espConfig::miscConfig.nfcSuccessPin != 255) {
            xQueueSend(gpio_led_handle, &status, 0);
          }
          if (espConfig::miscConfig.nfcNeopixelPin != 255) {
            xQueueSend(neopixel_handle, &status, 0);
          }
          if ((espConfig::miscConfig.gpioActionPin != 255 && espConfig::miscConfig.hkGpioControlledState) || espConfig::miscConfig.hkDumbSwitchMode) {
            const gpioLockAction action{ .source = gpioLockAction::HOMEKEY, .action = 0 };
            xQueueSend(gpio_lock_handle, &action, 0);
          }
          if (espConfig::miscConfig.hkAltActionInitPin != 255 && espConfig::miscConfig.hkAltActionPin != 255) {
            uint8_t status = 2;
            xQueueSend(gpio_led_handle, &status, 0);
          }
          if (hkAltActionActive) {
            // mqtt_publish(espConfig::mqttData.hkAltActionTopic, "alt_action", 0, false);
          }
          json payload;
          payload["issuerId"] = hex_representation(std::get<0>(authResult));
          payload["endpointId"] = hex_representation(std::get<1>(authResult));
          payload["readerId"] = hex_representation(readerData.reader_id);
          payload["homekey"] = true;
          std::string payloadStr = payload.dump();
          // mqtt_publish(espConfig::mqttData.hkTopic, payloadStr, 0, false);
          if (espConfig::miscConfig.lockAlwaysUnlock) {
            if (espConfig::miscConfig.gpioActionPin == 255 || !espConfig::miscConfig.hkGpioControlledState) {
              lockCurrentState->setVal(lockStates::UNLOCKED);
              lockTargetState->setVal(lockStates::UNLOCKED);
              // mqtt_publish(espConfig::mqttData.lockStateTopic, std::to_string(lockStates::UNLOCKED), 1, true);
            }
          } else if (espConfig::miscConfig.lockAlwaysLock) {
            if (espConfig::miscConfig.gpioActionPin == 255 || espConfig::miscConfig.hkGpioControlledState) {
              lockCurrentState->setVal(lockStates::LOCKED);
              lockTargetState->setVal(lockStates::LOCKED);
              // mqtt_publish(espConfig::mqttData.lockStateTopic, std::to_string(lockStates::LOCKED), 1, true);
            }
          } else {
            int currentState = lockCurrentState->getVal();
            if (espConfig::mqttData.lockEnableCustomState) {
              if (currentState == lockStates::UNLOCKED) {
                // mqtt_publish(espConfig::mqttData.lockCustomStateTopic, std::to_string(espConfig::mqttData.customLockActions["LOCK"]), 0, false);
              } else if (currentState == lockStates::LOCKED) {
                // mqtt_publish(espConfig::mqttData.lockCustomStateTopic, std::to_string(espConfig::mqttData.customLockActions["UNLOCK"]), 0, false);
              }
            }
          }

          auto stopTime = std::chrono::high_resolution_clock::now();
          LOG(I, "Total Time (detection->auth->gpio->mqtt): %lli ms", std::chrono::duration_cast<std::chrono::milliseconds>(stopTime - startTime).count());
        } else {
          bool status = false;
          if (espConfig::miscConfig.nfcFailPin != 255) {
            xQueueSend(gpio_led_handle, &status, 0);
          }
          if (espConfig::miscConfig.nfcNeopixelPin != 255) {
            xQueueSend(neopixel_handle, &status, 0);
          }
          LOG(W, "We got status FlowFailed, mqtt untouched!");
        }
        nfc->setRFField(0x02, 0x01);
      } else if(!espConfig::mqttData.nfcTagNoPublish) {
        LOG(W, "Invalid Response, probably not Homekey, publishing target's UID");
        bool status = false;
        if (espConfig::miscConfig.nfcFailPin != 255) {
          xQueueSend(gpio_led_handle, &status, 0);
        }
        if (espConfig::miscConfig.nfcNeopixelPin != 255) {
          xQueueSend(neopixel_handle, &status, 0);
        }
        json payload;
        payload["atqa"] = hex_representation(std::vector<uint8_t>(atqa, atqa + 2));
        payload["sak"] = hex_representation(std::vector<uint8_t>(sak, sak + 1));
        payload["uid"] = hex_representation(std::vector<uint8_t>(uid, uid + uidLen));
        payload["homekey"] = false;
        std::string payload_dump = payload.dump();
        // mqtt_publish(espConfig::mqttData.hkTopic.c_str(), payload_dump.c_str(), 0, 0, false);
      }
      vTaskDelay(50 / portTICK_PERIOD_MS);
      nfc->inRelease();
      int counter = 50;
      bool deviceStillInField = nfc->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen);
      LOG(D, "Target still present: %d", deviceStillInField);
      while (deviceStillInField) {
        if (counter == 0) break;
        vTaskDelay(50 / portTICK_PERIOD_MS);
        nfc->inRelease();
        deviceStillInField = nfc->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen);
        --counter;
        LOG(D, "Target still present: %d Counter=%d", deviceStillInField, counter);
      }
      nfc->inRelease();
      nfc->setPassiveActivationRetries(0);
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
  return;
}

void onEvent(arduino_event_id_t event, arduino_event_info_t info) {
  uint8_t mac[6] = { 0, 0, 0, 0, 0, 0 };
  char macStr[13] = {0};
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      LOG(I, "ETH Started");
      ETH.macAddress(mac);
      sprintf(macStr, "ESP32_%02X%02X%02X", mac[0], mac[1], mac[2]);
      ETH.setHostname(macStr);
      break;
    case ARDUINO_EVENT_ETH_CONNECTED: LOG(I, "ETH Connected"); break;
    case ARDUINO_EVENT_ETH_GOT_IP:    LOG(I, "ETH Got IP: '%s'\n", esp_netif_get_desc(info.got_ip.esp_netif)); break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      LOG(I, "ETH Lost IP");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      LOG(I, "ETH Disconnected");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      LOG(I, "ETH Stopped");
      break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  const esp_app_desc_t* app_desc = esp_app_get_description();
  std::string app_version = app_desc->version;
  gpio_led_handle = xQueueCreate(2, sizeof(uint8_t));
  neopixel_handle = xQueueCreate(2, sizeof(uint8_t));
  gpio_lock_handle = xQueueCreate(2, sizeof(gpioLockAction));
  size_t len;
  const char* TAG = "SETUP";
  nvs_open("SAVED_DATA", NVS_READWRITE, &savedData);
  if (!nvs_get_blob(savedData, "READERDATA", NULL, &len)) {
    std::vector<uint8_t> savedBuf(len);
    nvs_get_blob(savedData, "READERDATA", savedBuf.data(), &len);
    LOG(D, "NVS READERDATA LENGTH: %d", len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, savedBuf.data(), savedBuf.size(), ESP_LOG_VERBOSE);
    nlohmann::json data = nlohmann::json::from_msgpack(savedBuf);
    if (!data.is_discarded()) {
      data.get_to<readerData_t>(readerData);
      LOG(I, "Reader Data loaded from NVS");
    }
  }
  if (!nvs_get_blob(savedData, "MISCDATA", NULL, &len)) {
    std::vector<uint8_t> dataBuf(len);
    nvs_get_blob(savedData, "MISCDATA", dataBuf.data(), &len);
    std::string str(dataBuf.begin(), dataBuf.end());
    LOG(D, "NVS MISCDATA LENGTH: %d", len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, dataBuf.data(), dataBuf.size(), ESP_LOG_VERBOSE);
    auto isValidJson = nlohmann::json::accept(dataBuf);
    if (isValidJson) {
      nlohmann::json data = nlohmann::json::parse(str);
      if (!data.is_discarded()) {
        data.get_to<espConfig::misc_config_t>(espConfig::miscConfig);
        LOG(I, "Misc Config loaded from NVS");
      }
    } else {
      nlohmann::json data = nlohmann::json::from_msgpack(dataBuf);
      if (!data.is_discarded()) {
        data.get_to<espConfig::misc_config_t>(espConfig::miscConfig);
        LOG(I, "Misc Config loaded from NVS");
      }
    }
  }
  pn532spi = new PN532_SPI(espConfig::miscConfig.nfcGpioPins[0], espConfig::miscConfig.nfcGpioPins[1], espConfig::miscConfig.nfcGpioPins[2], espConfig::miscConfig.nfcGpioPins[3]);
  nfc = new PN532(*pn532spi);
  nfc->begin();
  if (espConfig::miscConfig.nfcSuccessPin && espConfig::miscConfig.nfcSuccessPin != 255) {
    pinMode(espConfig::miscConfig.nfcSuccessPin, OUTPUT);
    digitalWrite(espConfig::miscConfig.nfcSuccessPin, !espConfig::miscConfig.nfcSuccessHL);
  }
  if (espConfig::miscConfig.nfcFailPin && espConfig::miscConfig.nfcFailPin != 255) {
    pinMode(espConfig::miscConfig.nfcFailPin, OUTPUT);
    digitalWrite(espConfig::miscConfig.nfcFailPin, !espConfig::miscConfig.nfcFailHL);
  }
  if (espConfig::miscConfig.gpioActionPin && espConfig::miscConfig.gpioActionPin != 255) {
    pinMode(espConfig::miscConfig.gpioActionPin, OUTPUT);
  }
  if (espConfig::miscConfig.hkAltActionInitPin != 255) {
    pinMode(espConfig::miscConfig.hkAltActionInitPin, INPUT);
    if (espConfig::miscConfig.hkAltActionPin != 255) {
      pinMode(espConfig::miscConfig.hkAltActionPin, OUTPUT);
    }
    if (espConfig::miscConfig.hkAltActionInitLedPin != 255) {
      pinMode(espConfig::miscConfig.hkAltActionInitLedPin, OUTPUT);
    }
  }
  if (!LittleFS.begin(true)) {
    LOG(I, "An Error has occurred while mounting LITTLEFS");
    return;
  }
  listDir(LittleFS, "/", 0);
  LOG(I, "LittleFS used space: %d / %d", LittleFS.usedBytes(), LittleFS.totalBytes());
  if (espConfig::miscConfig.ethernetEnabled) {
    Network.onEvent(onEvent);
    if (espConfig::miscConfig.ethActivePreset != 255) {
      if (espConfig::miscConfig.ethActivePreset >= eth_config_ns::boardPresets.size()) {
        LOG(E, "Invalid preset index, not initializing ethernet!");
      } else {
        eth_board_presets_t ethPreset = eth_config_ns::boardPresets[espConfig::miscConfig.ethActivePreset];
        if (!ethPreset.ethChip.emac) {
          ETH.begin(ethPreset.ethChip.phy_type, 1, ethPreset.spi_conf.pin_cs, ethPreset.spi_conf.pin_irq, ethPreset.spi_conf.pin_rst, SPI2_HOST, ethPreset.spi_conf.pin_sck, ethPreset.spi_conf.pin_miso, ethPreset.spi_conf.pin_mosi, ethPreset.spi_conf.spi_freq_mhz);
        } else {
  #if CONFIG_ETH_USE_ESP32_EMAC
          ETH.begin(ethPreset.ethChip.phy_type, ethPreset.rmii_conf.phy_addr, ethPreset.rmii_conf.pin_mcd, ethPreset.rmii_conf.pin_mdio, ethPreset.rmii_conf.pin_power, ethPreset.rmii_conf.pin_rmii_clock);
  #else
          LOG(E, "Selected a chip without MAC but %s doesn't have a builtin MAC, cannot initialize ethernet!", CONFIG_IDF_TARGET);
  #endif
        }
      }
    } else if (espConfig::miscConfig.ethActivePreset == 255) {
      eth_chip_desc_t chipType = eth_config_ns::supportedChips[eth_phy_type_t(espConfig::miscConfig.ethPhyType)];
      if (!chipType.emac) {
        ETH.begin(chipType.phy_type, 1, espConfig::miscConfig.ethSpiConfig[1], espConfig::miscConfig.ethSpiConfig[2], espConfig::miscConfig.ethSpiConfig[3], SPI2_HOST, espConfig::miscConfig.ethSpiConfig[4], espConfig::miscConfig.ethSpiConfig[5], espConfig::miscConfig.ethSpiConfig[6], espConfig::miscConfig.ethSpiConfig[0]);
      } else {
#if CONFIG_ETH_USE_ESP32_EMAC
        ETH.begin(chipType.phy_type, espConfig::miscConfig.ethRmiiConfig[0], espConfig::miscConfig.ethRmiiConfig[1], espConfig::miscConfig.ethRmiiConfig[2], espConfig::miscConfig.ethRmiiConfig[3], eth_clock_mode_t(espConfig::miscConfig.ethRmiiConfig[4]));
#endif
      }
    }
  }
  if (espConfig::miscConfig.controlPin != 255) {
    homeSpan.setControlPin(espConfig::miscConfig.controlPin);
  }
  if (espConfig::miscConfig.hsStatusPin != 255) {
    homeSpan.setStatusPin(espConfig::miscConfig.hsStatusPin);
  }
  homeSpan.setStatusAutoOff(15);
  homeSpan.setLogLevel(0);
  homeSpan.setSketchVersion(app_version.c_str());

  LOG(I, "READER GROUP ID (%d): %s", readerData.reader_gid.size(), red_log::bufToHexString(readerData.reader_gid.data(), readerData.reader_gid.size()).c_str());
  LOG(I, "READER UNIQUE ID (%d): %s", readerData.reader_id.size(), red_log::bufToHexString(readerData.reader_id.data(), readerData.reader_id.size()).c_str());

  LOG(I, "HOMEKEY ISSUERS: %d", readerData.issuers.size());
  for (auto&& issuer : readerData.issuers) {
    LOG(D, "Issuer ID: %s, Public Key: %s", red_log::bufToHexString(issuer.issuer_id.data(), issuer.issuer_id.size()).c_str(), red_log::bufToHexString(issuer.issuer_pk.data(), issuer.issuer_pk.size()).c_str());
  }
  homeSpan.enableAutoStartAP();
  homeSpan.enableOTA(espConfig::miscConfig.otaPasswd.c_str());
  homeSpan.setPortNum(1201);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  char macStr[9] = { 0 };
  sprintf(macStr, "%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3]);
  homeSpan.setHostNameSuffix(macStr);
  homeSpan.begin(Category::Locks, espConfig::miscConfig.deviceName.c_str(), "HK-", "HomeKey-ESP32");

  new SpanUserCommand('L', "Set Log Level", setLogLevel);
  new SpanUserCommand('F', "Set HomeKey Flow", setFlow);
  new SpanUserCommand('P', "Print Issuers", print_issuers);
  new SpanUserCommand('R', "Remove Endpoints", [](const char*) {
    for (auto&& issuer : readerData.issuers) {
      issuer.endpoints.clear();
    }
    save_to_nvs();
    });
  new SpanUserCommand('N', "Btr status low", [](const char* arg) {
    const char* TAG = "BTR_LOW";
    if (strncmp(arg + 1, "0", 1) == 0) {
      statusLowBtr->setVal(0);
      LOG(I, "Low status set to NORMAL");
    } else if (strncmp(arg + 1, "1", 1) == 0) {
      statusLowBtr->setVal(1);
      LOG(I, "Low status set to LOW");
    }
  });
  new SpanUserCommand('B', "Btr level", [](const char* arg) {
    uint8_t level = atoi(static_cast<const char *>(arg + 1));
    btrLevel->setVal(level);
  });

  new SpanAccessory();
  new NFCAccessoryInformation();
  new Service::HAPProtocolInformation();
  new Characteristic::Version();
  new LockManagement();
  new LockMechanism();
  new NFCAccess();
  if (espConfig::miscConfig.proxBatEnabled) {
    new PhysicalLockBattery();
  }
  homeSpan.setControllerCallback(pairCallback);
  homeSpan.setConnectionCallback(wifiCallback);
  if (espConfig::miscConfig.nfcNeopixelPin != 255) {
    pixel = std::make_shared<Pixel>(espConfig::miscConfig.nfcNeopixelPin, pixelTypeMap[espConfig::miscConfig.neoPixelType]);
    xTaskCreate(neopixel_task, "neopixel_task", 4096, NULL, 2, &neopixel_task_handle);
  }
  if (espConfig::miscConfig.nfcSuccessPin != 255 || espConfig::miscConfig.nfcFailPin != 255) {
    xTaskCreate(nfc_gpio_task, "nfc_gpio_task", 4096, NULL, 2, &gpio_led_task_handle);
  }
  if (espConfig::miscConfig.gpioActionPin != 255 || espConfig::miscConfig.hkDumbSwitchMode) {
    xTaskCreate(gpio_task, "gpio_task", 4096, NULL, 2, &gpio_lock_task_handle);
  }
  if (espConfig::miscConfig.hkAltActionInitPin != 255) {
    xTaskCreate(alt_action_task, "alt_action_task", 2048, NULL, 2, &alt_action_task_handle);
  }
  xTaskCreate(nfc_thread_entry, "nfc_task", 8192, NULL, 1, &nfc_poll_task);
}

//////////////////////////////////////

void loop() {
  homeSpan.poll();
  vTaskDelay(5);
}

// Add hex_representation function near the top with other utility functions
std::string hex_representation(const std::vector<uint8_t>& data) {
    std::string result;
    result.reserve(data.size() * 2);
    for (uint8_t byte : data) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", byte);
        result += hex;
    }
    return result;
}
