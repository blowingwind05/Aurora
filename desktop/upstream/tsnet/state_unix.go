//go:build linux || darwin

// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"golang.org/x/sys/unix"
)

func prepareStateDir(dir string) (func(), error) {
	if err := os.MkdirAll(dir, 0700); err != nil {
		return nil, err
	}
	fd, err := unix.Open(dir, unix.O_RDONLY|unix.O_DIRECTORY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	defer unix.Close(fd)

	var info unix.Stat_t
	if err := unix.Fstat(fd, &info); err != nil {
		return nil, err
	}
	if info.Uid != uint32(os.Geteuid()) || info.Mode&0077 != 0 {
		return nil, errors.New("state directory must be owned by the current user and private (0700)")
	}

	lock, err := unix.Openat(fd, ".aurora.lock", unix.O_RDWR|unix.O_CREAT|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0600)
	if err != nil {
		return nil, err
	}
	if err := unix.Flock(lock, unix.LOCK_EX|unix.LOCK_NB); err != nil {
		unix.Close(lock)
		return nil, fmt.Errorf("lock embedded node state: %w", err)
	}
	err = unix.Fstatat(fd, filepath.Base(stateFile), &info, unix.AT_SYMLINK_NOFOLLOW)
	if err == nil && (info.Mode&unix.S_IFMT != unix.S_IFREG || info.Uid != uint32(os.Geteuid()) ||
		info.Mode&0077 != 0 || info.Nlink != 1) {
		unix.Close(lock)
		return nil, errors.New("embedded node state must be a private regular file")
	}
	if err != nil && !errors.Is(err, unix.ENOENT) {
		unix.Close(lock)
		return nil, err
	}
	return func() { _ = unix.Close(lock) }, nil
}
