#pragma once

#include "device.h"
#include "mode.h"

#include <stdbool.h>
#include <stddef.h>

#define MP_MAX_CAMERAS 5
#define MP_MAX_LINKS 10

struct mp_media_link_config {
        char source_name[100];
        char target_name[100];
        int source_port;
        int target_port;
};

MPDeviceList *mp_load_config();

const char *mp_get_device_make();
const char *mp_get_device_model();
