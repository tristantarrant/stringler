#!/usr/bin/make -f
# DPF build for Stringler (driven by the top-level Makefile)

NAME = stringler

FILES_DSP = PluginStringler.cpp

ifneq ($(NOUI),true)
FILES_UI  = UIStringler.cpp
UI_TYPE   = cairo
endif

include ../../dpf/Makefile.plugins.mk

# lv2_sep: separate DSP and UI binaries, so the DSP that mod-host loads
# does not depend on X11/cairo.
TARGETS ?= lv2_sep vst3 clap jack

all: $(TARGETS)
