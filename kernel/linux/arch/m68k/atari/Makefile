#
# Makefile for Linux arch/m68k/atari source directory
#

obj-y		:= config.o time.o debug.o ataints.o stdma.o \
			atasound.o stram.o atari_ksyms.o

ifeq ($(CONFIG_PCI),y)
obj-$(CONFIG_HADES)	+= hades-pci.o
endif
