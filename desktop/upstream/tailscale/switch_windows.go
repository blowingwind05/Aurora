// SPDX-License-Identifier: BSD-3-Clause

package main

import (
	"context"
	"errors"
	"fmt"
	"time"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/svc"
	"golang.org/x/sys/windows/svc/mgr"
	"tailscale.com/client/local"
)

const (
	serviceWait = 20 * time.Second
	localWait   = 25 * time.Second
	pollDelay   = 200 * time.Millisecond
)

type switchService interface {
	Config() (mgr.Config, error)
	Query() (svc.Status, error)
	Start(...string) error
	Control(svc.Cmd) (svc.Status, error)
	setStartType(uint32) error
	name() string
}

type scmService struct{ *mgr.Service }

func (s scmService) name() string { return s.Name }

func (s scmService) setStartType(startType uint32) error {
	// Preserve the service image, dependencies, account, delayed-start flag and recovery policy.
	return windows.ChangeServiceConfig(s.Handle, windows.SERVICE_NO_CHANGE, startType,
		windows.SERVICE_NO_CHANGE, nil, nil, nil, nil, nil, nil, nil)
}

type serviceSnapshot struct {
	service   switchService
	startType uint32
	running   bool
}

type switchPolicy struct {
	targetStartup func(uint32) (uint32, error)
	ready         func() error
	finish        func([]serviceSnapshot) error
}

func snapshot(service switchService) (serviceSnapshot, error) {
	if service == nil {
		return serviceSnapshot{}, nil
	}
	config, err := service.Config()
	if err != nil {
		return serviceSnapshot{}, err
	}
	status, err := stableStatus(service)
	if err != nil {
		return serviceSnapshot{}, err
	}
	return serviceSnapshot{service, config.StartType, status.State == svc.Running}, nil
}

// switchServices stops all outgoing services before starting the selected one.
// finish is reserved for the final legacy SCM deletion. A nil target selects a
// missing system installation, leaving the embedded services stopped.
func switchServices(target switchService, inactive []switchService, policy switchPolicy) (int, error) {
	targetBefore, err := snapshot(target)
	if err != nil {
		return serviceFailure, err
	}
	targetStartup := uint32(mgr.StartAutomatic)
	if target != nil && policy.targetStartup != nil {
		targetStartup, err = policy.targetStartup(targetBefore.startType)
		if err != nil {
			return serviceFailure, err
		}
	}
	previous := make([]serviceSnapshot, 0, len(inactive))
	for _, service := range inactive {
		before, err := snapshot(service)
		if err != nil {
			return serviceFailure, err
		}
		previous = append(previous, before)
	}

	changed := 0
	targetChanged := false
	rollback := func(cause error) (int, error) {
		// If the target cannot be put back, restarting the other service could
		// run both route managers at once. Keep it disabled and report the error.
		if targetChanged {
			if err := targetBefore.restore(); err != nil {
				return rollbackFailure, errors.Join(cause, fmt.Errorf("restore %s: %w", target.name(), err))
			}
		}
		var failures error
		for i := changed - 1; i >= 0; i-- {
			before := previous[i]
			if err := before.restore(); err != nil {
				failures = errors.Join(failures, fmt.Errorf("restore %s: %w", before.service.name(), err))
			}
		}
		if failures != nil {
			return rollbackFailure, errors.Join(cause, failures)
		}
		return serviceFailure, cause
	}

	for _, before := range previous {
		if before.startType != mgr.StartDisabled {
			if err := before.service.setStartType(mgr.StartDisabled); err != nil {
				return rollback(fmt.Errorf("disable %s: %w", before.service.name(), err))
			}
		}
		changed++
		if err := stopService(before.service); err != nil {
			return rollback(fmt.Errorf("stop %s: %w", before.service.name(), err))
		}
	}
	if target != nil {
		if targetBefore.startType != targetStartup {
			if err := target.setStartType(targetStartup); err != nil {
				return rollback(fmt.Errorf("enable %s: %w", target.name(), err))
			}
		}
		targetChanged = true
		if err := startService(target); err != nil {
			return rollback(fmt.Errorf("start %s: %w", target.name(), err))
		}
		if err := policy.ready(); err != nil {
			return rollback(fmt.Errorf("wait for %s LocalAPI: %w", target.name(), err))
		}
	}
	if policy.finish != nil {
		if err := policy.finish(previous); err != nil {
			return rollback(err)
		}
	}
	if target == nil {
		return systemMissing, nil
	}
	return 0, nil
}

