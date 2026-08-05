# pgcortrix — PGXS Makefile (independent build, NOT part of the main cortrix/
# CMake; L1-α briefing §6 / pgcortrix).
#
# Standard PostgreSQL extension build via PGXS:
#   make                       # stages the SQL + control (no compiled module in V1)
#   make install               # copies pgcortrix.control + sql/*.sql into PG's
#                              #   share/extension, and python/*.py into the
#                              #   plpython3u site-packages (see install-python)
#   make installcheck          # pg_regress (requires a live PG — D3.5, not CI)
#
# V1 is pure plpython3u + SQL, so there is no MODULE_big / OBJS to compile. The
# Python helper is installed alongside so `import pgcortrix_helper` resolves
# inside the plpython3u interpreter.

EXTENSION   = pgcortrix
DATA        = sql/pgcortrix--1.0.sql
PG_CONFIG  ?= pg_config

# Python helper module(s) shipped into the plpython3u site-packages.
PYTHON_SRC  = python/pgcortrix_helper.py

# pg_regress test scripts live under test/ once a live PG is available (D3.5).
# Until then `make installcheck` is a no-op-equivalent (no REGRESS targets).
# REGRESS   = pgcortrix_basic          # enabled in D3.5

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Resolve the plpython3u site-packages dir (PG's bundled python). Overridable.
PLPYTHON_SITELIB ?= $(shell $(PG_CONFIG) --pkglibdir)/plpython

.PHONY: install-python
install-python:
	@echo "Installing Python helper -> $(PLPYTHON_SITELIB)"
	@mkdir -p "$(PLPYTHON_SITELIB)"
	@cp $(PYTHON_SRC) "$(PLPYTHON_SITELIB)/"

# Chain the Python install onto the standard `install` target.
install: install-python
