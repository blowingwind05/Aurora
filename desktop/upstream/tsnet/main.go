// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"

	"tailscale.com/envknob"
	"tailscale.com/logtail"
)

const runtimeInfo = "aurora-tailnet/1"

func main() {
	if len(os.Args) == 2 && os.Args[1] == "--aurora-runtime-info" {
		fmt.Fprintln(os.Stdout, runtimeInfo)
		return
	}
	if err := run(os.Args[1:], os.Stdin, os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, "AuroraTailnet:", err)
		os.Exit(1)
	}
}

func run(args []string, stdin *os.File, stdout io.Writer) error {
	flags := flag.NewFlagSet("AuroraTailnet", flag.ContinueOnError)
	flags.SetOutput(os.Stderr)
	stateDir := flags.String("state-dir", "", "absolute path to the private per-user node directory")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 || !filepath.IsAbs(*stateDir) {
		return errors.New("--state-dir must be an absolute path")
	}
	dir := filepath.Clean(*stateDir)
	if dir == filepath.Dir(dir) {
		return errors.New("--state-dir must name a dedicated directory")
	}
	release, err := prepareStateDir(dir)
	if err != nil {
		return fmt.Errorf("open private state directory: %w", err)
	}
	defer release()

	// The desktop never supplies an auth key or a control server override.
	for _, name := range []string{
		"TS_AUTHKEY", "TS_CLIENT_ID", "TS_CLIENT_SECRET", "TS_ID_TOKEN",
		"TS_AUDIENCE", "TS_CONTROL_URL", "TSNET_FORCE_LOGIN",
	} {
		if err := os.Unsetenv(name); err != nil {
			return fmt.Errorf("clear inherited Tailscale configuration: %w", err)
		}
	}
	envknob.SetNoLogsNoSupport()
	logtail.Disable()
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	stopRead := context.AfterFunc(ctx, func() { _ = stdin.Close() })
	defer stopRead()

	node := startNode(ctx, dir)
	defer node.close()
	return serveRPC(ctx, stdin, stdout, node.handle)
}
