# Host unit tests for the DSP maths (components/dsp_core). Run: make -C test/host
CC ?= cc
CFLAGS += -std=c11 -O2 -Wall -Wextra -Werror -I../../components/dsp_core/include
LDLIBS += -lm

BUILD := build
SRCS := test_dsp.c ../../components/dsp_core/dsp_core.c

.PHONY: test clean
test: $(BUILD)/test_dsp
	./$(BUILD)/test_dsp

$(BUILD)/test_dsp: $(SRCS) ../../components/dsp_core/include/dsp_core.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

clean:
	rm -rf $(BUILD)
