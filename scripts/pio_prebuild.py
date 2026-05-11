"""PlatformIO pre-build script: single-source config generation.

Reads config/config.toml as the single source of truth and generates
sdkconfig.defaults.

Also:
  - Sets ESP_IDF_VERSION env for Kconfig backward-compat.
  - Cleans non-essential directories from managed_components.
  - Writes config_hash.h for firmware-side config audit.
  - Hash-based incremental: only rewrites defaults when content changes.
"""

import hashlib
import os
import shutil
import sys

try:
    import tomllib
except ImportError:
    try:
        import tomli as tomllib
    except ImportError:
        sys.exit("tomli not installed. Run: pip install tomli")


def _identity(v, _ctx):
    return v


def _bool_y(v, _ctx):
    return "y" if v else None


def _bool_n(v, _ctx):
    return "n" if v else None


def _quoted(v, _ctx):
    return '"%s"' % v


def _int_str(v, _ctx):
    return str(int(v))


CONFIG_MAP = {
    "wifi.ssid":                       ("CONFIG_WIFI_SSID",               _quoted),
    "wifi.password":                   ("CONFIG_WIFI_PASSWORD",           _quoted),
    "wifi.enable_wpa3_sae":            ("CONFIG_ESP_WIFI_ENABLE_WPA3_SAE", _bool_y),
    "wifi.enable_compile_time_creds":  ("CONFIG_WIFI_CREDENTIALS_COMPILE_TIME", _bool_y),
    "wifi.buffers.static_rx_num":      ("CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM", _int_str),
    "wifi.buffers.dynamic_rx_num":     ("CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM", _int_str),
    "wifi.buffers.dynamic_tx_num":     ("CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM", _int_str),
    "wifi.event_task.stack_size":      ("CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE", _int_str),

    "airplay.force_v1":                ("CONFIG_AIRPLAY_FORCE_V1",        _bool_y),

    "audio.output.target_latency_ms":  ("CONFIG_AUDIO_TARGET_LATENCY_MS", _int_str),
    "audio.output.max_buffer_ms":      ("CONFIG_AUDIO_MAX_BUFFER_MS",     _int_str),
    "audio.output.mono_downmix":       ("CONFIG_OUTPUT_MONO",             _bool_y),
    "audio.resampler.taps":            ("CONFIG_RESAMPLER_TAPS",          _int_str),

    "voice.enabled_at_boot":           ("CONFIG_VOICE_MODE_DEFAULT_ENABLED", _bool_y),
    "voice.url":                       ("CONFIG_VOICE_REALTIME_URL",      _quoted),
    "voice.api_key":                   ("CONFIG_VOICE_API_KEY",           _quoted),
    "voice.model":                     ("CONFIG_VOICE_MODEL",             _quoted),
    "voice.input_sample_rate":         ("CONFIG_VOICE_INPUT_SAMPLE_RATE", _int_str),
    "voice.output_sample_rate":        ("CONFIG_VOICE_OUTPUT_SAMPLE_RATE", _int_str),
    "voice.uplink_frame_ms":           ("CONFIG_VOICE_UPLINK_FRAME_MS",   _int_str),

    "voice.omni.voice":                ("CONFIG_VOICE_OMNI_VOICE",        _quoted),

    "voice.transcription.enabled":      ("CONFIG_VOICE_ENABLE_INPUT_TRANSCRIPTION", _bool_y),
    "voice.transcription.model":        ("CONFIG_VOICE_INPUT_TRANSCRIPTION_MODEL", _quoted),
    "voice.tools.enabled":              ("CONFIG_VOICE_TOOLS_ENABLE", _bool_y),

    "env.enabled":                    ("CONFIG_ENV_MONITOR_ENABLE", _bool_y),
    "env.poll_interval_sec":          ("CONFIG_ENV_MONITOR_POLL_INTERVAL_SEC", _int_str),
    "env.cooldown_sec":               ("CONFIG_ENV_MONITOR_COOLDOWN_SEC", _int_str),
    "env.temp_hysteresis_c":          ("CONFIG_ENV_TEMP_HYSTERESIS_C", _int_str),
    "env.humidity_hysteresis_pct":    ("CONFIG_ENV_HUMIDITY_HYSTERESIS_PCT", _int_str),
    "env.summer_temp_min_c":          ("CONFIG_ENV_SUMMER_TEMP_MIN_C", _int_str),
    "env.summer_temp_max_c":          ("CONFIG_ENV_SUMMER_TEMP_MAX_C", _int_str),
    "env.summer_humidity_min_pct":    ("CONFIG_ENV_SUMMER_HUMIDITY_MIN_PCT", _int_str),
    "env.summer_humidity_max_pct":    ("CONFIG_ENV_SUMMER_HUMIDITY_MAX_PCT", _int_str),
    "env.winter_temp_min_c":          ("CONFIG_ENV_WINTER_TEMP_MIN_C", _int_str),
    "env.winter_temp_max_c":          ("CONFIG_ENV_WINTER_TEMP_MAX_C", _int_str),
    "env.winter_humidity_min_pct":    ("CONFIG_ENV_WINTER_HUMIDITY_MIN_PCT", _int_str),
    "env.winter_humidity_max_pct":    ("CONFIG_ENV_WINTER_HUMIDITY_MAX_PCT", _int_str),
    "env.debug_log":                  ("CONFIG_ENV_MONITOR_DEBUG_LOG", _bool_y),

    "voice.activation.enabled":         ("CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE", _bool_y),
    "voice.activation.phrase":          ("CONFIG_VOICE_ACTIVATION_PHRASE", _quoted),
    "voice.activation.followup_window_ms": ("CONFIG_VOICE_ACTIVATION_FOLLOWUP_WINDOW_MS", _int_str),
    "voice.vad.silence_timeout_ms":    ("CONFIG_VOICE_VAD_SILENCE_TIMEOUT_MS", _int_str),
    "voice.vad.min_speech_ms":         ("CONFIG_VOICE_VAD_MIN_SPEECH_MS", _int_str),
    "voice.vad.consecutive_frames":    ("CONFIG_VOICE_VAD_CONSECUTIVE_FRAMES", _int_str),

    "voice.session.idle_timeout_ms":    ("CONFIG_VOICE_SESSION_IDLE_TIMEOUT_MS", _int_str),
    "voice.session.context_max_chars":  ("CONFIG_VOICE_CONTEXT_MAX_CHARS", _int_str),

    "voice.health.append_timeout_ms":   ("CONFIG_VOICE_APPEND_HEALTH_TIMEOUT_MS", _int_str),
    "voice.health.debug_event_log":     ("CONFIG_VOICE_DEBUG_EVENT_LOG",  _bool_y),
    "voice.debug.tls_heap_log":         ("CONFIG_VOICE_DEBUG_TLS_HEAP", _bool_y),

    "voice.capture.mic_in_gain_db":       ("CONFIG_VOICE_MIC_IN_GAIN_DB",    _int_str),
    "voice.capture.stall_timeout_ms":   ("CONFIG_VOICE_CAPTURE_STALL_TIMEOUT_MS", _int_str),
    "voice.capture.recovery_retry_max": ("CONFIG_VOICE_CAPTURE_RECOVERY_RETRY_MAX", _int_str),
    "voice.capture.stall_cycle_max":    ("CONFIG_VOICE_CAPTURE_STALL_CYCLE_MAX", _int_str),
    "voice.capture.recovery_cooldown_ms": ("CONFIG_VOICE_CAPTURE_RECOVERY_COOLDOWN_MS", _int_str),

    "voice.activation.wake_model":     ("CONFIG_VOICE_WAKE_MODEL",        _quoted),

    "system.cpu_freq_mhz":             ("__SYSTEM_CPU_FREQ_MHZ_PLACEHOLDER__", _int_str),
    "system.monitor_baud":             ("CONFIG_ESPTOOLPY_MONITOR_BAUD",   _int_str),

    "system.log.enable_colors":        ("CONFIG_LOG_COLORS",               _bool_y),

    "system.lwip.max_sockets":         ("CONFIG_LWIP_MAX_SOCKETS",         _int_str),

    "system.freertos.allow_ext_mem_task_create": ("CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM", _bool_y),

    "system.spiram.spiram_malloc_always_internal": ("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL", _int_str),

    "tls.default_mem_alloc":           ("CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC", _bool_y),
    "tls.certificate_bundle":          ("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", _bool_y),
    "tls.certificate_bundle_default_cmn": ("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN", _bool_y),

    "mdns.max_services":               ("CONFIG_MDNS_MAX_SERVICES",         _int_str),
    "mdns.task_from_spiram":           ("CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM", _bool_y),
    "mdns.memory_alloc_spiram":        ("CONFIG_MDNS_MEMORY_ALLOC_SPIRAM",  _bool_y),

    "lvgl.build_examples":             ("CONFIG_LV_BUILD_EXAMPLES",         _bool_n),
    "lvgl.build_demos":                ("CONFIG_LV_BUILD_DEMOS",            _bool_n),
    "lvgl.cjk_font_enabled":           ("CONFIG_LV_USE_FONT_SOURCE_HAN_SANS_SC_16_CJK", _bool_y),

    "ui.anim_interval_ms":             ("CONFIG_SCREEN_UI_ANIM_INTERVAL_MS", _int_str),
    "ui.anim_stress_interval_ms":     ("CONFIG_SCREEN_UI_ANIM_STRESS_INTERVAL_MS", _int_str),
}

