#pragma once

#include <kestrel.h>
#include <string.h>

struct talon_type_info {
    const char *name;
    const char *default_mime;
};

static const struct talon_type_info talon_types[] = {
    [K_PARCEL_TEXT]   = { "text",   "text/plain" },
    [K_PARCEL_PATH]   = { "path",   "text/x-path" },
    [K_PARCEL_URL]    = { "url",    "text/uri-list" },
    [K_PARCEL_COLOR]  = { "color",  "text/x-color" },
    [K_PARCEL_NUMBER] = { "number", "text/x-number" },
    [K_PARCEL_KREC]   = { "krec",   "text/x-krec" },
    [K_PARCEL_IMAGE]  = { "image",  "application/octet-stream" },
};

static inline const char *talon_type_name(uint32_t type)
{
    if (type < sizeof(talon_types) / sizeof(talon_types[0]) &&
        talon_types[type].name)
        return talon_types[type].name;
    return "?";
}

static inline int talon_type_parse(const char *name, uint32_t *out)
{
    if (!name || !out)
        return -1;
    for (uint32_t type = 0; type < sizeof(talon_types) / sizeof(talon_types[0]);
         type++) {
        if (talon_types[type].name && strcmp(name, talon_types[type].name) == 0) {
            *out = type;
            return 0;
        }
    }
    return -1;
}

static inline const char *talon_type_default_mime(uint32_t type)
{
    if (type < sizeof(talon_types) / sizeof(talon_types[0]) &&
        talon_types[type].default_mime)
        return talon_types[type].default_mime;
    return "application/octet-stream";
}
