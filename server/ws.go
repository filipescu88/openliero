package main

// WebSocket relay for the web build. Browsers cannot send UDP, so two browser
// players cannot reach each other (or the UDP signaling server) directly.
// Instead both open a WebSocket to this server, meet in a room by code, and
// the server forwards their game messages to each other.
//
// Every WebSocket message is binary with a 1-byte type prefix.
//
// Client → Server:
//   WSCreateRoom  [0x01] + optional [16: resume token]
//   WSJoinRoom    [0x02] + [6: room code ASCII] + optional [16: resume token]
//   WSResume      [0x03] + [6: room code] + [16: resume token] + [4: data received, BE]
//   WSLeave       [0x04]                         leaving for good; the room closes
//   WSAck         [0x05] + [4: data received, BE]   answered with WSAckReply
//   WSData        [0x10] + [N: payload]          forwarded to the peer
//   WSPing        [0x20] + [N: any]              answered with WSPong + same bytes
//
// Server → Client:
//   WSRoomCreated [0x81] + [6: room code]        to the host
//   WSJoined      [0x82] + [6: room code]        to the joiner
//   WSPeerJoined  [0x83]                         to the host
//   WSPeerLeft    [0x84]                         the other side is gone; room closed
//   WSResumed     [0x85] + [4: data received from you, BE]
//   WSPeerAway    [0x86]                         the other side lost its connection
//   WSPeerBack    [0x87]                         ...and resumed it
//   WSAckReply    [0x88] + [4: data received from you, BE]
//   WSData        [0x10] + [N: payload]          from the peer, payload unchanged
//   WSPong        [0x21] + [N: echoed bytes]
//   WSError       [0x8F] + [1: error code] + [N: message]   (codes as in protocol.go)
//
// A room lives as long as its players (an unjoined room is closed after
// wsRoomWaitTimeout). A player who sent no resume token leaves the room when
// its connection ends, and the other side gets WSPeerLeft at once.
//
// A player who did send one may lose the connection without leaving: phones
// drop a page's sockets when the browser goes to the background (for example
// to paste the room code into a messenger). The room then waits
// (resumeGrace; longer while nobody has joined yet), the other side gets
// WSPeerAway, and the peer's data is kept for the absent player. A new
// connection takes the seat back with WSResume: it says how many data
// messages it got, the server answers WSResumed with how many it got from
// that player, and each side re-sends what the other is missing. Data
// messages are counted per direction from 0; WSAck lets the server forget
// what a player already has. Only WSLeave (or the grace running out) ends
// such a seat.
//
// The server pings every connection, which also keeps proxies such as
// Cloudflare (100 s idle limit) from dropping a quiet one.

import (
	"context"
	"crypto/subtle"
	"encoding/binary"
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
	WSResume      byte = 0x03
	WSLeave       byte = 0x04
	WSAck         byte = 0x05
	WSData        byte = 0x10
	WSPing        byte = 0x20
	WSPong        byte = 0x21
	WSRoomCreated byte = 0x81
	WSJoined      byte = 0x82
	WSPeerJoined  byte = 0x83
	WSPeerLeft    byte = 0x84
	WSResumed     byte = 0x85
	WSPeerAway    byte = 0x86
	WSPeerBack    byte = 0x87
	WSAckReply    byte = 0x88
	WSError       byte = 0x8F

	WSTokenLen = 16
)

const (
	// Map and TC archives travel as single messages, so the limit is generous.
	wsMaxMessage = 8 << 20
	// What may wait for one slow connection, and what is kept for a player
	// who may resume (a resume re-sends it all at once).
	wsSendQueueBytes   = 3 * wsMaxMessage
	wsHistoryBytes     = 2 * wsMaxMessage
	wsWriteTimeout     = 10 * time.Second
	wsPingPeriod       = 20 * time.Second
	wsRoomWaitTimeout  = 30 * time.Minute
	wsResumeGrace      = 5 * time.Minute
	wsResumeGraceAlone = 15 * time.Minute // nobody joined yet: the host is sharing the code
	wsMaxRoomsDefault  = maxRooms
	wsPeerLeftReadTime = 2 * time.Second
)

