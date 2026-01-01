/* kwin.c - implements the KDE Wayland output management protocol
 *
 * Copyright 2024 VirtualBuddy Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#ifdef HAVE_KWIN

#include <glib.h>
#include <syslog.h>
#include <string.h>
#include <wayland-client.h>

#include "vdagentd-proto.h"
#include "kwin.h"

/* Include generated protocol headers */
#include "kde-output-device-v2-client-protocol.h"
#include "kde-output-management-v2-client-protocol.h"

/* Minimum protocol version we require */
#define MIN_OUTPUT_DEVICE_VERSION 2
#define MIN_OUTPUT_MANAGEMENT_VERSION 2

/* Mode information */
typedef struct KWinMode {
    struct kde_output_device_mode_v2 *mode;
    int width;
    int height;
    int refresh;    /* in mHz */
    gboolean preferred;
    gboolean current;
} KWinMode;

/* Output device information */
typedef struct KWinOutput {
    struct kde_output_device_v2 *device;
    char *name;         /* Connector name like "Virtual-1" */
    char *uuid;
    int x, y;
    int width, height;
    int physical_width, physical_height;
    int transform;
    double scale;
    gboolean enabled;
    GArray *modes;      /* Array of KWinMode */
    KWinMode *current_mode;
    KWinMode *preferred_mode;
    gboolean done;      /* Have we received the 'done' event? */
} KWinOutput;

struct VDAgentKWin {
    struct wl_display *display;
    struct wl_registry *registry;
    struct kde_output_management_v2 *output_management;
    GHashTable *connector_mapping;  /* Connector name -> SPICE display ID */
    GArray *outputs;                /* Array of KWinOutput* */
    uint32_t output_management_version;
    gboolean roundtrip_done;

    /* Configuration state */
    struct kde_output_configuration_v2 *pending_config;
    gboolean config_applied;
    gboolean config_failed;
};

/* Forward declarations */
static void kwin_output_destroy(KWinOutput *output);
static KWinOutput *kwin_find_output_by_device(VDAgentKWin *kwin,
                                               struct kde_output_device_v2 *device);

/* ============================================================================
 * Mode listener callbacks
 * ============================================================================ */

static void mode_handle_size(void *data,
                             struct kde_output_device_mode_v2 *mode_obj,
                             int32_t width, int32_t height)
{
    KWinMode *mode = data;
    mode->width = width;
    mode->height = height;
}

static void mode_handle_refresh(void *data,
                                struct kde_output_device_mode_v2 *mode_obj,
                                int32_t refresh)
{
    KWinMode *mode = data;
    mode->refresh = refresh;
}

static void mode_handle_preferred(void *data,
                                  struct kde_output_device_mode_v2 *mode_obj)
{
    KWinMode *mode = data;
    mode->preferred = TRUE;
}

static void mode_handle_removed(void *data,
                                struct kde_output_device_mode_v2 *mode_obj)
{
    /* Mode was removed - we'll handle cleanup on output destruction */
}

static const struct kde_output_device_mode_v2_listener mode_listener = {
    .size = mode_handle_size,
    .refresh = mode_handle_refresh,
    .preferred = mode_handle_preferred,
    .removed = mode_handle_removed,
};

/* ============================================================================
 * Output device listener callbacks
 * ============================================================================ */

static void output_handle_geometry(void *data,
                                   struct kde_output_device_v2 *device,
                                   int32_t x, int32_t y,
                                   int32_t physical_width, int32_t physical_height,
                                   int32_t subpixel,
                                   const char *make, const char *model,
                                   int32_t transform)
{
    KWinOutput *output = data;
    output->x = x;
    output->y = y;
    output->physical_width = physical_width;
    output->physical_height = physical_height;
    output->transform = transform;
}

static void output_handle_current_mode(void *data,
                                       struct kde_output_device_v2 *device,
                                       struct kde_output_device_mode_v2 *mode_obj)
{
    KWinOutput *output = data;

    /* Find the mode in our array and mark it as current */
    for (guint i = 0; i < output->modes->len; i++) {
        KWinMode *mode = &g_array_index(output->modes, KWinMode, i);
        if (mode->mode == mode_obj) {
            mode->current = TRUE;
            output->current_mode = mode;
            output->width = mode->width;
            output->height = mode->height;
        } else {
            mode->current = FALSE;
        }
    }
}

