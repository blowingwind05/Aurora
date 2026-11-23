// SPDX-License-Identifier: BSD-3-Clause

package main

import (
	"crypto/rand"
	"crypto/sha256"
	"embed"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/svc/mgr"
	"tailscale.com/auroradata"
	"tailscale.com/paths"
)

const (
	serviceName      = auroradata.WindowsServiceName
	systemName       = "Tailscale"
	runtimeVersion   = "1.104.7-aurora-service-v2"
	legacyVersion    = "1.104.7"
	legacyDaemonHash = "1f39be733c35bc52fac8004f62c57ed9cb249755b09c94235b3392362f2fefe1"
	wintunHash       = "e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce"
	existing         = 2
	unsafeLocation   = 3
	serviceFailure   = 4
	busy             = 5
	systemMissing    = 6
	rollbackFailure  = 7
	stockInstalled   = 8
	administrator    = "S-1-5-32-544"
	localSystem      = "S-1-5-18"
	trustedInstaller = "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464"
	users            = "S-1-5-32-545"
	readExecute      = 0x1200a9 // FILE_GENERIC_READ | FILE_GENERIC_EXECUTE
	allFileAccess    = 0x1f01ff
	privateACL       = "O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
	runtimeACL       = privateACL + "(A;OICI;0x1200a9;;;BU)"
)

//go:embed payload/tailscale.exe payload/tailscaled.exe payload/wintun.dll
var payload embed.FS

var payloadNames = []string{"tailscale.exe", "tailscaled.exe", "wintun.dll"}

func main() {
	if len(os.Args) != 2 || (os.Args[1] != "embedded" && os.Args[1] != "system") {
		os.Exit(1)
	}
	code, err := activate(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, "Aurora Tailscale:", err)
	}
	os.Exit(code)
}

