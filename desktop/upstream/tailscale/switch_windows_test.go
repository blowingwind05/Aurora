// SPDX-License-Identifier: BSD-3-Clause

package main

import (
	"errors"
	"testing"

	"golang.org/x/sys/windows/svc"
	"golang.org/x/sys/windows/svc/mgr"
	"tailscale.com/paths"
)

type fakeService struct {
	label     string
	state     svc.State
	startType uint32
	onStart   func() error
	onStop    func() error
	configErr error
}

func (s *fakeService) name() string { return s.label }
func (s *fakeService) Config() (mgr.Config, error) {
	return mgr.Config{StartType: s.startType}, nil
}
func (s *fakeService) Query() (svc.Status, error) {
	return svc.Status{State: s.state}, nil
}
func (s *fakeService) setStartType(value uint32) error {
	if s.configErr != nil {
		return s.configErr
	}
	s.startType = value
	return nil
}
func (s *fakeService) Control(command svc.Cmd) (svc.Status, error) {
	if command != svc.Stop {
		return svc.Status{}, errors.New("unexpected command")
	}
	if s.onStop != nil {
		if err := s.onStop(); err != nil {
			return svc.Status{}, err
		}
	}
	s.state = svc.Stopped
	return s.Query()
}
func (s *fakeService) Start(...string) error {
	if s.onStart != nil {
		if err := s.onStart(); err != nil {
			return err
		}
	}
	s.state = svc.Running
	return nil
}

func TestEmbeddedPipe(t *testing.T) {
	if got, want := paths.DefaultTailscaledSocket(), paths.WindowsProtectedPipePrefix+`AuroraTailscale\tailscaled`; got != want {
		t.Fatalf("socket = %q, want %q", got, want)
	}
}

func TestEmbeddedStockRegistrationGuard(t *testing.T) {
	root := `C:\Program Files\AuroraTailnet`
	for _, test := range []struct {
		name  string
		image string
		start uint32
		want  int
	}{
		{"stock automatic", `"C:\Program Files\Tailscale\tailscaled.exe"`, mgr.StartAutomatic, stockInstalled},
		{"stock manual", `"C:\Program Files\Tailscale\tailscaled.exe"`, mgr.StartManual, stockInstalled},
		{"stock disabled", `"C:\Program Files\Tailscale\tailscaled.exe"`, mgr.StartDisabled, stockInstalled},
		{"legacy proceeds to hash verification", `"C:\Program Files\AuroraTailnet\1.104.7\tailscaled.exe"`, mgr.StartAutomatic, 0},
		{"private runtime in vendor slot", `"C:\Program Files\AuroraTailnet\1.104.7-aurora-service-v2\tailscaled.exe"`, mgr.StartDisabled, stockInstalled},
	} {
		t.Run(test.name, func(t *testing.T) {
			code, err := embeddedRegistrationGuard(root, func() (mgr.Config, error) {
				return mgr.Config{
					BinaryPathName:   test.image,
					StartType:        test.start,
					ServiceStartName: "LocalSystem",
					ServiceType:      0x10, // SERVICE_WIN32_OWN_PROCESS
				}, nil
			})
			if code != test.want || (err != nil) != (test.want != 0) {
				t.Fatalf("guard returned %d, %v; want %d", code, err, test.want)
			}
		})
	}
	code, err := embeddedRegistrationGuard(root, func() (mgr.Config, error) {
		return mgr.Config{}, errors.New("cannot inspect registration")
	})
	if code != stockInstalled || !errors.Is(err, errStockInstalled) {
		t.Fatalf("unreadable registration returned %d, %v", code, err)
	}
}

func TestSwitchStopsOtherBeforeStartAndMigration(t *testing.T) {
	source := &fakeService{label: "legacy", state: svc.Running, startType: mgr.StartAutomatic}
	target := &fakeService{label: "embedded", state: svc.Stopped, startType: mgr.StartDisabled}
	target.onStart = func() error {
		if source.state != svc.Stopped || source.startType != mgr.StartDisabled {
			t.Fatal("target started before the outgoing service was disabled and stopped")
		}
		return nil
	}
	ready, removed := false, false
	code, err := switchServices(target, []switchService{source}, switchPolicy{ready: func() error {
		ready = true
		return nil
	}, finish: func([]serviceSnapshot) error {
		if !ready || target.state != svc.Running || source.state != svc.Stopped {
			t.Fatal("legacy removal preceded successful startup")
		}
		removed = true
		return nil
	}})
	if code != 0 || err != nil || !removed || target.startType != mgr.StartAutomatic {
		t.Fatalf("switch = %d, %v, removed = %v, target start = %d", code, err, removed, target.startType)
	}
}

func TestSystemMissingStopsBothBeforeRemovingLegacy(t *testing.T) {
	legacy := &fakeService{label: "legacy", state: svc.Running, startType: mgr.StartAutomatic}
	private := &fakeService{label: "private", state: svc.Stopped, startType: mgr.StartManual}
	removed := false
	code, err := switchServices(nil, []switchService{private, legacy}, switchPolicy{ready: func() error {
		t.Fatal("a missing system installation has no LocalAPI to check")
		return nil
	}, finish: func([]serviceSnapshot) error {
		removed = true
		for _, service := range []*fakeService{legacy, private} {
			if service.state != svc.Stopped || service.startType != mgr.StartDisabled {
				t.Fatalf("%s still active before legacy removal", service.name())
			}
		}
		return nil
	}})
	if code != systemMissing || err != nil || !removed {
		t.Fatalf("switch = %d, %v, removed = %v", code, err, removed)
	}
}

