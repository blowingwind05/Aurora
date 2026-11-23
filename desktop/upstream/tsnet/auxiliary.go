// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"
	"time"

	"tailscale.com/tailcfg"
)

const pairingPort = 43826

type auxiliaryLease struct {
	peer netip.Addr
	id   tailcfg.StableNodeID
	stop func()
}

type presencePublisher struct {
	template atomic.Pointer[[presenceSize]byte]
	account  nodeAccount
	stop     func()
}

func (n *embeddedNode) handleAux(ctx context.Context, method string, raw json.RawMessage) (any, error) {
	switch method {
	case "publish":
		var args struct {
			Enabled *bool                `json:"enabled"`
			Owner   tailcfg.UserID       `json:"owner"`
			NodeID  tailcfg.StableNodeID `json:"nodeId"`
			Packet  []byte               `json:"packet"`
		}
		if err := decodeParams(raw, &args); err != nil {
			return nil, err
		}
		if args.Enabled == nil {
			return nil, errors.New("publish requires enabled")
		}
		if !*args.Enabled {
			n.stopPublisher()
			return map[string]bool{"enabled": false}, nil
		}
		if !validPresence(args.Packet, 2) {
			return nil, errors.New("publish requires a kind-2 presence packet")
		}
		snapshot, err := n.requireAccount(ctx, args.Owner, args.NodeID)
		if err != nil {
			return nil, err
		}
		if err := n.publishPresence(snapshot, args.Packet); err != nil {
			return nil, err
		}
		return map[string]bool{"enabled": true}, nil
	case "pair-host":
		var args struct {
			LocalPort uint16               `json:"localPort"`
			Key       []byte               `json:"key"`
			Owner     tailcfg.UserID       `json:"owner"`
			NodeID    tailcfg.StableNodeID `json:"nodeId"`
		}
		if err := decodeParams(raw, &args); err != nil {
			return nil, err
		}
		if args.LocalPort == 0 || args.LocalPort == streamPort+1 || len(args.Key) != sha256.Size {
			return nil, errors.New("pair-host requires a local port and a 32-byte key")
		}
		snapshot, err := n.requireAccount(ctx, args.Owner, args.NodeID)
		if err != nil {
			return nil, err
		}
		return n.startPairHost(snapshot, args.LocalPort, args.Key)
	case "pair-open":
		var args struct {
			PeerID  tailcfg.StableNodeID `json:"peerId"`
			Address string               `json:"address"`
			Key     []byte               `json:"key"`
		}
		if err := decodeParams(raw, &args); err != nil {
			return nil, err
		}
		ip, err := netip.ParseAddr(args.Address)
		if err != nil || !gameStreamTailnet.Contains(ip) || len(args.Key) != sha256.Size || args.PeerID == "" {
			return nil, errors.New("pair-open requires a tailnet IPv4 peer, peer ID and 32-byte key")
		}
		if _, err := n.readStatus(ctx); err != nil {
			return nil, err
		}
		snapshot := n.snapshot.Load()
		if snapshot == nil || snapshot.peers[ip] != args.PeerID || !n.verifyPeer(ctx, snapshot, ip, args.PeerID) {
			return nil, errors.New("pairing peer is not an untagged device of the current account")
		}
		return n.startPairDial(snapshot, ip, args.PeerID, args.Key)
	case "pair-source":
		var args struct {
			PeerID     tailcfg.StableNodeID `json:"peerId"`
			Address    string               `json:"address"`
			SourcePort uint16               `json:"sourcePort"`
			Owner      tailcfg.UserID       `json:"owner"`
			NodeID     tailcfg.StableNodeID `json:"nodeId"`
		}
		if err := decodeParams(raw, &args); err != nil {
			return nil, err
		}
		ip, err := netip.ParseAddr(args.Address)
		if err != nil || !gameStreamTailnet.Contains(ip) || args.PeerID == "" || args.SourcePort == 0 ||
			args.Owner.IsZero() || args.NodeID == "" {
			return nil, errors.New("pair-source requires a tailnet IPv4 peer, source port and local account")
		}
		verified := n.verifyPairSource(ctx, ip, args.PeerID, args.SourcePort, args.Owner, args.NodeID)
		return map[string]bool{"verified": verified}, nil
	default:
		return nil, errors.New("unknown embedded node method")
	}
}