func activate(mode string) (code int, retErr error) {
	admin, err := windows.CreateWellKnownSid(windows.WinBuiltinAdministratorsSid)
	if err != nil {
		return serviceFailure, err
	}
	elevated, err := windows.Token(0).IsMember(admin)
	if err != nil || !elevated {
		return serviceFailure, errors.New("administrator permission is required")
	}

	machineData := auroradata.WindowsMachineDataDir()
	root := filepath.Dir(machineData)
	manager, err := mgr.Connect()
	if err != nil {
		return serviceFailure, err
	}
	defer manager.Disconnect()
	if mode == "embedded" {
		// Check the registration before creating a setup directory or lock.
		if code, err := checkEmbeddedStock(manager, root); code != 0 {
			return code, err
		}
	}

	releaseParents, err := lockInstallAncestors(filepath.Dir(root))
	if err != nil {
		return unsafeLocation, err
	}
	defer releaseParents()
	rootHandle, err := ensureDirectory(root, true)
	if err != nil {
		return unsafeLocation, err
	}
	defer windows.CloseHandle(rootHandle)
	lock, err := lockActivation(root)
	if errors.Is(err, windows.ERROR_SHARING_VIOLATION) {
		return busy, err
	}
	if err != nil {
		return unsafeLocation, err
	}
	defer windows.CloseHandle(lock)

	embedded, err := openService(manager, serviceName)
	if err != nil {
		return serviceOpenFailure(err)
	}
	if embedded != nil {
		defer embedded.Close()
	}
	stock, err := openService(manager, systemName)
	if err != nil {
		if mode == "embedded" && errors.Is(err, windows.ERROR_ACCESS_DENIED) {
			return stockInstalled, errStockInstalled
		}
		return serviceOpenFailure(err)
	}
	if stock != nil {
		defer stock.Close()
		if mode == "embedded" {
			if code, err := embeddedRegistrationGuard(root, stock.Config); code != 0 {
				return code, err
			}
		}
	}

	runtimeDir := filepath.Join(root, runtimeVersion)
	daemon := filepath.Join(runtimeDir, "tailscaled.exe")
	if embedded != nil {
		image, err := serviceImage(embedded.Config)
		if err != nil || !strings.EqualFold(image, daemon) {
			return existing, fmt.Errorf("cannot verify %s registration: %v", serviceName, err)
		}
	}
	legacy, stockImage, unlockStock, err := verifyStockService(stock, root)
	if err != nil {
		return existing, err
	}
	defer unlockStock()
	var parking *systemParking
	if mode == "system" && stock != nil && !legacy {
		state, unlockState, err := readSystemParking(root, stockImage)
		if err != nil {
			return unsafeLocation, fmt.Errorf("read system Tailscale parking record: %w", err)
		}
		defer unlockState()
		parking = state
	}

	// Register disabled so a reboot or a recovery action cannot race the switch.
	if embedded != nil || legacy || mode == "embedded" {
		dataHandle, err := ensureDirectory(machineData, false)
		if err != nil {
			return unsafeLocation, err
		}
		defer windows.CloseHandle(dataHandle)
		if embedded == nil {
			if err := deployRuntime(root, runtimeDir); err != nil {
				return unsafeLocation, err
			}
		}
		runtimeHandle, err := openDirectory(runtimeDir)
		if err != nil {
			return unsafeLocation, err
		}
		defer windows.CloseHandle(runtimeHandle)
		if err := verifyRuntime(runtimeHandle, runtimeDir); err != nil {
			return unsafeLocation, err
		}
		if embedded == nil {
			if err := registerService(daemon); err != nil {
				return serviceFailure, err
			}
			embedded, err = openService(manager, serviceName)
			if err != nil || embedded == nil {
				return serviceFailure, fmt.Errorf("open the new %s service: %v", serviceName, err)
			}
			defer embedded.Close()
			image, err := serviceImage(embedded.Config)
			if err != nil || !strings.EqualFold(image, daemon) {
				return existing, fmt.Errorf("cannot verify new %s registration: %v", serviceName, err)
			}
			defer func() {
				if code != 0 && code != systemMissing && code != rollbackFailure {
					if err := embedded.Delete(); err != nil {
						code = rollbackFailure
						retErr = errors.Join(retErr, fmt.Errorf("remove the new registration: %w", err))
					}
				}
			}()
		}
	}

	var target switchService
	var inactive []switchService
	var policy switchPolicy
	socket := paths.DefaultTailscaledSocket()
	if mode == "embedded" {
		target = scmService{embedded}
		if legacy {
			if code, err := embeddedRegistrationGuard(root, stock.Config); code != 0 {
				return code, err
			}
			inactive = append(inactive, scmService{stock})
		} else if code, err := checkEmbeddedStock(manager, root); code != 0 {
			// A vendor installer can register Tailscale while our private image is deploying.
			return code, err
		}
	} else {
		if embedded != nil {
			inactive = append(inactive, scmService{embedded})
		}
		if stock != nil {
			if legacy {
				inactive = append(inactive, scmService{stock})
			} else {
				target = scmService{stock}
				socket = paths.WindowsProtectedPipePrefix + `Tailscale\tailscaled`
				policy.targetStartup = parking.selectedStartup
				policy.finish = func([]serviceSnapshot) error { return parking.clear() }
			}
		}
	}
	if legacy {
		// Delete only the verified legacy SCM entry after the rest of the switch succeeds.
		// The old binaries and the existing machine store are left in place.
		policy.finish = func([]serviceSnapshot) error {
			if _, err := embeddedRegistrationGuard(root, stock.Config); err != nil {
				return fmt.Errorf("legacy registration changed during the switch: %w", err)
			}
			if err := stock.Delete(); err != nil {
				return fmt.Errorf("remove the legacy registration: %w", err)
			}
			return nil
		}
	}
	policy.ready = func() error { return waitLocalAPI(socket, target) }
	return switchServices(target, inactive, policy)
}

func lockActivation(root string) (windows.Handle, error) {
	path, err := windows.UTF16PtrFromString(filepath.Join(root, "activation.lock"))
	if err != nil {
		return 0, err
	}
	handle, err := windows.CreateFile(path, windows.GENERIC_READ|windows.GENERIC_WRITE,
		0, nil, windows.OPEN_ALWAYS, windows.FILE_FLAG_OPEN_REPARSE_POINT, 0)
	if err != nil {
		return 0, err
	}
	var info windows.ByHandleFileInformation
	if err := windows.GetFileInformationByHandle(handle, &info); err != nil {
		windows.CloseHandle(handle)
		return 0, err
	}
	if info.FileAttributes&(windows.FILE_ATTRIBUTE_REPARSE_POINT|windows.FILE_ATTRIBUTE_DIRECTORY) != 0 || info.NumberOfLinks != 1 {
		windows.CloseHandle(handle)
		return 0, errors.New("invalid activation lock")
	}
	return handle, nil
}

