// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/netip"
	"sync"
	"time"

	"tailscale.com/tailcfg"
)

// The two halves forward the pinned Sunshine GameStream endpoints verbatim.
// In particular the web administration endpoint (base+1) is never exposed.
var gameStreamTCP = [...]int{0, -5, 21} // HTTP, pinned HTTPS, RTSP
var gameStreamUDP = [...]int{9, 10, 11} // video, ENet control, audio

var gameStreamLoopback = netip.AddrFrom4([4]byte{127, 0, 0, 1})
var gameStreamTailnet = netip.MustParsePrefix("100.64.0.0/10")

const (
	gameStreamDialTimeout = 5 * time.Second
	gameStreamUDPIdle     = 45 * time.Second
	gameStreamPeerLimit   = 32 // per listening port
)

type gameStreamNode interface {
	TailscaleIPs() (netip.Addr, netip.Addr)
	Listen(network, address string) (net.Listener, error)
	ListenPacket(network, address string) (net.PacketConn, error)
	Dial(ctx context.Context, network, address string) (net.Conn, error)
}

// A controller relay is scoped to a single peer and one IPC lease.
// A host relay lives for as long as Aurora's bundled Sunshine is accepting connections.
type gameStreamRelay struct {
	BasePort    int
	ctx         context.Context
	cancel      context.CancelFunc
	done        chan struct{}
	pairSources *gameStreamPairSources
}

func (r *gameStreamRelay) Close() error {
	r.cancel()
	<-r.done
	return nil
}

type relayDial func(context.Context, string, string) (net.Conn, error)

type gameStreamPairSources struct {
	identify func(netip.Addr) tailcfg.StableNodeID
	mu       sync.Mutex
	byPort   map[uint16]*gameStreamPairSource
}

type gameStreamPairSource struct {
	*net.TCPConn
	sources *gameStreamPairSources
	port    uint16
	peer    netip.Addr
	peerID  tailcfg.StableNodeID
}

func (s *gameStreamPairSources) track(conn net.Conn, peer netip.Addr, peerID tailcfg.StableNodeID) net.Conn {
	tcp, ok := conn.(*net.TCPConn)
	if !ok {
		return conn
	}
	local := tcp.LocalAddr().(*net.TCPAddr).AddrPort()
	if local.Addr().Unmap() != gameStreamLoopback || local.Port() == 0 {
		return conn
	}
	source := &gameStreamPairSource{TCPConn: tcp, sources: s, port: local.Port(), peer: peer, peerID: peerID}
	s.mu.Lock()
	s.byPort[source.port] = source
	s.mu.Unlock()
	return source
}

func (s *gameStreamPairSource) Close() error {
	s.sources.mu.Lock()
	if s.sources.byPort[s.port] == s {
		delete(s.sources.byPort, s.port)
	}
	s.sources.mu.Unlock()
	return s.TCPConn.Close()
}

func (r *gameStreamRelay) pairSource(port uint16) *gameStreamPairSource {
	if r.pairSources == nil || r.ctx.Err() != nil {
		return nil
	}
	r.pairSources.mu.Lock()
	defer r.pairSources.mu.Unlock()
	return r.pairSources.byPort[port]
}

type gameStreamBindings struct {
	tcp []net.Listener
	udp []net.PacketConn
}

func (b *gameStreamBindings) close() {
	for _, listener := range b.tcp {
		listener.Close()
	}
	for _, socket := range b.udp {
		socket.Close()
	}
}

func startHostRelay(ctx context.Context, node gameStreamNode, basePort int,
	identifyPeer func(netip.Addr) tailcfg.StableNodeID,
) (*gameStreamRelay, error) {
	if identifyPeer == nil {
		return nil, errors.New("GameStream host requires a peer authorization callback")
	}
	localIP, _ := node.TailscaleIPs()
	if !gameStreamTailnet.Contains(localIP) {
		return nil, errors.New("GameStream host requires a ready tsnet IPv4 address")
	}
	if !validGameStreamBase(basePort) {
		return nil, fmt.Errorf("invalid GameStream base port %d", basePort)
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}

	bindings, err := bindGameStream(localIP, basePort, node.Listen, node.ListenPacket)
	if err != nil {
		return nil, err
	}
	dialer := &net.Dialer{}
	sources := &gameStreamPairSources{identify: identifyPeer, byPort: make(map[uint16]*gameStreamPairSource)}
	allowPeer := func(ip netip.Addr) bool { return identifyPeer(ip) != "" }
	return runGameStream(ctx, bindings, basePort, gameStreamLoopback, basePort, dialer.DialContext, allowPeer, sources)
}

