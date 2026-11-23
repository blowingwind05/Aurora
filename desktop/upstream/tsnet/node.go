// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/netip"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"tailscale.com/client/local"
	"tailscale.com/ipn"
	"tailscale.com/ipn/ipnstate"
	"tailscale.com/ipn/store"
	"tailscale.com/tailcfg"
	"tailscale.com/tsnet"
	"tailscale.com/types/logger"
)

const (
	stateFile      = "tailscaled.state"
	streamPort     = 48989
	requestTimeout = 3 * time.Second
)

type nodeAccount struct {
	id      tailcfg.StableNodeID
	owner   tailcfg.UserID
	tailnet tailcfg.StableTailnetID
	ip      netip.Addr
}

type nodeSnapshot struct {
	account  nodeAccount
	peers    map[netip.Addr]tailcfg.StableNodeID
	verified sync.Map // netip.Addr -> bool, valid only for this snapshot
}

type nodeLease struct {
	relay *gameStreamRelay
	peer  netip.Addr
	id    tailcfg.StableNodeID
}

type embeddedNode struct {
	ctx      context.Context
	cancel   context.CancelFunc
	done     chan struct{}
	ready    chan struct{}
	server   *tsnet.Server
	client   *local.Client // immutable after ready is closed
	startErr error

	mu          sync.Mutex
	snapshot    atomic.Pointer[nodeSnapshot]
	wantsLogin  bool
	loginQueued bool
	loginErr    error
	loggingOut  bool
	host        *gameStreamRelay
	leases      map[string]nodeLease
	aux         map[string]auxiliaryLease
	publisher   *presencePublisher
}

func startNode(parent context.Context, dir string) *embeddedNode {
	ctx, cancel := context.WithCancel(parent)
	hostname, _ := os.Hostname()
	hostname, _, _ = strings.Cut(hostname, ".")
	hostname = "aurora-" + strings.Trim(hostname, "-")
	if hostname == "aurora-" {
		hostname = "aurora-desktop"
	}
	n := &embeddedNode{
		ctx: ctx, cancel: cancel, done: make(chan struct{}), ready: make(chan struct{}),
		server: &tsnet.Server{
			Dir: dir, Hostname: hostname, ControlURL: ipn.DefaultControlURL,
			Logf: logger.Discard, UserLogf: logger.Discard,
		},
		leases: make(map[string]nodeLease),
		aux:    make(map[string]auxiliaryLease),
	}
	go n.run(dir)
	return n
}

func (n *embeddedNode) run(dir string) {
	defer close(n.done)
	n.server.Store, n.startErr = store.NewFileStore(logger.Discard, filepath.Join(dir, stateFile))
	if n.startErr != nil {
		close(n.ready)
		return
	}
	defer n.server.Close()
	n.client, n.startErr = n.server.LocalClient()
	close(n.ready)
	if n.startErr != nil {
		return
	}
	n.mu.Lock()
	if n.loginQueued && n.ctx.Err() == nil {
		ctx, cancel := context.WithTimeout(n.ctx, requestTimeout)
		_, n.loginErr = n.login(ctx)
		cancel()
	}
	n.loginQueued = false
	n.mu.Unlock()
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-n.ctx.Done():
			n.mu.Lock()
			n.updateRoutes(nil)
			n.mu.Unlock()
			return
		case <-ticker.C:
			n.mu.Lock()
			ctx, cancel := context.WithTimeout(n.ctx, requestTimeout)
			_, _ = n.readStatus(ctx)
			cancel()
			n.mu.Unlock()
		}
	}
}

func (n *embeddedNode) close() {
	n.cancel()
	<-n.done
}

func (n *embeddedNode) handle(parent context.Context, method string, raw json.RawMessage) (any, error) {
	if method == "presence" {
		return n.handlePresence(parent, raw)
	}
	n.mu.Lock()
	defer n.mu.Unlock()
	ctx, cancel := context.WithTimeout(parent, requestTimeout)
	defer cancel()

	switch method {
	case "status":
		if err := decodeParams(raw, &struct{}{}); err != nil {
			return nil, err
		}
		return n.readStatus(ctx)
	case "login":
		if err := decodeParams(raw, &struct{}{}); err != nil {
			return nil, err
		}
		select {
		case <-n.ready:
		default:
			n.loginQueued = true
			return map[string]bool{"pending": true}, nil
		}
		n.loginQueued = false
		pending, err := n.login(ctx)
		return map[string]bool{"pending": pending}, err
	case "logout":
		if err := decodeParams(raw, &struct{}{}); err != nil {
			return nil, err
		}
		if err := n.available(); err != nil {
			return nil, err
		}
		n.loginQueued = false
		n.loginErr = nil
		n.wantsLogin = false
		n.loggingOut = true
		n.updateRoutes(nil)
		logoutCtx, cancel := context.WithTimeout(parent, 8*time.Second)
		defer cancel()
		if err := n.client.Logout(logoutCtx); err != nil {
			n.loggingOut = false
			return nil, fmt.Errorf("sign out of embedded node: %w", err)
		}
		return struct{}{}, nil
	case "host":
		var args struct {
			Enabled *bool `json:"enabled"`
		}
		if err := decodeParams(raw, &args); err != nil {
			return nil, err
		}
		if args.Enabled == nil {
			return nil, errors.New("host requires enabled")
		}
		return n.setHost(ctx, *args.Enabled)
	case "open":
		var args struct {
			Address string `json:"address"`
			Port    int    `json:"port"`
		}
		if err := decodeParams(raw, &args); err != nil {
			return nil, err
		}
		return n.openStream(ctx, args.Address, args.Port)
	case "close":
		return n.closeLease(raw)
	default:
		return n.handleAux(ctx, method, raw)
	}
}

