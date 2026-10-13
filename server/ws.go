package main

// WebSocket relay for the web build. Browsers cannot send UDP, so two browser
// players cannot reach each other (or the UDP signaling server) directly.
// Instead both open a WebSocket to this server, meet in a room by code, and
// the server forwards their game messages to each other.
//
// Every WebSocket message is binary with a 1-byte type prefix.
//
// Client → Server:
//   WSCreateRoom  [0x01]
//   WSJoinRoom    [0x02] + [6: room code ASCII]
//   WSData        [0x10] + [N: payload]          forwarded to the peer
//   WSPing        [0x20] + [N: any]              answered with WSPong + same bytes
//
// Server → Client:
//   WSRoomCreated [0x81] + [6: room code]        to the host
//   WSJoined      [0x82] + [6: room code]        to the joiner
//   WSPeerJoined  [0x83]                         to the host
//   WSPeerLeft    [0x84]                         the other side is gone; room closed
//   WSData        [0x10] + [N: payload]          from the peer, payload unchanged
//   WSPong        [0x21] + [N: echoed bytes]
//   WSError       [0x8F] + [1: error code] + [N: message]   (codes as in protocol.go)
//
// A room lives as long as its host's connection (an unjoined room is closed
// after wsRoomWaitTimeout). When either side disconnects, the other gets
// WSPeerLeft and the room is gone; a new match needs a new room. The server
// pings every connection, which also keeps proxies such as Cloudflare
// (100 s idle limit) from dropping a quiet one.

import (
	"context"
	"errors"
	"log"
	"net/http"
	"sync"
	"time"

	"github.com/coder/websocket"
)

const (
	WSCreateRoom  byte = 0x01
	WSJoinRoom    byte = 0x02
	WSData        byte = 0x10
	WSPing        byte = 0x20
	WSPong        byte = 0x21
	WSRoomCreated byte = 0x81
	WSJoined      byte = 0x82
	WSPeerJoined  byte = 0x83
	WSPeerLeft    byte = 0x84
	WSError       byte = 0x8F
)

const (
	// Map and TC archives travel as single messages, so the limit is generous.
	wsMaxMessage       = 8 << 20
	wsSendQueue        = 256
	wsWriteTimeout     = 10 * time.Second
	wsPingPeriod       = 20 * time.Second
	wsRoomWaitTimeout  = 30 * time.Minute
	wsMaxRoomsDefault  = maxRooms
	wsPeerLeftReadTime = 2 * time.Second
)

type wsPeer struct {
	conn *websocket.Conn
	send chan []byte
	// Guarded by WSHub.mu.
	room *wsRoom
	// Closed when the peer's connection is torn down; stops its writer.
	done      chan struct{}
	closeOnce sync.Once
}

type wsRoom struct {
	code    string
	host    *wsPeer
	client  *wsPeer
	created time.Time
}

// WSHub matches browser players into rooms and relays between them.
type WSHub struct {
	mu       sync.Mutex
	rooms    map[string]*wsRoom
	maxRooms int
	// OriginPatterns are extra allowed Origin hosts (e.g. "localhost:8000").
	// Same-host origins are always allowed.
	originPatterns []string
}

func NewWSHub(originPatterns []string) *WSHub {
	return &WSHub{
		rooms:          make(map[string]*wsRoom),
		maxRooms:       wsMaxRoomsDefault,
		originPatterns: originPatterns,
	}
}

// RoomCount reports open rooms (for tests and logs).
func (h *WSHub) RoomCount() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.rooms)
}

func (h *WSHub) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{OriginPatterns: h.originPatterns})
	if err != nil {
		// Accept already wrote the HTTP error (e.g. 403 for a foreign origin).
		return
	}
	conn.SetReadLimit(wsMaxMessage)

	p := &wsPeer{conn: conn, send: make(chan []byte, wsSendQueue), done: make(chan struct{})}
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()

	go p.writeLoop(ctx)
	go p.pingLoop(ctx)

	h.readLoop(ctx, p)
	h.leave(p)
	p.close(websocket.StatusNormalClosure, "")
}

func (h *WSHub) readLoop(ctx context.Context, p *wsPeer) {
	for {
		typ, data, err := p.conn.Read(ctx)
		if err != nil {
			return
		}
		if typ != websocket.MessageBinary || len(data) < 1 {
			p.sendError(ErrInvalidMsg, "binary messages only")
			continue
		}
		switch data[0] {
		case WSCreateRoom:
			h.create(p)
		case WSJoinRoom:
			if len(data) < 1+RoomCodeLen {
				p.sendError(ErrInvalidMsg, "too short")
				continue
			}
			h.join(p, string(data[1:1+RoomCodeLen]))
		case WSData:
			h.relay(p, data)
		case WSPing:
			pong := make([]byte, len(data))
			copy(pong, data)
			pong[0] = WSPong
			p.enqueue(pong)
		default:
			p.sendError(ErrInvalidMsg, "unknown type")
		}
	}
}

