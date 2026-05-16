#pragma once
#include <Arduino.h>

// Fetch the current delivery status for tracking_number from Swiss Post.
// Makes 4 sequential HTTPS calls to the undocumented ekp-web API.
// On success, sets out_status to one of:
//   "Shipped" | "Customs" | "In delivery" | "Delivered"
// Returns false on network or parse error (out_status is unchanged).
bool parcel_fetch(const char* tracking_number, String& out_status);
