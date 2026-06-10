#ifndef PT_PINOUT_H
#define PT_PINOUT_H

/* ===== Panel Timings (from your original pinout) ===== */
#define PT_LCD_PCLK_HZ_MIN 14000000
/* PERF EXPERIMENT: 17 MHz (~40 Hz refresh, was 23 MHz / ~54 Hz). The frame buffer lives in
 * PSRAM, so scan-out competes with LVGL rendering for the same PSRAM bus; a lower pixel clock
 * cuts that always-on read traffic (and the bounce-buffer copy CPU tax) by ~25%. 40 Hz is still
 * well above what the render loop can produce (~10 FPS). Revert to 23000000 if the panel
 * flickers/shimmers. */
#define PT_LCD_PCLK_HZ 17000000

#define PT_LCD_H_RES 800
#define PT_LCD_HSYNC_PULSE_WIDTH 4
#define PT_LCD_HSYNC_BACK_PORCH 8
#define PT_LCD_HSYNC_FRONT_PORCH 8

#define PT_LCD_V_RES 480
#define PT_LCD_VSYNC_PULSE_WIDTH 4
#define PT_LCD_VSYNC_BACK_PORCH 16
#define PT_LCD_VSYNC_FRONT_PORCH 16

/* ===== Panel GPIOs ===== */
#define PT_LCD_PCLK_PIN 5
#define PT_LCD_DE_PIN 38
#define PT_LCD_RESET_PIN 46
#define PT_LCD_HSYNC_PIN -1
#define PT_LCD_VSYNC_PIN -1
/* Panel Data pins */
#ifndef PT_LCD_DATA0_PIN
#define PT_LCD_DATA0_PIN 17
#define PT_LCD_DATA1_PIN 18
#define PT_LCD_DATA2_PIN 48
#define PT_LCD_DATA3_PIN 47
#define PT_LCD_DATA4_PIN 39
#define PT_LCD_DATA5_PIN 11
#define PT_LCD_DATA6_PIN 12
#define PT_LCD_DATA7_PIN 13
#define PT_LCD_DATA8_PIN 14
#define PT_LCD_DATA9_PIN 15
#define PT_LCD_DATA10_PIN 16
#define PT_LCD_DATA11_PIN 6
#define PT_LCD_DATA12_PIN 7
#define PT_LCD_DATA13_PIN 8
#define PT_LCD_DATA14_PIN 9
#define PT_LCD_DATA15_PIN 10
#endif

/* Backlight range in percent */
#define PT_BL_PIN 21
#define PT_BL_MIN 0
#define PT_BL_MAX 100
#define PT_BL_LEDC_TIMER LEDC_TIMER_1
#define PT_BL_LEDC_SPEED_MODE LEDC_LOW_SPEED_MODE
#define PT_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define PT_BL_LEDC_RESOLUTION LEDC_TIMER_11_BIT
#define PT_BL_FREQUENCY_HZ 30000

/* --------- Pins / Panel size --------- */

#define PT_GT911_REG_STATUS 0x814E
#define PT_GT911_REG_POINT1 0x814F
#define PT_GT911_MAX_POINTS 5
#define PT_GT911_I2C_SCL_GPIO 1
#define PT_GT911_I2C_SDA_GPIO 2
#define PT_GT911_RST_GPIO 41
#define PT_GT911_INT_GPIO 40
#define PT_GT911_MAX_X PT_LCD_H_RES
#define PT_GT911_MAX_Y PT_LCD_V_RES

#endif /* PINOUT_H */
