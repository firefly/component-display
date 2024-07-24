/**
 *
 * Resources:
 * - https://github.com/espressif/esp-idf/blob/master/examples/peripherals/spi_master/main/spi_master_example_main.c
 */



#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <hal/gpio_ll.h>
#include "soc/gpio_struct.h"

#include "firefly-display.h"
#include "commands.h"

// If using a display with the CS pin pulled low;
// this is now managed by the bus encoding
//#define NO_CS_PIN  (1)

#define DISPLAY_HEIGHT    240
#define DISPLAY_WIDTH     240

#define FRAGMENT_HEIGHT   24

// This is a HARD requirement; otherwise expect infinite loops and nothing to work
#if (240 % FRAGMENT_HEIGHT) != 0
#error "Fragment Height is not a factor of 240"
#endif

// The height of each fragment; this **MUST** be a factor of 240 (or there will be infinite loops)
const uint8_t FfxDisplayFragmentHeight = FRAGMENT_HEIGHT;
const uint8_t FfxDisplayFragmentWidth = DISPLAY_WIDTH;

const uint8_t FfxDisplayFragmentCount = DISPLAY_HEIGHT / FRAGMENT_HEIGHT;


// ST7789 Initialization Sequence
// Place data into DRAM. Constant data gets placed into DROM by default, which is not accessible by DMA.
DRAM_ATTR static const uint8_t st7789_init_sequence[] = {
    CommandResetPin,   0,
    CommandWait,       1,
    CommandResetPin,   1,
    CommandWait,       6,
    CommandMADCTL,     1,   0,
    CommandCOLMOD,     1,
      (CommandCOLMOD_1_format_65k | CommandCOLMOD_1_width_16bit),
    CommandRAMCTRL,    2,
      (CommandRAMCTRL_1),
      (CommandRAMCTRL_2 | CommandRAMCTRL_2_endian_little | CommandRAMCTRL_2_trans_msb),
    CommandPORCTRL,    5,   0x0c, 0x0c, 0x00, 0x33, 0x33,
    CommandGCTRL,      1,   0x45, // @TODO: Fill in ; Vgh=13.65V, Vgl=-10.43V
    CommandVCOMS,      1,   0x2b, // @TODO: Fill in ; VCOM=1.175V
    CommandLCMCTRL,    1,
      (CommandLCMCTRL_1_XBGR | CommandLCMCTRL_1_XMX | CommandLCMCTRL_1_XMH),
    CommandVDVVRHEN,   2,   0x01, 0xff,
    CommandVRHS,       1,   0x11, // @TODO: Fill in ; Vap=4.4+
    CommandVDVS,       1,   0x20, // @TODO: Fill in ; VDV=0
    CommandFRCTRL2,    1,   (CommandFRCTRL2_1_60hz),
    CommandPWCTRL1,    2,
      CommandPWCTRL1_1,
      (CommandPWCTRL1_2_AVDD_6_8 | CommandPWCTRL1_2_AVCL_4_8 | CommandPWCTRL1_2_VDS_2_3),
    CommandPVGAMCTRL, 14,
      0xd0, 0x00, 0x05, 0x0e, 0x15, 0x0d, 0x37, 0x43, 0x47, 0x09,
      0x15, 0x12, 0x16, 0x19,
    CommandNVGAMCTRL, 14,
      0xd0, 0x00, 0x05, 0x0d, 0x0c, 0x06, 0x2d, 0x44, 0x40, 0x0e,
      0x1c, 0x18, 0x16, 0x19,
    CommandSLPOUT,     0,
    CommandWait,       6, // only need to wait for 5ms
    CommandDISPON,     0,
    CommandINVON,      0,
    CommandNORON,      0,
    CommandCASET,      4,
      (0 >> 8),
      (0 & 0xff),
      ((DISPLAY_WIDTH - 1) >> 8),
      ((DISPLAY_WIDTH - 1) & 0xff),
    CommandDone
};

typedef enum MessageType {
    MessageTypeCommand      = 0,
    MessageTypeData         = 1
} MessageType;


typedef struct _Context {
    // The render function to use when rendering a fragment to the buffer
    FfxRenderFunc renderFunc;
    void *context;

    // The SPI device (low-speed during initialization, then upgraded to high-speed)
    spi_device_handle_t spi;

    // The prepared SPI transactions for sending fragments
    spi_transaction_t transactions[4];

    // Two fragments, one for inflight data to the SPI hardware and one for a backbuffer
    uint8_t *fragments[2];

    // The currently inflight fragment (-1 for none; first round)
    int8_t inflightFragment;

    // The pins for D/C (Data/Contral) and Reset
    uint8_t pinDC;
    uint8_t pinReset;

    // The co-routine state
    uint8_t currentY;
    uint32_t frame;  // @todo: unused?
    uint16_t fps;

    // Gather statistics on frame rate
    uint16_t frameCount;
    uint32_t t0;
} _Context;