func (h *WSHub) create(p *wsPeer) {
	h.mu.Lock()
	if p.room != nil {
		h.mu.Unlock()
		p.sendError(ErrInvalidMsg, "already in a room")
		return
	}
	code := ""
	if len(h.rooms) < h.maxRooms {
		code = newRoomCode(func(c string) bool { _, taken := h.rooms[c]; return taken })
	}
	if code == "" {
		h.mu.Unlock()
		p.sendError(ErrRoomFull, "server full")
		return
	}
	room := &wsRoom{code: code, host: p, created: time.Now()}
	h.rooms[code] = room
	p.room = room
	h.mu.Unlock()

	log.Printf("ws: room %s created", code)
	p.enqueue(append([]byte{WSRoomCreated}, code...))
	go h.expireIfUnjoined(room)
}

func (h *WSHub) join(p *wsPeer, code string) {
	h.mu.Lock()
	if p.room != nil {
		h.mu.Unlock()
		p.sendError(ErrInvalidMsg, "already in a room")
		return
	}
	room, ok := h.rooms[code]
	if !ok {
		h.mu.Unlock()
		p.sendError(ErrRoomNotFound, "room not found")
		return
	}
	if room.client != nil {
		h.mu.Unlock()
		p.sendError(ErrRoomFull, "room full")
		return
	}
	room.client = p
	p.room = room
	host := room.host
	h.mu.Unlock()

	log.Printf("ws: room %s joined", code)
	p.enqueue(append([]byte{WSJoined}, code...))
	host.enqueue([]byte{WSPeerJoined})
}

func (h *WSHub) relay(p *wsPeer, data []byte) {
	h.mu.Lock()
	var other *wsPeer
	if room := p.room; room != nil {
		if room.host == p {
			other = room.client
		} else {
			other = room.host
		}
	}
	h.mu.Unlock()

	if other == nil {
		p.sendError(ErrInvalidMsg, "no peer")
		return
	}
	// Read returns a fresh slice per message, so it can be handed over as is.
	other.enqueue(data)
}

// leave removes p from its room; the room closes and the other side is told.
func (h *WSHub) leave(p *wsPeer) {
	h.mu.Lock()
	room := p.room
	if room == nil {
		h.mu.Unlock()
		return
	}
	delete(h.rooms, room.code)
	var other *wsPeer
	if room.host == p {
		other = room.client
	} else {
		other = room.host
	}
	room.host = nil
	room.client = nil
	p.room = nil
	if other != nil {
		other.room = nil
	}
	h.mu.Unlock()

	log.Printf("ws: room %s closed", room.code)
	if other != nil {
		other.enqueue([]byte{WSPeerLeft})
	}
}

func (h *WSHub) expireIfUnjoined(room *wsRoom) {
	time.Sleep(wsRoomWaitTimeout)
	h.mu.Lock()
	expired := h.rooms[room.code] == room && room.client == nil
	host := room.host
	h.mu.Unlock()
	if expired && host != nil {
		host.sendError(ErrRoomNotFound, "room expired")
		h.leave(host)
	}
}

// enqueue queues a message for the writer. A peer that cannot keep up (its
// queue is full) is disconnected rather than stalling the relay.
func (p *wsPeer) enqueue(msg []byte) {
	select {
	case p.send <- msg:
	case <-p.done:
	default:
		go p.close(websocket.StatusPolicyViolation, "too slow")
	}
}

func (p *wsPeer) sendError(code byte, message string) {
	msg := make([]byte, 0, 2+len(message))
	msg = append(msg, WSError, code)
	p.enqueue(append(msg, message...))
}

func (p *wsPeer) writeLoop(ctx context.Context) {
	for {
		select {
		case msg := <-p.send:
			wctx, cancel := context.WithTimeout(ctx, wsWriteTimeout)
			err := p.conn.Write(wctx, websocket.MessageBinary, msg)
			cancel()
			if err != nil {
				p.close(websocket.StatusGoingAway, "write failed")
				return
			}
		case <-ctx.Done():
			return
		case <-p.done:
			return
		}
	}
}

func (p *wsPeer) pingLoop(ctx context.Context) {
	ticker := time.NewTicker(wsPingPeriod)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			pctx, cancel := context.WithTimeout(ctx, wsPingPeriod)
			err := p.conn.Ping(pctx)
			cancel()
			if err != nil && !errors.Is(err, context.Canceled) {
				p.close(websocket.StatusGoingAway, "ping timeout")
				return
			}
		case <-ctx.Done():
			return
		case <-p.done:
			return
		}
	}
}

func (p *wsPeer) close(code websocket.StatusCode, reason string) {
	p.closeOnce.Do(func() {
		close(p.done)
		// Close waits for the close handshake; give the peer a moment, then drop.
		ctx, cancel := context.WithTimeout(context.Background(), wsPeerLeftReadTime)
		defer cancel()
		go func() {
			<-ctx.Done()
			p.conn.CloseNow()
		}()
		p.conn.Close(code, reason)
	})
}