func (n *embeddedNode) available() error {
	select {
	case <-n.ready:
		if n.startErr != nil {
			return fmt.Errorf("start embedded node: %w", n.startErr)
		}
		return nil
	default:
		return errors.New("embedded node is starting")
	}
}

func (n *embeddedNode) login(ctx context.Context) (bool, error) {
	n.loginErr = nil
	status, err := n.readStatus(ctx)
	if err != nil {
		return false, err
	}
	if status.BackendState == ipn.Running.String() {
		return false, nil
	}
	if n.loggingOut {
		return false, errors.New("embedded node is signing out; retry when ready")
	}
	prefs, err := n.client.GetPrefs(ctx)
	if err != nil {
		return false, fmt.Errorf("read embedded login preferences: %w", err)
	}
	if !prefs.WantRunning {
		_, err = n.client.EditPrefs(ctx, &ipn.MaskedPrefs{
			WantRunningSet: true,
			Prefs:          ipn.Prefs{WantRunning: true},
		})
		if err != nil {
			return false, fmt.Errorf("enable embedded node for login: %w", err)
		}
	}
	if err := n.client.StartLoginInteractive(ctx); err != nil {
		return false, fmt.Errorf("start interactive login: %w", err)
	}
	n.wantsLogin = true
	return true, nil
}

// readStatus and all route mutations are serialized under mu. The packet path
// uses the immutable account snapshot and never waits on a relay closing under mu.
func (n *embeddedNode) readStatus(ctx context.Context) (*ipnstate.Status, error) {
	select {
	case <-n.ready:
		if n.startErr != nil {
			return nil, fmt.Errorf("start embedded node: %w", n.startErr)
		}
	default:
		return &ipnstate.Status{BackendState: "Starting"}, nil
	}
	if n.loginErr != nil {
		return nil, fmt.Errorf("start interactive login: %w", n.loginErr)
	}
	status, err := n.client.Status(ctx)
	if err != nil {
		n.updateRoutes(nil)
		return nil, fmt.Errorf("read embedded node status: %w", err)
	}
	if n.loggingOut && (status.BackendState == ipn.NeedsLogin.String() || status.BackendState == ipn.NoState.String() ||
		status.BackendState == ipn.Stopped.String()) {
		n.loggingOut = false
	}
	if n.loggingOut {
		n.updateRoutes(nil)
		return &ipnstate.Status{BackendState: ipn.Stopped.String()}, nil
	}
	if status.BackendState == ipn.Running.String() {
		n.wantsLogin = false
	}
	if !n.wantsLogin || !officialLoginURL(status.AuthURL) {
		status.AuthURL = ""
	}
	n.updateRoutes(snapshotFromStatus(status))
	return status, nil
}

func officialLoginURL(raw string) bool {
	u, err := url.Parse(raw)
	return err == nil && u.Scheme == "https" && u.User == nil &&
		(u.Host == "login.tailscale.com" || u.Host == "controlplane.tailscale.com")
}

func snapshotFromStatus(status *ipnstate.Status) *nodeSnapshot {
	self := status.Self
	if status.BackendState != ipn.Running.String() || self == nil || self.ID == "" || self.UserID.IsZero() ||
		self.ShareeNode || (self.Tags != nil && self.Tags.Len() != 0) || status.CurrentTailnet == nil {
		return nil
	}
	snapshot := &nodeSnapshot{
		account: nodeAccount{id: self.ID, owner: self.UserID, tailnet: status.CurrentTailnet.StableID},
		peers:   make(map[netip.Addr]tailcfg.StableNodeID),
	}
	for _, ip := range status.TailscaleIPs {
		if gameStreamTailnet.Contains(ip) {
			snapshot.account.ip = ip
			break
		}
	}
	if !snapshot.account.ip.IsValid() {
		return nil
	}
	for _, peer := range status.Peer {
		if peer == nil || peer.ID == "" || peer.ID == self.ID || peer.UserID != self.UserID ||
			!peer.AltSharerUserID.IsZero() || peer.ShareeNode || (peer.Tags != nil && peer.Tags.Len() != 0) {
			continue
		}
		for _, ip := range peer.TailscaleIPs {
			if ip.IsValid() && ip != snapshot.account.ip {
				snapshot.peers[ip] = peer.ID
			}
		}
	}
	return snapshot
}

