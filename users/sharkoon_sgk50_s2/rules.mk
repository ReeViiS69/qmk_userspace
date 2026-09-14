# gpio dma rgb driver
WS2812_DRIVER = custom
OPT_DEFS += -DWB32_DMA_REQUIRED

# atleast my sharkoon_sgk50_s2 has C variant but qmk assumes b which limits mcu to 28kb ram and 128kb onboard flash.
MCU_LDSCRIPT = WB32FQ95xC
OPT_DEFS += -DWB32FQ95xC

SRC += sharkoon_sgk50_s2.c

LTO_ENABLE = yes

# Common QMK features
COMMAND_ENABLE = no
BOOTMAGIC_ENABLE = yes
RAW_ENABLE = yes
SEND_STRING_ENABLE = yes
DYNAMIC_KEYMAP_ENABLE = yes
TRI_LAYER_ENABLE = yes
MOUSEKEY_ENABLE = yes
CAPS_WORD_ENABLE = yes

LAYER_LOCK_ENABLE = yes
REPEAT_KEY_ENABLE = yes
CONSOLE_ENABLE = yes
KEYBOARD_SHARED_EP = yes
