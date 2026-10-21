/* Compile the new panel and launch-word helpers with the production SDK.
 * SPDX-License-Identifier: MIT */
#include "../../../apps/dosbox/network_panel.h"
#include "../../../apps/dosbox/network_settings.h"
void dosbox_network_panel_compile(struct du_canvas *canvas,struct ciuki_dosvm_command *command,unsigned game)
{
    unsigned mode=dosbox_network_load(game);
    dosvm_launch_network_set(command,mode);
    dosbox_network_panel(canvas,dosbox_network_label(mode),"Changes apply on relaunch.");
}
