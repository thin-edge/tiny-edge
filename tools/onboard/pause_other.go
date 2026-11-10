//go:build !windows

package main

import "github.com/thin-edge/tiny-edge/tools/onboard/internal/ui"

// pauseIfStartedByDoubleClick: macOS Terminal and Linux terminals keep the
// window open after the program ends, so there is nothing to do.
func pauseIfStartedByDoubleClick(ui.UI) {}
