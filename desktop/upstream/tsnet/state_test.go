// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"path/filepath"
	"testing"
)

func TestNodeStateExclusive(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "Tailnet")
	closeState, err := prepareStateDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	if closeAgain, err := prepareStateDir(dir); err == nil {
		closeAgain()
		closeState()
		t.Fatal("a second node opened the same persisted state")
	}
	closeState()
	closeState, err = prepareStateDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	closeState()
}