static void delay(uint32_t duration) {
    vTaskDelay((duration + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
}

static uint32_t ticks() {
    return xTaskGetTickCount();
}

static void* st7789_wrapTransaction(_Context *context, MessageType dc) {
    return (void*)((dc << 7) | context->pinDC);
}

// This is only used during initialization
static void st7789_send(_Context *context, MessageType dc, const uint8_t *data, int length) {
    if (length == 0) { return; }

    // Create the SPI transaction
    spi_transaction_t transaction;
    memset(&transaction, 0, sizeof(spi_transaction_t));
    transaction.tx_buffer = data;
    transaction.length = 8 * length;
    transaction.user = st7789_wrapTransaction(context, dc);

    // Send and wait for completion
    esp_err_t result = spi_device_polling_transmit(context->spi, &transaction);
    assert(result == ESP_OK);
}

// The ST7789 requires a GPIO pin to be set high for data and low
// for commands. Before each transaction this is called, which
// determines the transaxction type from the user data, which is
// set using the st7789_wrapTransaction function.
static void IRAM_ATTR st7789_spi_pre_transfer_callback(spi_transaction_t *txn) {
    int user = (int)(txn->user);

    // We manage GPIO directly to keep it in IRAM, so we can call it from an ISR
    uint32_t level = (user >> 7);
    gpio_num_t gpio_num = (user & 0x7f);

    if (level) {
        REG_WRITE((GPIO_OUT_W1TS_REG), (1 << gpio_num));
    } else {
        REG_WRITE((GPIO_OUT_W1TC_REG), (1 << gpio_num));
    }
}

// Initialize all pins and send the initialization sequence to the display
static void st7789_init(_Context *context, FfxDisplayRotation rotation) {
    // Initialize non-SPI GPIOs (this is critical, especially if one of these pins is
    // part of the native SPI pins, even if the signal is set to -1)
    gpio_reset_pin(context->pinDC);
    gpio_reset_pin(context->pinReset);

    gpio_set_direction(context->pinDC, GPIO_MODE_OUTPUT);
    gpio_set_direction(context->pinReset, GPIO_MODE_OUTPUT);

    // if (NO_CS_PIN) {
    //     gpio_set_direction(10, GPIO_MODE_OUTPUT);
    //     gpio_set_level(10, 0);
    // }

    uint32_t cmdIndex = 0;
    while (true) {
        uint8_t cmd = st7789_init_sequence[cmdIndex++];

        // Wait psedo-command...
        if (cmd == CommandWait) {
            delay(st7789_init_sequence[cmdIndex++]);
            continue;
        }

        if (cmd == CommandResetPin) {
            gpio_set_level(context->pinReset, st7789_init_sequence[cmdIndex++]);
            continue;
        }

        // Done psedo-command...
        if (cmd == CommandDone) { break; }

        st7789_send(context, MessageTypeCommand, &cmd, 1);

        // ST7789 command + parameters
        uint8_t paramCount = st7789_init_sequence[cmdIndex++];

        // Setting the Addressing requires injecting the screen rotation.
        // We only support 2 rotations for now, since they can be managed
        // exclusively with setup commands; other modes require adjusting
        // the offsets of memory writes, since the 320x240 memory only
        // uses 240x240, so the 80x240 pixels that are not shown differ
        // depending on the rotation.
        if (cmd == CommandMADCTL) {
            uint8_t operand = 0;
            switch (rotation) {
                case FfxDisplayRotationRibbonBottom:
                    operand = 0;
                    break;
                case FfxDisplayRotationRibbonRight:
                    operand = (CommandMADCTL_1_page_column | CommandMADCTL_1_column);
                    break;
            }
            st7789_send(context, MessageTypeData, &operand, 1);

        } else {
            st7789_send(context, MessageTypeData, &st7789_init_sequence[cmdIndex], paramCount);
        }

        cmdIndex += paramCount;
    }
}

// Asynchronously send a fragment (240 x FRAGMENT_HEIGHT) to the
// display using DMA. This will return immediately, and a call to
// the st7789_await_fragment function is required to the wait
// for these transactions to complete. Between the calls to
// st7789_asend_fragment and st7789_await_fragment the CPU is
// free to perform other tasks.
static void st7789_asend_fragment(_Context *context) {

    // The current y position
    uint32_t y = context->currentY;
    context->transactions[1].tx_data[0] = y >> 8;                           // Start row (high)
    context->transactions[1].tx_data[1] = y & 0xff;                         // start row (low)
    context->transactions[1].tx_data[2] = (y + FfxDisplayFragmentHeight - 1) >> 8;    // End row (high)
    context->transactions[1].tx_data[3] = (y + FfxDisplayFragmentHeight - 1) & 0xff;  // End row (low)

    // Fragment data
    context->transactions[3].tx_buffer = context->fragments[context->inflightFragment];

    // Queue and send (asynchronously) all command and data transactions for this fragment
    for (int i = 0; i < 4; i++) {
       esp_err_t result = spi_device_queue_trans(context->spi, &(context->transactions[i]), portMAX_DELAY);
       assert(result == ESP_OK);
        // DEBUG: SYNC; comment onut await calls
        //esp_err_t result = spi_device_polling_transmit(context->spi, &(context->transactions[i]));
        //assert(result == ESP_OK);
    }
}

// Wait for all the asynchronously sent transactions to complete.
// See: st7789_asend_fragment
static void st7789_await_fragment(_Context *context) {

    // Wait for all in-flight transactions are done
    spi_transaction_t *transaction;
    for (int i = 0; i < 4; i++) {
        esp_err_t result = spi_device_get_trans_result(context->spi, &transaction, portMAX_DELAY);
        assert(result == ESP_OK);
    }
}

// Initialize the display driver for the ST7789 on a SPI bus. This
// requires using malloc because the memory acquired must be
// DMA-compatible.
FfxDisplayContext ffx_display_init(FfxDisplaySpiBus spiBus, uint8_t pinDC,
        uint8_t pinReset, FfxDisplayRotation rotation,
        FfxRenderFunc renderFunc, void *renderContext) {

    // Check the dimensions are safe (the #error checks this too)
    assert((DISPLAY_HEIGHT % FfxDisplayFragmentHeight) == 0);

    _Context *context = malloc(sizeof(_Context));
    memset(context, 0, sizeof(_Context));
    for (int i = 0; i < 2; i++) {
        size_t byteCount = DISPLAY_WIDTH * FfxDisplayFragmentHeight * 2;
        uint8_t *data = heap_caps_malloc(byteCount, MALLOC_CAP_DMA);
        assert(data != NULL && (((int)(data)) % 4) == 0);
        memset(data, 0, byteCount);
        context->fragments[i] = data;
    }

    context->renderFunc = renderFunc;
    context->context = renderContext;

    context->inflightFragment = -1;

    // GPIO pins
    context->pinDC = pinDC;
    context->pinReset = pinReset;

    // Current top Y coordinate to render
    context->currentY = 0;

    // Setup the Transaction parameters that are the same (ish) for all display updates
    for (uint32_t i = 0; i < 4; i++) {
        memset(&(context->transactions[i]), 0, sizeof(spi_transaction_t));
        context->transactions[i].rx_buffer = NULL;
        context->transactions[i].flags = SPI_TRANS_USE_TXDATA;
    }

    // Page Address Set - Command
    context->transactions[0].length = 8;
    context->transactions[0].tx_data[0] = CommandRASET;
    context->transactions[0].user = st7789_wrapTransaction(context, MessageTypeCommand);

    // Page Address Set - Value
    context->transactions[1].length = 8 * 4;
    context->transactions[1].user = st7789_wrapTransaction(context, MessageTypeData);

    // Memory Write - Command
    context->transactions[2].length = 8;
    context->transactions[2].tx_data[0] = CommandRAMWR;
    context->transactions[2].user = st7789_wrapTransaction(context, MessageTypeCommand);

    // Memory Write - Value (remove the SPI_TRANS_USE_TXDATA flag)
    context->transactions[3].length = DISPLAY_WIDTH * 8 * 2 * FfxDisplayFragmentHeight;
    context->transactions[3].user = st7789_wrapTransaction(context, MessageTypeData);
    context->transactions[3].flags = 0;

    // Get the selected device macro; @TODO: encode this into SPI_BUS
    spi_host_device_t hostDevice = _DECODE_SPI_BUS_HOST(spiBus);

    // Bus Configuration
    {
        spi_bus_config_t busConfig = {
            .miso_io_num = -1,    // _DECODE_SPI_BUS_MISO(spiBus),
            .mosi_io_num = _DECODE_SPI_BUS_MOSI(spiBus),
            .sclk_io_num = _DECODE_SPI_BUS_SCLK(spiBus),
            .max_transfer_sz = FfxDisplayFragmentHeight * DISPLAY_WIDTH * 2 + 8,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .flags = 0
        };

        //printf("[disp] SPI Bus: MOSI=%d, CLK=%d\n", busConfig.mosi_io_num, busConfig.sclk_io_num);

        esp_err_t result = spi_bus_initialize(hostDevice, &busConfig, SPI_DMA_CH_AUTO);
        assert(result == ESP_OK);
    }

    // Device Interface Configuration
    spi_device_interface_config_t devConfig = {
        .clock_speed_hz = SPI_MASTER_FREQ_40M,           // Clock out at 40 MHz

        // For ST7789 w/ a CS pin, which needs to be pulled low
        .mode = 0,                                       // SPI mode 0 (CPOL = 0, CPHA = 0)
        .spics_io_num = _DECODE_SPI_BUS_CS0(spiBus),     // CS pin (Chip Select)

        .queue_size = 7,                                 // Allow 7 in-flight transactions
        .pre_cb = st7789_spi_pre_transfer_callback,      // Handles the D/C gpio (Data/Command)
        .flags = 0 //SPI_DEVICE_NO_DUMMY,
    };

    // For ST7789 w/o a CS (i.e. pulled to ground)
    //if (NO_CS_PIN) {

    // SPI mode 3 (CPOL = 1, CPHA = 1)
    devConfig.mode = 3;

    // CS pin (not used)
    devConfig.spics_io_num = -1;

    //}

    // Create a low-speed SPI device for initialization
    esp_err_t result = spi_bus_add_device(hostDevice, &devConfig, &(context->spi));
    assert (result == ESP_OK);

    // if (NO_CS_PIN) {
    //     gpio_reset_pin(10);
    //     gpio_set_direction(10, GPIO_MODE_OUTPUT);
    //     gpio_set_level(10, 0);
    //     delay(10);
    // }

    // Initialize the display controller (with the low-speed SPI device)
    st7789_init(context, rotation);

    // Remove the low-speed SPI and replace it with a high-speed one
    result = spi_bus_remove_device(context->spi);
    assert (result == ESP_OK);

    // Probably not necessary; ensure the old low-speed SPI device does not interfere with the new high-speed one
    memset(&(context->spi), 0, sizeof(spi_device_handle_t));

    // Now configure a high-speed SPI interface for sending fragments
    devConfig.clock_speed_hz = SPI_MASTER_FREQ_80M;
    result = spi_bus_add_device(hostDevice, &devConfig, &(context->spi));
    assert (result == ESP_OK);

    // Bookkeeping for statistics
    context->frame = 0;
    context->frameCount = 0;
    context->t0 = ticks();

    return context;
}

// 565  5 3   3 5
//#define RGB_HI(V)  (((V) & 0xf8) | ((V) & 0xfc) >> 5)
//#define RGB_LO(V)  ((((V) & 0xfc) << 3) | ((V) & 0xf8) >> 3)

// Release the resources for this display driver
void ffx_display_free(FfxDisplayContext context) {
    heap_caps_free(((_Context*)context)->fragments[0]);
    heap_caps_free(((_Context*)context)->fragments[1]);
    free(context);
}

uint16_t ffx_display_fps(FfxDisplayContext _context) {
    _Context *context = _context;
    if (!context) { return 0; }
    return context->fps;
}

// Render a fragment against the scene graph. This and the scene graph handles
// snapshots of its state so it can be updated freely.
uint32_t ffx_display_renderFragment(FfxDisplayContext _context) {

    _Context *context = _context;

    context->frame++;

    // Advance the fragment starting Y
    uint32_t y0 = context->currentY;

    // Select the free fragment (keep in mind inflightFragment can be -1, 0, or 1)
    uint8_t backbufferFragment = (context->inflightFragment == 0) ? 1: 0;

    //scene_render(scene, context->fragments[backbufferFragment], y0, DisplayFragmentHeight);
    context->renderFunc(context->fragments[backbufferFragment], y0, context->context);

    // Wait for the previous (if any; first time does not) transactions to complete
    if (context->inflightFragment != -1) {
        st7789_await_fragment(context);
    }

    // Swap inflight with backbuffer fragments
    context->inflightFragment = backbufferFragment;

    // Send the new fragment we just generated in the backbuffer (asynchronously)
    st7789_asend_fragment(context);

    context->currentY += FfxDisplayFragmentHeight;

    // The last fragment...
    if (context->currentY == DISPLAY_HEIGHT) {
        context->currentY = 0;

        // Update statistics and optionally dump them to the terminal
        context->frameCount++;

        // Update the FPS stats every 1s
        uint32_t now = ticks();
        uint32_t dt = now - context->t0;
        if (dt > 1000) {
            context->fps = 1000 * context->frameCount / dt;
            context->frameCount = 0;
            context->t0 = now;
        }

        // Return that the frame is complete
        return 1;
    }

    return 0;
}
