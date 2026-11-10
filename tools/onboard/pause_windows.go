//go:build windows

package main

import (
	"bufio"
	"os"
	"unsafe"

	"golang.org/x/sys/windows"

	"github.com/thin-edge/tiny-edge/tools/onboard/internal/ui"
)

var getConsoleProcessList = windows.NewLazySystemDLL("kernel32.dll").NewProc("GetConsoleProcessList")

// pauseIfStartedByDoubleClick keeps the console window open when the tool
// was started by double-clicking it: then it is the only process in its
// console, and the window closes as soon as it exits.
func pauseIfStartedByDoubleClick(u ui.UI) {
	if !u.Interactive() {
		return
	}
	ids := make([]uint32, 4)
	n, _, _ := getConsoleProcessList.Call(uintptr(unsafe.Pointer(&ids[0])), uintptr(len(ids)))
	if n != 1 {
		return
	}
	u.Say("Press Enter to close this window.")
	_, _ = bufio.NewReader(os.Stdin).ReadString('\n')
}