func startControllerRelay(ctx context.Context, node gameStreamNode, peer netip.Addr, basePort int) (*gameStreamRelay, error) {
	// IPC must select this IP from the connected node's peers. Accept only a
	// literal tailnet IPv4 address here, and never let the client choose a target
	// via a packet, DNS name, or through the local listener.
	if !gameStreamTailnet.Contains(peer) || peer == netip.MustParseAddr("100.100.100.100") {
		return nil, errors.New("GameStream controller requires a tailnet IPv4 peer")
	}
	if !validGameStreamBase(basePort) {
		return nil, fmt.Errorf("invalid GameStream base port %d", basePort)
	}
	for attempt := 0; attempt < 32; attempt++ {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		// Keep the first socket open while claiming the rest of the cluster.
		http, err := net.Listen("tcp4", gameStreamAddress(gameStreamLoopback, 0))
		if err != nil {
			return nil, fmt.Errorf("assign GameStream loopback port: %w", err)
		}
		localBase := http.Addr().(*net.TCPAddr).Port
		if !validGameStreamBase(localBase) {
			http.Close()
			continue
		}
		bindings, err := bindGameStream(gameStreamLoopback, localBase,
			func(network, address string) (net.Listener, error) {
				if address == gameStreamAddress(gameStreamLoopback, localBase) {
					return http, nil
				}
				return net.Listen(network, address)
			}, net.ListenPacket)
		if err == nil {
			return runGameStream(ctx, bindings, localBase, peer, basePort, node.Dial, func(ip netip.Addr) bool {
				return ip == gameStreamLoopback
			}, nil)
		}
	}
	return nil, errors.New("could not reserve a GameStream loopback port cluster")
}

func validGameStreamBase(port int) bool {
	return port > 5 && port <= 65535-21
}

func gameStreamAddress(ip netip.Addr, port int) string {
	return netip.AddrPortFrom(ip, uint16(port)).String()
}

func bindGameStream(ip netip.Addr, basePort int,
	listen func(string, string) (net.Listener, error),
	listenPacket func(string, string) (net.PacketConn, error),
) (*gameStreamBindings, error) {
	bindings := &gameStreamBindings{}
	for _, offset := range gameStreamTCP {
		address := gameStreamAddress(ip, basePort+offset)
		listener, err := listen("tcp4", address)
		if err != nil {
			bindings.close()
			return nil, fmt.Errorf("bind GameStream TCP %s: %w", address, err)
		}
		bindings.tcp = append(bindings.tcp, listener)
	}
	for _, offset := range gameStreamUDP {
		address := gameStreamAddress(ip, basePort+offset)
		socket, err := listenPacket("udp4", address)
		if err != nil {
			bindings.close()
			return nil, fmt.Errorf("bind GameStream UDP %s: %w", address, err)
		}
		bindings.udp = append(bindings.udp, socket)
	}
	return bindings, nil
}

func runGameStream(parent context.Context, bindings *gameStreamBindings, localBase int,
	target netip.Addr, targetBase int, dial relayDial, allowPeer func(netip.Addr) bool, pairSources *gameStreamPairSources,
) (*gameStreamRelay, error) {
	if err := parent.Err(); err != nil {
		bindings.close()
		return nil, err
	}
	ctx, cancel := context.WithCancel(parent)
	relay := &gameStreamRelay{BasePort: localBase, ctx: ctx, cancel: cancel, done: make(chan struct{}), pairSources: pairSources}
	var workers sync.WaitGroup
	for i, listener := range bindings.tcp {
		offset := gameStreamTCP[i]
		address := gameStreamAddress(target, targetBase+offset)
		var sources *gameStreamPairSources
		if offset == 0 {
			sources = pairSources
		}
		workers.Go(func() {
			defer cancel()
			forwardGameStreamTCP(ctx, listener, address, dial, allowPeer, sources)
		})
	}
	for i, socket := range bindings.udp {
		address := gameStreamAddress(target, targetBase+gameStreamUDP[i])
		workers.Go(func() {
			defer cancel()
			forwardGameStreamUDP(ctx, socket, address, dial, allowPeer)
		})
	}
	go func() {
		<-ctx.Done()
		bindings.close()
		workers.Wait()
		close(relay.done)
	}()
	return relay, nil
}

