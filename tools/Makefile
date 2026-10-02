# Host-side analysis tools for Argus RTSP Watcher recordings.
# These are host tools (stdlib only); they are NOT cross-compiled into the
# player extension. record-frames.sh needs no build.

GO  ?= go

.DEFAULT_GOAL := help

.PHONY: help
help: ## List targets
	@grep -E '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) | \
		awk 'BEGIN{FS=":.*?## "}{printf "  %-12s %s\n", $$1, $$2}'

.PHONY: build
build: bin/framebox bin/boxstats ## Build all tool binaries into ./bin

bin/framebox: $(wildcard cmd/framebox/*.go) $(wildcard internal/analytics/*.go) go.mod
	$(GO) build -o bin/framebox ./cmd/framebox

bin/boxstats: $(wildcard cmd/boxstats/*.go) $(wildcard internal/analytics/*.go) go.mod
	$(GO) build -o bin/boxstats ./cmd/boxstats

.PHONY: test
test: run-tests ## Alias for run-tests

.PHONY: run-tests
run-tests: ## Run unit tests
	$(GO) test ./...

.PHONY: clean
clean: ## Remove build artifacts
	rm -rf bin