CHOICE_MAP = {
    "audio.output.sample_rate_hz": {
        44100: [("CONFIG_OUTPUT_SAMPLE_RATE_44100", "y"),
                ("CONFIG_OUTPUT_SAMPLE_RATE_48000", None),
                ("CONFIG_OUTPUT_SAMPLE_RATE_HZ", "44100")],
        48000: [("CONFIG_OUTPUT_SAMPLE_RATE_44100", None),
                ("CONFIG_OUTPUT_SAMPLE_RATE_48000", "y"),
                ("CONFIG_OUTPUT_SAMPLE_RATE_HZ", "48000")],
    },
    "audio.output.fidelity_mode": {
        "pure":     [("CONFIG_AUDIO_FIDELITY_MODE_PURE", "y"),
                     ("CONFIG_AUDIO_FIDELITY_MODE_ENHANCED", None)],
        "enhanced": [("CONFIG_AUDIO_FIDELITY_MODE_PURE", None),
                     ("CONFIG_AUDIO_FIDELITY_MODE_ENHANCED", "y")],
    },
    "voice.session.prompt_preset": {
        "balanced":        [("CONFIG_VOICE_PROMPT_PRESET_BALANCED", "y"),
                            ("CONFIG_VOICE_PROMPT_PRESET_CONVERSATIONAL", None),
                            ("CONFIG_VOICE_PROMPT_PRESET_FACTUAL", None)],
        "conversational":  [("CONFIG_VOICE_PROMPT_PRESET_BALANCED", None),
                            ("CONFIG_VOICE_PROMPT_PRESET_CONVERSATIONAL", "y"),
                            ("CONFIG_VOICE_PROMPT_PRESET_FACTUAL", None)],
        "factual":         [("CONFIG_VOICE_PROMPT_PRESET_BALANCED", None),
                            ("CONFIG_VOICE_PROMPT_PRESET_CONVERSATIONAL", None),
                            ("CONFIG_VOICE_PROMPT_PRESET_FACTUAL", "y")],
    },
    "voice.activation.wake_model": {
        "wn9_hiesp":       [("CONFIG_VOICE_WAKE_HIESP", "y"),
                            ("CONFIG_VOICE_WAKE_HIESP_9S", None),
                            ("CONFIG_VOICE_WAKE_NIHAOXIAOZHI", None),
                            ("CONFIG_VOICE_WAKE_ALEXA", None),
                            ("CONFIG_VOICE_WAKE_JARVIS", None),
                            ("CONFIG_VOICE_WAKE_HIJOY", None),
                            ("CONFIG_VOICE_WAKE_COMPUTER", None),
                            ("CONFIG_SR_WN_WN9_HIESP", "y"),
                            ("# CONFIG_SR_WN_WN9S_HIESP is not set", None)],
        "wn9s_hiesp":      [("CONFIG_VOICE_WAKE_HIESP", None),
                            ("CONFIG_VOICE_WAKE_HIESP_9S", "y"),
                            ("CONFIG_VOICE_WAKE_NIHAOXIAOZHI", None),
                            ("CONFIG_VOICE_WAKE_ALEXA", None),
                            ("CONFIG_VOICE_WAKE_JARVIS", None),
                            ("CONFIG_VOICE_WAKE_HIJOY", None),
                            ("CONFIG_VOICE_WAKE_COMPUTER", None),
                            ("# CONFIG_SR_WN_WN9_HIESP is not set", None),
                            ("CONFIG_SR_WN_WN9S_HIESP", "y")],
        "wn9_nihaoxiaozhi_tts": [("CONFIG_VOICE_WAKE_HIESP", None),
                            ("CONFIG_VOICE_WAKE_HIESP_9S", None),
                            ("CONFIG_VOICE_WAKE_NIHAOXIAOZHI", "y"),
                            ("CONFIG_VOICE_WAKE_ALEXA", None),
                            ("CONFIG_VOICE_WAKE_JARVIS", None),
                            ("CONFIG_VOICE_WAKE_HIJOY", None),
                            ("CONFIG_VOICE_WAKE_COMPUTER", None),
                            ("CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS", "y"),
                            ("# CONFIG_SR_WN_WN9S_NIHAOXIAOZHI is not set", None)],
        "wn9_alexa":       [("CONFIG_VOICE_WAKE_HIESP", None),
                            ("CONFIG_VOICE_WAKE_HIESP_9S", None),
                            ("CONFIG_VOICE_WAKE_NIHAOXIAOZHI", None),
                            ("CONFIG_VOICE_WAKE_ALEXA", "y"),
                            ("CONFIG_VOICE_WAKE_JARVIS", None),
                            ("CONFIG_VOICE_WAKE_HIJOY", None),
                            ("CONFIG_VOICE_WAKE_COMPUTER", None),
                            ("CONFIG_SR_WN_WN9_ALEXA", "y")],
        "wn9_jarvis_tts":  [("CONFIG_VOICE_WAKE_HIESP", None),
                            ("CONFIG_VOICE_WAKE_HIESP_9S", None),
                            ("CONFIG_VOICE_WAKE_NIHAOXIAOZHI", None),
                            ("CONFIG_VOICE_WAKE_ALEXA", None),
                            ("CONFIG_VOICE_WAKE_JARVIS", "y"),
                            ("CONFIG_VOICE_WAKE_HIJOY", None),
                            ("CONFIG_VOICE_WAKE_COMPUTER", None),
                            ("CONFIG_SR_WN_WN9_JARVIS_TTS", "y")],
        "wn9_hijoy_tts":   [("CONFIG_VOICE_WAKE_HIESP", None),
                            ("CONFIG_VOICE_WAKE_HIESP_9S", None),
                            ("CONFIG_VOICE_WAKE_NIHAOXIAOZHI", None),
                            ("CONFIG_VOICE_WAKE_ALEXA", None),
                            ("CONFIG_VOICE_WAKE_JARVIS", None),
                            ("CONFIG_VOICE_WAKE_HIJOY", "y"),
                            ("CONFIG_VOICE_WAKE_COMPUTER", None),
                            ("CONFIG_SR_WN_WN9_HIJOY_TTS", "y")],
        "wn9_computer_tts": [("CONFIG_VOICE_WAKE_HIESP", None),
                            ("CONFIG_VOICE_WAKE_HIESP_9S", None),
                            ("CONFIG_VOICE_WAKE_NIHAOXIAOZHI", None),
                            ("CONFIG_VOICE_WAKE_ALEXA", None),
                            ("CONFIG_VOICE_WAKE_JARVIS", None),
                            ("CONFIG_VOICE_WAKE_HIJOY", None),
                            ("CONFIG_VOICE_WAKE_COMPUTER", "y"),
                            ("CONFIG_SR_WN_WN9_COMPUTER_TTS", "y")],
    },
    "system.cpu_freq_mhz": {
        240: [("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240", "y"),
              ("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ", "240"),
              ("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160", None),
              ("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80", None)],
        160: [("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240", None),
              ("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ", "160"),
              ("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160", "y"),
              ("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80", None)],
        80:  [("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240", None),
              ("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ", "80"),
              ("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160", None),
              ("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80", "y")],
    },
    "system.flash_mode": {
        "qio":  [("CONFIG_ESPTOOLPY_FLASHMODE_QIO", "y"),
                 ("CONFIG_ESPTOOLPY_FLASHMODE", '"qio"'),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_QOUT", None),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_DIO", None),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_DOUT", None)],
        "qout": [("CONFIG_ESPTOOLPY_FLASHMODE_QIO", None),
                 ("CONFIG_ESPTOOLPY_FLASHMODE", '"qout"'),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_QOUT", "y"),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_DIO", None),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_DOUT", None)],
        "dio":  [("CONFIG_ESPTOOLPY_FLASHMODE_QIO", None),
                 ("CONFIG_ESPTOOLPY_FLASHMODE", '"dio"'),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_QOUT", None),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_DIO", "y"),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_DOUT", None)],
        "dout": [("CONFIG_ESPTOOLPY_FLASHMODE_QIO", None),
                 ("CONFIG_ESPTOOLPY_FLASHMODE", '"dout"'),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_QOUT", None),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_DIO", None),
                 ("CONFIG_ESPTOOLPY_FLASHMODE_DOUT", "y")],
    },
    "system.flash_size_mb": {
        2:  [("CONFIG_ESPTOOLPY_FLASHSIZE_2MB", "y"),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_4MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_8MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_16MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE", '"2MB"')],
        4:  [("CONFIG_ESPTOOLPY_FLASHSIZE_2MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_4MB", "y"),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_8MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_16MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE", '"4MB"')],
        8:  [("CONFIG_ESPTOOLPY_FLASHSIZE_2MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_4MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_8MB", "y"),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_16MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE", '"8MB"')],
        16: [("CONFIG_ESPTOOLPY_FLASHSIZE_2MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_4MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_8MB", None),
             ("CONFIG_ESPTOOLPY_FLASHSIZE_16MB", "y"),
             ("CONFIG_ESPTOOLPY_FLASHSIZE", '"16MB"')],
    },
    "system.spiram_mode": {
        "quad": [("CONFIG_SPIRAM_MODE_QUAD", "y"),
                 ("CONFIG_SPIRAM_MODE_OCT", None)],
        "oct":  [("CONFIG_SPIRAM_MODE_QUAD", None),
                 ("CONFIG_SPIRAM_MODE_OCT", "y")],
    },
    "system.console": {
        "usb_serial_jtag": [("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG", "y"),
                            ("CONFIG_ESP_CONSOLE_UART_DEFAULT", None),
                            ("CONFIG_ESP_CONSOLE_SECONDARY_NONE", "y"),
                            ("CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG", None)],
        "uart":            [("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG", None),
                            ("CONFIG_ESP_CONSOLE_UART_DEFAULT", "y"),
                            ("CONFIG_ESP_CONSOLE_SECONDARY_NONE", None),
                            ("CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG", None)],
    },
    "system.log.default_level": {
        "debug":   [("CONFIG_LOG_DEFAULT_LEVEL_DEBUG", "y"),
                    ("CONFIG_LOG_DEFAULT_LEVEL_INFO", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL_WARN", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL_ERROR", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL", "4")],
        "info":    [("CONFIG_LOG_DEFAULT_LEVEL_DEBUG", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL_INFO", "y"),
                    ("CONFIG_LOG_DEFAULT_LEVEL_WARN", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL_ERROR", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL", "3")],
        "warn":    [("CONFIG_LOG_DEFAULT_LEVEL_DEBUG", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL_INFO", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL_WARN", "y"),
                    ("CONFIG_LOG_DEFAULT_LEVEL_ERROR", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL", "2")],
        "error":   [("CONFIG_LOG_DEFAULT_LEVEL_DEBUG", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL_INFO", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL_WARN", None),
                    ("CONFIG_LOG_DEFAULT_LEVEL_ERROR", "y"),
                    ("CONFIG_LOG_DEFAULT_LEVEL", "1")],
    },
    "system.log.maximum_level": {
        "debug": [("CONFIG_LOG_MAXIMUM_LEVEL_DEBUG", "y"),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_INFO", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_WARN", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_ERROR", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL", "4")],
        "info":  [("CONFIG_LOG_MAXIMUM_LEVEL_DEBUG", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_INFO", "y"),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_WARN", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_ERROR", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL", "3")],
        "warn":  [("CONFIG_LOG_MAXIMUM_LEVEL_DEBUG", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_INFO", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_WARN", "y"),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_ERROR", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL", "2")],
        "error": [("CONFIG_LOG_MAXIMUM_LEVEL_DEBUG", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_INFO", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_WARN", None),
                  ("CONFIG_LOG_MAXIMUM_LEVEL_ERROR", "y"),
                  ("CONFIG_LOG_MAXIMUM_LEVEL", "1")],
    },
}

STATIC_DEFAULTS = [
    "# Generated from config/config.toml — do not edit directly.",
    "",
    "CONFIG_BOARD_M5STACK_CORES3=y",
    "CONFIG_SPIRAM=y",
    "CONFIG_SPIRAM_SPEED_80M=y",
    "CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y",
    # mbedTLS mem alloc mode: only [tls] in config.toml (no EXTERNAL here — it conflicted with DEFAULT).
    # Incoming handshake records (server cert chains) often exceed 4KB; too small causes handshake failures.
    "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384",
    "CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=16384",
    "CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=y",
    "CONFIG_LWIP_UDP_RECVMBOX_SIZE=64",
    "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64",
    "CONFIG_LWIP_TCP_RECVMBOX_SIZE=32",
    "CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096",
    "CONFIG_PARTITION_TABLE_CUSTOM=y",
    'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="components/board_cores3/partitions.csv"',
    "CONFIG_COMPILER_OPTIMIZATION_SIZE=y",
    "CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y",
    "CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK=y",
    # WakeNet model is now controlled by config.toml [voice.activation] wake_model.
    # Default to wn9_hiesp if not specified; do not hardcode here.
    # TLS encrypt from PSRAM + HW AES-DMA can exhaust internal DMA bounce buffers; software AES avoids it.
    "# CONFIG_MBEDTLS_HARDWARE_AES is not set",
    # CPU1 IDLE task watchdog: disabled because taskLVGL (priority 23) always preempts
    # IDLE1 (priority 0), causing spurious TWDT triggers. CPU0 IDLE0 remains monitored.
    "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n",
    "CONFIG_TASK_WDT_CHECK_IDLE_TASK_CPU1=n",
    "",
    # PM / DFS (Dynamic Frequency Scaling): CPU idles at 80 MHz, ramps to 160 MHz on load.
    "CONFIG_PM_ENABLE=y",
    "CONFIG_PM_DFS_IN_ISR=y",
    "CONFIG_PM_LIGHTSLEEP_RTC_OSC_CAL_INTERVAL=30",
    "",
]


def _nested_get(cfg, dotted_path):
    parts = dotted_path.split(".")
    cur = cfg
    for p in parts:
        if not isinstance(cur, dict) or p not in cur:
            return None
        cur = cur[p]
    return cur


def _generate_config_lines(cfg):
    lines = []
    seen_keys = set()

    for toml_path, (config_key, xform) in CONFIG_MAP.items():
        raw = _nested_get(cfg, toml_path)
        if raw is None:
            continue
        val = xform(raw, {})
        if val is not None:
            lines.append("%s=%s" % (config_key, val))
        else:
            lines.append("# %s is not set" % config_key)
        seen_keys.add(config_key)

    for toml_path, branches in CHOICE_MAP.items():
        raw = _nested_get(cfg, toml_path)
        if raw is None:
            continue
        branch = branches.get(raw)
        if branch is None:
            print("WARNING: unknown choice value '%s' for '%s'"
                  % (raw, toml_path))
            continue
        for config_key, val in branch:
            if val is not None:
                lines.append("%s=%s" % (config_key, val))
            else:
                lines.append("# %s is not set" % config_key)
            seen_keys.add(config_key)

    lines.sort()
    return lines


def _compute_hash(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _read_existing(filepath):
    if not os.path.isfile(filepath):
        return None
    with open(filepath, "r") as f:
        return f.read()


def _write_if_changed(filepath, new_content):
    old = _read_existing(filepath)
    if old is not None and _compute_hash(old) == _compute_hash(new_content):
        return False

    with open(filepath, "w") as f:
        f.write(new_content)

    return True


def _voice_kv_from_text(text):
    """Parse CONFIG_VOICE_* assignments from sdkconfig or defaults text."""
    out = {}
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if not s.startswith("CONFIG_VOICE_"):
            continue
        if "=" not in s:
            continue
        key, val = s.split("=", 1)
        out[key] = val
    return out


def _voice_defaults_mismatch_defaults_vs_sdkconfig(defaults_content, sdkconfig_path):
    """True if built sdkconfig disagrees with any CONFIG_VOICE_* line we emit in defaults."""
    want = _voice_kv_from_text(defaults_content)
    if not want:
        return False
    built_text = _read_existing(sdkconfig_path)
    if built_text is None:
        return True
    have = _voice_kv_from_text(built_text)
    for key, val in want.items():
        if have.get(key) != val:
            return True
    return False


def _write_config_hash(project_dir, cfg_content):
    h = _compute_hash(cfg_content)[:16]
    out_path = os.path.join(project_dir, "components", "app_core",
                            "config_hash.h")
    content = (
        "/* Auto-generated by pio_prebuild.py — do not edit. */\n"
        '#define CONFIG_BUILD_HASH "%s"\n' % h
    )
    _write_if_changed(out_path, content)


def _set_esp_idf_version():
    idf_ver = os.environ.get("PIDF_VER")
    if idf_ver is None:
        idf_ver = "5.3.2"
    os.environ.setdefault("ESP_IDF_VERSION", idf_ver)


def _cleanup_nonessential_dirs(project_dir):
    dirs_to_remove = [
        "lvgl__lvgl/examples",
        "lvgl__lvgl/demos",
        "lvgl__lvgl/tests",
        "espressif__mdns/examples",
        "espressif__mdns/tests",
        "espressif__esp32-camera/examples",
        "espressif__esp32-camera/test",
        "espressif__esp_codec_dev/test_apps",
        "espressif__esp_audio_codec/test_apps",
        "espressif__esp_lcd_ili9341/test_apps",
        "espressif__libsodium/test_apps",
    ]
    base_path = os.path.join(project_dir, "managed_components")
    for rel_path in dirs_to_remove:
        full_path = os.path.join(base_path, rel_path)
        if os.path.isdir(full_path):
            try:
                shutil.rmtree(full_path)
            except Exception:
                pass


def _main():
    project_dir = (
        os.environ.get("PROJECT_DIR")
        or os.environ.get("PLATFORMIO_PROJECT_DIR")
        or os.environ.get("PWD")
    )
    candidates = []
    if project_dir:
        candidates.append(os.path.abspath(project_dir))
    if len(sys.argv) > 1 and os.path.isdir(sys.argv[1]):
        candidates.append(os.path.abspath(sys.argv[1]))
    candidates.append(os.getcwd())
    script_path = os.path.abspath(sys.argv[0]) if len(sys.argv) > 0 else os.getcwd()
    candidates.append(os.path.abspath(os.path.join(os.path.dirname(script_path), "..")))

    resolved = None
    for base in candidates:
        cur = os.path.abspath(base)
        while True:
            probe = os.path.join(cur, "config", "config.toml")
            if os.path.isfile(probe):
                resolved = cur
                break
            parent = os.path.dirname(cur)
            if parent == cur:
                break
            cur = parent
        if resolved:
            break
    if resolved:
        project_dir = resolved
    elif candidates:
        project_dir = candidates[0]
    else:
        project_dir = os.getcwd()

    _set_esp_idf_version()

    toml_path = os.path.join(project_dir, "config", "config.toml")
    if not os.path.isfile(toml_path):
        print("  [config] config/config.toml not found; skipping generation")
        _cleanup_nonessential_dirs(project_dir)
        return

    with open(toml_path, "rb") as f:
        cfg = tomllib.load(f)

    voice_tr = cfg.get("voice", {}).get("transcription", {})
    if voice_tr.get("enabled") and voice_tr.get("model") not in (None, "", "gummy-realtime-v1"):
        print(
            "  [config] WARNING: voice.transcription.model should be "
            '"gummy-realtime-v1" for DashScope Qwen-Omni Realtime; got %r'
            % (voice_tr.get("model"),)
        )

    config_lines = _generate_config_lines(cfg)
    config_lines = [l for l in config_lines
                    if not l.startswith("__SYSTEM_CPU_FREQ_MHZ_PLACEHOLDER__")]

    content = "\n".join(STATIC_DEFAULTS + config_lines) + "\n"

    # Must match PlatformIO env folder `.pio/build/<PIOENV>/`, not hardware `meta.board`.
    pio_env = os.environ.get("PIOENV")
    build_subdir = pio_env if pio_env else "m5cores3"
    build_dir = os.path.join(project_dir, ".pio", "build", build_subdir)
    sdkconfig_path = os.path.join(build_dir, "sdkconfig")

    gen_dir = os.path.join(project_dir, "config", "generated")
    os.makedirs(gen_dir, exist_ok=True)

    gen_defaults = os.path.join(gen_dir, "sdkconfig.defaults")
    root_defaults = os.path.join(project_dir, "sdkconfig.defaults")

    changed_gen = _write_if_changed(gen_defaults, content)
    if os.path.exists(root_defaults):
        try:
            os.remove(root_defaults)
        except OSError as exc:
            print("  [config] WARNING: could not remove %s: %s" % (root_defaults, exc))
    stale_voice = _voice_defaults_mismatch_defaults_vs_sdkconfig(content, sdkconfig_path)

    if changed_gen or stale_voice:
        if os.path.isfile(sdkconfig_path):
            try:
                os.remove(sdkconfig_path)
            except OSError as exc:
                print("  [config] WARNING: could not remove %s: %s" % (sdkconfig_path, exc))
        if changed_gen:
            print("  [config] config.toml -> config/generated/sdkconfig.defaults; "
                  "root sdkconfig.defaults removed; sdkconfig cleared for regen")
        elif stale_voice:
            print("  [config] voice keys in sdkconfig out of sync with defaults; sdkconfig cleared")
    else:
        print("  [config] config/generated/sdkconfig.defaults unchanged; voice keys match built sdkconfig")

    _write_config_hash(project_dir, content)
    _cleanup_nonessential_dirs(project_dir)


_main()
