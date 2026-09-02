# Shared make rules for the pcsx2-web homebrew fixtures. Each fixture sets
# EE_BIN, EE_OBJS and optionally EE_LIBS before including this file. Builds
# run inside the pinned ps2dev image (see web/scripts/build-fixtures.sh).

FIXTURE_COMMON := $(dir $(lastword $(MAKEFILE_LIST)))

EE_INCS += -I$(FIXTURE_COMMON)
EE_OBJS += $(FIXTURE_COMMON)fixture_tty.o $(FIXTURE_COMMON)fixture_gs.o
EE_LIBS += -ldraw -lgraph -lpacket -lpacket2 -ldma
EE_DVP ?= dvp-as

all: $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

%.o: %.vsm
	$(EE_DVP) $< -o $@

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
