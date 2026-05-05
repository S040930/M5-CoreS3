#pragma once

#include "rtsp_handler_common.h"

/**
 * Event callback function type.
 * @param event The event that occurred
 * @param data  Event-specific data (NULL for events with no data)
 * @param user_data Pointer registered with rtsp_events_register()
 */
typedef void (*rtsp_event_callback_t)(rtsp_event_t event,
                                      const rtsp_event_data_t *data,
                                      void *user_data);

/**
 * Register a listener for RTSP events.
 * @param callback Function to call when an event occurs
 * @param user_data Pointer passed to callback (can be NULL)
 * @return 0 on success, -1 if max listeners reached
 */
int rtsp_events_register(rtsp_event_callback_t callback, void *user_data);

/**
 * Unregister a previously registered listener.
 * @param callback The callback to remove
 */
void rtsp_events_unregister(rtsp_event_callback_t callback);

/**
 * Emit an event to all registered listeners.
 * Called internally by RTSP handlers.
 * @param event The event to emit
 * @param data  Event-specific data (NULL for events with no data)
 */
void rtsp_events_emit(rtsp_event_t event, const rtsp_event_data_t *data);
