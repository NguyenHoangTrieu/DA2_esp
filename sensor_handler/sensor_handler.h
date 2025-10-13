#ifndef SENSOR_HANDLER_H
#define SENSOR_HANDLER_H

#include <stdint.h>

// Start the sensor handler task (should be called from app_main)
void sensor_handler_start(void);

// Get latest HTU21 temperature in Celsius
float sensor_get_temp(void);

// Get latest HTU21 humidity (%RH)
float sensor_get_humid(void);

// Get latest analog soil moisture value (ADC raw)
uint16_t sensor_get_soil_analog(void);

// Get latest digital soil moisture status (0: wet, 1: dry - depends on probe)
uint8_t sensor_get_soil_digital(void);

// Optionally set soil threshold for activating pump relay
void sensor_set_soil_threshold(uint16_t threshold);

#endif // SENSOR_HANDLER_H
