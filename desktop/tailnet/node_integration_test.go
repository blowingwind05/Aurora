// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http/httptest"
	"net/netip"
	"strconv"
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

func startLocalTailnet(t *testing.T, ctx context.Context) (host, peer *embeddedNode, hostIP, peerIP netip.Addr) {
	t.Helper()
	envknob.SetNoLogsNoSupport()
	t.Setenv("NO_PROXY", "*")
	netns.SetEnabled(false)
	t.Cleanup(func() { netns.SetEnabled(true) })
	control := &testcontrol.Server{
		AllNodesSameUser: true,
		AllOnline:        true,
		DERPMap:          integration.RunDERPAndSTUN(t, logger.Discard, "127.0.0.1"),
		DNSConfig:        &tailcfg.DNSConfig{Proxied: true},
		MagicDNSDomain:   "aurora-test.ts.net",
		Logf:             logger.Discard,
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
		node := &embeddedNode{
			ctx: ctx, server: s, client: client,
			aux: make(map[string]auxiliaryLease),
		}
		t.Cleanup(node.closeAux)
		return node, ip
	}
	host, hostIP = start("aurora-host")
	peer, peerIP = start("aurora-peer")
	find := func(node *embeddedNode, remote netip.Addr) {
		t.Helper()
		for {
			status, err := node.client.Status(ctx)
			if err != nil {
				t.Fatal(err)
			}
			snapshot := snapshotFromStatus(status)
			if snapshot != nil && snapshot.peers[remote] != "" {
				node.snapshot.Store(snapshot)
				return
			}
			select {
			case <-ctx.Done():
				t.Fatal("test control did not advertise the same-owner peer")
			case <-time.After(15 * time.Millisecond):
			}
		}
	}
	find(host, peerIP)
	find(peer, hostIP)
	return
}

func TestTsnetPresenceBetweenSameOwnerNodes(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 20*time.Second)
	defer cancel()
	host, peer, hostIP, peerIP := startLocalTailnet(t, ctx)
	hostAccount := host.snapshot.Load()
	peerAccount := peer.snapshot.Load()
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

func TestTsnetGrantChannelBindsTheActualPeer(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 20*time.Second)
	defer cancel()
	host, peer, hostIP, peerIP := startLocalTailnet(t, ctx)
	local, err := net.ListenTCP("tcp4", &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer local.Close()
	deadline, _ := ctx.Deadline()
	if err := local.SetDeadline(deadline); err != nil {
		t.Fatal(err)
	}
	hostKey := bytes.Repeat([]byte{'h'}, 32)
	dialKey := bytes.Repeat([]byte{'d'}, 32)
	hostSnapshot := host.snapshot.Load()
	peerSnapshot := peer.snapshot.Load()
	handle := func(node *embeddedNode, method string, params map[string]any) any {
		t.Helper()
		raw, err := json.Marshal(params)
		if err != nil {
			t.Fatal(err)
		}
		result, err := node.handleAux(ctx, method, raw)
		if err != nil {
			t.Fatalf("%s: %v", method, err)
		}
		return result
	}
	handle(host, "pair-host", map[string]any{
		"key": hostKey, "localPort": local.Addr().(*net.TCPAddr).Port,
		"owner": hostSnapshot.account.owner, "nodeId": hostSnapshot.account.id,
	})
	dial := handle(peer, "pair-open", map[string]any{
		"key": dialKey, "peerId": peerSnapshot.peers[hostIP], "address": hostIP.String(),
	})
	reply, err := json.Marshal(dial)
	if err != nil {
		t.Fatal(err)
	}
	var lease struct{ LocalPort int }
	if err := json.Unmarshal(reply, &lease); err != nil || lease.LocalPort <= 0 {
		t.Fatalf("dial lease: %s, %v", reply, err)
	}
	client, err := (&net.Dialer{}).DialContext(ctx, "tcp4", net.JoinHostPort("127.0.0.1", strconv.Itoa(lease.LocalPort)))
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	read := func(conn net.Conn, count int) []byte {
		t.Helper()
		if err := conn.SetDeadline(deadline); err != nil {
			t.Fatal(err)
		}
		data := make([]byte, count)
		if _, err := io.ReadFull(conn, data); err != nil {
			t.Fatal(err)
		}
		return data
	}
	write := func(conn net.Conn, data []byte) {
		t.Helper()
		if count, err := conn.Write(data); err != nil || count != len(data) {
			t.Fatalf("wrote %d of %d bytes: %v", count, len(data), err)
		}
	}
	clientNonce := read(client, 32)
	write(client, pairProof("AURORA/PAIR/DIAL/1", dialKey, clientNonce, nil))
	hostChannel, err := local.AcceptTCP()
	if err != nil {
		t.Fatal(err)
	}
	defer hostChannel.Close()
	hostNonce := bytes.Repeat([]byte{'n'}, 32)
	write(hostChannel, hostNonce)
	peerBytes, loopBytes := peerIP.As4(), gameStreamLoopback.As4()
	payload := append(append([]byte{4}, peerBytes[:]...), loopBytes[:]...)
	expected := append(payload, pairProof("AURORA/PAIR/HOST/1", hostKey, hostNonce, payload)...)
	if got := read(hostChannel, len(expected)); !bytes.Equal(got, expected) {
		t.Fatalf("host proof did not bind %s to the bridge", peerIP)
	}
	request := []byte("AURORA/AUTH/1\n")
	write(client, request)
	if got := read(hostChannel, len(request)); !bytes.Equal(got, request) {
		t.Fatalf("control request = %q", got)
	}
	revalidate := func(nodeID tailcfg.StableNodeID) bool {
		return handle(host, "pair-source", map[string]any{
			"address": peerIP.String(), "peerId": nodeID, "sourcePort": 0,
			"owner": hostSnapshot.account.owner, "nodeId": hostSnapshot.account.id,
		}).(map[string]bool)["verified"]
	}
	if !revalidate(hostSnapshot.peers[peerIP]) || revalidate("not-the-peer") {
		t.Fatal("control revalidation did not require the actual peer's stable ID")
	}
	notice := []byte("synthetic grant reply")
	write(hostChannel, notice)
	if got := read(client, len(notice)); !bytes.Equal(got, notice) {
		t.Fatalf("control reply = %q", got)
	}
}
