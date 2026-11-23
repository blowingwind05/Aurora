// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"context"
	"encoding/json"
	"net/http/httptest"
	"net/netip"
	"testing"
	"time"

	"tailscale.com/envknob"
	"tailscale.com/ipn/store/mem"
	"tailscale.com/net/netns"
	"tailscale.com/tailcfg"
	"tailscale.com/tsnet"
	"tailscale.com/tstest/integration"
	"tailscale.com/tstest/integration/testcontrol"
	"tailscale.com/types/logger"
)

func TestTsnetPresenceBetweenSameOwnerNodes(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 20*time.Second)
	defer cancel()
	envknob.SetNoLogsNoSupport()
	netns.SetEnabled(false)
	t.Cleanup(func() { netns.SetEnabled(true) })
	control := &testcontrol.Server{
		AllNodesSameUser: true,
		AllOnline:       true,
		DERPMap:         integration.RunDERPAndSTUN(t, logger.Discard, "127.0.0.1"),
		DNSConfig:       &tailcfg.DNSConfig{Proxied: true},
		MagicDNSDomain:  "aurora-test.ts.net",
		Logf:            logger.Discard,
	}
	control.HTTPTestServer = httptest.NewServer(control)
	t.Cleanup(control.HTTPTestServer.Close)
	start := func(name string) (*embeddedNode, netip.Addr) {
		t.Helper()
		s := &tsnet.Server{
			Dir: t.TempDir(), Hostname: name, ControlURL: control.HTTPTestServer.URL,
			Ephemeral: true, Store: new(mem.Store), Logf: logger.Discard, UserLogf: logger.Discard,
		}
		t.Cleanup(func() { _ = s.Close() })
		if _, err := s.Up(ctx); err != nil {
			t.Fatal(err)
		}
		client, err := s.LocalClient()
		if err != nil {
			t.Fatal(err)
		}
		ip, _ := s.TailscaleIPs()
		return &embeddedNode{ctx: ctx, server: s, client: client}, ip
	}
	host, hostIP := start("aurora-host")
	peer, peerIP := start("aurora-peer")
	find := func(node *embeddedNode, remote netip.Addr) *nodeSnapshot {
		t.Helper()
		for {
			status, err := node.client.Status(ctx)
			if err != nil {
				t.Fatal(err)
			}
			snapshot := snapshotFromStatus(status)
			if snapshot != nil && snapshot.peers[remote] != "" {
				node.snapshot.Store(snapshot)
				return snapshot
			}
			select {
			case <-ctx.Done():
				t.Fatal("test control did not advertise the same-owner peer")
			case <-time.After(15 * time.Millisecond):
			}
		}
	}
	hostAccount := find(host, peerIP)
	peerAccount := find(peer, hostIP)
	if !host.allowInbound(hostAccount.account, peerIP) || host.allowInbound(hostAccount.account, netip.MustParseAddr("100.100.100.100")) {
		t.Fatal("unexpected identity check result")
	}
	var template [presenceSize]byte
	copy(template[:], presenceMagic)
	template[presenceKind] = 2
	template[presenceNonceEnd] = 66
	if err := host.publishPresence(hostAccount, template[:]); err != nil {
		t.Fatal(err)
	}
	defer host.stopPublisher()
	request := template
	request[presenceKind] = 1
	request[presenceKind+1] = 77
	params, err := json.Marshal(map[string]any{
		"peerId": peerAccount.peers[hostIP], "address": hostIP.String(), "packet": request[:],
	})
	if err != nil {
		t.Fatal(err)
	}
	for {
		result, err := peer.handlePresence(ctx, params)
		if err == nil {
			packet := result.([]byte)
			if packet[presenceKind+1] != 77 || packet[presenceNonceEnd] != 66 {
				t.Fatalf("unexpected presence reply: nonce %d, identity %d", packet[presenceKind+1], packet[presenceNonceEnd])
			}
			return
		}
		if ctx.Err() != nil {
			t.Fatalf("no presence reply through the local tailnet: %v", err)
		}
	}
}