static void output_handle_mode(void *data,
                               struct kde_output_device_v2 *device,
                               struct kde_output_device_mode_v2 *mode_obj)
{
    KWinOutput *output = data;

    KWinMode mode = {
        .mode = mode_obj,
        .width = 0,
        .height = 0,
        .refresh = 0,
        .preferred = FALSE,
        .current = FALSE,
    };

    kde_output_device_mode_v2_add_listener(mode_obj, &mode_listener, &mode);
    g_array_append_val(output->modes, mode);
}

static void output_handle_done(void *data,
                               struct kde_output_device_v2 *device)
{
    KWinOutput *output = data;
    output->done = TRUE;

    /* Find preferred mode */
    for (guint i = 0; i < output->modes->len; i++) {
        KWinMode *mode = &g_array_index(output->modes, KWinMode, i);
        if (mode->preferred) {
            output->preferred_mode = mode;
            break;
        }
    }

    syslog(LOG_DEBUG, "kwin: output %s done: %dx%d+%d+%d, enabled=%d",
           output->name ? output->name : "(unknown)",
           output->width, output->height, output->x, output->y,
           output->enabled);
}

static void output_handle_scale(void *data,
                                struct kde_output_device_v2 *device,
                                wl_fixed_t scale)
{
    KWinOutput *output = data;
    output->scale = wl_fixed_to_double(scale);
}

static void output_handle_edid(void *data,
                               struct kde_output_device_v2 *device,
                               const char *raw)
{
    /* We don't use EDID data */
}

static void output_handle_enabled(void *data,
                                  struct kde_output_device_v2 *device,
                                  int32_t enabled)
{
    KWinOutput *output = data;
    output->enabled = (enabled != 0);
}

static void output_handle_uuid(void *data,
                               struct kde_output_device_v2 *device,
                               const char *uuid)
{
    KWinOutput *output = data;
    g_free(output->uuid);
    output->uuid = g_strdup(uuid);
}

static void output_handle_serial_number(void *data,
                                        struct kde_output_device_v2 *device,
                                        const char *serial)
{
    /* We don't use serial number */
}

static void output_handle_eisa_id(void *data,
                                  struct kde_output_device_v2 *device,
                                  const char *eisa_id)
{
    /* We don't use EISA ID */
}

static void output_handle_capabilities(void *data,
                                       struct kde_output_device_v2 *device,
                                       uint32_t flags)
{
    /* We don't use capabilities flags */
}

static void output_handle_overscan(void *data,
                                   struct kde_output_device_v2 *device,
                                   uint32_t overscan)
{
    /* We don't use overscan */
}

static void output_handle_vrr_policy(void *data,
                                     struct kde_output_device_v2 *device,
                                     uint32_t policy)
{
    /* We don't use VRR policy */
}

static void output_handle_rgb_range(void *data,
                                    struct kde_output_device_v2 *device,
                                    uint32_t range)
{
    /* We don't use RGB range */
}

static void output_handle_name(void *data,
                               struct kde_output_device_v2 *device,
                               const char *name)
{
    KWinOutput *output = data;
    g_free(output->name);
    output->name = g_strdup(name);
    syslog(LOG_DEBUG, "kwin: output name: %s", name);
}

static const struct kde_output_device_v2_listener output_device_listener = {
    .geometry = output_handle_geometry,
    .current_mode = output_handle_current_mode,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
    .edid = output_handle_edid,
    .enabled = output_handle_enabled,
    .uuid = output_handle_uuid,
    .serial_number = output_handle_serial_number,
    .eisa_id = output_handle_eisa_id,
    .capabilities = output_handle_capabilities,
    .overscan = output_handle_overscan,
    .vrr_policy = output_handle_vrr_policy,
    .rgb_range = output_handle_rgb_range,
    .name = output_handle_name,
};

/* ============================================================================
 * Configuration listener callbacks
 * ============================================================================ */

static void config_handle_applied(void *data,
                                  struct kde_output_configuration_v2 *config)
{
    VDAgentKWin *kwin = data;
    kwin->config_applied = TRUE;
    syslog(LOG_DEBUG, "kwin: configuration applied successfully");
}

static void config_handle_failed(void *data,
                                 struct kde_output_configuration_v2 *config)
{
    VDAgentKWin *kwin = data;
    kwin->config_failed = TRUE;
    syslog(LOG_WARNING, "kwin: configuration failed to apply");
}

