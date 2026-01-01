/* mutter.c - implements the DBUS interface to mutter

 Copyright 2020 Red Hat, Inc.

 Red Hat Authors:
 Julien Ropé <jrope@redhat.com>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <glib.h>
#include <gio/gio.h>

#include <syslog.h>

#include "vdagentd-proto.h"
#include "mutter.h"

// MUTTER DBUS FORMAT STRINGS
#define MODE_BASE_FORMAT "siiddad"
#define MODE_FORMAT "(" MODE_BASE_FORMAT "a{sv})"
#define MODES_FORMAT "a" MODE_FORMAT
#define MONITOR_SPEC_FORMAT "(ssss)"
#define MONITOR_FORMAT "(" MONITOR_SPEC_FORMAT MODES_FORMAT "a{sv})"
#define MONITORS_FORMAT "a" MONITOR_FORMAT

#define LOGICAL_MONITOR_MONITORS_FORMAT "a" MONITOR_SPEC_FORMAT
#define LOGICAL_MONITOR_FORMAT "(iidub" LOGICAL_MONITOR_MONITORS_FORMAT "a{sv})"
#define LOGICAL_MONITORS_FORMAT "a" LOGICAL_MONITOR_FORMAT

#define CURRENT_STATE_FORMAT "(u" MONITORS_FORMAT LOGICAL_MONITORS_FORMAT "a{sv})"

// ApplyMonitorsConfig format strings
// Each monitor in the apply config: (connector_name, mode_id, properties)
#define APPLY_MONITOR_FORMAT "(ssa{sv})"
#define APPLY_MONITORS_FORMAT "a" APPLY_MONITOR_FORMAT
// Each logical monitor in the apply config: (x, y, scale, transform, primary, monitors)
#define APPLY_LOGICAL_MONITOR_FORMAT "(iidub" APPLY_MONITORS_FORMAT ")"
#define APPLY_LOGICAL_MONITORS_FORMAT "a" APPLY_LOGICAL_MONITOR_FORMAT

// Apply configuration methods
#define APPLY_METHOD_VERIFY    0
#define APPLY_METHOD_TEMPORARY 1
#define APPLY_METHOD_PERSISTENT 2


struct VDAgentMutterDBus {
    GDBusProxy *dbus_proxy;
    GHashTable *connector_mapping;
};

/**
 * Initialise a communication to Mutter through its DBUS interface.
 *
 * Errors can indicate that another compositor is used. This is not a blocker, and we should default
 * to use a different API then.
 *
 * Returns:
 * An initialise VDAgentMutterDBus structure if successful.
 * NULL if an error occurred.
 */
VDAgentMutterDBus *vdagent_mutter_create(GHashTable *connector_mapping)
{
    GError *error = NULL;
    VDAgentMutterDBus *mutter = g_new0(VDAgentMutterDBus, 1);

    mutter->connector_mapping = g_hash_table_ref(connector_mapping);

    GDBusProxyFlags flags = (G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START
                            | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES
                            | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS);

    mutter->dbus_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                                       flags,
                                                       NULL,
                                                       "org.gnome.Mutter.DisplayConfig",
                                                       "/org/gnome/Mutter/DisplayConfig",
                                                       "org.gnome.Mutter.DisplayConfig",
                                                       NULL,
                                                       &error);
    if (!mutter->dbus_proxy) {
        syslog(LOG_WARNING, "display: failed to create dbus proxy: %s", error->message);
        g_clear_error(&error);
        vdagent_mutter_destroy(mutter);
        return NULL;
    }

    return mutter;
}


void vdagent_mutter_destroy(VDAgentMutterDBus *mutter)
{
    g_clear_object(&mutter->dbus_proxy);
    g_hash_table_unref(mutter->connector_mapping);
    g_free(mutter);
}

/** Look through a list of logical monitor to find the one provided.
 *  Returns the corresponding x and y position of the monitor on the desktop.
 *  This function is a helper to vdagent_mutter_get_resolution().
 *
 *  Parameters:
 *  logical_monitor: initialized GVariant iterator. It will be copied to look through the items
 *                   so that its original position is not modified.
 *  connector: name of the connector that must be found
 *  x and y: will received the found position
 *
 */
