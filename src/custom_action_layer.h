#ifndef CUSTOM_ACTION_LAYER_H_
#define CUSTOM_ACTION_LAYER_H_

#include <stdbool.h>

int custom_action_layer_init(void);
void custom_action_layer_set_num_lock(bool enabled);
bool custom_action_layer_is_num_lock_active(void);

#endif /* CUSTOM_ACTION_LAYER_H_ */