func (n *embeddedNode) updateRoutes(next *nodeSnapshot) {
	previous := n.snapshot.Swap(next)
	if next == nil || previous == nil || previous.account != next.account {
		if n.host != nil {
			n.host.Close()
			n.host = nil
		}
		n.closeAux()
	}
	for lease, entry := range n.leases {
		if next == nil || previous == nil || previous.account != next.account || next.peers[entry.peer] != entry.id {
			entry.relay.Close()
			delete(n.leases, lease)
		}
	}
	n.pruneAux(next)
}

// A packet callback is used for TCP and UDP. Cache the local WhoIs result for
// the current status snapshot to avoid per-packet HTTP on the video path.
func (n *embeddedNode) allowInbound(account nodeAccount, ip netip.Addr) bool {
	return n.inboundPeer(account, ip) != ""
}

func (n *embeddedNode) inboundPeer(account nodeAccount, ip netip.Addr) tailcfg.StableNodeID {
	snapshot := n.snapshot.Load()
	if snapshot == nil || snapshot.account != account {
		return ""
	}
	id := snapshot.peers[ip]
	if id == "" {
		return ""
	}
	verified, cached := snapshot.verified.Load(ip)
	if !cached {
		ctx, cancel := context.WithTimeout(n.ctx, time.Second)
		defer cancel()
		verified = n.verifyPeer(ctx, snapshot, ip, id)
		snapshot.verified.Store(ip, verified)
	}
	current := n.snapshot.Load()
	if !verified.(bool) || current == nil || current.account != account || current.peers[ip] != id {
		return ""
	}
	return id
}

func (n *embeddedNode) verifyPeer(ctx context.Context, snapshot *nodeSnapshot, ip netip.Addr, id tailcfg.StableNodeID) bool {
	who, err := n.client.WhoIs(ctx, ip.String())
	current := n.snapshot.Load()
	if err != nil || who == nil || who.Node == nil || who.UserProfile == nil || current == nil ||
		current.account != snapshot.account || current.peers[ip] != id {
		return false
	}
	return who.Node.StableID == id && who.Node.User == snapshot.account.owner &&
		who.UserProfile.ID == snapshot.account.owner && !who.Node.IsTagged() &&
		who.Node.Sharer.IsZero() && (!who.Node.Hostinfo.Valid() || !who.Node.Hostinfo.ShareeNode())
}

func (n *embeddedNode) setHost(ctx context.Context, enabled bool) (any, error) {
	if !enabled {
		if n.host != nil {
			n.host.Close()
			n.host = nil
		}
		return map[string]bool{"enabled": false}, nil
	}
	if _, err := n.readStatus(ctx); err != nil {
		return nil, err
	}
	snapshot := n.snapshot.Load()
	if snapshot == nil {
		return nil, errors.New("embedded node is not ready for hosting")
	}
	if n.host == nil {
		relay, err := startHostRelay(n.ctx, n.server, streamPort, func(ip netip.Addr) tailcfg.StableNodeID {
			return n.inboundPeer(snapshot.account, ip)
		})
		if err != nil {
			return nil, err
		}
		n.host = relay
	}
	return map[string]bool{"enabled": true}, nil
}

func (n *embeddedNode) openStream(ctx context.Context, address string, port int) (any, error) {
	ip, err := netip.ParseAddr(address)
	if err != nil || !gameStreamTailnet.Contains(ip) || port != streamPort {
		return nil, errors.New("open requires a tailnet IPv4 peer and the bundled Sunshine port 48989")
	}
	if _, err := n.readStatus(ctx); err != nil {
		return nil, err
	}
	snapshot := n.snapshot.Load()
	if snapshot == nil || snapshot.peers[ip] == "" || !n.verifyPeer(ctx, snapshot, ip, snapshot.peers[ip]) {
		return nil, errors.New("stream peer is not an untagged device of the current account")
	}
	if len(n.leases) >= 32 {
		return nil, errors.New("too many open embedded stream leases")
	}
	relay, err := startControllerRelay(n.ctx, n.server, ip, port)
	if err != nil {
		return nil, err
	}
	lease := newLeaseID()
	n.leases[lease] = nodeLease{relay: relay, peer: ip, id: snapshot.peers[ip]}
	return struct {
		Lease string `json:"lease"`
		Port  int    `json:"port"`
	}{lease, relay.BasePort}, nil
}

func newLeaseID() string {
	var random [16]byte
	_, _ = rand.Read(random[:])
	return hex.EncodeToString(random[:])
}