func (before serviceSnapshot) restore() error {
	service := before.service
	if !before.running {
		if err := deactivate(service); err != nil {
			return err
		}
		return service.setStartType(before.startType)
	}
	// A running service can have been disabled by an administrator without
	// being stopped. Allow a restart before restoring that unusual setting.
	startType := before.startType
	if startType == mgr.StartDisabled {
		startType = mgr.StartManual
	}
	if err := service.setStartType(startType); err != nil {
		return err
	}
	if err := startService(service); err != nil {
		return err
	}
	if startType != before.startType {
		return service.setStartType(before.startType)
	}
	return nil
}

func deactivate(service switchService) error {
	if err := service.setStartType(mgr.StartDisabled); err != nil {
		return err
	}
	return stopService(service)
}

func stopService(service switchService) error {
	status, err := stableStatus(service)
	if err != nil || status.State == svc.Stopped {
		return err
	}
	if _, err := service.Control(svc.Stop); err != nil && !errors.Is(err, windows.ERROR_SERVICE_NOT_ACTIVE) {
		return err
	}
	deadline := time.Now().Add(serviceWait)
	for {
		status, err := service.Query()
		if err != nil || status.State == svc.Stopped {
			return err
		}
		if time.Now().After(deadline) {
			return errors.New("service did not stop")
		}
		time.Sleep(pollDelay)
	}
}

func startService(service switchService) error {
	status, err := stableStatus(service)
	if err != nil || status.State == svc.Running {
		return err
	}
	if err := service.Start(); err != nil && !errors.Is(err, windows.ERROR_SERVICE_ALREADY_RUNNING) {
		return err
	}
	status, err = stableStatus(service)
	if err != nil {
		return err
	}
	if status.State != svc.Running {
		return fmt.Errorf("service exited during startup (Windows %d, service %d)",
			status.Win32ExitCode, status.ServiceSpecificExitCode)
	}
	return nil
}

func stableStatus(service switchService) (svc.Status, error) {
	deadline := time.Now().Add(serviceWait)
	for {
		status, err := service.Query()
		if err != nil || status.State == svc.Running || status.State == svc.Stopped {
			return status, err
		}
		if status.State != svc.StartPending && status.State != svc.StopPending {
			return status, fmt.Errorf("unexpected state %d for %s", status.State, service.name())
		}
		if time.Now().After(deadline) {
			return status, fmt.Errorf("service transition timed out: %s", service.name())
		}
		time.Sleep(pollDelay)
	}
}

func waitLocalAPI(socket string, service switchService) error {
	ctx, cancel := context.WithTimeout(context.Background(), localWait)
	defer cancel()
	client := local.Client{Socket: socket, UseSocketOnly: true}
	for {
		status, err := service.Query()
		if err != nil {
			return err
		}
		if status.State != svc.Running {
			return errors.New("service stopped before LocalAPI was ready")
		}
		probe, stopProbe := context.WithTimeout(ctx, time.Second)
		_, err = client.StatusWithoutPeers(probe)
		stopProbe()
		if err == nil {
			// LocalAPI is available for existing accounts and newly installed
			// clients. Login state is not part of service readiness.
			return nil
		}
		select {
		case <-ctx.Done():
			return fmt.Errorf("LocalAPI did not respond: %w", err)
		case <-time.After(pollDelay):
		}
	}
}
