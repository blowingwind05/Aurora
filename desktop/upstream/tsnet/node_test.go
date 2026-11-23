// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/netip"
	"testing"

	"tailscale.com/client/local"
	"tailscale.com/client/tailscale/apitype"
	"tailscale.com/ipn/ipnstate"
	"tailscale.com/tailcfg"
)

type localAPITransport func(*http.Request) (*http.Response, error)

func (f localAPITransport) RoundTrip(req *http.Request) (*http.Response, error) { return f(req) }

func TestInboundPeerRequiresCurrentSameOwnerWhoIs(t *testing.T) {
	hostIP := netip.MustParseAddr("100.65.1.1")
	peerIP := netip.MustParseAddr("100.65.1.2")
	otherIP := netip.MustParseAddr("100.65.1.3")
	owner := tailcfg.UserID(1234)
	status := &ipnstate.Status{
		BackendState:   "Running",
		TailscaleIPs:   []netip.Addr{hostIP},
		CurrentTailnet: &ipnstate.TailnetStatus{StableID: "tailnet"},
		Self:           &ipnstate.PeerStatus{ID: "host", UserID: owner},
	}
	genuine := &tailcfg.Node{StableID: "peer", User: owner}
	who := apitype.WhoIsResponse{Node: genuine, UserProfile: &tailcfg.UserProfile{ID: owner}}
	n := &embeddedNode{
		ctx: t.Context(),
		client: &local.Client{
			OmitAuth: true,
			Transport: localAPITransport(func(req *http.Request) (*http.Response, error) {
				if req.URL.Path != "/localapi/v0/whois" || req.URL.Query().Get("addr") != peerIP.String() {
					t.Fatalf("unexpected LocalAPI request: %s", req.URL)
				}
				data, err := json.Marshal(who)
				if err != nil {
					return nil, err
				}
				return &http.Response{StatusCode: http.StatusOK, Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(data))}, nil
			}),
		},
	}
	// Use the JSON decoder for the wire-shaped peer map instead of synthesizing keys.
	if err := json.Unmarshal([]byte(`{"Peer":{"nodekey:0101010101010101010101010101010101010101010101010101010101010101":{"ID":"peer","UserID":1234,"TailscaleIPs":["100.65.1.2"]},"nodekey:0202020202020202020202020202020202020202020202020202020202020202":{"ID":"different-owner","UserID":4321,"TailscaleIPs":["100.65.1.3"]}}}`), status); err != nil {
		t.Fatal(err)
	}
	snapshot := snapshotFromStatus(status)
	if snapshot == nil || snapshot.peers[peerIP] != "peer" || snapshot.peers[otherIP] != "" {
		t.Fatal("status failed to select the current owner's peers")
	}
	n.snapshot.Store(snapshot)
	if !n.allowInbound(snapshot.account, peerIP) || n.allowInbound(snapshot.account, otherIP) {
		t.Fatal("unexpected peer authorization result")
	}

	genuine.Tags = []string{"tag:server"}
	fresh := snapshotFromStatus(status)
	n.snapshot.Store(fresh)
	if n.allowInbound(snapshot.account, peerIP) {
		t.Fatal("a newly tagged peer retained authorization")
	}

	genuine.Tags = nil
	changed := snapshotFromStatus(status)
	changed.account.id = "different-host"
	n.snapshot.Store(changed)
	if n.allowInbound(snapshot.account, peerIP) {
		t.Fatal("a listener from the previous account generation retained authorization")
	}
}