func openService(manager *mgr.Mgr, name string) (*mgr.Service, error) {
	service, err := manager.OpenService(name)
	if errors.Is(err, windows.ERROR_SERVICE_DOES_NOT_EXIST) {
		return nil, nil
	}
	return service, err
}

func serviceOpenFailure(err error) (int, error) {
	if errors.Is(err, windows.ERROR_SERVICE_MARKED_FOR_DELETE) {
		return busy, err
	}
	return serviceFailure, err
}

var errStockInstalled = errors.New("system Tailscale is registered; embedded activation is blocked")

func checkEmbeddedStock(manager *mgr.Mgr, root string) (int, error) {
	handle, err := windows.OpenService(manager.Handle, windows.StringToUTF16Ptr(systemName), windows.SERVICE_QUERY_CONFIG)
	switch {
	case errors.Is(err, windows.ERROR_SERVICE_DOES_NOT_EXIST):
		return 0, nil
	case errors.Is(err, windows.ERROR_ACCESS_DENIED):
		return stockInstalled, errStockInstalled
	case err != nil:
		return serviceOpenFailure(err)
	}
	service := &mgr.Service{Name: systemName, Handle: handle}
	defer service.Close()
	return embeddedRegistrationGuard(root, service.Config)
}

// Only the exact legacy Aurora registration can proceed to the existing hash check.
// SCM running state and startup mode do not affect this decision.
func embeddedRegistrationGuard(root string, config func() (mgr.Config, error)) (int, error) {
	image, err := serviceImage(config)
	if err != nil || !strings.EqualFold(image, filepath.Join(root, legacyVersion, "tailscaled.exe")) {
		return stockInstalled, errStockInstalled
	}
	return 0, nil
}

func serviceImage(readConfig func() (mgr.Config, error)) (string, error) {
	config, err := readConfig()
	if err != nil {
		return "", err
	}
	argv, err := windows.DecomposeCommandLine(config.BinaryPathName)
	if err != nil || len(argv) != 1 || !filepath.IsAbs(argv[0]) || len(filepath.VolumeName(argv[0])) != 2 ||
		!strings.EqualFold(filepath.Base(argv[0]), "tailscaled.exe") ||
		!strings.EqualFold(config.BinaryPathName, syscall.EscapeArg(argv[0])) ||
		!strings.EqualFold(config.ServiceStartName, "LocalSystem") ||
		config.ServiceType != windows.SERVICE_WIN32_OWN_PROCESS {
		return "", errors.New("unexpected Tailscale service configuration")
	}
	return filepath.Clean(argv[0]), nil
}

func verifyStockService(service *mgr.Service, root string) (bool, systemImageID, func(), error) {
	var noImage systemImageID
	if service == nil {
		return false, noImage, func() {}, nil
	}
	image, err := serviceImage(service.Config)
	if err != nil {
		return false, noImage, nil, err
	}
	legacy := strings.EqualFold(image, filepath.Join(root, legacyVersion, "tailscaled.exe"))
	relative, err := filepath.Rel(root, image)
	if err == nil && relative != ".." && !strings.HasPrefix(relative, ".."+string(filepath.Separator)) && !legacy {
		return false, noImage, nil, errors.New("unrecognized Aurora image occupies the system service name")
	}

	unlock, err := lockInstallAncestors(filepath.Dir(image))
	if err != nil {
		return false, noImage, nil, err
	}
	exe, err := openProtectedFile(image)
	if err != nil {
		unlock()
		return false, noImage, nil, err
	}
	closeStock := func() {
		exe.Close()
		unlock()
	}
	if !legacy {
		id, err := imageIdentity(exe)
		if err != nil {
			closeStock()
			return false, noImage, nil, err
		}
		return false, id, closeStock, nil
	}
	if err := verifyFileHash(exe, legacyDaemonHash); err != nil {
		closeStock()
		return false, noImage, nil, err
	}
	driver, err := openProtectedFile(filepath.Join(filepath.Dir(image), "wintun.dll"))
	if err != nil {
		closeStock()
		return false, noImage, nil, err
	}
	if err := verifyFileHash(driver, wintunHash); err != nil {
		driver.Close()
		closeStock()
		return false, noImage, nil, err
	}
	return true, noImage, func() {
		driver.Close()
		closeStock()
	}, nil
}

