package main

import (
	"bytes"
	"encoding/binary"
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

func startWSServer(t *testing.T) (*WSHub, string) {
	t.Helper()
	hub := NewWSHub(nil)
	ts := httptest.NewServer(hub)
	t.Cleanup(ts.Close)
	return hub, "ws" + strings.TrimPrefix(ts.URL, "http")
}

func wsDial(t *testing.T, url string) *websocket.Conn {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	c, _, err := websocket.Dial(ctx, url, nil)
	if err != nil {
		t.Fatal(err)
	}
	c.SetReadLimit(wsMaxMessage + 1)
	t.Cleanup(func() { c.CloseNow() })
	return c
}

func wsSend(t *testing.T, c *websocket.Conn, msg ...byte) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	if err := c.Write(ctx, websocket.MessageBinary, msg); err != nil {
		t.Fatal(err)
	}
}

func wsRead(t *testing.T, c *websocket.Conn) []byte {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	typ, data, err := c.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if typ != websocket.MessageBinary {
		t.Fatalf("got message type %v, want binary", typ)
	}
	return data
}

func expectType(t *testing.T, msg []byte, want byte) {
	t.Helper()
	if len(msg) == 0 || msg[0] != want {
		t.Fatalf("got %x, want type 0x%02x", msg, want)
	}
}

// createAndJoin pairs two fresh connections and returns them with the code.
func createAndJoin(t *testing.T, url string) (host, client *websocket.Conn, code string) {
	t.Helper()
	host = wsDial(t, url)
	wsSend(t, host, WSCreateRoom)
	created := wsRead(t, host)
	expectType(t, created, WSRoomCreated)
	if len(created) != 1+RoomCodeLen {
		t.Fatalf("RoomCreated length %d", len(created))
	}
	code = string(created[1:])

	client = wsDial(t, url)
	wsSend(t, client, append([]byte{WSJoinRoom}, code...)...)
	joined := wsRead(t, client)
	expectType(t, joined, WSJoined)
	if string(joined[1:]) != code {
		t.Fatalf("joined %q, want %q", joined[1:], code)
	}
	expectType(t, wsRead(t, host), WSPeerJoined)
	return host, client, code
}

func TestWSRelayBothWays(t *testing.T) {
	_, url := startWSServer(t)
	host, client, code := createAndJoin(t, url)

	for _, ch := range code {
		if !strings.ContainsRune("ABCDEFGHJKLMNPQRSTUVWXYZ23456789", ch) {
			t.Fatalf("code %q has unexpected character %q", code, ch)
		}
	}

	wsSend(t, host, WSData, 1, 2, 3)
	if got := wsRead(t, client); !bytes.Equal(got, []byte{WSData, 1, 2, 3}) {
		t.Fatalf("client got %x", got)
	}
	wsSend(t, client, WSData, 9, 8)
	if got := wsRead(t, host); !bytes.Equal(got, []byte{WSData, 9, 8}) {
		t.Fatalf("host got %x", got)
	}
}

func TestWSRelayKeepsOrder(t *testing.T) {
	_, url := startWSServer(t)
	host, client, _ := createAndJoin(t, url)
	for i := 0; i < 200; i++ {
		wsSend(t, host, WSData, byte(i))
	}
	for i := 0; i < 200; i++ {
		if got := wsRead(t, client); !bytes.Equal(got, []byte{WSData, byte(i)}) {
			t.Fatalf("message %d: got %x", i, got)
		}
	}
}

func TestWSRelayLargeMessage(t *testing.T) {
	_, url := startWSServer(t)
	host, client, _ := createAndJoin(t, url)
	payload := bytes.Repeat([]byte{0xAB}, 300_000) // a map transfer is 100 KB+
	wsSend(t, host, append([]byte{WSData}, payload...)...)
	got := wsRead(t, client)
	if len(got) != 1+len(payload) || !bytes.Equal(got[1:], payload) {
		t.Fatalf("large message mangled: %d bytes", len(got))
	}
}

func TestWSPingPong(t *testing.T) {
	_, url := startWSServer(t)
	c := wsDial(t, url)
	wsSend(t, c, WSPing, 0xDE, 0xAD)
	if got := wsRead(t, c); !bytes.Equal(got, []byte{WSPong, 0xDE, 0xAD}) {
		t.Fatalf("got %x", got)
	}
}

func TestWSJoinUnknownRoom(t *testing.T) {
	_, url := startWSServer(t)
	c := wsDial(t, url)
	wsSend(t, c, append([]byte{WSJoinRoom}, "ZZZZZZ"...)...)
	msg := wsRead(t, c)
	expectType(t, msg, WSError)
	if msg[1] != ErrRoomNotFound {
		t.Fatalf("error code %d", msg[1])
	}
}

