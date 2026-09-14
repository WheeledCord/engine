# The ordinary entry point. Keep Makefile.core as the implementation while projects and scripts
# migrate; every target and variable (for example SDK= or PREFIX=) passes through unchanged.
.DEFAULT_GOAL := all
.PHONY: all
all:
	$(MAKE) -f Makefile.core all

%:
	$(MAKE) -f Makefile.core $@
