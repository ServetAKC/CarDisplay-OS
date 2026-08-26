#pragma once

// Single source of truth for the firmware version. Referenced by the boot
// banner, the player header, the README generator and the release tag. Do not
// duplicate the number anywhere else.
#define CARDISPLAY_NAME "Car Display OS"
#define CARDISPLAY_VERSION "0.3.17"
#define CARDISPLAY_BUILD_TAG CARDISPLAY_NAME " v" CARDISPLAY_VERSION
