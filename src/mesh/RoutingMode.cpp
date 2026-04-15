#include "RoutingMode.h"

static RoutingMode g_routingMode = RoutingMode::AODV;

RoutingMode getRoutingMode()
{
    return g_routingMode;
}

void setRoutingMode(RoutingMode mode)
{
    g_routingMode = mode;
}

const char *routingModeToString(RoutingMode mode)
{
    switch (mode) {
    case RoutingMode::AODV:
        return "AODV";
    case RoutingMode::MANAGED_FLOODING:
        return "MANAGED_FLOODING";
    default:
        return "UNKNOWN";
    }
}
