#pragma once

#include "esp_event.h"
#include "event_ids.h"
#include "event_types.h"

ESP_EVENT_DECLARE_BASE(EVENT_BUS_BASE);

esp_err_t event_bus_init(void);

esp_err_t event_bus_publish(event_id_t id, void *data, size_t data_size);

esp_err_t event_bus_subscribe(event_id_t id, esp_event_handler_t handler, void *handler_args);

esp_err_t event_bus_unsubscribe(event_id_t id, esp_event_handler_t handler);

#define EVENT_BUS_PUBLISH(_id) event_bus_publish(_id, NULL, 0)

#define EVENT_BUS_PUBLISH_WITH_DATA(_id, _data) event_bus_publish(_id, _data, sizeof(*(_data)))