func TestSwitchRollback(t *testing.T) {
	for _, phase := range []string{"enable", "start", "LocalAPI", "legacy deletion"} {
		t.Run(phase, func(t *testing.T) {
			source := &fakeService{label: "source", state: svc.Running, startType: mgr.StartManual}
			target := &fakeService{label: "target", state: svc.Stopped, startType: mgr.StartDisabled}
			source.onStart = func() error {
				if target.state != svc.Stopped {
					t.Fatal("outgoing service restarted before failed target stopped")
				}
				return nil
			}
			failure := errors.New("simulated switch error")
			ready := func() error { return nil }
			finish := func([]serviceSnapshot) error { return nil }
			switch phase {
			case "enable":
				target.configErr = failure
			case "start":
				target.onStart = func() error { return failure }
			case "LocalAPI":
				ready = func() error { return failure }
			case "legacy deletion":
				finish = func([]serviceSnapshot) error { return failure }
			}
			code, err := switchServices(target, []switchService{source}, switchPolicy{ready: ready, finish: finish})
			if code != serviceFailure || !errors.Is(err, failure) {
				t.Fatalf("switch = %d, %v", code, err)
			}
			if source.state != svc.Running || source.startType != mgr.StartManual ||
				target.state != svc.Stopped || target.startType != mgr.StartDisabled {
				t.Fatalf("wrong rollback state: source %+v, target %+v", source, target)
			}
		})
	}
}

func TestIncompleteTargetRollbackDoesNotRestartOutgoingService(t *testing.T) {
	source := &fakeService{label: "source", state: svc.Running, startType: mgr.StartAutomatic}
	target := &fakeService{label: "target", state: svc.Stopped, startType: mgr.StartDisabled}
	stopFailure := errors.New("cannot stop target")
	target.onStop = func() error { return stopFailure }
	code, err := switchServices(target, []switchService{source}, switchPolicy{ready: func() error {
		return errors.New("LocalAPI unavailable")
	}})
	if code != rollbackFailure || !errors.Is(err, stopFailure) {
		t.Fatalf("switch = %d, %v", code, err)
	}
	if source.state != svc.Stopped || source.startType != mgr.StartDisabled {
		t.Fatal("source should remain stopped while the target is still running")
	}
}

func TestStockStartupPolicy(t *testing.T) {
	image := systemImageID{Path: "C:/Program Files/Tailscale/tailscaled.exe", Volume: 10, Index: 30}
	stale := image
	stale.Index++
	for _, test := range []struct {
		name    string
		current uint32
		record  *parkedSystemStartup
		want    uint32
	}{
		{"manual without parking", mgr.StartManual, nil, mgr.StartManual},
		{"automatic without parking", mgr.StartAutomatic, nil, mgr.StartAutomatic},
		{"parked manual", mgr.StartDisabled, &parkedSystemStartup{image, mgr.StartManual}, mgr.StartManual},
		{"parked automatic", mgr.StartDisabled, &parkedSystemStartup{image, mgr.StartAutomatic}, mgr.StartAutomatic},
		{"administrator disabled", mgr.StartDisabled, nil, 0},
		{"disabled with stale record", mgr.StartDisabled, &parkedSystemStartup{stale, mgr.StartManual}, 0},
		{"administrator changed to manual", mgr.StartManual, &parkedSystemStartup{image, mgr.StartAutomatic}, mgr.StartManual},
	} {
		t.Run(test.name, func(t *testing.T) {
			parking := &systemParking{image: image, prior: test.record}
			stock := &fakeService{label: "stock", state: svc.Stopped, startType: test.current}
			private := &fakeService{label: "private", state: svc.Running, startType: mgr.StartAutomatic}
			code, err := switchServices(stock, []switchService{private}, switchPolicy{
				targetStartup: parking.selectedStartup,
				ready:         func() error { return nil },
			})
			if test.want == 0 {
				if code != serviceFailure || err == nil || stock.startType != test.current ||
					private.state != svc.Running || private.startType != mgr.StartAutomatic {
					t.Fatalf("administrator-disabled switch = %d, %v; stock %+v, private %+v", code, err, stock, private)
				}
			} else if code != 0 || err != nil || stock.startType != test.want || stock.state != svc.Running {
				t.Fatalf("switch = %d, %v; stock %+v", code, err, stock)
			}
		})
	}
}

func TestParkedStockFailureRestoresParking(t *testing.T) {
	image := systemImageID{Path: "C:/Program Files/Tailscale/tailscaled.exe", Volume: 10, Index: 21}
	parking := &systemParking{image: image, prior: &parkedSystemStartup{image, mgr.StartManual}}
	stock := &fakeService{label: "stock", state: svc.Stopped, startType: mgr.StartDisabled}
	private := &fakeService{label: "private", state: svc.Running, startType: mgr.StartAutomatic}
	failure := errors.New("stock LocalAPI never started")
	code, err := switchServices(stock, []switchService{private}, switchPolicy{
		targetStartup: parking.selectedStartup,
		ready:         func() error { return failure },
		finish: func([]serviceSnapshot) error {
			t.Fatal("failed switch cleared the parking record")
			return nil
		},
	})
	if code != serviceFailure || !errors.Is(err, failure) || stock.state != svc.Stopped ||
		stock.startType != mgr.StartDisabled || private.state != svc.Running ||
		private.startType != mgr.StartAutomatic {
		t.Fatalf("switch = %d, %v; stock %+v, private %+v", code, err, stock, private)
	}
}
