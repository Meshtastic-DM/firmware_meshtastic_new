#pragma once

#include <stdint.h>

enum class RoutingMode : uint8_t {
    AODV = 0,
    MANAGED_FLOODING = 1,
};

RoutingMode getRoutingMode();
void setRoutingMode(RoutingMode mode);
const char *routingModeToString(RoutingMode mode);