type wsPeer struct {
	conn *websocket.Conn
	// Guarded by WSHub.mu.
	room *wsRoom
	seat int

	qmu    sync.Mutex
	queue  [][]byte
	queued int           // bytes in queue
	wake   chan struct{} // capacity 1: the writer has work
	// Closed when the peer's connection is torn down; stops its writer.
	done      chan struct{}
	closeOnce sync.Once
}

// wsSeat is one player's place in a room. It outlives the player's
// connection when the player can resume (has a token). Guarded by WSHub.mu.
type wsSeat struct {
	peer  *wsPeer // nil while away
	token []byte  // empty: cannot resume
	// Data messages for this player, numbered from 0: sent counts all of
	// them, hist keeps those from histStart on that the player has not
	// acknowledged yet.
	sent      uint32
	hist      [][]byte
	histStart uint32
	histBytes int
	recv      uint32 // data messages received from this player
	awayTimer *time.Timer
}

type wsRoom struct {
	code    string
	seats   [2]*wsSeat // host, joiner (nil until someone joins)
	created time.Time
}

// WSHub matches browser players into rooms and relays between them.
type WSHub struct {
	mu       sync.Mutex
	rooms    map[string]*wsRoom
	maxRooms int
	// How long a room waits for a player who lost the connection.
	resumeGrace      time.Duration
	resumeGraceAlone time.Duration
	// OriginPatterns are extra allowed Origin hosts (e.g. "localhost:8000").
	// Same-host origins are always allowed.
	originPatterns []string
}

