/* kwin.h - implements the KDE Wayland output management protocol
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

#ifndef SRC_VDAGENT_KWIN_H_
#define SRC_VDAGENT_KWIN_H_

#include <glib.h>
#include <spice/vd_agent.h>

typedef struct VDAgentKWin VDAgentKWin;

/**
 * Create a KWin Wayland output management client.
 *
 * This connects to the Wayland display and binds to the
 * kde_output_device_v2 and kde_output_management_v2 protocols.
 *
 * @param connector_mapping Hash table mapping connector names to SPICE display IDs
 * @return A new VDAgentKWin instance, or NULL if KWin protocols are unavailable
 */
VDAgentKWin *vdagent_kwin_create(GHashTable *connector_mapping);

/**
 * Destroy a KWin client and free resources.
 *
 * @param kwin The KWin client to destroy
 */
void vdagent_kwin_destroy(VDAgentKWin *kwin);

/**
 * Get current display resolutions from KWin.
 *
 * @param kwin The KWin client
 * @param width Output: total desktop width
 * @param height Output: total desktop height
 * @param screen_count Output: number of screens
 * @return GArray of vdagentd_guest_xorg_resolution, or NULL on failure
 */
GArray *vdagent_kwin_get_resolutions(VDAgentKWin *kwin,
                                     int *width, int *height, int *screen_count);

/**
 * Set monitor configuration via KWin.
 *
 * This applies the requested resolution and position changes using
 * the kde_output_management_v2 protocol.
 *
 * @param kwin The KWin client
 * @param mon_config The monitor configuration to apply
 * @return 0 on success, -1 on failure
 */
int vdagent_kwin_set_monitor_config(VDAgentKWin *kwin,
                                    VDAgentMonitorsConfig *mon_config);

/**
 * Check if KWin protocols are available.
 *
 * @param kwin The KWin client
 * @return TRUE if KWin protocols are bound, FALSE otherwise
 */
gboolean vdagent_kwin_is_available(VDAgentKWin *kwin);

#endif /* SRC_VDAGENT_KWIN_H_ */