func forwardGameStreamTCP(parent context.Context, listener net.Listener, target string, dial relayDial,
	allowPeer func(netip.Addr) bool, pairSources *gameStreamPairSources,
) {
	ctx, cancel := context.WithCancel(parent)
	var workers sync.WaitGroup
	defer func() {
		cancel()
		workers.Wait()
	}()
	slots := make(chan struct{}, gameStreamPeerLimit)
	for {
		client, err := listener.Accept()
		if err != nil {
			return
		}
		remote, ok := client.RemoteAddr().(*net.TCPAddr)
		if !ok {
			client.Close()
			continue
		}
		peer := remote.AddrPort().Addr().Unmap()
		var peerID tailcfg.StableNodeID
		if pairSources != nil {
			peerID = pairSources.identify(peer)
			ok = peerID != ""
		} else {
			ok = allowPeer(peer)
		}
		if !ok {
			client.Close()
			continue
		}
		select {
		case slots <- struct{}{}:
			workers.Go(func() {
				defer func() { <-slots }()
				defer client.Close()
				dialContext, cancel := context.WithTimeout(ctx, gameStreamDialTimeout)
				server, err := dial(dialContext, "tcp4", target)
				cancel()
				if err == nil {
					// Publish the source port before copying any request bytes to Sunshine.
					if pairSources != nil {
						server = pairSources.track(server, peer, peerID)
					}
					spliceGameStreamTCP(ctx, client, server)
				}
			})
		default:
			client.Close()
		}
	}
}

func spliceGameStreamTCP(ctx context.Context, client, server net.Conn) {
	closeBoth := func() {
		client.Close()
		server.Close()
	}
	defer closeBoth()
	stop := context.AfterFunc(ctx, closeBoth)
	defer stop()

	copyStream := func(destination, source net.Conn) {
		if _, err := io.Copy(destination, source); err == nil {
			if writer, ok := destination.(interface{ CloseWrite() error }); ok && writer.CloseWrite() == nil {
				return
			}
		}
		closeBoth()
	}
	var directions sync.WaitGroup
	directions.Go(func() { copyStream(server, client) })
	copyStream(client, server)
	directions.Wait()
}

// Audio/video start with Moonlight probes. Control starts with an ENet client
// connect. Keep each source IP:port pinned to a connected socket until idle so
// unsolicited replies after those probes reach the correct client.
func forwardGameStreamUDP(ctx context.Context, socket net.PacketConn, target string, dial relayDial,
	allowPeer func(netip.Addr) bool,
) {
	var workers sync.WaitGroup
	var mu sync.Mutex
	peers := make(map[netip.AddrPort]net.Conn)
	defer func() {
		mu.Lock()
		for _, conn := range peers {
			conn.Close()
		}
		mu.Unlock()
		workers.Wait()
	}()

	buffer := make([]byte, 65535)
	for {
		size, source, err := socket.ReadFrom(buffer)
		if err != nil {
			return
		}
		udp, ok := source.(*net.UDPAddr)
		if !ok || !allowPeer(udp.AddrPort().Addr().Unmap()) {
			continue
		}
		key := udp.AddrPort()
		mu.Lock()
		conn := peers[key]
		canAdd := len(peers) < gameStreamPeerLimit
		mu.Unlock()
		if conn == nil {
			if !canAdd {
				continue
			}
			dialContext, cancel := context.WithTimeout(ctx, gameStreamDialTimeout)
			conn, err = dial(dialContext, "udp4", target)
			cancel()
			if err != nil {
				continue
			}
			mu.Lock()
			peers[key] = conn
			mu.Unlock()
			workers.Go(func() {
				defer func() {
					conn.Close()
					mu.Lock()
					delete(peers, key)
					mu.Unlock()
				}()
				reply := make([]byte, 65535)
				for {
					conn.SetReadDeadline(time.Now().Add(gameStreamUDPIdle))
					count, err := conn.Read(reply)
					if err != nil {
						return
					}
					if !allowPeer(key.Addr().Unmap()) {
						return
					}
					if _, err := socket.WriteTo(reply[:count], udp); err != nil && ctx.Err() != nil {
						return
					}
				}
			})
		}
		conn.SetReadDeadline(time.Now().Add(gameStreamUDPIdle))
		conn.SetWriteDeadline(time.Now().Add(gameStreamDialTimeout))
		conn.Write(buffer[:size])
	}
}
