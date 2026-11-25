// SPDX-License-Identifier: GPL-3.0-only

package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"sync/atomic"
	"testing"
	"time"

	"tailscale.com/tailcfg"
)

// Route virtual tailnet endpoints through real kernel sockets on loopback.
// This fixture runs both relay halves without a machine VPN or tailnet login.
type relayTestNode struct {
	exposed map[string]string
	failUDP bool
}

var relayTestPeer = netip.MustParseAddr("100.64.0.9")

func (n *relayTestNode) TailscaleIPs() (netip.Addr, netip.Addr) {
	return relayTestPeer, netip.Addr{}
}

func (n *relayTestNode) Listen(network, address string) (net.Listener, error) {
	listener, err := net.Listen(network, "127.0.0.1:0")
	if err == nil {
		n.exposed[network+" "+address] = listener.Addr().String()
	}
	return listener, err
}

func (n *relayTestNode) ListenPacket(network, address string) (net.PacketConn, error) {
	if n.failUDP {
		return nil, errors.New("test UDP bind failure")
	}
	socket, err := net.ListenPacket(network, "127.0.0.1:0")
	if err == nil {
		n.exposed[network+" "+address] = socket.LocalAddr().String()
	}
	return socket, err
}

func (n *relayTestNode) Dial(ctx context.Context, network, address string) (net.Conn, error) {
	exposed, ok := n.exposed[network+" "+address]
	if !ok {
		return nil, errors.New("test target not published")
	}
	return (&net.Dialer{}).DialContext(ctx, network, exposed)
}

