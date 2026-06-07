#pragma once

#include <Arduino.h>

bool wifi_client_connected();
String get_wifi_ip();
bool setup_wifi();
void loop_wifi();
