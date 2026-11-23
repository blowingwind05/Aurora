// SPDX-License-Identifier: BSD-3-Clause

package main

import (
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/svc/mgr"
)

type systemImageID struct {
	Path   string `json:"path"`
	Volume uint32 `json:"volume"`
	Index  uint64 `json:"index"`
}

type parkedSystemStartup struct {
	Image     systemImageID `json:"image"`
	StartType uint32        `json:"startType"`
}

type systemParking struct {
	path  string
	image systemImageID
	prior *parkedSystemStartup
}

func imageIdentity(file *os.File) (systemImageID, error) {
	var info windows.ByHandleFileInformation
	if err := windows.GetFileInformationByHandle(windows.Handle(file.Fd()), &info); err != nil {
		return systemImageID{}, err
	}
	index := uint64(info.FileIndexHigh)<<32 | uint64(info.FileIndexLow)
	if index == 0 {
		return systemImageID{}, errors.New("the system service image has no stable file identity")
	}
	return systemImageID{file.Name(), info.VolumeSerialNumber, index}, nil
}

func readSystemParking(root string, image systemImageID) (*systemParking, func(), error) {
	directory := filepath.Join(root, "SwitchState")
	handle, err := ensureDirectory(directory, false)
	if err != nil {
		return nil, nil, err
	}
	release := func() { windows.CloseHandle(handle) }
	state := &systemParking{path: filepath.Join(directory, "stock-startup.json"), image: image}
	file, err := openProtectedFile(state.path)
	if errors.Is(err, os.ErrNotExist) {
		return state, release, nil
	}
	if err != nil {
		release()
		return nil, nil, err
	}
	defer file.Close()
	payload, err := io.ReadAll(io.LimitReader(file, 4097))
	if err != nil {
		release()
		return nil, nil, err
	}
	var prior parkedSystemStartup
	if len(payload) > 4096 || json.Unmarshal(payload, &prior) != nil ||
		!validOriginalStartup(prior.StartType) || prior.Image.Index == 0 || !filepath.IsAbs(prior.Image.Path) {
		release()
		return nil, nil, errors.New("invalid system Tailscale parking record")
	}
	state.prior = &prior
	return state, release, nil
}

func validOriginalStartup(value uint32) bool {
	return value == mgr.StartManual || value == mgr.StartAutomatic
}

func (p *systemParking) selectedStartup(current uint32) (uint32, error) {
	if validOriginalStartup(current) {
		return current, nil
	}
	if current == mgr.StartDisabled && p.prior != nil &&
		p.prior.Image.Volume == p.image.Volume && p.prior.Image.Index == p.image.Index &&
		strings.EqualFold(p.prior.Image.Path, p.image.Path) && validOriginalStartup(p.prior.StartType) {
		return p.prior.StartType, nil
	}
	return 0, errors.New("the system Tailscale startup type is disabled by policy or has no matching Aurora parking record")
}

func (p *systemParking) save(before uint32) error {
	if before == mgr.StartDisabled {
		// Keep any earlier record. A pre-disabled service without a record
		// remains disabled when someone later tries to select it.
		return nil
	}
	if !validOriginalStartup(before) {
		return errors.New("unexpected system Tailscale startup type")
	}
	value, err := json.Marshal(parkedSystemStartup{Image: p.image, StartType: before})
	if err != nil {
		return err
	}
	temporary := filepath.Join(filepath.Dir(p.path), ".parked-"+rand.Text())
	file, err := os.OpenFile(temporary, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	defer os.Remove(temporary)
	_, writeErr := file.Write(value)
	syncErr := file.Sync()
	closeErr := file.Close()
	if err := errors.Join(writeErr, syncErr, closeErr); err != nil {
		return err
	}
	from, err := windows.UTF16PtrFromString(temporary)
	if err != nil {
		return err
	}
	to, err := windows.UTF16PtrFromString(p.path)
	if err != nil {
		return err
	}
	if err := windows.MoveFileEx(from, to, windows.MOVEFILE_REPLACE_EXISTING|windows.MOVEFILE_WRITE_THROUGH); err != nil {
		return fmt.Errorf("commit the original system startup mode: %w", err)
	}
	return nil
}

func (p *systemParking) clear() error {
	if err := os.Remove(p.path); err != nil && !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("clear the system startup parking record: %w", err)
	}
	return nil
}