static void vdagent_mutter_get_monitor_position(GVariantIter *logical_monitors,
                                                const gchar *connector, int *x, int *y)
{
    GVariant *logical_monitor = NULL;
    GVariantIter *logical_monitor_iterator = g_variant_iter_copy(logical_monitors);
    while (g_variant_iter_next(logical_monitor_iterator, "@"LOGICAL_MONITOR_FORMAT,
                               &logical_monitor)) {
        GVariantIter *tmp_monitors = NULL;

        g_variant_get_child(logical_monitor, 0, "i", x);
        g_variant_get_child(logical_monitor, 1, "i", y);
        g_variant_get_child(logical_monitor, 5, LOGICAL_MONITOR_MONITORS_FORMAT, &tmp_monitors);

        g_variant_unref(logical_monitor);

        GVariant *tmp_monitor = NULL;
        gboolean found = FALSE;
        while (!found && g_variant_iter_next(tmp_monitors, "@"MONITOR_SPEC_FORMAT, &tmp_monitor)) {
            const gchar *tmp_connector;

            g_variant_get_child(tmp_monitor, 0, "&s", &tmp_connector);

            if (g_strcmp0(connector, tmp_connector) == 0) {
                found = TRUE;
            }
            g_variant_unref(tmp_monitor);
        }

        g_variant_iter_free(tmp_monitors);

        if (found) {
            break;
        }
        *x = *y = 0;
    }
    g_variant_iter_free(logical_monitor_iterator);
}

GArray *vdagent_mutter_get_resolutions(VDAgentMutterDBus *mutter,
                                       int *desktop_width, int *desktop_height, int *screen_count)
{
    GError *error = NULL;
    GArray *res_array = NULL;

    // keep track of monitors we find and are not mapped to SPICE displays
    // we will map them back later (assuming display ID == monitor index)
    // this prevents the need from looping twice on all DBUS items
    GArray *not_found_array = NULL;

    if (!mutter) {
        return res_array;
    }

    GVariant *values = g_dbus_proxy_call_sync(mutter->dbus_proxy,
                                              "GetCurrentState",
                                              NULL,
                                              G_DBUS_CALL_FLAGS_NONE,
                                              -1,   // use proxy default timeout
                                              NULL,
                                              &error);
    if (!values) {
        syslog(LOG_WARNING, "display: failed to call GetCurrentState from mutter over DBUS");
        if (error != NULL) {
            syslog(LOG_WARNING, "   error message: %s", error->message);
            g_clear_error(&error);
        }
        return res_array;
    }

    res_array = g_array_new(FALSE, FALSE, sizeof(struct vdagentd_guest_xorg_resolution));
    not_found_array = g_array_new(FALSE, FALSE, sizeof(struct vdagentd_guest_xorg_resolution));

    GVariantIter *monitors = NULL;
    GVariantIter *logical_monitors = NULL;

    g_variant_get_child(values, 1, MONITORS_FORMAT, &monitors);
    g_variant_get_child(values, 2, LOGICAL_MONITORS_FORMAT, &logical_monitors);

    // list monitors
    GVariant *monitor = NULL;
    *screen_count = g_variant_iter_n_children(monitors);

    while (g_variant_iter_next(monitors, "@"MONITOR_FORMAT, &monitor)) {

        const gchar *connector = NULL;
        GVariantIter *modes = NULL;
        GVariant *monitor_specs = NULL;

        g_variant_get_child(monitor, 0, "@"MONITOR_SPEC_FORMAT, &monitor_specs);
        g_variant_get_child(monitor_specs, 0, "&s", &connector);
        g_variant_get_child(monitor, 1, MODES_FORMAT, &modes);

        g_variant_unref(monitor_specs);
        g_variant_unref(monitor);

        // list modes
        GVariant *mode = NULL;
        while (g_variant_iter_next(modes, "@"MODE_FORMAT, &mode)) {
            GVariant *properties = NULL;
            gboolean is_current;

            g_variant_get_child(mode, 6, "@a{sv}", &properties);
            if (!g_variant_lookup(properties, "is-current", "b", &is_current)) {
                is_current = FALSE;
            }
            g_variant_unref(properties);

            if (!is_current) {
                g_variant_unref(mode);
                continue;
            }

            struct vdagentd_guest_xorg_resolution curr;
            vdagent_mutter_get_monitor_position(logical_monitors, connector, &curr.x, &curr.y);
            g_variant_get_child(mode, 1, "i", &curr.width);
            g_variant_get_child(mode, 2, "i", &curr.height);
            g_variant_unref(mode);

            // compute the size of the desktop based on the dimension of the monitors
            if (curr.x + curr.width > *desktop_width) {
                *desktop_width = curr.x + curr.width;
            }
            if (curr.y + curr.height > *desktop_height) {
                *desktop_height = curr.y + curr.height;
            }

            gpointer value;
            if (g_hash_table_lookup_extended(mutter->connector_mapping, connector, NULL, &value)) {
                curr.display_id = GPOINTER_TO_UINT(value);
                syslog(LOG_DEBUG,
                       "Found monitor %s with geometry %dx%d+%d-%d - associating it to SPICE display #%d",
                       connector, curr.width, curr.height, curr.x, curr.y, curr.display_id);
                g_array_append_val(res_array, curr);
            } else {
                syslog(LOG_DEBUG, "No SPICE display found for connector %s", connector);
                g_array_append_val(not_found_array, curr);
            }

            break;
        }
        g_variant_iter_free(modes);
    }

    g_variant_iter_free(logical_monitors);
    g_variant_iter_free(monitors);

    int i;

    if (res_array->len == 0) {
        syslog(LOG_DEBUG, "%s: No Spice display ID matching - assuming display ID == Monitor index",
                __FUNCTION__);
        g_array_free(res_array, TRUE);
        res_array = not_found_array;

        struct vdagentd_guest_xorg_resolution *res;
        res = (struct vdagentd_guest_xorg_resolution*)res_array->data;
        for (i = 0; i < res_array->len; i++) {
            res[i].display_id = i;
        }
    }
    else {
        g_array_free(not_found_array, TRUE);
    }

    g_variant_unref(values);
    return res_array;
}

