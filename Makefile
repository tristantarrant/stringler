# Stringler - guitar MIDI: turns MIDI notes into expressive guitar MIDI
# (legato glides, vibrato, chord shifting, scale filtering, CC dynamics).
# Functional recreation of the core tracking/expression pipeline of a
# commercial guitar-MIDI plugin (see README, "Research" section).
#
# Built with DPF: one codebase -> LV2 (+ modgui), VST3, CLAP and a JACK
# standalone app. The audio path is a transparent passthrough; the
# instrument logic is the MIDI engine in plugins/Stringler/PluginStringler.cpp.
#
# Targets:
#   make               build all formats into bin/
#   make NOUI=true     DSP only (build hosts without cairo/X11)
#   make install       install the LV2 bundle into $(INSTALL_DIR)
#   make install-all   also VST3 and CLAP into $(VST3_DIR) / $(CLAP_DIR)
#   make test          LV2 C-API test harness (MIDI engine behaviour)
#   make test-ui       drive the real UI under Xvfb (needs xvfb-run, XTest)
#   make test-modgui   check the modgui in headless Chrome (needs PIL)
#   make test-clap     CLAP load/process smoke test
#   make test-all      all of the above
#   make brand         regenerate plugins/Stringler/BrandPath.hpp (needs fontTools)
#   make presets       regenerate plugins/Stringler/Presets.hpp from presets/*.json
#   make screenshot    re-render screenshot.png (README image) from the artwork
#   make modgui        re-render the modgui images from the artwork
#   make clean
#
# The audio path is a transparent stereo passthrough; the MIDI engine
# lives in the DPF wrapper. The lv2_sep target keeps the DSP binary free
# of X11/cairo so mod-host can load it.

PLUG    := stringler
BUNDLE  := bin/$(PLUG).lv2

PREFIX      ?= /usr
INSTALL_DIR ?= $(PREFIX)/lib/lv2
VST3_DIR    ?= $(PREFIX)/lib/vst3
CLAP_DIR    ?= $(PREFIX)/lib/clap
BIN_DIR     ?= $(PREFIX)/bin

TTL_GEN := dpf/utils/lv2_ttl_generator

.PHONY: screenshot all plugin ttl brand presets modgui install install-all test test-clap test-ui test-modgui test-all clean distclean

all: ttl

# queried by the CI action before building
features:
	@echo "features: LV2 (DSP+UI+modgui), VST3, CLAP, JACK standalone"

dpf/Makefile.plugins.mk:
	git submodule update --init --recursive

$(TTL_GEN): dpf/Makefile.plugins.mk
	$(MAKE) -C dpf/utils/lv2-ttl-generator

plugin: dpf/Makefile.plugins.mk
	$(MAKE) -C plugins/Stringler NOUI=$(NOUI)

# generate the LV2 TTL (incl. presets.ttl from the plugin programs) and
# add the static modgui
ttl: plugin $(TTL_GEN) modgui
	EXE_WRAPPER="$(EXE_WRAPPER)" dpf/utils/generate-ttl.sh bin
	# DPF prefixes audio port symbols with "lv2_"; restore the plugin's
	# original "in"/"out" (pedalboards store connections by symbol)
	# -i.bak works with both GNU sed (Linux) and BSD sed (macOS)
	sed -i.bak -e 's/lv2:symbol "lv2_in"/lv2:symbol "in"/' \
	           -e 's/lv2:symbol "lv2_out"/lv2:symbol "out"/' \
	           -e 's/lv2:symbol "lv2_in_r"/lv2:symbol "in_r"/' \
	           -e 's/lv2:symbol "lv2_out_r"/lv2:symbol "out_r"/' $(BUNDLE)/$(PLUG)_dsp.ttl
	rm -f $(BUNDLE)/$(PLUG)_dsp.ttl.bak
	rm -rf $(BUNDLE)/modgui
	cp -r modgui $(BUNDLE)/modgui
	mv $(BUNDLE)/modgui/modgui.ttl $(BUNDLE)/modgui.ttl

brand:
	python3 tools/gen_brand.py --font Bitcount --text dataforte --ns sl_brand --out BrandPath.hpp

presets:
	python3 tools/gen_presets.py presets/stringler.json plugins/Stringler/Presets.hpp

build/render_modgui: tools/render_modgui.cpp plugins/Stringler/Artwork.hpp
	mkdir -p build
	$(CXX) -O2 -std=c++17 -Wall -o $@ $< $$(pkg-config --cflags --libs cairo)

modgui: build/render_modgui
	mkdir -p modgui
	build/render_modgui modgui

# README image: the face at its default settings, 1.5x the design size
screenshot: build/render_modgui
	build/render_modgui --preview screenshot.png 1.5

install: ttl
	mkdir -p "$(DESTDIR)$(INSTALL_DIR)"
	rm -rf "$(DESTDIR)$(INSTALL_DIR)/$(PLUG).lv2"
	cp -r $(BUNDLE) "$(DESTDIR)$(INSTALL_DIR)/"

