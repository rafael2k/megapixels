#include "camera_config.h"
#include "device.h"
#include <stdio.h>

int
main(int argc, char *argv[])
{
        MPDeviceList *devices = mp_load_config();
        while (devices) {
                mp_device_setup(devices->device);
                printf("--[ %s ]--\n", devices->device->cfg_name);
                printf("sensor driver: %s\n", devices->device->cfg_driver_name);
                printf("csi driver: %s\n", devices->device->cfg_media_driver_name);
                printf("media path: %s\n", devices->device->media_path);
                printf("video path: %s\n", devices->device->video_path);
                printf("sensor path: %s\n", devices->device->sensor_path);
                printf("\n");
                printf("sensor: entity %d pad %d\n", devices->device->sensor_entity, devices->device->sensor_pad);
                printf("video: entity %d pad %d\n", devices->device->csi_entity, devices->device->csi_pad);
                printf("\n\n");
                devices = devices->next;
        }
}
