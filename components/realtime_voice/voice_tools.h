#pragma once

#include <stdbool.h>
#include <stddef.h>

bool voice_tools_dispatch(const char *name, const char *arguments_json, char *output, size_t output_cap);
