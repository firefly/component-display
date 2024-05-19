#ifndef __FIREFLY_DISPLAY_H__
#define __FIREFLY_DISPLAY_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


#include <stdint.h>

//#define DEBUG_SHOW_FPS  (1)


// https://docs.espressif.com/projects/esp-idf/en/latest/api-reference/peripherals/spi_master.html#gpio-matrix-and-iomux

#define _ENCODE_SPI_BUS(cs0,sclk,miso,mosi) (((cs0) << 21) | ((sclk) << 14) | ((miso) << 7) | ((mosi) << 0))

#define _DECODE_SPI_BUS_CS0(bus) (((bus) >> 21) & 0x1f)
#define _DECODE_SPI_BUS_SCLK(bus) (((bus) >> 14) & 0x1f)
#define _DECODE_SPI_BUS_MISO(bus) (((bus) >> 7) & 0x1f)
#define _DECODE_SPI_BUS_MOSI(bus) (((bus) >> 0) & 0x1f)


/**
 *  SPI Bus Options
 */
typedef enum DisplaySpiBus {
    // ESP32
    //DisplaySpiBusHspi = _ENCODE_SPI_BUS(15, 14, 12, 13),
    //DisplaySpiBusVspi = _ENCODE_SPI_BUS(5, 18, 19, 23),

    // ESP32-C3
    DisplaySpiBus2_cs = _ENCODE_SPI_BUS(10, 6, 2, 7),
    DisplaySpiBus2 = _ENCODE_SPI_BUS(0, 6, 2, 7)
} DisplaySpiBus;


/**
 *  This assumes the pins on the board are opposite the ribbon side.
 */
typedef enum DisplayRotation {
    DisplayRotationPinsTop,
    DisplayRotationPinsLeft
} DisplayRotation;

extern const uint8_t DisplayFragmentHeight;
extern const uint8_t DisplayFragmentWidth;
extern const uint8_t DisplayFragmentCount;

/**
 *  A function called per fragment to render to the buffer.
 */
typedef void (*RenderFunc)(uint8_t *buffer, uint32_t y0, void *context);

/**
 *  Display Context Object.
 *
 *  This is intentionally opaque; do not inspect or rely on internals.
 */
typedef void* DisplayContext;

/**
 *  Initializes the display, sending all necessary commands to
 *  configure the screen, blocking the current thread until
 *  complete.
 *
 *  This allocates DMA-compatible RAM and returns NULL if the
 *  memory cannot be allocated.
 */
DisplayContext display_init(DisplaySpiBus spiBus, uint8_t pinDC,
    uint8_t pinReset, DisplayRotation rotation,
    RenderFunc renderFunc, void *ctx);

/**
 *  Release all allocated buffers.
 *
 *  The DisplayContext must not be used after calling this
 *  without calling init again first.
 */
void display_free(DisplayContext context);

//uint32_t display_renderScene(DisplayContext context, SceneContext scene);
// uint32_t display_renderScene(DisplayContext context);

/**
 *  Renders the next fragment, blocking the current task until
 *  complete, calling the [[RenderFunc]] with the fragment buffer.
 *
 *  This returns 1 if the last fragment of the frame was rendered,
 *  otherwise returns 0.
 */
uint32_t display_renderFragment(DisplayContext context);

/**
 *  Returns the current FPS statistic.
 */
uint16_t display_fps(DisplayContext context);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __FIREFLY_DISPLAY_H__ */
