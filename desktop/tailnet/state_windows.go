// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"unsafe"

	"golang.org/x/sys/windows"
)

func prepareStateDir(dir string) (func(), error) {
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return nil, err
	}
	system, err := windows.CreateWellKnownSid(windows.WinLocalSystemSid)
	if err != nil {
		return nil, err
	}
	admins, err := windows.CreateWellKnownSid(windows.WinBuiltinAdministratorsSid)
	if err != nil {
		return nil, err
	}
	private := func(sid *windows.SID) bool {
		return sid.Equals(user.User.Sid) || sid.Equals(system) || sid.Equals(admins)
	}

	if err := os.MkdirAll(filepath.Dir(dir), 0700); err != nil {
		return nil, err
	}
	path, err := windows.UTF16PtrFromString(dir)
	if err != nil {
		return nil, err
	}
	sid := user.User.Sid.String()
	sd, err := windows.SecurityDescriptorFromString("O:" + sid + "D:P(A;OICI;FA;;;" + sid + ")(A;OICI;FA;;;SY)")
	if err != nil {
		return nil, err
	}
	attributes := windows.SecurityAttributes{SecurityDescriptor: sd}
	attributes.Length = uint32(unsafe.Sizeof(attributes))
	if err := windows.CreateDirectory(path, &attributes); err != nil && !errors.Is(err, windows.ERROR_ALREADY_EXISTS) {
		return nil, err
	}

	directory, err := windows.CreateFile(path, windows.READ_CONTROL, windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE,
		nil, windows.OPEN_EXISTING, windows.FILE_FLAG_BACKUP_SEMANTICS|windows.FILE_FLAG_OPEN_REPARSE_POINT, 0)
	if err != nil {
		return nil, err
	}
	keep := false
	defer func() {
		if !keep {
			_ = windows.CloseHandle(directory)
		}
	}()
	if err := checkPrivatePath(directory, true, user.User.Sid, private); err != nil {
		return nil, fmt.Errorf("check embedded node state directory: %w", err)
	}

	lockPath, err := windows.UTF16PtrFromString(filepath.Join(dir, ".aurora.lock"))
	if err != nil {
		return nil, err
	}
	// An elevated token can default new files to the Administrators owner.
	// Give a newly created lock the same explicit user owner as its directory.
	lock, err := windows.CreateFile(lockPath, windows.GENERIC_READ|windows.GENERIC_WRITE, 0, &attributes,
		windows.OPEN_ALWAYS, windows.FILE_FLAG_OPEN_REPARSE_POINT, 0)
	if err != nil {
		return nil, fmt.Errorf("lock embedded node state: %w", err)
	}
	if err := checkPrivatePath(lock, false, user.User.Sid, private); err != nil {
		windows.CloseHandle(lock)
		return nil, fmt.Errorf("check embedded node state lock: %w", err)
	}

	statePath, err := windows.UTF16PtrFromString(filepath.Join(dir, stateFile))
	if err != nil {
		windows.CloseHandle(lock)
		return nil, err
	}
	state, err := windows.CreateFile(statePath, windows.READ_CONTROL, windows.FILE_SHARE_READ,
		nil, windows.OPEN_EXISTING, windows.FILE_FLAG_OPEN_REPARSE_POINT, 0)
	if err == nil {
		err = checkPrivatePath(state, false, user.User.Sid, private)
		windows.CloseHandle(state)
	}
	if err != nil && !errors.Is(err, windows.ERROR_FILE_NOT_FOUND) {
		windows.CloseHandle(lock)
		return nil, fmt.Errorf("check persisted embedded node state: %w", err)
	}

	keep = true
	return func() {
		windows.CloseHandle(lock)
		windows.CloseHandle(directory)
	}, nil
}

func checkPrivatePath(handle windows.Handle, directory bool, owner *windows.SID, trusted func(*windows.SID) bool) error {
	var info windows.ByHandleFileInformation
	if err := windows.GetFileInformationByHandle(handle, &info); err != nil {
		return err
	}
	if info.FileAttributes&windows.FILE_ATTRIBUTE_REPARSE_POINT != 0 ||
		(info.FileAttributes&windows.FILE_ATTRIBUTE_DIRECTORY != 0) != directory ||
		(!directory && info.NumberOfLinks != 1) {
		return errors.New("embedded node state uses a reparse point or an unexpected file type")
	}
	sd, err := windows.GetSecurityInfo(handle, windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return err
	}
	actualOwner, _, err := sd.Owner()
	if err != nil || actualOwner == nil || !actualOwner.Equals(owner) {
		return errors.New("embedded node state must be owned by the current user")
	}
	dacl, _, err := sd.DACL()
	if err != nil || dacl == nil {
		return errors.New("embedded node state requires a private access control list")
	}
	for i := uint16(0); i < dacl.AceCount; i++ {
		var ace *windows.ACCESS_ALLOWED_ACE
		if err := windows.GetAce(dacl, uint32(i), &ace); err != nil {
			return err
		}
		switch ace.Header.AceType {
		case windows.ACCESS_DENIED_ACE_TYPE:
		case windows.ACCESS_ALLOWED_ACE_TYPE:
			if !trusted((*windows.SID)(unsafe.Pointer(&ace.SidStart))) {
				return errors.New("embedded node state grants access to another user or group")
			}
		default:
			return errors.New("embedded node state has an unsupported access control entry")
		}
	}
	return nil
}
