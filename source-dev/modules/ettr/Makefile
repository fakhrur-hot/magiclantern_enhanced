# define the module name - make sure name is max 8 characters
MODULE_NAME = ettr
# gyro_bridge.o / mlv_metadata.o intentionally dropped 2026-07-23: the video
# gyro/MLV-metadata subsystem is compiled out (see ETTR_VIDEO_GYRO_METADATA in
# ettr.c). Keeping them linked would re-import lua_* symbols and re-add the
# per-frame hot-shoe poll that regressed ETTR with a flash mounted. Restore
# both objects here (and flip the flag in ettr.c) to re-enable the feature.
MODULE_OBJS = $(BUILD_DIR)/ettr.o
TOP_DIR = ../..

# include modules environment
include $(TOP_DIR)/modules/Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/module_complete
