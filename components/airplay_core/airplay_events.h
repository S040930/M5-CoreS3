#pragma once

void airplay_publish_client_connected(const char *device_name, const char *peer_address);
void airplay_publish_client_disconnected(void);
void airplay_publish_playback_start(void);
void airplay_publish_playback_pause(void);
void airplay_publish_playback_stop(void);
void airplay_publish_stream_start(void);
void airplay_publish_stream_stop(void);
