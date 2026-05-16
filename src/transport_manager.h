#ifndef TRANSPORT_MANAGER_H_
#define TRANSPORT_MANAGER_H_

#include "mode_event.h"

int transport_manager_init(void);
enum app_mode transport_manager_get_active_mode(void);

#endif /* TRANSPORT_MANAGER_H_ */