func bindSunshineFixture(t *testing.T) (*gameStreamBindings, int) {
	t.Helper()
	for attempt := 0; attempt < 32; attempt++ {
		http, err := net.Listen("tcp4", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		base := http.Addr().(*net.TCPAddr).Port
		if !validGameStreamBase(base) {
			http.Close()
			continue
		}
		bindings, err := bindGameStream(gameStreamLoopback, base,
			func(network, address string) (net.Listener, error) {
				if address == http.Addr().String() {
					return http, nil
				}
				return net.Listen(network, address)
			}, net.ListenPacket)
		if err == nil {
			t.Cleanup(bindings.close)
			return bindings, base
		}
	}
	t.Fatal("could not reserve Sunshine test cluster")
	return nil, 0
}

func TestGameStreamRelayRoundTrip(t *testing.T) {
	sunshine, sunshineBase := bindSunshineFixture(t)
	node := &relayTestNode{exposed: make(map[string]string)}
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	var allow atomic.Bool
	allow.Store(true)
	host, err := startHostRelay(ctx, node, sunshineBase, func(ip netip.Addr) tailcfg.StableNodeID {
		if ip == gameStreamLoopback && allow.Load() {
			return "test-peer"
		}
		return ""
	})
	if err != nil {
		t.Fatal(err)
	}
	defer host.Close()
	controller, err := startControllerRelay(ctx, node, relayTestPeer, sunshineBase)
	if err != nil {
		t.Fatal(err)
	}
	defer controller.Close()
	if controller.BasePort == sunshineBase {
		t.Fatal("controller collided with native Sunshine")
	}

	for _, offset := range []int{0, -5, 21} {
		if node.exposed["tcp4 "+gameStreamAddress(relayTestPeer, sunshineBase+offset)] == "" {
			t.Errorf("expected TCP offset %d", offset)
		}
	}
	for _, offset := range []int{9, 10, 11} {
		if node.exposed["udp4 "+gameStreamAddress(relayTestPeer, sunshineBase+offset)] == "" {
			t.Errorf("expected UDP offset %d", offset)
		}
	}
	if len(node.exposed) != 6 || node.exposed["tcp4 "+gameStreamAddress(relayTestPeer, sunshineBase+1)] != "" {
		t.Fatalf("unexpected host ports: %v", node.exposed)
	}

	for _, test := range []struct {
		name    string
		index   int
		offset  int
		payload []byte
	}{
		{"serverinfo", 0, 0, []byte("GET /serverinfo HTTP/1.1\r\n\r\n")},
		{"opaque RTSP", 2, 21, []byte{0x80, 0, 0, 0x23, 0x5b, 0x7f, 0xff}},
	} {
		t.Run(test.name, func(t *testing.T) {
			go func() {
				conn, err := sunshine.tcp[test.index].Accept()
				if err != nil {
					return
				}
				defer conn.Close()
				conn.SetDeadline(time.Now().Add(5 * time.Second))
				// Waiting for EOF also tests TCP half-close across both hops.
				data, err := io.ReadAll(conn)
				if err == nil {
					conn.Write(data)
				}
			}()
			conn, err := net.DialTimeout("tcp4", gameStreamAddress(gameStreamLoopback, controller.BasePort+test.offset), 5*time.Second)
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			conn.SetDeadline(time.Now().Add(5 * time.Second))
			if _, err := conn.Write(test.payload); err != nil {
				t.Fatal(err)
			}
			if err := conn.(*net.TCPConn).CloseWrite(); err != nil {
				t.Fatal(err)
			}
			got, err := io.ReadAll(conn)
			if err != nil || !bytes.Equal(got, test.payload) {
				t.Fatalf("TCP message %x, %v", got, err)
			}
		})
	}

	t.Run("HTTPS certificate survives both relays", func(t *testing.T) {
		server := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
			io.WriteString(w, "paired GameStream host")
		}))
		server.Listener.Close()
		server.Listener = sunshine.tcp[1]
		server.StartTLS()
		defer server.Close()
		roots := x509.NewCertPool()
		roots.AddCert(server.Certificate())
		transport := &http.Transport{TLSClientConfig: &tls.Config{RootCAs: roots}}
		defer transport.CloseIdleConnections()
		client := &http.Client{Transport: transport, Timeout: 5 * time.Second}
		response, err := client.Get("https://" + gameStreamAddress(gameStreamLoopback, controller.BasePort-5) + "/serverinfo")
		if err != nil {
			t.Fatal(err)
		}
		defer response.Body.Close()
		got, err := io.ReadAll(response.Body)
		if err != nil || string(got) != "paired GameStream host" ||
			!bytes.Equal(response.TLS.PeerCertificates[0].Raw, server.Certificate().Raw) {
			t.Fatalf("HTTPS response %q, %v", got, err)
		}
	})

	for _, offset := range []int{9, 10, 11} {
		for i := 0; i < 2; i++ {
			conn, err := net.Dial("udp4", gameStreamAddress(gameStreamLoopback, controller.BasePort+offset))
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			conn.SetDeadline(time.Now().Add(5 * time.Second))
			message := []byte{byte(offset), byte(i), 0x80, 0xff}
			if _, err := conn.Write(message); err != nil {
				t.Fatal(err)
			}
			socket := sunshine.udp[offset-9]
			socket.SetReadDeadline(time.Now().Add(5 * time.Second))
			probe := make([]byte, len(message))
			count, client, err := socket.ReadFrom(probe)
			if err != nil || !bytes.Equal(probe[:count], message) {
				t.Fatalf("UDP %d probe %x, %v", offset, probe[:count], err)
			}
			// A burst from the host must traverse the same mapping after one probe.
			for _, reply := range [][]byte{message, {0xff, 0x12, byte(offset), byte(i)}} {
				if _, err := socket.WriteTo(reply, client); err != nil {
					t.Fatal(err)
				}
				count, err = conn.Read(probe)
				if err != nil || !bytes.Equal(probe[:count], reply) {
					t.Fatalf("UDP %d reply %x, %v", offset, probe[:count], err)
				}
			}
		}
	}

	allow.Store(false)
	t.Run("reject unapproved peer before forwarding", func(t *testing.T) {
		sunshine.tcp[0].(*net.TCPListener).SetDeadline(time.Now().Add(250 * time.Millisecond))
		sunshine.udp[0].SetReadDeadline(time.Now().Add(250 * time.Millisecond))
		tcp, err := node.Dial(t.Context(), "tcp4", gameStreamAddress(relayTestPeer, sunshineBase))
		if err != nil {
			t.Fatal(err)
		}
		defer tcp.Close()
		_, _ = tcp.Write([]byte("deny this TCP source"))
		udp, err := node.Dial(t.Context(), "udp4", gameStreamAddress(relayTestPeer, sunshineBase+9))
		if err != nil {
			t.Fatal(err)
		}
		defer udp.Close()
		if _, err := udp.Write([]byte("deny this UDP source")); err != nil {
			t.Fatal(err)
		}
		if rejected, err := sunshine.tcp[0].Accept(); err == nil {
			rejected.Close()
			t.Error("unauthorized TCP reached Sunshine")
		}
		if _, _, err := sunshine.udp[0].ReadFrom(make([]byte, 64)); err == nil {
			t.Error("unauthorized UDP reached Sunshine")
		}
	})

	cancel()
	controller.Close()
	host.Close()
	listener, err := net.Listen("tcp4", gameStreamAddress(gameStreamLoopback, controller.BasePort))
	if err != nil {
		t.Fatalf("controller base still bound after cancellation: %v", err)
	}
	listener.Close()
}

func TestGameStreamRelayRestoresPartialBind(t *testing.T) {
	node := &relayTestNode{exposed: make(map[string]string), failUDP: true}
	relay, err := startHostRelay(t.Context(), node, 48989, func(netip.Addr) tailcfg.StableNodeID { return "test-peer" })
	if relay != nil || err == nil || len(node.exposed) != 3 {
		t.Fatalf("expected cleanup after the first UDP bind fails, got %v, %v", relay, err)
	}
	for _, address := range node.exposed {
		listener, err := net.Listen("tcp4", address)
		if err != nil {
			t.Fatal(err)
		}
		listener.Close()
	}
	if _, err := startControllerRelay(t.Context(), node, gameStreamLoopback, 48989); err == nil {
		t.Fatal("a non-tailnet destination must be rejected")
	}
}
