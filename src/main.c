#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "key_matrix.h"

int main(void)
{
	int ret = key_matrix_init();

	if (ret != 0) {
		printk("key matrix init failed: %d\n", ret);
		return ret;
	}

	printk("keyboard app ready\n");
	return 0;
}
