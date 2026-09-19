/*
 * lcdui_input_vita.c - PS Vita input implementation for MIDP LCUI
 *
 * Purpose: Map Vita SceCtrl input to J2ME key codes
 *          Poll the Vita controller and translate button presses
 *          to the corresponding J2ME key event codes
 *
 * Vita Controller Mapping:
 *   Cross (X)       -> KEY_NUM0 (48)
 *   Circle (O)      -> KEY_NUM1 (49)
 *   Square (□)      -> KEY_NUM2 (50)
 *   Triangle (△)    -> KEY_NUM3 (51)
 *   D-Pad Up        -> KEY_UP (1)
 *   D-Pad Down      -> KEY_DOWN (2)
 *   D-Pad Left      -> KEY_LEFT (3)
 *   D-Pad Right     -> KEY_RIGHT (4)
 *   L1              -> KEY_SOFT1 (-1)
 *   R1              -> KEY_SOFT2 (-2)
 *   L2              -> GAME_C (-3)
 *   R2              -> GAME_D (-4)
 *   START           -> Custom (not standard J2ME)
 *   SELECT          -> Custom (not standard J2ME)
 */

#include <kni.h>
#include <psp2/ctrl.h>
#include <jvm.h>

#include "platform_vita.h"

/*
 * Vita to J2ME key code mapping table
 * Indexed by Vita button bitmask value
 */
static const struct {
    uint32_t vitaButton;
    int j2meKeyCode;
} keyMap[] = {
    {SCE_CTRL_CROSS,     KEY_NUM0},    // A button (X on Vita)
    {SCE_CTRL_CIRCLE,    KEY_NUM1},    // B button (O on Vita)
    {SCE_CTRL_SQUARE,    KEY_NUM2},    // X button (□ on Vita)
    {SCE_CTRL_TRIANGLE,  KEY_NUM3},    // Y button (△ on Vita)
    {SCE_CTRL_UP,        KEY_UP},      // Up direction
    {SCE_CTRL_DOWN,      KEY_DOWN},    // Down direction
    {SCE_CTRL_LEFT,      KEY_LEFT},    // Left direction
    {SCE_CTRL_RIGHT,     KEY_RIGHT},   // Right direction
    {SCE_CTRL_LTRIGGER,  KEY_SOFT1},   // L1 shoulder button
    {SCE_CTRL_RTRIGGER,  KEY_SOFT2},   // R1 shoulder button
    {SCE_CTRL_L2,        GAME_C},      // L2 trigger
    {SCE_CTRL_R2,        GAME_D},      // R2 trigger
    {0,                  -1}           // Sentinel (no match)
};

/*
 * Current key state table
 * Tracks which J2ME key codes are currently pressed
 */
static int keyStates[256] = {0};

/**
 * Initialize the Vita input system
 * Sets up controller sampling mode
 */
int platform_vita_input_init(void) {
    // Set analog wide mode for better compatibility
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    return 0;
}

/**
 * Poll the Vita controller and update key state table
 * Must be called regularly (e.g., from the main game loop)
 */
void platform_vita_poll_input(void) {
    SceCtrlData padData;

    // Read current controller state from port 0
    int result = sceCtrlPeekBufferPositive(0, &padData, 1);

    if (result >= 0) {
        // Update key states based on pressed buttons
        for (int i = 0; keyMap[i].vitaButton != 0; i++) {
            if (padData.buttons & keyMap[i].vitaButton) {
                // Button is pressed - set key state
                int keyCode = keyMap[i].j2meKeyCode;
                if (keyCode >= 0 && keyCode < 256) {
                    keyStates[keyCode] = 1;
                }
            } else {
                // Button is released - clear key state
                int keyCode = keyMap[i].j2meKeyCode;
                if (keyCode >= 0 && keyCode < 256) {
                    keyStates[keyCode] = 0;
                }
            }
        }
    }
}

/**
 * Get the current state of a J2ME key code
 * @param keyCode J2ME key code to check
 * @return KNI_TRUE if the key is currently pressed, KNI_FALSE otherwise
 */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_lcdui_EventHandler_getKeyState) {
    int j2meKeyCode = KNI_GetParameterAsInt(1);

    // Ensure we poll the controller first
    platform_vita_poll_input();

    // Check the key state table
    jboolean isPressed = KNI_FALSE;

    if (j2meKeyCode >= 0 && j2meKeyCode < 256) {
        isPressed = (keyStates[j2meKeyCode] != 0) ? KNI_TRUE : KNI_FALSE;
    }

    KNI_ReturnBoolean(isPressed);
}

/**
 * Check if a specific J2ME key was just pressed (edge detection)
 * This detects transitions from released to pressed state
 */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_lcdui_EventHandler_keyPressed) {
    int j2meKeyCode = KNI_GetParameterAsInt(1);

    // Poll controller to get current state
    platform_vita_poll_input();

    // For edge detection, we need to track previous state
    // This is a simplified implementation
    static int prevKeyStates[256] = {0};
    int currentState = keyStates[j2meKeyCode];
    int previousState = prevKeyStates[j2meKeyCode];

    // Mark current as previous for next call
    prevKeyStates[j2meKeyCode] = currentState;

    // Return true if just pressed (transition from 0 to 1)
    jboolean result = KNI_FALSE;
    if (currentState == 1 && previousState == 0) {
        result = KNI_TRUE;
    }

    KNI_ReturnBoolean(result);
}