#include <hw/rgbled.h>
#include <core/debug.h>
#include <core/pins.h>

#define NUM_LEDS    1
#define BRIGHTNESS  25

#ifdef ENABLE_RGBWLED
CRGB leds[NUM_LEDS];  // FastLED LED array
#endif

int getNumLEDS() {
    return NUM_LEDS;
}

void setup_rgbled() {
#ifdef ENABLE_RGBWLED
    FastLED.addLeds<NEOPIXEL, RGBLED_DATA_PIN>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);

    setLED_RED();
    delay(500);
    setLED_GREEN();
    delay(500);
    setLED_BLUE();
    delay(500);
    setLED_WHITE();
    delay(500);
    setLED_OFF();

    // TODO: Add diagnostic patterns as noted in original comments
#endif
}

void loop_rgbled() {
    // You can add blinking logic here later
}

void setLED_RED() {
#ifdef ENABLE_RGBWLED
    leds[0] = CRGB::Red;
    FastLED.show();
#endif
}

void setLED_GREEN() {
#ifdef ENABLE_RGBWLED
    leds[0] = CRGB::Green;
    FastLED.show();
#endif
}

void setLED_BLUE() {
#ifdef ENABLE_RGBWLED
    leds[0] = CRGB::Blue;
    FastLED.show();
#endif
}

void setLED_WHITE() {
#ifdef ENABLE_RGBWLED
    leds[0] = CRGB::White;
    FastLED.show();
#endif
}

void setLED_OFF() {
#ifdef ENABLE_RGBWLED
    leds[0] = CRGB::Black;
    FastLED.show();
#endif
}