func (n *embeddedNode) verifyPairSource(ctx context.Context, ip netip.Addr, peerID tailcfg.StableNodeID,
	sourcePort uint16, owner tailcfg.UserID, nodeID tailcfg.StableNodeID,
) bool {
	snapshot, err := n.requireAccount(ctx, owner, nodeID)
	if err != nil || n.host == nil || snapshot.peers[ip] != peerID {
		return false
	}
	source := n.host.pairSource(sourcePort)
	if source == nil || source.peer != ip || source.peerID != peerID {
		return false
	}
	// Check WhoIs without the inbound cache and require the very same connection
	// after the lookup. A reused port cannot validate a stale pending request.
	return n.verifyPeer(ctx, snapshot, ip, peerID) && n.host.pairSource(sourcePort) == source
}

func (n *embeddedNode) requireAccount(ctx context.Context, owner tailcfg.UserID, id tailcfg.StableNodeID) (*nodeSnapshot, error) {
	if _, err := n.readStatus(ctx); err != nil {
		return nil, err
	}
	snapshot := n.snapshot.Load()
	if snapshot == nil || snapshot.account.owner != owner || snapshot.account.id != id {
		return nil, errors.New("embedded account changed or is not ready")
	}
	return snapshot, nil
}

func (n *embeddedNode) closeAux() {
	n.stopPublisher()
	for id := range n.aux {
		n.closeAuxLease(id)
	}
}

func (n *embeddedNode) pruneAux(snapshot *nodeSnapshot) {
	if snapshot == nil {
		return
	}
	for lease, entry := range n.aux {
		if entry.peer.IsValid() && snapshot.peers[entry.peer] != entry.id {
			n.closeAuxLease(lease)
		}
	}
}

func (n *embeddedNode) closeAuxLease(id string) {
	if lease, ok := n.aux[id]; ok {
		delete(n.aux, id)
		lease.stop()
	}
}

func (n *embeddedNode) stopPublisher() {
	if n.publisher != nil {
		n.publisher.stop()
		n.publisher = nil
	}
}

func (n *embeddedNode) publishPresence(snapshot *nodeSnapshot, template []byte) error {
	if n.publisher != nil && n.publisher.account == snapshot.account {
		packet := [presenceSize]byte(template)
		n.publisher.template.Store(&packet)
		return nil
	}
	n.stopPublisher()
	ip4, ip6 := n.server.TailscaleIPs()
	var sockets []net.PacketConn
	for _, ip := range []netip.Addr{ip4, ip6} {
		if !ip.IsValid() {
			continue
		}
		conn, err := n.server.ListenPacket("udp", netip.AddrPortFrom(ip, presencePort).String())
		if err != nil {
			for _, socket := range sockets {
				_ = socket.Close()
			}
			return fmt.Errorf("bind tailnet presence: %w", err)
		}
		sockets = append(sockets, conn)
	}
	if len(sockets) == 0 {
		return errors.New("embedded node has no presence address")
	}

	ctx, cancel := context.WithCancel(n.ctx)
	var workers sync.WaitGroup
	pub := &presencePublisher{account: snapshot.account}
	packet := [presenceSize]byte(template)
	pub.template.Store(&packet)
	pub.stop = func() {
		cancel()
		for _, conn := range sockets {
			_ = conn.Close()
		}
		workers.Wait()
	}
	n.publisher = pub
	for _, conn := range sockets {
		workers.Go(func() {
			request := make([]byte, presenceSize+1)
			for ctx.Err() == nil {
				count, source, err := conn.ReadFrom(request)
				if err != nil {
					return
				}
				addr, ok := source.(*net.UDPAddr)
				if !ok || !validPresence(request[:count], 1) ||
					!n.allowInbound(pub.account, addr.AddrPort().Addr().Unmap()) {
					continue
				}
				reply := *pub.template.Load()
				copy(reply[presenceKind+1:presenceNonceEnd], request[presenceKind+1:presenceNonceEnd])
				_ = conn.SetWriteDeadline(time.Now().Add(time.Second))
				_, _ = conn.WriteTo(reply[:], source)
			}
		})
	}
	return nil
}

