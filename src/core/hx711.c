/*
 * GBWUI — HX711 bit-bang driver
 *
 * Timing: PD_SCK HIGH/LOW minimum is 0.2 µs per HX711 datasheet.
 * esp_rom_delay_us(1) gives ~1 µs which is comfortably within limits.
 * DOUT is valid within 100 ns of PD_SCK falling edge — no extra delay needed.
 */

#include "hx711.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TARE_SAMPLES  16   /* averaged for zero offset; ~200 ms at 80 Hz */

static gpio_num_t s_dout;
static gpio_num_t s_pd_sck;
static int32_t    s_tare_offset = 0;

void hx711_init(gpio_num_t dout, gpio_num_t pd_sck)
{
    s_dout   = dout;
    s_pd_sck = pd_sck;

    gpio_reset_pin(s_dout);
    gpio_set_direction(s_dout, GPIO_MODE_INPUT);

    gpio_reset_pin(s_pd_sck);
    gpio_set_direction(s_pd_sck, GPIO_MODE_OUTPUT);
    gpio_set_level(s_pd_sck, 0);   /* keep CLK low = powered-up, ready */

    s_tare_offset = 0;
}

bool hx711_is_ready(void)
{
    return gpio_get_level(s_dout) == 0;
}

/*
 * Read one 24-bit sample from the HX711 and issue the 25th CLK pulse
 * to select Channel A / Gain 128 for the next conversion.
 * Caller must ensure hx711_is_ready() before calling.
 */
static int32_t read_raw(void)
{
    uint32_t raw = 0;

    for (int i = 0; i < 24; i++) {
        gpio_set_level(s_pd_sck, 1);
        esp_rom_delay_us(1);
        raw = (raw << 1) | (uint32_t)gpio_get_level(s_dout);
        gpio_set_level(s_pd_sck, 0);
        esp_rom_delay_us(1);
    }

    /* 25th pulse: sets Channel A, Gain 128 for next conversion */
    gpio_set_level(s_pd_sck, 1);
    esp_rom_delay_us(1);
    gpio_set_level(s_pd_sck, 0);
    esp_rom_delay_us(1);

    /* Sign-extend from 24-bit two's complement */
    if (raw & 0x800000u)
        raw |= 0xFF000000u;

    return (int32_t)raw;
}

bool hx711_wait_ready(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!hx711_is_ready()) {
        if (xTaskGetTickCount() >= deadline)
            return false;
        vTaskDelay(1);
    }
    return true;
}

bool hx711_tare(void)
{
    int64_t sum = 0;
    for (int i = 0; i < TARE_SAMPLES; i++) {
        if (!hx711_wait_ready(500))
            return false;
        sum += read_raw();
    }
    s_tare_offset = (int32_t)(sum / TARE_SAMPLES);
    return true;
}

bool hx711_read_grams(float cal_factor, float *out_g)
{
    if (!hx711_is_ready())
        return false;

    int32_t raw = read_raw() - s_tare_offset;
    *out_g = (float)raw * cal_factor;
    return true;
}

void hx711_power_cycle(void)
{
    gpio_set_level(s_pd_sck, 1);
    esp_rom_delay_us(100);  /* >60 µs enters power-down mode */
    gpio_set_level(s_pd_sck, 0);
    /* Next conversion will be ready in ~100 ms (80 Hz ODR) */
}
