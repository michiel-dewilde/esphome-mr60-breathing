# Host-side build. The DSP is plain C with no ESPHome or IDF dependency, so the
# firmware's own signal processing can be regression-tested on a workstation
# against the recordings that produced the published numbers.

CC      ?= cc
# The 180 s reference recording needs a deeper ring than the firmware ships
# with; see the DSP_MAX_SAMPLES note in dsp.h.
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -DDSP_MAX_SAMPLES=1024
LDLIBS  ?= -lm

REPLAY  := tools/replay/replay
SRC     := tools/replay/replay.c components/mr60_breathing/dsp.c

.PHONY: all test lang clean

all: $(REPLAY)

$(REPLAY): $(SRC) components/mr60_breathing/dsp.h
	$(CC) $(CFLAGS) -Icomponents/mr60_breathing -o $@ $(SRC) $(LDLIBS)

test: lang $(REPLAY)
	python3 tools/run_tests.py

lang:
	python3 tools/check_language.py

clean:
	rm -f $(REPLAY)
