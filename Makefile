# Top-level Makefile - builds the library, the init executable, and the users
# by invoking the three required sub-makefiles.

all: lib init user

lib:
	$(MAKE) -f Makefile.lib

init: lib
	$(MAKE) -f Makefile.init

user: lib
	$(MAKE) -f Makefile.user

clean:
	$(MAKE) -f Makefile.lib clean
	$(MAKE) -f Makefile.init clean
	$(MAKE) -f Makefile.user clean

.PHONY: all lib init user clean
