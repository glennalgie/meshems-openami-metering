/**
 * @file data_model.cpp
 * @brief Implementation of the Modbus data model
 * 
 * This file defines the data structures that represent the four standard Modbus
 * data types: coils, discrete inputs, holding registers, and input registers.
 * These arrays store the values that can be read from or written to by Modbus
 * master/client devices.
 */

 #include <Arduino.h>
 #include <data_model.h>

  PowerData readings[MODBUS_NUM_METERS]; // Array to hold readings for each meter
  
  /**
   * Coils (read-write digital outputs)
   * 
   * This array stores the state of all coils in the system.
   * Coils are binary (ON/OFF) values that can be read and written by Modbus masters.
   * They typically represent control outputs such as relays or digital outputs.
   */
  bool coils[MODBUS_NUM_COILS];
  
  /**
   * Discrete Inputs (read-only digital inputs)
   * 
   * This array stores the state of all discrete inputs in the system.
   * Discrete inputs are binary (ON/OFF) values that can only be read by Modbus masters.
   * They typically represent the state of digital inputs like switches or sensors.
   */
  bool discreteInputs[MODBUS_NUM_DISCRETE_INPUTS];
  
  /**
   * Holding Registers (read-write 16-bit registers)
   * 
   * This array stores the values of all holding registers in the system.
   * Holding registers are 16-bit values that can be read and written by Modbus masters.
   * They typically store configuration parameters or setpoints for the device.
   */
  uint16_t holdingRegisters[MODBUS_NUM_HOLDING_REGISTERS];
 
 /**
  * Input Registers (read-only 16-bit registers)
  * 
  * This array stores the values of all input registers in the system.
  * Input registers are 16-bit values that can only be read by Modbus masters.
  * They typically store measured values from sensors or status information.
  */
 uint16_t inputRegisters[MODBUS_NUM_INPUT_REGISTERS];

/**
 * Current History Buffer
 * 
 * This structure stores historical current readings for timeline plotting.
 * It uses a circular buffer to store the most recent readings.
 */
CurrentHistory currentHistory = {
    .currentIndex = 0,
    .count = 0,
    .minValue = 0.0,
    .maxValue = 1.0  // Start with a default range
};

/**
 * Add a new current reading to the history buffer
 * 
 * @param value The current reading to add
 */
void addCurrentReading(float value) {
    // Get the index that will be overwritten (oldest value)
    int overwriteIdx = currentHistory.currentIndex;
    bool wasFull = (currentHistory.count >= CURRENT_HISTORY_SIZE);
    
    // Add the new value to the circular buffer
    currentHistory.values[overwriteIdx] = value;
    
    // Update the index (wrap around if necessary)
    currentHistory.currentIndex = (currentHistory.currentIndex + 1) % CURRENT_HISTORY_SIZE;
    
    // Update count (up to the buffer size)
    if (currentHistory.count < CURRENT_HISTORY_SIZE) {
        currentHistory.count++;
    }
    
    // Incremental min/max update - O(1) instead of O(n)
    if (currentHistory.count == 1) {
        // First value: initialize both min and max
        currentHistory.minValue = value;
        currentHistory.maxValue = value;
    } else {
        // Check if the new value extends the range
        if (value < currentHistory.minValue) {
            currentHistory.minValue = value;
        } else if (value > currentHistory.maxValue) {
            currentHistory.maxValue = value;
        } else if (wasFull) {
            // The overwritten value might have been the old min or max.
            // If new value didn't change min/max and we overwrote a potential
            // old min/max, we need to rescan to find the new min/max.
            // This rescan is rare (only when we overwrite and the old min/max
            // was at the position we just overwrote), amortized O(1).
            float oldMin = currentHistory.minValue;
            float oldMax = currentHistory.maxValue;
            // Rescan the entire buffer
            currentHistory.minValue = currentHistory.values[0];
            currentHistory.maxValue = currentHistory.values[0];
            for (int i = 1; i < currentHistory.count; i++) {
                if (currentHistory.values[i] < currentHistory.minValue) {
                    currentHistory.minValue = currentHistory.values[i];
                }
                if (currentHistory.values[i] > currentHistory.maxValue) {
                    currentHistory.maxValue = currentHistory.values[i];
                }
            }
        }
        
        // Ensure we have a minimum range (prevent division by zero)
        if (currentHistory.maxValue <= currentHistory.minValue) {
            currentHistory.maxValue = currentHistory.minValue + 1.0;
        }
    }
}