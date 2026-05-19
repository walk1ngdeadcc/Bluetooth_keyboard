#ifndef BLE_HOST_ACTION_SERVICE_H_
#define BLE_HOST_ACTION_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

int ble_host_action_service_init(void);
bool ble_host_action_service_is_ready(void);
int ble_host_action_service_send_trigger(uint8_t slot);

#endif /* BLE_HOST_ACTION_SERVICE_H_ */
