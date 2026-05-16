#ifndef POWER_MODULE_H_
#define POWER_MODULE_H_

#include <stdbool.h>
#include <stdint.h>

struct power_module_status {
	bool valid;
	bool charging;
	bool full;
	int battery_percent;
	int32_t battery_mv;
};

int power_module_init(void);
void power_module_get_status(struct power_module_status *status);

#endif /* POWER_MODULE_H_ */
