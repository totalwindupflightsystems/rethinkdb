# Stub Makefile for the vendored Python driver.
#
# The driver at driver/python3 is pure Python and importable in place (it
# contains the rethinkdb package with __init__.py and ast.py), so there is
# nothing to build. This target exists so that the integration test harness
# (test/common/utils.py, build_in_folder) can `make -C driver/python3`
# unconditionally when PYTHON_DRIVER is not set.

.PHONY: all

all:
	@true
