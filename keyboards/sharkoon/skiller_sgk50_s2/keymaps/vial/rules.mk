USER_NAME := sharkoon_sgk50_s2

VIA_ENABLE = yes
VIAL_ENABLE = yes
VIALRGB_ENABLE = yes
QMK_SETTINGS = yes

RGB_MATRIX_CUSTOM_USER = yes

TAP_DANCE_ENABLE = yes
COMBO_ENABLE = yes
KEY_OVERRIDE_ENABLE = yes

# Temporary benchmark build: share keyboard with the existing HID endpoint so
# EP1 can be used by QMK Console on the WB32FQ95xC's three-endpoint USB device.
KEYBOARD_SHARED_EP = yes
CONSOLE_ENABLE = yes
