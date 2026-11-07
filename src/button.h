// Button input and deep-sleep wake configuration.
#pragma once

#include <stdbool.h>

void button_init(void);
bool button_pressed(void);
// Arm the button as an EXT1 deep-sleep wake source.
void button_enable_wake(void);
