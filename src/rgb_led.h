#ifndef RGB_LED_H_
#define RGB_LED_H_

#include <stdint.h>

int rgb_led_init(void);
int rgb_led_set_all_red(uint8_t level);
int rgb_led_off(void);

#endif /* RGB_LED_H_ */
