KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(shell pwd)

KBUILD_OPTIONS	+= CONFIG_QCOM_QBT_HANDLER=m
EXTRA_CFLAGS	+= -I$(KERNEL_SRC)/../google-modules/fingerprint/qcom/qfs4008
EXTRA_CFLAGS	+= -I$(KERNEL_SRC)/../google-modules/fingerprint/qcom/qfs4008
EXTRA_CFLAGS	+= -I$(KERNEL_SRC)/../google-modules/gs/soc/include

include $(KERNEL_SRC)/../private/google-modules/soc/gs/Makefile.include

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) \
	$(KBUILD_OPTIONS) \
	EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
	KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" \
	$(@)
