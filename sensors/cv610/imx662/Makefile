LIB_NAME := libsns_imx662

CV610_CC ?= arm-openipc-linux-musleabi-gcc
CV610_AR ?= arm-openipc-linux-musleabi-ar
CV610_SDK_INC ?= ../../../../openhisilicon
COMMON := $(CV610_SDK_INC)/libraries/sensor/hi3516cv6xx/common

CFLAGS += -fPIC -Wall -Wextra -Werror -O2 \
	-I$(CV610_SDK_INC)/libraries/isp/include/hi3516cv6xx \
	-I$(CV610_SDK_INC)/libraries/isp/include/hi3516cv6xx/3a \
	-I$(CV610_SDK_INC)/libraries/isp/include/hi3516cv6xx/ext_inc \
	-I$(CV610_SDK_INC)/kernel/include/hi3516cv6xx \
	-I$(CV610_SDK_INC)/kernel/include/hi3516cv6xx/exp_inc \
	-I$(CV610_SDK_INC)/kernel/include/hi3516cv6xx/module \
	-I$(COMMON) -I.

SRCS := imx662_cmos.c imx662_sensor_ctl.c $(COMMON)/sensor_common.c
OBJS := imx662_cmos.o imx662_sensor_ctl.o sensor_common.o
DEPS := $(OBJS:.o=.d)

# The tuning tables and the register sequence live in headers, so without
# generated dependencies a header-only edit relinks the old objects and ships
# a binary that does not contain the change.
CFLAGS += -MMD -MP

.PHONY: all clean

all: $(LIB_NAME).so

%.o: %.c
	$(CV610_CC) $(CFLAGS) -c $< -o $@

sensor_common.o: $(COMMON)/sensor_common.c
	$(CV610_CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

$(LIB_NAME).so: $(OBJS)
	$(CV610_CC) -shared -o $@ $(OBJS) -lm

clean:
	rm -f $(OBJS) $(DEPS) $(LIB_NAME).so
