#pragma once

namespace espConfig {
    struct mqttConfig_t {
        bool lockEnableCustomState = false;
        bool nfcTagNoPublish = true;
        std::string mqttClientId = "";
        std::map<std::string, int> customLockActions;
    } mqttData;
} 