/**
 * Structure to hold information about a physical monitor needed for configuration
 */
typedef struct MutterMonitorInfo {
    gchar *connector;
    gchar *vendor;
    gchar *product;
    gchar *serial;
    GPtrArray *modes;     // Array of MutterModeInfo*
    int current_x;
    int current_y;
    gdouble current_scale;
    guint32 current_transform;
    gboolean is_primary;
    gboolean is_enabled;
    gchar *current_mode_id;
    int display_id;       // SPICE display ID (-1 if not mapped)
} MutterMonitorInfo;

typedef struct MutterModeInfo {
    gchar *mode_id;
    int width;
    int height;
    gdouble refresh_rate;
    gboolean is_current;
    gboolean is_preferred;
} MutterModeInfo;

static void mutter_mode_info_free(MutterModeInfo *mode)
{
    if (mode) {
        g_free(mode->mode_id);
        g_free(mode);
    }
}

static void mutter_monitor_info_free(MutterMonitorInfo *info)
{
    if (info) {
        g_free(info->connector);
        g_free(info->vendor);
        g_free(info->product);
        g_free(info->serial);
        g_free(info->current_mode_id);
        if (info->modes) {
            g_ptr_array_free(info->modes, TRUE);
        }
        g_free(info);
    }
}

/**
 * Parse monitor modes from GVariant
 */
static GPtrArray *parse_monitor_modes(GVariantIter *modes_iter)
{
    GPtrArray *modes = g_ptr_array_new_with_free_func((GDestroyNotify)mutter_mode_info_free);
    GVariant *mode = NULL;

    while (g_variant_iter_next(modes_iter, "@" MODE_FORMAT, &mode)) {
        MutterModeInfo *mode_info = g_new0(MutterModeInfo, 1);
        GVariant *properties = NULL;

        g_variant_get_child(mode, 0, "s", &mode_info->mode_id);
        g_variant_get_child(mode, 1, "i", &mode_info->width);
        g_variant_get_child(mode, 2, "i", &mode_info->height);
        g_variant_get_child(mode, 3, "d", &mode_info->refresh_rate);

        g_variant_get_child(mode, 6, "@a{sv}", &properties);
        if (!g_variant_lookup(properties, "is-current", "b", &mode_info->is_current)) {
            mode_info->is_current = FALSE;
        }
        if (!g_variant_lookup(properties, "is-preferred", "b", &mode_info->is_preferred)) {
            mode_info->is_preferred = FALSE;
        }
        g_variant_unref(properties);
        g_variant_unref(mode);

        g_ptr_array_add(modes, mode_info);
    }

    return modes;
}

