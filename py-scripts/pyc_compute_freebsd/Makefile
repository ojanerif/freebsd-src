#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Advanced Micro Devices, Inc.
#
# Author: Davi Chaves Azevedo

.PHONY: build dev clean test topo probe sweep count ibs demo

PYTHON ?= python3
CMD ?= /bin/true
SWEEP_MIN_KB ?= 1
SWEEP_MAX_KB ?= 262144
SWEEP_STEPS ?= 40
SWEEP_REPEAT ?= 3
SWEEP_SCALE ?= robust
SWEEP_TOP_HOTSPOTS ?= 12
SWEEP_ARGS = --min-kb $(SWEEP_MIN_KB) --max-kb $(SWEEP_MAX_KB) \
	--steps $(SWEEP_STEPS) --repeat $(SWEEP_REPEAT) \
	--scale $(SWEEP_SCALE) --top-hotspots $(SWEEP_TOP_HOTSPOTS)

build:
	$(PYTHON) setup.py build_ext --inplace

dev:
	$(PYTHON) -m pip install -e .

clean:
	rm -rf build/ dist/ *.egg-info .pytest_cache freebsd_cache_hotspot/*.so freebsd_cache_hotspot/__pycache__ tests/__pycache__
	find . -name '*.o' -delete

test:
	$(PYTHON) -m pytest tests -q

topo:
	$(PYTHON) -m freebsd_cache_hotspot topo

probe:
	$(PYTHON) -m freebsd_cache_hotspot probe --cpu 0 --bandwidth

count:
	$(PYTHON) -m freebsd_cache_hotspot count -- $(CMD)

sweep:
	@outdir=$$(mktemp -d /tmp/fch-sweep.XXXXXX) && \
	  $(PYTHON) -m freebsd_cache_hotspot sweep $(SWEEP_ARGS) \
	    --csv "$$outdir/fch-sweep.csv" && \
	  echo "Artifacts: $$outdir"

ibs:
	@outdir=$$(mktemp -d /tmp/fch-ibs.XXXXXX) && \
	  sudo $(PYTHON) -m freebsd_cache_hotspot ibs --l3miss --top 40 --outdir "$$outdir" -- $(CMD) && \
	  echo "Artifacts: $$outdir"

demo: build
	@echo '=== TESTS ===' && $(PYTHON) -m pytest tests -q
	@echo '=== TOPOLOGY ===' && $(PYTHON) -m freebsd_cache_hotspot topo
	@echo '=== PARALLEL PER-CCX SWEEP (live heatmap) ===' && \
	  outdir=$$(mktemp -d /tmp/fch-sweep.XXXXXX) && \
	  $(PYTHON) -m freebsd_cache_hotspot sweep $(SWEEP_ARGS) \
	    --csv "$$outdir/fch-sweep.csv" && \
	  echo "Artifacts: $$outdir"
	@echo '=== CORE PMC COUNTS ===' && \
	  outdir=$$(mktemp -d /tmp/fch-counts.XXXXXX) && \
	  sudo $(PYTHON) -m freebsd_cache_hotspot count \
	    --events cycles,instructions,dc_fill_l3,dc_fill_near_dram,dc_fill_far_dram,l2_miss \
	    --json "$$outdir/fch-counts.json" -- $(CMD) && \
	  echo "Artifacts: $$outdir"
	@echo '=== TOPDOWN ===' && \
	  sudo $(PYTHON) -m freebsd_cache_hotspot count --topdown -- $(CMD)
	@echo '=== IBS OP (L3 miss) ===' && \
	  outdir=$$(mktemp -d /tmp/fch-ibs.XXXXXX) && \
	  sudo $(PYTHON) -m freebsd_cache_hotspot ibs --period 65536 --l3miss --top 40 \
	    --outdir "$$outdir" -- $(CMD) && \
	  echo "Artifacts: $$outdir"
