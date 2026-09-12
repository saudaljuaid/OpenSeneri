# SPDX-License-Identifier: GPL-3.0-only
SHELL := /bin/sh
PYTHON := python3
CARGO := cargo

BUILD_DIR := build
IMAGE_DIR := image
IMAGE_TARGET := x86_64-unknown-none
IMAGE := $(IMAGE_DIR)/target/$(IMAGE_TARGET)/release/media-editor
LINKER_SCRIPT := targets/media-editor.ld

# Every flag here is load-bearing, and docs/PLATFORM_CONTRACT.md records the
# experiment that established it. The short version: Phipia identity-maps the
# whole low 4 GiB for itself, so a user image lives at 70 TiB, where no address
# fits a 32-bit displacement - which forces the large code model, forces GNU ld
# over lld, and forbids link-time optimisation against the precompiled
# standard library.
IMAGE_RUSTFLAGS := \
	-C linker=gcc \
	-C relocation-model=static \
	-C code-model=large \
	-C linker-flavor=gcc \
	-C link-arg=-fuse-ld=bfd \
	-C link-arg=-nostdlib \
	-C link-arg=-static \
	-C link-arg=-no-pie \
	-C link-arg=-Wl,-T,$(CURDIR)/$(LINKER_SCRIPT) \
	-C link-arg=-Wl,-z,noexecstack \
	-C link-arg=-Wl,--build-id=none \
	-C link-arg=-Wl,--fatal-warnings \
	-C link-arg=-Wl,--orphan-handling=error \
	--remap-path-prefix=$(CURDIR)=/media-editor

.PHONY: all audit check clean fmt hooks image lint reproducible test verify

all: lint check test image audit

hooks:
	git config core.hooksPath .githooks

lint:
	$(PYTHON) tools/lint.py
	$(PYTHON) tools/layering.py
	$(PYTHON) tools/counts.py

fmt:
	$(CARGO) fmt --all

check:
	$(CARGO) fmt --all -- --check
	$(CARGO) clippy --offline --all-targets --locked -- \
		-W clippy::pedantic -D warnings

test:
	$(CARGO) test --offline --locked

# The program image Phipia will run, once PHIP-01 and PHIP-02 exist.
image:
	RUSTFLAGS='$(IMAGE_RUSTFLAGS)' \
		$(CARGO) build --release --offline --locked \
		--target $(IMAGE_TARGET) --manifest-path $(IMAGE_DIR)/Cargo.toml
	@ls -l $(IMAGE)

audit: image
	$(PYTHON) tools/elf-audit.py $(IMAGE)
	$(PYTHON) tools/audit-control.py $(IMAGE)
	$(PYTHON) tools/footprint.py $(IMAGE)

# R-13.2: two clean builds of one commit are byte-identical. Building into two
# different directories keeps the second from being the first.
reproducible:
	rm -rf $(BUILD_DIR)/repro-a $(BUILD_DIR)/repro-b
	mkdir -p $(BUILD_DIR)/repro-a $(BUILD_DIR)/repro-b
	RUSTFLAGS='$(IMAGE_RUSTFLAGS)' CARGO_TARGET_DIR=$(CURDIR)/$(BUILD_DIR)/repro-a \
		$(CARGO) build --release --offline --locked \
		--target $(IMAGE_TARGET) --manifest-path $(IMAGE_DIR)/Cargo.toml
	RUSTFLAGS='$(IMAGE_RUSTFLAGS)' CARGO_TARGET_DIR=$(CURDIR)/$(BUILD_DIR)/repro-b \
		$(CARGO) build --release --offline --locked \
		--target $(IMAGE_TARGET) --manifest-path $(IMAGE_DIR)/Cargo.toml
	cmp $(BUILD_DIR)/repro-a/$(IMAGE_TARGET)/release/media-editor \
		$(BUILD_DIR)/repro-b/$(IMAGE_TARGET)/release/media-editor
	@echo 'reproducible: two clean builds are byte-identical'

verify: lint check test audit reproducible
	@sha256sum $(IMAGE) | awk '{ print toupper($$1) "  " $$2 }'
	@echo 'verify: passed'

clean:
	$(CARGO) clean
	$(CARGO) clean --manifest-path $(IMAGE_DIR)/Cargo.toml
	rm -rf $(BUILD_DIR)