/**
 * Find the position and transform for a connector from logical monitors
 */
static void find_logical_monitor_info(GVariantIter *logical_monitors,
                                       const gchar *connector,
                                       int *x, int *y,
                                       gdouble *scale, guint32 *transform,
                                       gboolean *is_primary)
{
    GVariant *logical_monitor = NULL;
    GVariantIter *iter_copy = g_variant_iter_copy(logical_monitors);

    *x = 0;
    *y = 0;
    *scale = 1.0;
    *transform = 0;
    *is_primary = FALSE;

    while (g_variant_iter_next(iter_copy, "@" LOGICAL_MONITOR_FORMAT, &logical_monitor)) {
        GVariantIter *monitors_iter = NULL;
        int lm_x, lm_y;
        gdouble lm_scale;
        guint32 lm_transform;
        gboolean lm_primary;

        g_variant_get_child(logical_monitor, 0, "i", &lm_x);
        g_variant_get_child(logical_monitor, 1, "i", &lm_y);
        g_variant_get_child(logical_monitor, 2, "d", &lm_scale);
        g_variant_get_child(logical_monitor, 3, "u", &lm_transform);
        g_variant_get_child(logical_monitor, 4, "b", &lm_primary);
        g_variant_get_child(logical_monitor, 5, LOGICAL_MONITOR_MONITORS_FORMAT, &monitors_iter);

        GVariant *monitor_spec = NULL;
        gboolean found = FALSE;
        while (!found && g_variant_iter_next(monitors_iter, "@" MONITOR_SPEC_FORMAT, &monitor_spec)) {
            const gchar *conn;
            g_variant_get_child(monitor_spec, 0, "&s", &conn);
            if (g_strcmp0(connector, conn) == 0) {
                found = TRUE;
            }
            g_variant_unref(monitor_spec);
        }

        g_variant_iter_free(monitors_iter);
        g_variant_unref(logical_monitor);

        if (found) {
            *x = lm_x;
            *y = lm_y;
            *scale = lm_scale;
            *transform = lm_transform;
            *is_primary = lm_primary;
            break;
        }
    }

    g_variant_iter_free(iter_copy);
}

/**
 * Parse all monitors from GetCurrentState response
 */
static GPtrArray *parse_monitors_from_state(VDAgentMutterDBus *mutter,
                                             GVariant *values,
                                             GVariantIter **out_logical_monitors)
{
    GPtrArray *monitors = g_ptr_array_new_with_free_func((GDestroyNotify)mutter_monitor_info_free);
    GVariantIter *monitors_iter = NULL;
    GVariantIter *logical_monitors = NULL;

    g_variant_get_child(values, 1, MONITORS_FORMAT, &monitors_iter);
    g_variant_get_child(values, 2, LOGICAL_MONITORS_FORMAT, &logical_monitors);

    GVariant *monitor = NULL;
    while (g_variant_iter_next(monitors_iter, "@" MONITOR_FORMAT, &monitor)) {
        MutterMonitorInfo *info = g_new0(MutterMonitorInfo, 1);
        GVariant *monitor_spec = NULL;
        GVariantIter *modes_iter = NULL;

        g_variant_get_child(monitor, 0, "@" MONITOR_SPEC_FORMAT, &monitor_spec);
        g_variant_get_child(monitor_spec, 0, "s", &info->connector);
        g_variant_get_child(monitor_spec, 1, "s", &info->vendor);
        g_variant_get_child(monitor_spec, 2, "s", &info->product);
        g_variant_get_child(monitor_spec, 3, "s", &info->serial);
        g_variant_unref(monitor_spec);

        g_variant_get_child(monitor, 1, MODES_FORMAT, &modes_iter);
        info->modes = parse_monitor_modes(modes_iter);
        g_variant_iter_free(modes_iter);

        // Find current mode
        for (guint i = 0; i < info->modes->len; i++) {
            MutterModeInfo *mode = g_ptr_array_index(info->modes, i);
            if (mode->is_current) {
                info->current_mode_id = g_strdup(mode->mode_id);
                break;
            }
        }

        // Get position and transform from logical monitors
        find_logical_monitor_info(logical_monitors, info->connector,
                                   &info->current_x, &info->current_y,
                                   &info->current_scale, &info->current_transform,
                                   &info->is_primary);

        info->is_enabled = (info->current_mode_id != NULL);

        // Look up SPICE display ID
        gpointer value;
        if (g_hash_table_lookup_extended(mutter->connector_mapping, info->connector, NULL, &value)) {
            info->display_id = GPOINTER_TO_INT(value);
        } else {
            info->display_id = -1;
        }

        syslog(LOG_DEBUG, "mutter: parsed monitor %s (display_id=%d, enabled=%d, mode=%s)",
               info->connector, info->display_id, info->is_enabled,
               info->current_mode_id ? info->current_mode_id : "none");

        g_variant_unref(monitor);
        g_ptr_array_add(monitors, info);
    }

    g_variant_iter_free(monitors_iter);

    if (out_logical_monitors) {
        *out_logical_monitors = logical_monitors;
    } else {
        g_variant_iter_free(logical_monitors);
    }

    return monitors;
}