func verifyFileHash(file *os.File, expected string) error {
	h := sha256.New()
	if _, err := io.Copy(h, file); err != nil {
		return err
	}
	if fmt.Sprintf("%x", h.Sum(nil)) != expected {
		return fmt.Errorf("legacy payload content mismatch: %s", filepath.Base(file.Name()))
	}
	return nil
}

func registerService(daemon string) error {
	system, err := windows.GetSystemDirectory()
	if err != nil {
		return err
	}
	// Run only the protected, verified binary. Discard the elevated user's environment, including
	// Tailscale debug overrides, so the administrative subcommand cannot load user configuration.
	command := exec.Command(daemon, "install-system-daemon")
	command.Dir = filepath.Dir(daemon)
	windowsRoot := filepath.Dir(system)
	command.Env = []string{"SystemRoot=" + windowsRoot, "WINDIR=" + windowsRoot, "PATH=" + system}
	command.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	return command.Run()
}

func deployRuntime(root, target string) error {
	if _, err := os.Lstat(target); err == nil {
		// A prior attempt may have published the image before registering the service.
		return nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}

	staging := filepath.Join(root, ".incoming-"+rand.Text())
	if err := createDirectory(staging, runtimeACL); err != nil {
		return err
	}
	defer os.RemoveAll(staging)
	for _, name := range payloadNames {
		content, err := payload.ReadFile("payload/" + name)
		if err != nil {
			return err
		}
		file, err := os.OpenFile(filepath.Join(staging, name), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			return err
		}
		_, writeErr := file.Write(content)
		syncErr := file.Sync()
		closeErr := file.Close()
		if err := errors.Join(writeErr, syncErr, closeErr); err != nil {
			return err
		}
	}
	from, err := windows.UTF16PtrFromString(staging)
	if err != nil {
		return err
	}
	to, err := windows.UTF16PtrFromString(target)
	if err != nil {
		return err
	}
	// MoveFile does not replace an existing target and the rename stays on the same volume.
	return windows.MoveFile(from, to)
}

func verifyRuntime(handle windows.Handle, dir string) error {
	if err := verifyDirectoryACL(handle, true); err != nil {
		return err
	}
	for _, name := range payloadNames {
		if err := verifyPayloadFile(dir, name); err != nil {
			return err
		}
	}
	return nil
}