func NewWSHub(originPatterns []string) *WSHub {
	return &WSHub{
		rooms:            make(map[string]*wsRoom),
		maxRooms:         wsMaxRoomsDefault,
		resumeGrace:      wsResumeGrace,
		resumeGraceAlone: wsResumeGraceAlone,
		originPatterns:   originPatterns,
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

	p := &wsPeer{conn: conn, wake: make(chan struct{}, 1), done: make(chan struct{})}
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()

	go p.writeLoop(ctx)
	go p.pingLoop(ctx)

	leaving := h.readLoop(ctx, p)
	h.disconnect(p, leaving)
	p.close(websocket.StatusNormalClosure, "")
}

// readLoop handles p's messages until the connection ends; true when the
// player said it is leaving (WSLeave).
func (h *WSHub) readLoop(ctx context.Context, p *wsPeer) bool {
	for {
		typ, data, err := p.conn.Read(ctx)
		if err != nil {
			return false
		}
		if typ != websocket.MessageBinary || len(data) < 1 {
			p.sendError(ErrInvalidMsg, "binary messages only")
			continue
		}
		switch data[0] {
		case WSCreateRoom:
			h.create(p, tokenAt(data, 1))
		case WSJoinRoom:
			if len(data) < 1+RoomCodeLen {
				p.sendError(ErrInvalidMsg, "too short")
				continue
			}
			h.join(p, string(data[1:1+RoomCodeLen]), tokenAt(data, 1+RoomCodeLen))
		case WSResume:
			if len(data) != 1+RoomCodeLen+WSTokenLen+4 {
				p.sendError(ErrInvalidMsg, "bad resume")
				continue
			}
			h.resume(p, string(data[1:1+RoomCodeLen]), data[1+RoomCodeLen:1+RoomCodeLen+WSTokenLen],
				binary.BigEndian.Uint32(data[1+RoomCodeLen+WSTokenLen:]))
		case WSLeave:
			return true
		case WSAck:
			if len(data) != 5 {
				p.sendError(ErrInvalidMsg, "bad ack")
				continue
			}
			h.ack(p, binary.BigEndian.Uint32(data[1:]))
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

// tokenAt returns the resume token at data[off:], or nil when there is none.
func tokenAt(data []byte, off int) []byte {
	if len(data) < off+WSTokenLen {
		return nil
	}
	return append([]byte(nil), data[off:off+WSTokenLen]...)
}

func withCount(typ byte, n uint32) []byte {
	msg := []byte{typ, 0, 0, 0, 0}
	binary.BigEndian.PutUint32(msg[1:], n)
	return msg
}

func (h *WSHub) create(p *wsPeer, token []byte) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if p.room != nil {
		p.sendError(ErrInvalidMsg, "already in a room")
		return
	}
	code := ""
	if len(h.rooms) < h.maxRooms {
		code = newRoomCode(func(c string) bool { _, taken := h.rooms[c]; return taken })
	}
	if code == "" {
		p.sendError(ErrRoomFull, "server full")
		return
	}
	room := &wsRoom{code: code, created: time.Now()}
	room.seats[0] = &wsSeat{peer: p, token: token}
	h.rooms[code] = room
	p.room = room
	p.seat = 0

	log.Printf("ws: room %s created", code)
	p.enqueue(append([]byte{WSRoomCreated}, code...))
	go h.expireIfUnjoined(room)
}

func (h *WSHub) join(p *wsPeer, code string, token []byte) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if p.room != nil {
		p.sendError(ErrInvalidMsg, "already in a room")
		return
	}
	room, ok := h.rooms[code]
	if !ok {
		p.sendError(ErrRoomNotFound, "room not found")
		return
	}
	if room.seats[1] != nil {
		p.sendError(ErrRoomFull, "room full")
		return
	}
	room.seats[1] = &wsSeat{peer: p, token: token}
	p.room = room
	p.seat = 1

	log.Printf("ws: room %s joined", code)
	p.enqueue(append([]byte{WSJoined}, code...))
	// An away host hears about the joiner when it resumes.
	if host := room.seats[0]; host.peer != nil {
		host.peer.enqueue([]byte{WSPeerJoined})
	} else {
		p.enqueue([]byte{WSPeerAway})
	}
}

// resume gives a seat back to a player who lost its connection.
// `got` is how many data messages the player has received in this room.
func (h *WSHub) resume(p *wsPeer, code string, token []byte, got uint32) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if p.room != nil {
		p.sendError(ErrInvalidMsg, "already in a room")
		return
	}
	room, ok := h.rooms[code]
	idx := -1
	if ok {
		for i, s := range room.seats {
			if s != nil && len(s.token) == WSTokenLen && subtle.ConstantTimeCompare(s.token, token) == 1 {
				idx = i
			}
		}
	}
	if idx < 0 {
		p.sendError(ErrRoomNotFound, "room closed")
		return
	}
	s := room.seats[idx]
	if got < s.histStart || got > s.sent {
		// What the player is missing was already dropped: no way to go on.
		log.Printf("ws: room %s cannot resume (has %d, kept from %d)", code, got, s.histStart)
		p.sendError(ErrRoomNotFound, "cannot resume")
		h.closeRoomLocked(room)
		return
	}
	if old := s.peer; old != nil {
		// The old connection is still around (the server has not noticed it
		// died yet); it no longer speaks for this seat.
		old.room = nil
		go old.close(websocket.StatusGoingAway, "resumed elsewhere")
	}
	if s.awayTimer != nil {
		s.awayTimer.Stop()
		s.awayTimer = nil
	}
	s.peer = p
	p.room = room
	p.seat = idx
	s.forget(got)

	log.Printf("ws: room %s seat %d resumed", code, idx)
	p.enqueue(withCount(WSResumed, s.recv))
	if idx == 1 {
		p.enqueue(append([]byte{WSJoined}, code...))
	}
	if o := room.seats[1-idx]; o != nil {
		if idx == 0 {
			p.enqueue([]byte{WSPeerJoined})
		}
		if o.peer == nil {
			p.enqueue([]byte{WSPeerAway})
		} else {
			o.peer.enqueue([]byte{WSPeerBack})
		}
	}
	for _, msg := range s.hist {
		p.enqueue(msg)
	}
}

func (h *WSHub) ack(p *wsPeer, got uint32) {
	h.mu.Lock()
	defer h.mu.Unlock()
	var recv uint32
	if p.room != nil {
		s := p.room.seats[p.seat]
		s.forget(got)
		recv = s.recv
	}
	p.enqueue(withCount(WSAckReply, recv))
}

func (h *WSHub) relay(p *wsPeer, data []byte) {
	h.mu.Lock()
	defer h.mu.Unlock()
	room := p.room
	if room == nil || room.seats[1-p.seat] == nil {
		p.sendError(ErrInvalidMsg, "no peer")
		return
	}
	room.seats[p.seat].recv++
	// Read returns a fresh slice per message, so it can be handed over as is.
	room.seats[1-p.seat].deliver(data)
}

// deliver sends a data message to the seat's player, keeping it for a resume.
func (s *wsSeat) deliver(msg []byte) {
	s.sent++
	if len(s.token) > 0 {
		s.hist = append(s.hist, msg)
		s.histBytes += len(msg)
		for s.histBytes > wsHistoryBytes && len(s.hist) > 1 {
			s.dropOldest()
		}
	}
	if s.peer != nil {
		s.peer.enqueue(msg)
	}
}

// forget drops the kept messages the player says it already has.
func (s *wsSeat) forget(got uint32) {
	for s.histStart < got && len(s.hist) > 0 {
		s.dropOldest()
	}
}

func (s *wsSeat) dropOldest() {
	s.histBytes -= len(s.hist[0])
	s.hist[0] = nil
	s.hist = s.hist[1:]
	s.histStart++
}

// disconnect handles p's connection ending. A player who can resume keeps
// its seat for a while; otherwise (or when leaving on purpose) the room
// closes and the other side is told.
func (h *WSHub) disconnect(p *wsPeer, leaving bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	room := p.room
	if room == nil {
		return
	}
	p.room = nil
	s := room.seats[p.seat]
	if s.peer != p {
		return
	}
	s.peer = nil
	if leaving || len(s.token) == 0 {
		h.closeRoomLocked(room)
		return
	}

	grace := h.resumeGrace
	if room.seats[1] == nil {
		grace = h.resumeGraceAlone
	}
	log.Printf("ws: room %s seat %d away", room.code, p.seat)
	s.awayTimer = time.AfterFunc(grace, func() { h.expireAway(room, s) })
	if o := room.seats[1-p.seat]; o != nil && o.peer != nil {
		o.peer.enqueue([]byte{WSPeerAway})
	}
}

func (h *WSHub) expireAway(room *wsRoom, s *wsSeat) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.rooms[room.code] == room && s.peer == nil {
		log.Printf("ws: room %s: player did not come back", room.code)
		h.closeRoomLocked(room)
	}
}