func TestWSRoomFull(t *testing.T) {
	_, url := startWSServer(t)
	_, _, code := createAndJoin(t, url)
	third := wsDial(t, url)
	wsSend(t, third, append([]byte{WSJoinRoom}, code...)...)
	msg := wsRead(t, third)
	expectType(t, msg, WSError)
	if msg[1] != ErrRoomFull {
		t.Fatalf("error code %d", msg[1])
	}
}

func TestWSDataWithoutPeer(t *testing.T) {
	_, url := startWSServer(t)
	host := wsDial(t, url)
	wsSend(t, host, WSCreateRoom)
	expectType(t, wsRead(t, host), WSRoomCreated)
	wsSend(t, host, WSData, 1)
	expectType(t, wsRead(t, host), WSError)
}

func TestWSPeerLeftClosesRoom(t *testing.T) {
	hub, url := startWSServer(t)
	host, client, code := createAndJoin(t, url)

	client.Close(websocket.StatusNormalClosure, "")
	expectType(t, wsRead(t, host), WSPeerLeft)

	deadline := time.Now().Add(2 * time.Second)
	for hub.RoomCount() != 0 && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if n := hub.RoomCount(); n != 0 {
		t.Fatalf("%d rooms left open", n)
	}

	// The code is gone, and the host may open a new room on the same connection.
	late := wsDial(t, url)
	wsSend(t, late, append([]byte{WSJoinRoom}, code...)...)
	expectType(t, wsRead(t, late), WSError)
	wsSend(t, host, WSCreateRoom)
	expectType(t, wsRead(t, host), WSRoomCreated)
}

func TestWSHostLeavingNotifiesClient(t *testing.T) {
	_, url := startWSServer(t)
	host, client, _ := createAndJoin(t, url)
	host.CloseNow()
	expectType(t, wsRead(t, client), WSPeerLeft)
}

func TestWSRejectsForeignOrigin(t *testing.T) {
	_, url := startWSServer(t)
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	hdr := http.Header{}
	hdr.Set("Origin", "https://evil.example")
	_, resp, err := websocket.Dial(ctx, url, &websocket.DialOptions{HTTPHeader: hdr})
	if err == nil {
		t.Fatal("dial from a foreign origin succeeded")
	}
	if resp == nil || resp.StatusCode != http.StatusForbidden {
		t.Fatalf("want 403, got %v", resp)
	}
}

func TestWSRejectsTextMessages(t *testing.T) {
	_, url := startWSServer(t)
	c := wsDial(t, url)
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	if err := c.Write(ctx, websocket.MessageText, []byte("hello")); err != nil {
		t.Fatal(err)
	}
	expectType(t, wsRead(t, c), WSError)
}

func testToken(b byte) []byte { return bytes.Repeat([]byte{b}, WSTokenLen) }

// createAndJoinResumable pairs two connections that both sent resume tokens.
func createAndJoinResumable(t *testing.T, url string) (host, client *websocket.Conn, code string) {
	t.Helper()
	host = wsDial(t, url)
	wsSend(t, host, append([]byte{WSCreateRoom}, testToken(1)...)...)
	created := wsRead(t, host)
	expectType(t, created, WSRoomCreated)
	code = string(created[1:])

	client = wsDial(t, url)
	wsSend(t, client, append(append([]byte{WSJoinRoom}, code...), testToken(2)...)...)
	expectType(t, wsRead(t, client), WSJoined)
	expectType(t, wsRead(t, host), WSPeerJoined)
	return host, client, code
}

func resumeMsg(code string, token []byte, got uint32) []byte {
	msg := append([]byte{WSResume}, code...)
	msg = append(msg, token...)
	return append(msg, withCount(0, got)[1:]...)
}

func expectCount(t *testing.T, msg []byte, typ byte, want uint32) {
	t.Helper()
	expectType(t, msg, typ)
	if len(msg) != 5 || binary.BigEndian.Uint32(msg[1:]) != want {
		t.Fatalf("got %x, want type 0x%02x with count %d", msg, typ, want)
	}
}

func TestWSResumeAfterDroppedConnection(t *testing.T) {
	hub, url := startWSServer(t)
	host, client, code := createAndJoinResumable(t, url)

	wsSend(t, host, WSData, 1)
	expectType(t, wsRead(t, client), WSData)
	wsSend(t, client, WSData, 10)
	expectType(t, wsRead(t, host), WSData) // the host has 1 message

	// The host's phone drops the socket; the room stays.
	host.CloseNow()
	expectType(t, wsRead(t, client), WSPeerAway)
	if n := hub.RoomCount(); n != 1 {
		t.Fatalf("%d rooms open, want 1", n)
	}
	wsSend(t, client, WSData, 11)
	wsSend(t, client, WSData, 12)

	back := wsDial(t, url)
	wsSend(t, back, resumeMsg(code, testToken(1), 1)...)
	expectCount(t, wsRead(t, back), WSResumed, 1) // the server got 1 from the host
	expectType(t, wsRead(t, back), WSPeerJoined)
	for _, want := range []byte{11, 12} {
		if got := wsRead(t, back); !bytes.Equal(got, []byte{WSData, want}) {
			t.Fatalf("resumed host got %x, want data %d", got, want)
		}
	}
	expectType(t, wsRead(t, client), WSPeerBack)

	wsSend(t, back, WSData, 2)
	if got := wsRead(t, client); !bytes.Equal(got, []byte{WSData, 2}) {
		t.Fatalf("client got %x", got)
	}
}