static const struct kde_output_configuration_v2_listener config_listener = {
    .applied = config_handle_applied,
    .failed = config_handle_failed,
};

/* ============================================================================
 * Registry listener callbacks
 * ============================================================================ */

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version)
{
    VDAgentKWin *kwin = data;

    if (strcmp(interface, kde_output_device_v2_interface.name) == 0) {
        if (version < MIN_OUTPUT_DEVICE_VERSION) {
            syslog(LOG_WARNING, "kwin: kde_output_device_v2 version %u too old (need %d)",
                   version, MIN_OUTPUT_DEVICE_VERSION);
            return;
        }

        struct kde_output_device_v2 *device =
            wl_registry_bind(registry, name, &kde_output_device_v2_interface,
                            MIN(version, (uint32_t)kde_output_device_v2_interface.version));

        KWinOutput *output = g_new0(KWinOutput, 1);
        output->device = device;
        output->modes = g_array_new(FALSE, TRUE, sizeof(KWinMode));
        output->scale = 1.0;
        output->enabled = TRUE;

        kde_output_device_v2_add_listener(device, &output_device_listener, output);
        g_array_append_val(kwin->outputs, output);

        syslog(LOG_DEBUG, "kwin: bound kde_output_device_v2 (name=%u, version=%u)",
               name, version);
    }
    else if (strcmp(interface, kde_output_management_v2_interface.name) == 0) {
        if (version < MIN_OUTPUT_MANAGEMENT_VERSION) {
            syslog(LOG_WARNING, "kwin: kde_output_management_v2 version %u too old (need %d)",
                   version, MIN_OUTPUT_MANAGEMENT_VERSION);
            return;
        }

        kwin->output_management_version = MIN(version,
            (uint32_t)kde_output_management_v2_interface.version);
        kwin->output_management =
            wl_registry_bind(registry, name, &kde_output_management_v2_interface,
                            kwin->output_management_version);

        syslog(LOG_DEBUG, "kwin: bound kde_output_management_v2 (name=%u, version=%u)",
               name, kwin->output_management_version);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                          uint32_t name)
{
    /* We handle output removal through the protocol events */
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

/* ============================================================================
 * Public API implementation
 * ============================================================================ */

VDAgentKWin *vdagent_kwin_create(GHashTable *connector_mapping)
{
    VDAgentKWin *kwin;

    kwin = g_new0(VDAgentKWin, 1);
    kwin->connector_mapping = g_hash_table_ref(connector_mapping);
    kwin->outputs = g_array_new(FALSE, TRUE, sizeof(KWinOutput*));

    /* Connect to Wayland display */
    kwin->display = wl_display_connect(NULL);
    if (!kwin->display) {
        syslog(LOG_DEBUG, "kwin: failed to connect to Wayland display");
        vdagent_kwin_destroy(kwin);
        return NULL;
    }

    /* Get registry and bind to KDE protocols */
    kwin->registry = wl_display_get_registry(kwin->display);
    wl_registry_add_listener(kwin->registry, &registry_listener, kwin);

    /* Do initial roundtrip to get globals */
    wl_display_roundtrip(kwin->display);

    /* Do another roundtrip to get output details */
    wl_display_roundtrip(kwin->display);

    /* Check if we got the required protocols */
    if (!kwin->output_management) {
        syslog(LOG_DEBUG, "kwin: kde_output_management_v2 not available (not KDE?)");
        vdagent_kwin_destroy(kwin);
        return NULL;
    }

    syslog(LOG_INFO, "kwin: KDE output management initialized with %u outputs",
           kwin->outputs->len);

    return kwin;
}

void vdagent_kwin_destroy(VDAgentKWin *kwin)
{
    if (!kwin) {
        return;
    }

    if (kwin->pending_config) {
        kde_output_configuration_v2_destroy(kwin->pending_config);
    }

    if (kwin->outputs) {
        for (guint i = 0; i < kwin->outputs->len; i++) {
            KWinOutput *output = g_array_index(kwin->outputs, KWinOutput*, i);
            kwin_output_destroy(output);
        }
        g_array_free(kwin->outputs, TRUE);
    }

    if (kwin->output_management) {
        kde_output_management_v2_destroy(kwin->output_management);
    }

    if (kwin->registry) {
        wl_registry_destroy(kwin->registry);
    }

    if (kwin->display) {
        wl_display_disconnect(kwin->display);
    }

    if (kwin->connector_mapping) {
        g_hash_table_unref(kwin->connector_mapping);
    }

    g_free(kwin);
}

static void kwin_output_destroy(KWinOutput *output)
{
    if (!output) {
        return;
    }

    if (output->modes) {
        for (guint i = 0; i < output->modes->len; i++) {
            KWinMode *mode = &g_array_index(output->modes, KWinMode, i);
            if (mode->mode) {
                kde_output_device_mode_v2_destroy(mode->mode);
            }
        }
        g_array_free(output->modes, TRUE);
    }

    if (output->device) {
        kde_output_device_v2_destroy(output->device);
    }

    g_free(output->name);
    g_free(output->uuid);
    g_free(output);
}

GArray *vdagent_kwin_get_resolutions(VDAgentKWin *kwin,
                                      int *width, int *height, int *screen_count)
{
    GArray *res_array;
    GArray *not_found_array;

    if (!kwin || !kwin->output_management) {
        return NULL;
    }

    /* Refresh output state */
    wl_display_roundtrip(kwin->display);

    res_array = g_array_new(FALSE, FALSE, sizeof(struct vdagentd_guest_xorg_resolution));
    not_found_array = g_array_new(FALSE, FALSE, sizeof(struct vdagentd_guest_xorg_resolution));

    *width = 0;
    *height = 0;
    *screen_count = 0;

    for (guint i = 0; i < kwin->outputs->len; i++) {
        KWinOutput *output = g_array_index(kwin->outputs, KWinOutput*, i);

        if (!output->done || !output->enabled) {
            continue;
        }

        (*screen_count)++;

        struct vdagentd_guest_xorg_resolution res = {
            .width = output->width,
            .height = output->height,
            .x = output->x,
            .y = output->y,
            .display_id = 0,
        };

        /* Compute desktop size */
        if (res.x + res.width > *width) {
            *width = res.x + res.width;
        }
        if (res.y + res.height > *height) {
            *height = res.y + res.height;
        }

        /* Look up SPICE display ID from connector mapping */
        gpointer value;
        if (output->name &&
            g_hash_table_lookup_extended(kwin->connector_mapping, output->name, NULL, &value)) {
            res.display_id = GPOINTER_TO_UINT(value);
            syslog(LOG_DEBUG, "kwin: output %s -> SPICE display #%d (%dx%d+%d+%d)",
                   output->name, res.display_id, res.width, res.height, res.x, res.y);
            g_array_append_val(res_array, res);
        } else {
            syslog(LOG_DEBUG, "kwin: no SPICE display for connector %s",
                   output->name ? output->name : "(null)");
            g_array_append_val(not_found_array, res);
        }
    }

    /* If no mappings found, assume display ID == monitor index */
    if (res_array->len == 0) {
        syslog(LOG_DEBUG, "kwin: no SPICE display ID matching - using monitor index");
        g_array_free(res_array, TRUE);
        res_array = not_found_array;

        struct vdagentd_guest_xorg_resolution *res =
            (struct vdagentd_guest_xorg_resolution*)res_array->data;
        for (guint i = 0; i < res_array->len; i++) {
            res[i].display_id = i;
        }
    } else {
        g_array_free(not_found_array, TRUE);
    }

    return res_array;
}

/* Find the best matching mode for a given resolution */
static KWinMode *kwin_find_mode(KWinOutput *output, int width, int height)
{
    KWinMode *best = NULL;
    int best_refresh = 0;

    for (guint i = 0; i < output->modes->len; i++) {
        KWinMode *mode = &g_array_index(output->modes, KWinMode, i);

        if (mode->width == width && mode->height == height) {
            /* Prefer higher refresh rates */
            if (!best || mode->refresh > best_refresh) {
                best = mode;
                best_refresh = mode->refresh;
            }
        }
    }

    return best;
}

int vdagent_kwin_set_monitor_config(VDAgentKWin *kwin,
                                     VDAgentMonitorsConfig *mon_config)
{
    if (!kwin || !kwin->output_management || !mon_config) {
        return -1;
    }

    /* Refresh output state first */
    wl_display_roundtrip(kwin->display);

    /* Create a new configuration */
    struct kde_output_configuration_v2 *config =
        kde_output_management_v2_create_configuration(kwin->output_management);

    if (!config) {
        syslog(LOG_ERR, "kwin: failed to create output configuration");
        return -1;
    }

    kwin->config_applied = FALSE;
    kwin->config_failed = FALSE;

    kde_output_configuration_v2_add_listener(config, &config_listener, kwin);

    syslog(LOG_DEBUG, "kwin: setting monitor config for %u monitors",
           mon_config->num_of_monitors);

    /* Apply configuration to each monitor */
    for (uint32_t i = 0; i < mon_config->num_of_monitors; i++) {
        VDAgentMonConfig *mc = &mon_config->monitors[i];

        /* Find the output for this SPICE display */
        KWinOutput *output = NULL;

        for (guint j = 0; j < kwin->outputs->len; j++) {
            KWinOutput *o = g_array_index(kwin->outputs, KWinOutput*, j);

            if (!o->name) {
                continue;
            }

            gpointer value;
            if (g_hash_table_lookup_extended(kwin->connector_mapping, o->name, NULL, &value)) {
                if (GPOINTER_TO_UINT(value) == i) {
                    output = o;
                    break;
                }
            }
        }

        /* If no mapping found, use output index */
        if (!output && i < kwin->outputs->len) {
            output = g_array_index(kwin->outputs, KWinOutput*, i);
        }

        if (!output) {
            syslog(LOG_WARNING, "kwin: no output found for SPICE display %u", i);
            continue;
        }

        /* Enable the output */
        kde_output_configuration_v2_enable(config, output->device, 1);

        /* Find a mode matching the requested resolution */
        KWinMode *mode = kwin_find_mode(output, mc->width, mc->height);

        if (mode) {
            syslog(LOG_DEBUG, "kwin: setting output %s to %dx%d @ %d.%03d Hz",
                   output->name, mode->width, mode->height,
                   mode->refresh / 1000, mode->refresh % 1000);
            kde_output_configuration_v2_mode(config, output->device, mode->mode);
        } else {
            syslog(LOG_WARNING, "kwin: no mode %dx%d available for %s",
                   mc->width, mc->height, output->name);
            /* Try the current mode as fallback */
            if (output->current_mode) {
                kde_output_configuration_v2_mode(config, output->device,
                                                  output->current_mode->mode);
            }
        }

        /* Set position */
        kde_output_configuration_v2_position(config, output->device, mc->x, mc->y);

        /* Keep current scale */
        kde_output_configuration_v2_scale(config, output->device,
                                          wl_fixed_from_double(output->scale));

        /* Keep current transform */
        kde_output_configuration_v2_transform(config, output->device, output->transform);
    }

    /* Apply the configuration */
    kde_output_configuration_v2_apply(config);
    wl_display_flush(kwin->display);

    /* Wait for the result (with timeout) */
    int max_wait = 50; /* 5 seconds */
    while (!kwin->config_applied && !kwin->config_failed && max_wait > 0) {
        wl_display_roundtrip(kwin->display);
        if (!kwin->config_applied && !kwin->config_failed) {
            g_usleep(100000); /* 100ms */
            max_wait--;
        }
    }

    kde_output_configuration_v2_destroy(config);

    if (kwin->config_failed) {
        syslog(LOG_WARNING, "kwin: configuration was rejected");
        return -1;
    }

    if (!kwin->config_applied) {
        syslog(LOG_WARNING, "kwin: configuration timed out");
        return -1;
    }

    return 0;
}

gboolean vdagent_kwin_is_available(VDAgentKWin *kwin)
{
    return kwin && kwin->output_management != NULL;
}

#else /* !HAVE_KWIN */

/* Stub implementations when KWin support is not compiled in */

VDAgentKWin *vdagent_kwin_create(GHashTable *connector_mapping)
{
    return NULL;
}

void vdagent_kwin_destroy(VDAgentKWin *kwin)
{
}

GArray *vdagent_kwin_get_resolutions(VDAgentKWin *kwin,
                                      int *width, int *height, int *screen_count)
{
    return NULL;
}

int vdagent_kwin_set_monitor_config(VDAgentKWin *kwin,
                                     VDAgentMonitorsConfig *mon_config)
{
    return -1;
}

gboolean vdagent_kwin_is_available(VDAgentKWin *kwin)
{
    return FALSE;
}

#endif /* HAVE_KWIN */