// closeRoomLocked removes the room; players still connected get WSPeerLeft.
func (h *WSHub) closeRoomLocked(room *wsRoom) {
	if h.rooms[room.code] == room {
		delete(h.rooms, room.code)
		log.Printf("ws: room %s closed", room.code)
	}
	for i, s := range room.seats {
		if s == nil {
			continue
		}
		if s.awayTimer != nil {
			s.awayTimer.Stop()
			s.awayTimer = nil
		}
		if s.peer != nil {
			s.peer.room = nil
			s.peer.enqueue([]byte{WSPeerLeft})
			s.peer = nil
		}
		s.hist = nil
		room.seats[i] = nil
	}
}

func (h *WSHub) expireIfUnjoined(room *wsRoom) {
	time.Sleep(wsRoomWaitTimeout)
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.rooms[room.code] != room || room.seats[1] != nil {
		return
	}
	if host := room.seats[0].peer; host != nil {
		host.room = nil
		room.seats[0].peer = nil
		host.sendError(ErrRoomNotFound, "room expired")
	}
	h.closeRoomLocked(room)
}

// enqueue queues a message for the writer. A peer that cannot keep up (too
// much is waiting) is disconnected rather than stalling the relay.
func (p *wsPeer) enqueue(msg []byte) {
	p.qmu.Lock()
	if p.queued+len(msg) > wsSendQueueBytes {
		p.qmu.Unlock()
		go p.close(websocket.StatusPolicyViolation, "too slow")
		return
	}
	p.queue = append(p.queue, msg)
	p.queued += len(msg)
	p.qmu.Unlock()
	select {
	case p.wake <- struct{}{}:
	default:
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
		case <-p.wake:
		case <-ctx.Done():
			return
		case <-p.done:
			return
		}
		p.qmu.Lock()
		batch := p.queue
		p.queue = nil
		p.queued = 0
		p.qmu.Unlock()
		for _, msg := range batch {
			wctx, cancel := context.WithTimeout(ctx, wsWriteTimeout)
			err := p.conn.Write(wctx, websocket.MessageBinary, msg)
			cancel()
			if err != nil {
				p.close(websocket.StatusGoingAway, "write failed")
				return
			}
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
