set( SRC_FILES
    "main.c"
    "receiver_state.c"
    "status_service.c"
    "airplay_service.c"
    "audio_pipeline.c"
    "system_actions.c"
    "usb_control_service.c"
    "alac_magic_cookie.c"
    "settings.c"
    "audio/audio_receiver.c"
    "audio/audio_stream.c"
    "audio/audio_stream_realtime.c"
    "audio/audio_stream_buffered.c"
    "audio/audio_decoder.c"
    "audio/audio_buffer.c"
    "audio/audio_timing.c"
    "audio/audio_crypto.c"
    "audio/audio_resample.c"
    "audio/audio_eq.c"
    "audio/audio_dsp.c"
    "rtsp/rtsp_server.c"
    "rtsp/rtsp_conn.c"
    "rtsp/rtsp_message.c"
    "rtsp/rtsp_handlers.c"
    "rtsp/rtsp_rsa.c"
    "rtsp/rtsp_events.c"
    "plist/plist_xml.c"
    "plist/bplist_parser.c"
    "plist/bplist_builder.c"
    "plist/base64.c"
    "network/wifi.c"
    "network/mdns_airplay.c"
    "network/ntp_clock.c"
    "network/socket_utils.c"
    "network/web_server.c"
    "network/dns_server.c"
    "network/structured_trace.c"
    "led.c"
    "audio/eq_events.c"
    "audio/audio_output_common.c"
    "audio/audio_output_cores3.c"
    "buttons.c"
    "playback_control.c"
    "dacp_client.c"
)

# W5500 SPI Ethernet
if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND SRC_FILES "network/ethernet.c")
endif()

if(CONFIG_ENABLE_DEV_DIAGNOSTICS)
    list(APPEND SRC_FILES "network/log_stream.c")
endif()

if(CONFIG_WEB_OTA_UPDATE)
    list(APPEND SRC_FILES "network/ota.c")
endif()

set( INC_DIRS
    "."
    "audio"
    "rtsp"
    "plist"
    "network"
)

set( DEPS
    driver
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_ringbuf
    esp_driver_i2s
    esp_timer
    esp_http_server
    esp_http_client
    json
    mbedtls
    esp_driver_ledc
    boards
    dac
    display
    audio-resampler
    esp_codec_dev
    m5stack_core_s3_noglib
    spiffs_storage
)

if(CONFIG_WEB_OTA_UPDATE)
    list(APPEND DEPS app_update)
endif()

if(CONFIG_BT_A2DP_ENABLE)
    list(APPEND DEPS bt)
endif()

if(CONFIG_ETH_W5500_ENABLED)
    list(APPEND DEPS esp_eth)
endif()

idf_component_register(
    SRCS ${SRC_FILES}
    INCLUDE_DIRS ${INC_DIRS}
    PRIV_REQUIRES ${DEPS}
)
