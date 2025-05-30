enum HK_COLOR
{
  TAN,
  GOLD,
  SILVER,
  BLACK
};

enum lockStates
{
  UNLOCKED,
  LOCKED,
  JAMMED,
  UNKNOWN,
  UNLOCKING,
  LOCKING
};

enum customLockStates
{
  C_LOCKED = 1,
  C_UNLOCKING = 2,
  C_UNLOCKED = 3,
  C_LOCKING = 4,
  C_JAMMED = 254,
  C_UNKNOWN = 255
};
// Custom Lock Actions to be used in MQTT_CUSTOM_STATE_TOPIC
enum customLockActions
{
  UNLOCK = 1,
  LOCK = 2
};

enum class gpioMomentaryStateStatus : uint8_t
{
  M_DISABLED = 0,
  M_HOME = 1 << 0,
  M_HK = 1 << 1,
  M_HOME_HK = (uint8_t)(M_HOME | M_HK)
};

// Miscellaneous
#define HOMEKEY_COLOR TAN
#define SETUP_CODE "46637726"  // HomeKit Setup Code (only for reference, has to be changed during WiFi Configuration or from WebUI)
#define OTA_PWD "homespan-ota" //custom password for ota
#define DEVICE_NAME "HK" //Device name
#define HOMEKEY_ALWAYS_UNLOCK 0 // Flag indicating if a successful Homekey authentication should always set and publish the unlock state
#define HOMEKEY_ALWAYS_LOCK 0  // Flag indicating if a successful Homekey authentication should always set and publish the lock state
#define HS_STATUS_LED 255 // HomeSpan Status LED GPIO pin
#define HS_PIN 255 // GPIO Pin for a Configuration Mode button (more info on https://github.com/HomeSpan/HomeSpan/blob/master/docs/UserGuide.md#device-configuration-mode)

// Actions
#define NFC_NEOPIXEL_PIN 255 // GPIO Pin used for NeoPixel
#define NEOPIXEL_SUCCESS_R 0 // Color value for Red - Success HK Auth
#define NEOPIXEL_SUCCESS_G 255 // Color value for Green - Success HK Auth
#define NEOPIXEL_SUCCESS_B 0 // Color value for Blue - Success HK Auth
#define NEOPIXEL_FAIL_R 255 // Color value for Red - Fail HK Auth
#define NEOPIXEL_FAIL_G 0 // Color value for Green - Fail HK Auth
#define NEOPIXEL_FAIL_B 0 // Color value for Blue - Fail HK Auth
#define NEOPIXEL_SUCCESS_TIME 1000 // GPIO Delay time in ms - Success HK Auth
#define NEOPIXEL_FAIL_TIME 1000 // GPIO Delay time in ms - Success HK Auth
#define NFC_SUCCESS_PIN 255 // GPIO Pin pulled HIGH or LOW (see NFC_SUCCESS_HL) on success HK Auth
#define NFC_SUCCESS_HL HIGH // Flag to define if NFC_SUCCESS_PIN should be held High or Low
#define NFC_SUCCESS_TIME 1000 // How long should NFC_SUCCESS_PIN be held High or Low
#define NFC_FAIL_PIN 255  // GPIO Pin pulled HIGH or LOW (see NFC_SUCCESS_HL) on failed HK Auth
#define NFC_FAIL_HL HIGH // Flag to define if NFC_FAIL_PIN should be held High or Low
#define NFC_FAIL_TIME 1000 // How long should NFC_FAIL_PIN be held High or Low
#define GPIO_ACTION_PIN 255
#define GPIO_ACTION_LOCK_STATE LOW
#define GPIO_ACTION_UNLOCK_STATE HIGH
#define GPIO_ACTION_MOMENTARY_STATE static_cast<uint8_t>(gpioMomentaryStateStatus::M_DISABLED)
#define GPIO_ACTION_MOMENTARY_TIMEOUT 5000
#define GPIO_HK_ALT_ACTION_INIT_PIN 255
#define GPIO_HK_ALT_ACTION_INIT_TIMEOUT 5000
#define GPIO_HK_ALT_ACTION_INIT_LED_PIN 255
#define GPIO_HK_ALT_ACTION_PIN 255
#define GPIO_HK_ALT_ACTION_TIMEOUT 5000
#define GPIO_HK_ALT_ACTION_GPIO_STATE HIGH

// WebUI
#define WEB_AUTH_ENABLED false
#define WEB_AUTH_USERNAME "admin"
#define WEB_AUTH_PASSWORD "password"