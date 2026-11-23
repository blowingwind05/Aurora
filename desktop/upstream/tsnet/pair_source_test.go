// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"net/netip"
	"sync"
	"testing"
	"time"

	"tailscale.com/client/local"
	"tailscale.com/client/tailscale/apitype"
	"tailscale.com/ipn/ipnstate"
	"tailscale.com/tailcfg"
)

type pairSourceTestNode struct {
	*relayTestNode
	peer netip.Addr
}

func (n pairSourceTestNode) Listen(network, address string) (net.Listener, error) {
	ln, err := n.relayTestNode.Listen(network, address)
	if err != nil {
		return nil, err
	}
	return pairSourceTestListener{Listener: ln, peer: n.peer}, nil
}

type pairSourceTestListener struct {
	net.Listener
	peer netip.Addr
}

func (l pairSourceTestListener) Accept() (net.Conn, error) {
	conn, err := l.Listener.Accept()
	if err != nil {
		return nil, err
	}
	return pairSourceTestConn{TCPConn: conn.(*net.TCPConn), peer: l.peer}, nil
}

// The socket is a real localhost TCP connection; this represents the virtual
// tailnet address that tsnet supplies on its accepted connections.
type pairSourceTestConn struct {
	*net.TCPConn
	peer netip.Addr
}

func (c pairSourceTestConn) RemoteAddr() net.Addr {
	port := c.TCPConn.RemoteAddr().(*net.TCPAddr).AddrPort().Port()
	return net.TCPAddrFromAddrPort(netip.AddrPortFrom(c.peer, port))
}

func TestPairSourceRequiresLiveForwardedHTTP(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 10*time.Second)
	defer cancel()
	peer := netip.MustParseAddr("100.64.0.10")
	owner := tailcfg.UserID(1234)
	status := &ipnstate.Status{
		BackendState:   "Running",
		TailscaleIPs:   []netip.Addr{relayTestPeer},
		CurrentTailnet: &ipnstate.TailnetStatus{StableID: "tailnet"},
		Self:           &ipnstate.PeerStatus{ID: "host", UserID: owner},
	}
	if err := json.Unmarshal([]byte(`{"Peer":{"nodekey:0101010101010101010101010101010101010101010101010101010101010101":{"ID":"peer","UserID":1234,"TailscaleIPs":["100.64.0.10"]}}}`), status); err != nil {
		t.Fatal(err)
	}
	who := apitype.WhoIsResponse{
		Node:        &tailcfg.Node{StableID: "peer", User: owner},
		UserProfile: &tailcfg.UserProfile{ID: owner},
	}
	var apiMu sync.Mutex
	var onWhoIs func()
	ready := make(chan struct{})
	close(ready)
	n := &embeddedNode{
		ctx: ctx, ready: ready,
		client: &local.Client{
			OmitAuth: true,
			Transport: localAPITransport(func(req *http.Request) (*http.Response, error) {
				apiMu.Lock()
				defer apiMu.Unlock()
				var body any
				switch req.URL.Path {
				case "/localapi/v0/status":
					body = status
				case "/localapi/v0/whois":
					if req.URL.Query().Get("addr") != peer.String() {
						return nil, errors.New("incorrect WhoIs target")
					}
					if onWhoIs != nil {
						onWhoIs()
					}
					body = who
				default:
					return nil, errors.New("unexpected LocalAPI request")
				}
				data, err := json.Marshal(body)
				if err != nil {
					return nil, err
				}
				return &http.Response{StatusCode: http.StatusOK, Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(data))}, nil
			}),
		},
	}
	snapshot := snapshotFromStatus(status)
	n.snapshot.Store(snapshot)
	bindings, base := bindSunshineFixture(t)
	network := pairSourceTestNode{relayTestNode: &relayTestNode{exposed: make(map[string]string)}, peer: peer}
	host, err := startHostRelay(ctx, network, base, func(ip netip.Addr) tailcfg.StableNodeID {
		return n.inboundPeer(snapshot.account, ip)
	})
	if err != nil {
		t.Fatal(err)
	}
	defer host.Close()
	n.host = host

	connect := func(index int, target string) (net.Conn, net.Conn, uint16) {
		t.Helper()
		client, err := network.Dial(ctx, "tcp4", target)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = client.Close() })
		deadline, _ := ctx.Deadline()
		_ = bindings.tcp[index].(*net.TCPListener).SetDeadline(deadline)
		sunshine, err := bindings.tcp[index].Accept()
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = sunshine.Close() })
		const request = "GET /pair HTTP/1.1\r\n\r\n"
		if _, err := io.WriteString(client, request); err != nil {
			t.Fatal(err)
		}
		_ = sunshine.SetReadDeadline(deadline)
		got := make([]byte, len(request))
		if _, err := io.ReadFull(sunshine, got); err != nil || string(got) != request {
			t.Fatalf("forwarded request = %q, %v", got, err)
		}
		return client, sunshine, sunshine.RemoteAddr().(*net.TCPAddr).AddrPort().Port()
	}
	check := func(id string, address netip.Addr, port uint16, account tailcfg.UserID, node string, want bool) {
		t.Helper()
		raw, err := json.Marshal(map[string]any{
			"peerId": id, "address": address.String(), "sourcePort": port, "owner": account, "nodeId": node,
		})
		if err != nil {
			t.Fatal(err)
		}
		result, err := n.handle(ctx, "pair-source", raw)
		if err != nil || result.(map[string]bool)["verified"] != want {
			t.Fatalf("pair-source(%s) = %v, %v; want verified=%t", raw, result, err, want)
		}
	}

	_, _, tlsPort := connect(1, gameStreamAddress(relayTestPeer, base-5))
	check("peer", peer, tlsPort, owner, "host", false)
	client, sunshine, port := connect(0, gameStreamAddress(relayTestPeer, base))
	check("peer", peer, port, owner, "host", true)
	check("different-peer", peer, port, owner, "host", false)
	check("peer", netip.MustParseAddr("100.64.0.11"), port, owner, "host", false)
	check("peer", peer, port, owner+1, "host", false)
	check("peer", peer, port, owner, "different-host", false)

	apiMu.Lock()
	who.Node.Tags = []string{"tag:server"}
	apiMu.Unlock()
	check("peer", peer, port, owner, "host", false)

	apiMu.Lock()
	who.Node.Tags = nil
	for _, peerStatus := range status.Peer {
		peerStatus.ID = "replacement"
	}
	who.Node.StableID = "replacement"
	apiMu.Unlock()
	check("replacement", peer, port, owner, "host", false)
	apiMu.Lock()
	for _, peerStatus := range status.Peer {
		peerStatus.ID = "peer"
	}
	who.Node.StableID = "peer"
	apiMu.Unlock()

	source := host.pairSource(port)
	if source == nil {
		t.Fatal("live mapping disappeared")
	}
	apiMu.Lock()
	onWhoIs = func() { _ = source.Close() }
	apiMu.Unlock()
	check("peer", peer, port, owner, "host", false)
	apiMu.Lock()
	onWhoIs = nil
	apiMu.Unlock()
	_ = client.Close()
	_ = sunshine.Close()
	check("peer", peer, port, owner, "host", false)
}