/**
 * Find the best mode ID for a given resolution
 */
static const gchar *find_mode_for_resolution(MutterMonitorInfo *info, int width, int height)
{
    MutterModeInfo *best = NULL;
    gdouble best_refresh = 0;

    for (guint i = 0; i < info->modes->len; i++) {
        MutterModeInfo *mode = g_ptr_array_index(info->modes, i);
        if (mode->width == width && mode->height == height) {
            if (!best || mode->refresh_rate > best_refresh) {
                best = mode;
                best_refresh = mode->refresh_rate;
            }
        }
    }

    if (best) {
        return best->mode_id;
    }

    // Fallback: try to find any mode and log a warning
    syslog(LOG_WARNING, "mutter: no mode found for %dx%d on %s, available modes:",
           width, height, info->connector);
    for (guint i = 0; i < info->modes->len; i++) {
        MutterModeInfo *mode = g_ptr_array_index(info->modes, i);
        syslog(LOG_WARNING, "  %s: %dx%d @ %.2fHz%s%s",
               mode->mode_id, mode->width, mode->height, mode->refresh_rate,
               mode->is_current ? " (current)" : "",
               mode->is_preferred ? " (preferred)" : "");
    }

    return NULL;
}

/**
 * Check if Mutter DBus is available and responsive
 */
gboolean vdagent_mutter_is_available(VDAgentMutterDBus *mutter)
{
    if (!mutter || !mutter->dbus_proxy) {
        return FALSE;
    }

    // Try to get the name owner to verify the proxy is valid
    gchar *name_owner = g_dbus_proxy_get_name_owner(mutter->dbus_proxy);
    if (!name_owner) {
        return FALSE;
    }
    g_free(name_owner);

    return TRUE;
}

/**
 * Apply monitor configuration using Mutter's ApplyMonitorsConfig DBus method.
 *
 * Returns 0 on success, -1 on failure.
 */
