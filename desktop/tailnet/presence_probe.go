// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/netip"
	"time"

	"tailscale.com/tailcfg"
)

const (
	presencePort     = 48988
	presenceSize     = 192
	presenceKind     = 17
	presenceNonceEnd = 34
)

const presenceMagic = "AURORA/PRESENCE/1"

// Probes run independently of route mutations. Both the selected address and the
// stable peer ID must still belong to the current owner when the reply is read.
func (n *embeddedNode) handlePresence(parent context.Context, raw json.RawMessage) (any, error) {
	var args struct {
		PeerID  tailcfg.StableNodeID `json:"peerId"`
		Address string               `json:"address"`
		Packet  []byte               `json:"packet"`
	}
	if err := decodeParams(raw, &args); err != nil {
		return nil, err
	}
	if !validPresence(args.Packet, 1) {
		return nil, errors.New("invalid presence request packet")
	}
	ip, err := netip.ParseAddr(args.Address)
	if err != nil || !ip.IsValid() {
		return nil, errors.New("presence requires a literal tailnet peer address")
	}
	ctx, cancel := context.WithTimeout(parent, 650*time.Millisecond)
	defer cancel()
	snapshot := n.snapshot.Load()
	if snapshot == nil || args.PeerID == "" || snapshot.peers[ip] != args.PeerID ||
		!n.verifyPeer(ctx, snapshot, ip, args.PeerID) {
		return nil, errors.New("presence peer is not an untagged device of the current account")
	}
	peer := netip.AddrPortFrom(ip, presencePort)
	conn, err := n.server.Dial(ctx, "udp", peer.String())
	if err != nil {
		return nil, fmt.Errorf("dial presence peer: %w", err)
	}
	defer conn.Close()
	stop := context.AfterFunc(ctx, func() { _ = conn.Close() })
	defer stop()
	deadline, _ := ctx.Deadline()
	if err := conn.SetDeadline(deadline); err != nil {
		return nil, err
	}
	if _, err := conn.Write(args.Packet); err != nil {
		return nil, fmt.Errorf("write presence probe: %w", err)
	}

	packet := make([]byte, presenceSize+1)
	for {
		count, err := conn.Read(packet)
		if err != nil {
			return nil, fmt.Errorf("read presence reply: %w", err)
		}
		if !validPresence(packet[:count], 2) ||
			!bytes.Equal(packet[presenceKind+1:presenceNonceEnd], args.Packet[presenceKind+1:presenceNonceEnd]) {
			continue
		}
		if !n.verifyPeer(ctx, snapshot, ip, args.PeerID) {
			return nil, errors.New("presence peer identity changed")
		}
		return packet[:count], nil
	}
}

func validPresence(packet []byte, kind byte) bool {
	return len(packet) == presenceSize && string(packet[:presenceKind]) == presenceMagic &&
		packet[presenceKind] == kind
}
