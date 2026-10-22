/* Compile expert-file launch helpers with the production SDK. SPDX-License-Identifier: MIT */
#include "../../../apps/dosbox/network_settings.h"
void dosbox_configuration_compile(struct ciuki_dosvm_command *command,unsigned game)
{
    command->launch.kernel=dosbox_setting_load(game);
    command->launch.memory=dosbox_memory_load(game);
    dosvm_launch_network_set(command,dosbox_network_load(game));
}
