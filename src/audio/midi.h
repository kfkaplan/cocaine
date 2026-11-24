#pragma once

#include "kernel/hooks.h"

void midi_audio_file_open(GuestCall& c);
void midi_audio_file_close(GuestCall& c);