func (n *embeddedNode) startPairHost(snapshot *nodeSnapshot, localPort uint16, key []byte) (any, error) {
	if len(n.aux) >= 16 {
		return nil, errors.New("too many auxiliary leases")
	}
	ln, err := n.server.Listen("tcp4", netip.AddrPortFrom(snapshot.account.ip, pairingPort).String())
	if err != nil {
		return nil, fmt.Errorf("bind tailnet pairing: %w", err)
	}
	target := netip.AddrPortFrom(gameStreamLoopback, localPort).String()
	lease := newLeaseID()
	stop := servePairing(n.ctx, ln, func(ctx context.Context, incoming net.Conn) {
		source, ok := incoming.RemoteAddr().(*net.TCPAddr)
		if !ok {
			return
		}
		ip := source.AddrPort().Addr().Unmap()
		if !gameStreamTailnet.Contains(ip) || !n.allowInbound(snapshot.account, ip) {
			return
		}
		dialer := &net.Dialer{Timeout: requestTimeout}
		dst, err := dialer.DialContext(ctx, "tcp4", target)
		if err != nil {
			return
		}
		defer dst.Close()
		stopIO := context.AfterFunc(ctx, func() { _ = dst.Close() })
		defer stopIO()
		addr := ip.As4()
		localSource := gameStreamLoopback.As4()
		payload := []byte{4, addr[0], addr[1], addr[2], addr[3], localSource[0], localSource[1], localSource[2], localSource[3]}
		if err := sendHostProof(dst, key, payload); err != nil {
			return
		}
		spliceGameStreamTCP(ctx, incoming, dst)
	})
	n.aux[lease] = auxiliaryLease{stop: stop}
	return map[string]string{"leaseId": lease}, nil
}

func (n *embeddedNode) startPairDial(snapshot *nodeSnapshot, ip netip.Addr, id tailcfg.StableNodeID, key []byte) (any, error) {
	if len(n.aux) >= 16 {
		return nil, errors.New("too many auxiliary leases")
	}
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	port := ln.Addr().(*net.TCPAddr).Port
	lease := newLeaseID()
	stop := servePairing(n.ctx, ln, func(ctx context.Context, incoming net.Conn) {
		if !checkDialProof(incoming, key) || !n.verifyPeer(ctx, snapshot, ip, id) {
			return
		}
		dialCtx, cancel := context.WithTimeout(ctx, requestTimeout)
		defer cancel()
		dst, err := n.server.Dial(dialCtx, "tcp4", netip.AddrPortFrom(ip, pairingPort).String())
		if err != nil {
			return
		}
		defer dst.Close()
		spliceGameStreamTCP(ctx, incoming, dst)
	})
	n.aux[lease] = auxiliaryLease{peer: ip, id: id, stop: stop}
	return struct {
		LeaseID   string `json:"leaseId"`
		LocalPort int    `json:"localPort"`
	}{lease, port}, nil
}

func servePairing(parent context.Context, ln net.Listener, handle func(context.Context, net.Conn)) func() {
	ctx, cancel := context.WithCancel(parent)
	done := make(chan struct{})
	stop := context.AfterFunc(ctx, func() { _ = ln.Close() })
	go func() {
		defer close(done)
		defer stop()
		var workers sync.WaitGroup
		defer workers.Wait()
		slots := make(chan struct{}, 8)
		for {
			conn, err := ln.Accept()
			if err != nil {
				cancel()
				return
			}
			select {
			case slots <- struct{}{}:
				workers.Go(func() {
					defer func() { <-slots }()
					defer conn.Close()
					unblock := context.AfterFunc(ctx, func() { _ = conn.Close() })
					defer unblock()
					handle(ctx, conn)
				})
			default:
				_ = conn.Close()
			}
		}
	}()
	return func() { cancel(); <-done }
}

func pairProof(purpose string, key, nonce, payload []byte) []byte {
	mac := hmac.New(sha256.New, key)
	_, _ = io.WriteString(mac, purpose)
	_, _ = mac.Write(nonce)
	_, _ = mac.Write(payload)
	return mac.Sum(nil)
}

func checkDialProof(conn net.Conn, key []byte) bool {
	_ = conn.SetDeadline(time.Now().Add(requestTimeout))
	defer conn.SetDeadline(time.Time{})
	nonce := make([]byte, sha256.Size)
	_, _ = rand.Read(nonce)
	if _, err := conn.Write(nonce); err != nil {
		return false
	}
	proof := make([]byte, sha256.Size)
	_, err := io.ReadFull(conn, proof)
	return err == nil && hmac.Equal(proof, pairProof("AURORA/PAIR/DIAL/1", key, nonce, nil))
}

func sendHostProof(conn net.Conn, key, payload []byte) error {
	_ = conn.SetDeadline(time.Now().Add(requestTimeout))
	defer conn.SetDeadline(time.Time{})
	nonce := make([]byte, sha256.Size)
	if _, err := io.ReadFull(conn, nonce); err != nil {
		return err
	}
	_, err := conn.Write(append(payload, pairProof("AURORA/PAIR/HOST/1", key, nonce, payload)...))
	return err
}
