
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#include "firefly-display.h"

#include "logo.h"

// The standard Firefly Pixie configuration. If using another
// device you will likely have to change these values.
#define DISPLAY_BUS        (FfxDisplaySpiBus2_nocs)
#define PIN_DISPLAY_DC     (4)
#define PIN_DISPLAY_RESET  (5)

static void delay(uint32_t duration) {
  vTaskDelay((duration + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
}

// Copy the logo.h onto the display
void renderFunc(uint8_t *buffer, uint32_t y0, void *context) {
  uint8_t *dst = &buffer[0];
  for (int y = y0; y < MIN(logo_height, y0 + FfxDisplayFragmentHeight); y++) {
    const uint8_t *src = &logo[y * logo_width * 2];
    for (int x = 0; x < MIN(logo_width, FfxDisplayFragmentWidth); x++) {
      *dst++ = *src++;
      *dst++ = *src++;
    }
  }
}

void app_main(void) {
  FfxDisplayContext display = ffx_display_init(DISPLAY_BUS, PIN_DISPLAY_DC,
    PIN_DISPLAY_RESET, FfxDisplayRotationRibbonRight, renderFunc, NULL);

  while (1) {
    uint32_t frameDone = ffx_display_renderFragment(display);
    if (frameDone) {
      printf("done!\n");
      while(1) { delay(1000); }
    }
  }
}
