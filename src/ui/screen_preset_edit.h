#pragma once

/* Load the preset add/edit screen.
 *   idx = -1 : add mode (creates a new preset)
 *   idx >= 0 : edit mode (edits/deletes preset at that index)
 * initial_weight is the starting value shown in the picker. */
void screen_preset_edit_load(int idx, float initial_weight);
