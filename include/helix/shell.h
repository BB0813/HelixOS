#pragma once

#include "helix/boot_info.h"

void shell_init(struct helix_boot_info *info);
/* Poll serial RX, echo, run a line when Enter pressed. Non-blocking. */
void shell_poll(void);
void shell_print_prompt(void);