func TestWSResumeResendsUnacknowledged(t *testing.T) {
	_, url := startWSServer(t)
	host, client, code := createAndJoinResumable(t, url)
	for i := byte(0); i < 5; i++ {
		wsSend(t, host, WSData, i)
	}
	for i := 0; i < 5; i++ {
		wsRead(t, client)
	}
	wsSend(t, client, withCount(WSAck, 2)...)
	expectCount(t, wsRead(t, client), WSAckReply, 0)

	// The last three were "lost" on the dying connection.
	client.CloseNow()
	back := wsDial(t, url)
	wsSend(t, back, resumeMsg(code, testToken(2), 2)...)
	expectCount(t, wsRead(t, back), WSResumed, 0)
	expectType(t, wsRead(t, back), WSJoined)
	for _, want := range []byte{2, 3, 4} {
		if got := wsRead(t, back); !bytes.Equal(got, []byte{WSData, want}) {
			t.Fatalf("got %x, want data %d", got, want)
		}
	}

	// Asking for what was acknowledged (and dropped) cannot work.
	back.CloseNow()
	again := wsDial(t, url)
	wsSend(t, again, resumeMsg(code, testToken(2), 0)...)
	expectType(t, wsRead(t, again), WSError)
}

func TestWSResumeNeedsTheToken(t *testing.T) {
	_, url := startWSServer(t)
	host, client, code := createAndJoinResumable(t, url)
	host.CloseNow()
	expectType(t, wsRead(t, client), WSPeerAway)

	thief := wsDial(t, url)
	wsSend(t, thief, resumeMsg(code, testToken(9), 0)...)
	msg := wsRead(t, thief)
	expectType(t, msg, WSError)
	if msg[1] != ErrRoomNotFound {
		t.Fatalf("error code %d", msg[1])
	}
}

func TestWSLeaveClosesResumableRoom(t *testing.T) {
	hub, url := startWSServer(t)
	host, client, _ := createAndJoinResumable(t, url)
	wsSend(t, client, WSLeave)
	expectType(t, wsRead(t, host), WSPeerLeft)
	deadline := time.Now().Add(2 * time.Second)
	for hub.RoomCount() != 0 && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if n := hub.RoomCount(); n != 0 {
		t.Fatalf("%d rooms left open", n)
	}
}

func TestWSAwayPlayerRunsOutOfTime(t *testing.T) {
	hub, url := startWSServer(t)
	hub.resumeGrace = 50 * time.Millisecond
	host, client, code := createAndJoinResumable(t, url)
	client.CloseNow()
	expectType(t, wsRead(t, host), WSPeerAway)
	expectType(t, wsRead(t, host), WSPeerLeft)

	late := wsDial(t, url)
	wsSend(t, late, resumeMsg(code, testToken(2), 0)...)
	expectType(t, wsRead(t, late), WSError)
}

func TestWSHostAwayBeforeAnyoneJoins(t *testing.T) {
	_, url := startWSServer(t)
	host := wsDial(t, url)
	wsSend(t, host, append([]byte{WSCreateRoom}, testToken(1)...)...)
	created := wsRead(t, host)
	expectType(t, created, WSRoomCreated)
	code := string(created[1:])

	// The host switches to a messenger to send the code; the socket dies.
	host.CloseNow()
	time.Sleep(50 * time.Millisecond)

	client := wsDial(t, url)
	wsSend(t, client, append([]byte{WSJoinRoom}, code...)...)
	expectType(t, wsRead(t, client), WSJoined)
	expectType(t, wsRead(t, client), WSPeerAway)
	wsSend(t, client, WSData, 7)

	back := wsDial(t, url)
	wsSend(t, back, resumeMsg(code, testToken(1), 0)...)
	expectCount(t, wsRead(t, back), WSResumed, 0)
	expectType(t, wsRead(t, back), WSPeerJoined)
	if got := wsRead(t, back); !bytes.Equal(got, []byte{WSData, 7}) {
		t.Fatalf("host got %x", got)
	}
	expectType(t, wsRead(t, client), WSPeerBack)
}
