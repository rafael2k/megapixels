#pragma once

#include "camera.h"
#include "flash.h"
#include "mode.h"

#include <linux/limits.h>
#include <linux/media.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool
mp_find_device_path(struct media_v2_intf_devnode devnode, char *path, int length);

typedef struct _MPDevice MPDevice;

MPDevice *mp_device_find(const char *driver_name, const char *dev_name);
MPDevice *mp_device_from_path(const char *path);
MPDevice *mp_device_new(int fd);
void mp_device_close(MPDevice *device);

bool mp_device_setup(MPDevice *device);

bool mp_device_setup_entity_link(MPDevice *device,
                                 uint32_t source_entity_id,
                                 uint32_t sink_entity_id,
                                 uint32_t source_index,
                                 uint32_t sink_index,
                                 bool enabled);

bool mp_device_setup_link(MPDevice *device,
                          uint32_t source_pad_id,
                          uint32_t sink_pad_id,
                          bool enabled);

bool mp_entity_pad_set_format(MPDevice *device,
                              const struct media_v2_entity *entity,
                              uint32_t pad,
                              MPMode *mode);

const struct media_device_info *mp_device_get_info(const MPDevice *device);
const struct media_v2_entity *mp_device_find_entity(const MPDevice *device,
                                                    const char *driver_name);
const struct media_v2_entity *mp_device_find_entity_type(const MPDevice *device,
                                                         const uint32_t type);
const struct media_v2_entity *mp_device_get_entity(const MPDevice *device,
                                                   uint32_t id);
const struct media_v2_entity *mp_device_get_entities(const MPDevice *device);
size_t mp_device_get_num_entities(const MPDevice *device);
const struct media_v2_interface *
mp_device_find_entity_interface(const MPDevice *device, uint32_t entity_id);
const struct media_v2_interface *mp_device_get_interface(const MPDevice *device,
                                                         uint32_t id);
const struct media_v2_interface *mp_device_get_interfaces(const MPDevice *device);
size_t mp_device_get_num_interfaces(const MPDevice *device);
const struct media_v2_pad *mp_device_get_pad_from_entity(const MPDevice *device,
                                                         uint32_t entity_id);
const struct media_v2_pad *mp_device_get_pad(const MPDevice *device, uint32_t id);
const struct media_v2_pad *mp_device_get_pads(const MPDevice *device);
size_t mp_device_get_num_pads(const MPDevice *device);
const struct media_v2_link *mp_device_find_entity_link(const MPDevice *device,
                                                       uint32_t entity_id);
const struct media_v2_link *mp_device_find_link_from(const MPDevice *device,
                                                     uint32_t source);
const struct media_v2_link *mp_device_find_link_to(const MPDevice *device,
                                                   uint32_t sink);
const struct media_v2_link *
mp_device_find_link_between(const MPDevice *device, uint32_t source, uint32_t sink);
const struct media_v2_link *mp_device_get_link(const MPDevice *device, uint32_t id);
const struct media_v2_link *mp_device_get_links(const MPDevice *device);
size_t mp_device_get_num_links(const MPDevice *device);

struct _MPDeviceLink {
        char source_name[100];
        char target_name[100];
        int source_port;
        int target_port;
};
typedef struct _MPDeviceLink MPDeviceLink;

typedef struct _MPDeviceLinkList MPDeviceLinkList;

struct _MPDeviceLinkList {
        MPDeviceLink *link;
        MPDeviceLinkList *next;
};

struct _MPDevice {
        // Configuration data
        char cfg_name[100];
        char cfg_driver_name[260];
        char cfg_media_driver_name[260];
        int rotate;
        bool mirrored;

        // Picture metadata
        float colormatrix[9];
        float forwardmatrix[9];
        float previewmatrix[9];
        int blacklevel;
        int whitelevel;
        float focallength;
        float cropfactor;
        double fnumber;
        int iso_min;
        int iso_max;

        // Flash info
        char cfg_flash_path[260];
        bool flash_display;
        bool has_flash;

        // Pixelformat config
        MPMode capture_mode;
        MPMode preview_mode;

        // Media graph config
        MPDeviceLinkList *media_links;

        // Runtime data
        char media_path[PATH_MAX];
        char video_path[PATH_MAX];
        char sensor_path[PATH_MAX];
        int media_fd;
        int video_fd;
        int sensor_fd;
        uint32_t sensor_entity;
        uint32_t csi_entity;
        uint32_t sensor_pad;
        uint32_t csi_pad;

        // IO pipeline state
        MPCamera *camera;
        MPFlash *flash;

        int gain_ctrl;
        int gain_max;

        bool has_auto_focus_continuous;
        bool has_auto_focus_start;


        // Gifts from the kernel
        struct media_device_info media_info;
        struct media_v2_entity *entities;
        size_t num_entities;
        struct media_v2_interface *interfaces;
        size_t num_interfaces;
        struct media_v2_pad *pads;
        size_t num_pads;
        struct media_v2_link *links;
        size_t num_links;
};

typedef struct _MPDeviceList MPDeviceList;

struct _MPDeviceList {
        MPDevice *device;
        MPDeviceList *next;
};

MPDeviceList *mp_device_list_new();
void mp_device_list_free(MPDeviceList *device_list);

MPDevice *mp_device_list_find_remove(MPDeviceList **device_list,
                                     const char *driver_name,
                                     const char *dev_name);
MPDevice *mp_device_list_remove(MPDeviceList **device_list);

MPDevice *mp_device_list_get(const MPDeviceList *device_list);
const char *mp_device_list_get_path(const MPDeviceList *device_list);
MPDeviceList *mp_device_list_next(const MPDeviceList *device_list);