func openProtectedFile(name string) (*os.File, error) {
	path, err := windows.UTF16PtrFromString(name)
	if err != nil {
		return nil, err
	}
	handle, err := windows.CreateFile(path, windows.GENERIC_READ|windows.READ_CONTROL, windows.FILE_SHARE_READ,
		nil, windows.OPEN_EXISTING, windows.FILE_FLAG_OPEN_REPARSE_POINT, 0)
	if err != nil {
		return nil, err
	}
	file := os.NewFile(uintptr(handle), name)
	var info windows.ByHandleFileInformation
	err = windows.GetFileInformationByHandle(handle, &info)
	if err == nil && (info.FileAttributes&(windows.FILE_ATTRIBUTE_REPARSE_POINT|windows.FILE_ATTRIBUTE_DIRECTORY) != 0 ||
		info.NumberOfLinks != 1) {
		err = fmt.Errorf("unexpected file type: %s", name)
	}
	if err == nil {
		err = verifyInstallParent(handle, false)
	}
	if err == nil {
		// Reject short names and other aliases before using a path as an installation identity.
		resolved := make([]uint16, len(name)+8)
		n, resolveErr := windows.GetFinalPathNameByHandle(handle, &resolved[0], uint32(len(resolved)), 0)
		if resolveErr != nil || n == 0 || n >= uint32(len(resolved)) ||
			!strings.EqualFold(windows.UTF16ToString(resolved[:n]), `\\?\`+name) {
			err = fmt.Errorf("unexpected protected file path: %s", name)
		}
	}
	if err != nil {
		file.Close()
		return nil, err
	}
	return file, nil
}

func verifyPayloadFile(dir, name string) error {
	file, err := openProtectedFile(filepath.Join(dir, name))
	if err != nil {
		return err
	}
	defer file.Close()
	h := sha256.New()
	content, err := payload.ReadFile("payload/" + name)
	if err != nil {
		return err
	}
	info, err := file.Stat()
	if err != nil {
		return err
	}
	if info.Size() != int64(len(content)) {
		return fmt.Errorf("unexpected payload size: %s", name)
	}
	if _, err := io.Copy(h, file); err != nil {
		return err
	}
	digest := sha256.Sum256(content)
	if [sha256.Size]byte(h.Sum(nil)) != digest {
		return fmt.Errorf("payload content mismatch: %s", name)
	}
	return nil
}

func ensureDirectory(dir string, publicRead bool) (windows.Handle, error) {
	sddl := privateACL
	if publicRead {
		sddl = runtimeACL
	}
	if err := createDirectory(dir, sddl); err != nil && !errors.Is(err, windows.ERROR_ALREADY_EXISTS) {
		return 0, err
	}
	handle, err := openDirectory(dir)
	if err != nil {
		return 0, err
	}
	if err := verifyDirectoryACL(handle, publicRead); err != nil {
		windows.CloseHandle(handle)
		return 0, fmt.Errorf("%s: %w", dir, err)
	}
	return handle, nil
}

func createDirectory(dir, sddl string) error {
	path, err := windows.UTF16PtrFromString(dir)
	if err != nil {
		return err
	}
	descriptor, err := windows.SecurityDescriptorFromString(sddl)
	if err != nil {
		return err
	}
	attributes := windows.SecurityAttributes{Length: uint32(unsafe.Sizeof(windows.SecurityAttributes{})),
		SecurityDescriptor: descriptor}
	return windows.CreateDirectory(path, &attributes)
}

func openDirectory(dir string) (windows.Handle, error) {
	path, err := windows.UTF16PtrFromString(dir)
	if err != nil {
		return 0, err
	}
	handle, err := windows.CreateFile(path, windows.READ_CONTROL|windows.FILE_LIST_DIRECTORY,
		windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE, nil, windows.OPEN_EXISTING,
		windows.FILE_FLAG_BACKUP_SEMANTICS|windows.FILE_FLAG_OPEN_REPARSE_POINT, 0)
	if err != nil {
		return 0, err
	}
	var info windows.ByHandleFileInformation
	if err := windows.GetFileInformationByHandle(handle, &info); err != nil {
		windows.CloseHandle(handle)
		return 0, err
	}
	if info.FileAttributes&windows.FILE_ATTRIBUTE_REPARSE_POINT != 0 ||
		info.FileAttributes&windows.FILE_ATTRIBUTE_DIRECTORY == 0 {
		windows.CloseHandle(handle)
		return 0, fmt.Errorf("expected an ordinary protected directory: %s", dir)
	}
	return handle, nil
}

// A custom Program Files location must not permit a standard user to replace any
// ancestor of the directory used by the LocalSystem service.
func lockInstallAncestors(dir string) (func(), error) {
	volume := filepath.VolumeName(dir) + string(filepath.Separator)
	drive, err := windows.UTF16PtrFromString(volume)
	if err != nil || windows.GetDriveType(drive) != windows.DRIVE_FIXED {
		return nil, errors.New("Program Files must be on a fixed local drive")
	}
	var paths []string
	for path := dir; ; path = filepath.Dir(path) {
		paths = append(paths, path)
		if path == volume {
			break
		}
	}
	var handles []windows.Handle
	closeHandles := func() {
		for _, handle := range handles {
			windows.CloseHandle(handle)
		}
	}
	for index := len(paths) - 1; index >= 0; index-- {
		path := paths[index]
		handle, err := openDirectory(path)
		if err != nil {
			closeHandles()
			return nil, err
		}
		handles = append(handles, handle)
		if err := verifyInstallParent(handle, path == volume); err != nil {
			closeHandles()
			return nil, fmt.Errorf("%s: %w", path, err)
		}
	}
	return closeHandles, nil
}

func verifyInstallParent(handle windows.Handle, volumeRoot bool) error {
	descriptor, err := windows.GetSecurityInfo(handle, windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return err
	}
	owner, _, err := descriptor.Owner()
	if err != nil || owner == nil || !trustedWindowsSID(owner.String()) {
		return errors.New("unexpected owner of a service installation ancestor")
	}
	dacl, _, err := descriptor.DACL()
	if err != nil || dacl == nil {
		return errors.New("missing access list on a service installation ancestor")
	}
	const fileDeleteChild = 0x0040     // FILE_DELETE_CHILD
	const fileAddSubdirectory = 0x0004 // FILE_ADD_SUBDIRECTORY
	mutation := windows.ACCESS_MASK(windows.GENERIC_ALL | windows.GENERIC_WRITE |
		windows.DELETE | windows.WRITE_DAC | windows.WRITE_OWNER | fileDeleteChild |
		windows.FILE_WRITE_DATA | windows.FILE_WRITE_ATTRIBUTES | windows.FILE_WRITE_EA)
	if !volumeRoot {
		mutation |= fileAddSubdirectory
	}
	for index := uint32(0); index < uint32(dacl.AceCount); index++ {
		var ace *windows.ACCESS_ALLOWED_ACE
		if err := windows.GetAce(dacl, index, &ace); err != nil {
			return err
		}
		if ace.Header.AceFlags&windows.INHERIT_ONLY_ACE != 0 || ace.Header.AceType == windows.ACCESS_DENIED_ACE_TYPE {
			continue
		}
		if ace.Header.AceType != windows.ACCESS_ALLOWED_ACE_TYPE {
			return errors.New("unexpected access entry on a service installation ancestor")
		}
		sid := (*windows.SID)(unsafe.Pointer(&ace.SidStart)).String()
		if ace.Mask&mutation != 0 && !trustedWindowsSID(sid) {
			return errors.New("unprivileged users can modify a service installation ancestor")
		}
	}
	return nil
}

func trustedWindowsSID(sid string) bool {
	return sid == localSystem || sid == administrator || sid == trustedInstaller
}

func verifyDirectoryACL(handle windows.Handle, publicRead bool) error {
	descriptor, err := windows.GetSecurityInfo(handle, windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return err
	}
	owner, _, err := descriptor.Owner()
	if err != nil {
		return err
	}
	if owner == nil || (owner.String() != administrator && owner.String() != localSystem) {
		return errors.New("unexpected owner of protected directory")
	}
	control, _, err := descriptor.Control()
	if err != nil || control&windows.SE_DACL_PROTECTED == 0 {
		return errors.New("protected directory inherits permissions")
	}
	dacl, _, err := descriptor.DACL()
	if err != nil {
		return err
	}
	expected := map[string]windows.ACCESS_MASK{administrator: allFileAccess, localSystem: allFileAccess}
	if publicRead {
		expected[users] = readExecute
	}
	if dacl == nil || int(dacl.AceCount) != len(expected) {
		return errors.New("unexpected access list on protected directory")
	}
	for index := uint32(0); index < uint32(dacl.AceCount); index++ {
		var ace *windows.ACCESS_ALLOWED_ACE
		if err := windows.GetAce(dacl, index, &ace); err != nil {
			return err
		}
		if ace.Header.AceType != windows.ACCESS_ALLOWED_ACE_TYPE ||
			ace.Header.AceFlags&(windows.OBJECT_INHERIT_ACE|windows.CONTAINER_INHERIT_ACE) !=
				(windows.OBJECT_INHERIT_ACE|windows.CONTAINER_INHERIT_ACE) {
			return errors.New("unexpected access entry on protected directory")
		}
		sid := (*windows.SID)(unsafe.Pointer(&ace.SidStart)).String()
		access, ok := expected[sid]
		if !ok || (ace.Mask != access && !(access == allFileAccess && ace.Mask == windows.GENERIC_ALL)) {
			return errors.New("unsafe permissions on protected directory")
		}
		delete(expected, sid)
	}
	return nil
}