int vdagent_mutter_set_monitor_config(VDAgentMutterDBus *mutter,
                                       VDAgentMonitorsConfig *mon_config)
{
    GError *error = NULL;
    guint32 serial;
    GVariant *values = NULL;
    GPtrArray *monitors = NULL;
    int ret = -1;

    if (!mutter || !mutter->dbus_proxy || !mon_config) {
        return -1;
    }

    syslog(LOG_DEBUG, "mutter: applying monitor config for %u monitors",
           mon_config->num_of_monitors);

    // Get current state to obtain serial and monitor information
    values = g_dbus_proxy_call_sync(mutter->dbus_proxy,
                                     "GetCurrentState",
                                     NULL,
                                     G_DBUS_CALL_FLAGS_NONE,
                                     -1,
                                     NULL,
                                     &error);
    if (!values) {
        syslog(LOG_WARNING, "mutter: failed to get current state: %s",
               error ? error->message : "unknown error");
        g_clear_error(&error);
        return -1;
    }

    g_variant_get_child(values, 0, "u", &serial);
    monitors = parse_monitors_from_state(mutter, values, NULL);

    // Build the logical monitors configuration
    GVariantBuilder logical_monitors_builder;
    g_variant_builder_init(&logical_monitors_builder, G_VARIANT_TYPE(APPLY_LOGICAL_MONITORS_FORMAT));

    // Track which monitors we've configured
    gboolean *configured = g_new0(gboolean, monitors->len);

    // Configure monitors according to mon_config
    for (guint i = 0; i < mon_config->num_of_monitors; i++) {
        VDAgentMonConfig *mc = &mon_config->monitors[i];

        // Find the monitor for this SPICE display
        MutterMonitorInfo *target = NULL;
        int target_idx = -1;

        for (guint j = 0; j < monitors->len; j++) {
            MutterMonitorInfo *info = g_ptr_array_index(monitors, j);
            if (info->display_id == (int)i) {
                target = info;
                target_idx = j;
                break;
            }
        }

        // If no mapping found, use monitor index
        if (!target && i < monitors->len) {
            target = g_ptr_array_index(monitors, i);
            target_idx = i;
        }

        if (!target) {
            syslog(LOG_WARNING, "mutter: no monitor found for SPICE display %u", i);
            continue;
        }

        if (target_idx >= 0) {
            configured[target_idx] = TRUE;
        }

        // Find the mode for the requested resolution
        const gchar *mode_id = find_mode_for_resolution(target, mc->width, mc->height);
        if (!mode_id) {
            // Use current mode as fallback
            mode_id = target->current_mode_id;
            if (!mode_id) {
                syslog(LOG_WARNING, "mutter: no valid mode for monitor %s", target->connector);
                continue;
            }
            syslog(LOG_WARNING, "mutter: using current mode %s for %s (requested %dx%d not available)",
                   mode_id, target->connector, mc->width, mc->height);
        } else {
            syslog(LOG_DEBUG, "mutter: setting %s to mode %s (%dx%d) at position (%d,%d)",
                   target->connector, mode_id, mc->width, mc->height, mc->x, mc->y);
        }

        // Build monitor entry for this logical monitor
        GVariantBuilder monitor_builder;
        g_variant_builder_init(&monitor_builder, G_VARIANT_TYPE(APPLY_MONITORS_FORMAT));

        GVariantBuilder props_builder;
        g_variant_builder_init(&props_builder, G_VARIANT_TYPE("a{sv}"));
        // No special properties needed for now

        g_variant_builder_add(&monitor_builder, "(ss@a{sv})",
                              target->connector,
                              mode_id,
                              g_variant_builder_end(&props_builder));

        // Build logical monitor entry
        // Use position from mon_config, keep current scale and transform
        g_variant_builder_add(&logical_monitors_builder, "(iidub@" APPLY_MONITORS_FORMAT ")",
                              (gint32)mc->x,
                              (gint32)mc->y,
                              target->current_scale > 0 ? target->current_scale : 1.0,
                              target->current_transform,
                              (i == 0),  // First monitor is primary
                              g_variant_builder_end(&monitor_builder));
    }

    g_free(configured);

    // Build global properties (empty for now)
    GVariantBuilder props_builder;
    g_variant_builder_init(&props_builder, G_VARIANT_TYPE("a{sv}"));

    // Call ApplyMonitorsConfig
    GVariant *result = g_dbus_proxy_call_sync(mutter->dbus_proxy,
                                               "ApplyMonitorsConfig",
                                               g_variant_new("(uu@" APPLY_LOGICAL_MONITORS_FORMAT "@a{sv})",
                                                             serial,
                                                             APPLY_METHOD_TEMPORARY,
                                                             g_variant_builder_end(&logical_monitors_builder),
                                                             g_variant_builder_end(&props_builder)),
                                               G_DBUS_CALL_FLAGS_NONE,
                                               5000,  // 5 second timeout
                                               NULL,
                                               &error);

    if (!result) {
        syslog(LOG_WARNING, "mutter: ApplyMonitorsConfig failed: %s",
               error ? error->message : "unknown error");
        g_clear_error(&error);
        ret = -1;
    } else {
        syslog(LOG_DEBUG, "mutter: ApplyMonitorsConfig succeeded");
        g_variant_unref(result);
        ret = 0;
    }

    g_ptr_array_free(monitors, TRUE);
    g_variant_unref(values);

    return ret;
}
