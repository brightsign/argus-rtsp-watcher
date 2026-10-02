BIN := bin/argus-example-client
GO  ?= go

.DEFAULT_GOAL := help

help:            ## Print available targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN{FS=":.*?## "}{printf "  %-12s %s\n", $$1, $$2}'

deps:            ## Download and tidy module dependencies
	$(GO) mod tidy

build:           ## Build the CLI to build/argus-example-client
	$(GO) build -o $(BIN) .

test: run-tests  ## Alias for run-tests

run-tests:       ## Run tests
	$(GO) test ./...

run: build       ## Build then run (pass ARGS="--broker host:1883 --health")
	$(BIN) $(ARGS)

clean:           ## Remove build artifacts
	rm -rf bin

.PHONY: help deps build test run-tests run clean