install-all: install
	mkdir -p "$(DESTDIR)$(VST3_DIR)" "$(DESTDIR)$(CLAP_DIR)" "$(DESTDIR)$(BIN_DIR)"
	rm -rf "$(DESTDIR)$(VST3_DIR)/$(PLUG).vst3"
	cp -r bin/$(PLUG).vst3 "$(DESTDIR)$(VST3_DIR)/"
	cp bin/$(PLUG).clap "$(DESTDIR)$(CLAP_DIR)/"
	cp bin/$(PLUG) "$(DESTDIR)$(BIN_DIR)/$(PLUG)"

test: ttl
	$(CC) -O2 -Idpf/distrho/src -o build/sl_test tests/sl_test.c -ldl -lm
	build/sl_test $(CURDIR)/$(BUNDLE)/$(PLUG)_dsp.so

# Drives the real UI under Xvfb with XTest: host->UI values, knob drags,
# wheel, double-click reset, switches and footswitch clicks, resizing.
# LV2 control ports (0-based): mode 6, bend 7, steal 8, gate 9,
# legato 10, legatoms 11, vibdepth 12, vibrate 13, chordshift 14,
# chordwheel 15, transpose 16, scale 17, root 18, octdown 19,
# split 20, lowschan 21, highschan 22, brightamt 23, brightcc 24,
# loudamt 25, loudcc 26, velnorm 27, outchan 28, source 29,
# tracklow 30, trackres 31, trackbright 32, trackjitter 33, bendtrig 34, bypass 35.
UI_SHOTS ?= build/ui-shots
test-ui: ttl
	mkdir -p build $(UI_SHOTS)
	$(CC) -O2 -o build/ui_test tests/ui_test.c -ldl -lm -lX11 -lXtst
	xvfb-run -a -s "-screen 0 1400x1400x24" build/ui_test \
	  $(CURDIR)/$(BUNDLE)/$(PLUG)_ui.so http://dataforte.net/plugins/$(PLUG) \
	  $(CURDIR)/$(BUNDLE)/ 480x660 portmap:lv2_enabled=35 \
	  size:480x660 shot:$(UI_SHOTS)/default.png \
	  set:6=1 set:7=48 set:35=0 shot:$(UI_SHOTS)/host-values.png \
	  hover:55,100 shot:$(UI_SHOTS)/tooltip.png \
	  drag:55,100,-60 expect:7\>12 \
	  wheel:415,100,3 expect:11\>30 \
	  drag:55,200,60 expect:12\<200 dclick:55,200 expect:12=0 \
	  click:55,43.75 expect:6=0 \
	  set:17=0 click:205,300 expect:17=1 \
	  set:20=0 click:205,462 expect:20=40 \
	  click:85,620 expect:29=0 \
	  click:85,620 expect:29=1 \
	  click:85,620 expect:29=2 \
	  drag:50,555,-30 expect:30\>40 \
	  click:240,620 expect:35=1 \
	  resize:690x950 size:690x950 shot:$(UI_SHOTS)/690x950.png \
	  resize:500x373 shot:$(UI_SHOTS)/500x373-letterbox.png \
	  click:240,620 expect:35=0

# CLAP: load through the factory, activate, process, list parameters
test-clap: ttl
	mkdir -p build
	$(CC) -O2 -std=c11 -Idpf/distrho/src -o build/clap_check tests/clap_check.c -ldl -lm
	build/clap_check bin/$(PLUG).clap net.dataforte.$(PLUG) 30

test-all: test test-clap test-ui test-modgui

# Renders the modgui in headless Chrome at the given values and checks
# every control against the cairo artwork (needs google-chrome + PIL).
# Every film strip must be placed at the same frame as the cairo render,
# so all 24 parameter values are passed (plus the LED state, which
# follows bypass).
test-modgui: build/render_modgui modgui
	build/render_modgui --preview build/modgui-ref.png 1.6 \
	  0 12 1 0.1 1 30 0 5 0 0 0 0 0 1 50 1 1 0 1 0 7 0 1 2 40 0 2 0 50 > /dev/null
	python3 tests/modgui_check.py modgui build build/modgui-ref.png \
	  mode=0:0:1 bend=12:0:48 steal=1:0:12 gate=0.1:0:1 \
	  legato=1:0:1 legatoms=30:0:100 vibdepth=0:0:200 vibrate=5:0:10 \
	  chordshift=0:-12:12 chordwheel=0:0:3 transpose=0:-24:24 \
	  scale=0:0:17 root=0:0:11 octdown=1:0:2 split=50:0:64 \
	  lowschan=1:1:16 highschan=1:1:16 brightamt=0:0:2 brightcc=1:0:127 \
	  loudamt=0:0:2 loudcc=7:0:127 velnorm=0:0:127 outchan=1:1:16 \
	  source=2:0:2 tracklow=40:33:60 trackres=0:0:3 \
	  trackbright=2:0:4 trackjitter=0:0:3 bendtrig=50:0:100 bypass=0:0:1 led=on

clean:
	$(MAKE) -C plugins/Stringler clean
	rm -rf bin build

distclean: clean
	$(MAKE) -C dpf clean
