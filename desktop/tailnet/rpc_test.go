// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"context"
	"encoding/json"
	"io"
	"testing"
	"time"
)

func TestPresenceDoesNotBlockStatus(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 3*time.Second)
	defer cancel()
	in, requests := io.Pipe()
	responses, out := io.Pipe()
	defer requests.Close()
	defer responses.Close()
	stop := context.AfterFunc(ctx, func() { _ = requests.Close(); _ = responses.Close() })
	defer stop()
	done := make(chan error, 1)
	started := make(chan struct{})
	finishProbe := make(chan struct{})
	go func() {
		defer out.Close()
		done <- serveRPC(ctx, in, out, func(ctx context.Context, method string, _ json.RawMessage) (any, error) {
			if method == "presence" {
				close(started)
				select {
				case <-finishProbe:
				case <-ctx.Done():
					return nil, ctx.Err()
				}
			}
			return map[string]string{"method": method}, nil
		})
	}()
	if _, err := io.WriteString(requests, "{\"id\":1,\"method\":\"presence\"}\n"); err != nil {
		t.Fatal(err)
	}
	select {
	case <-started:
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	if _, err := io.WriteString(requests, "{\"id\":2,\"method\":\"status\"}\n"); err != nil {
		t.Fatal(err)
	}
	decoder := json.NewDecoder(responses)
	var result rpcResponse
	if err := decoder.Decode(&result); err != nil || !result.OK || string(result.ID) != "2" {
		t.Fatalf("status did not reply while presence was pending: %+v, %v", result, err)
	}
	close(finishProbe)
	if err := decoder.Decode(&result); err != nil || !result.OK || string(result.ID) != "1" {
		t.Fatalf("presence did not reply: %+v, %v", result, err)
	}
	_ = requests.Close()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}
