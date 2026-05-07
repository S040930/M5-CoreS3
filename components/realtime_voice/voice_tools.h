#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct cJSON cJSON;

bool voice_tools_dispatch(const char *name, const char *arguments_json, char *output, size_t output_cap);
void voice_tools_append_session_schemas(cJSON *tools);
