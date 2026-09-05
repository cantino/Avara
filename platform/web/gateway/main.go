// avara-gw relays Avara datagrams between browser clients.
//
// Browsers cannot open UDP sockets, so the web build tunnels its datagrams to
// this gateway over a WebSocket. Each session is handed a virtual IPv4 address
// out of 10.0.0.0/8 and the gateway routes datagrams between sessions by that
// address, so CUDPComm in the client keeps working with ordinary addresses.
//
// The frame format is described in src/net/AvaraTCPWeb.cpp.
//
// Only the standard library is used, so the container needs no module fetch.
package main

import (
	"crypto/rand"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"flag"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

const (
	cmdHello   = 0x01
	cmdData    = 0x02
	cmdWelcome = 0x81
	cmdRelay   = 0x82

	maxDatagram  = 16 * 1024
	writeTimeout = 5 * time.Second
	idleTimeout  = 120 * time.Second
)

// A session can only ever reach another session on this same gateway, so the
// relay cannot be pointed at a third party. What it can still do is exhaust
// this process, so cap how many sessions exist and how fast each one may send.
var (
	maxSessions   = 512
	sendRate      = 2000.0 // datagrams per second, sustained
	sendBurst     = 4000.0
	maxSessionAge = 6 * time.Hour
)

type session struct {
	id   uint32 // low 24 bits of the virtual address
	ip   uint32
	port uint16
	conn net.Conn
	out  chan []byte
	once sync.Once

	tokens float64 // token bucket, refilled on use
	last   time.Time
}

// allow reports whether this session may relay one more datagram now.
func (s *session) allow(now time.Time) bool {
	if s.last.IsZero() {
		s.last = now
		s.tokens = sendBurst
	}
	s.tokens += now.Sub(s.last).Seconds() * sendRate
	s.last = now
	if s.tokens > sendBurst {
		s.tokens = sendBurst
	}
	if s.tokens < 1 {
		return false
	}
	s.tokens--
	return true
}

func (s *session) close() {
	s.once.Do(func() {
		close(s.out)
		s.conn.Close()
	})
}

type registry struct {
	mu sync.RWMutex
	m  map[uint32]*session
}

func (r *registry) add(s *session) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.m[s.id] = s
}

func (r *registry) remove(id uint32) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.m, id)
}

func (r *registry) lookup(id uint32) *session {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.m[id]
}

func (r *registry) claim() (uint32, error) {
	r.mu.RLock()
	full := len(r.m) >= maxSessions
	r.mu.RUnlock()
	if full {
		return 0, errors.New("gateway full")
	}
	for attempt := 0; attempt < 64; attempt++ {
		n, err := rand.Int(rand.Reader, big.NewInt(0xfffffe))
		if err != nil {
			return 0, err
		}
		id := uint32(n.Int64()) + 1 // never zero; zero is not a room code
		r.mu.Lock()
		if _, taken := r.m[id]; !taken {
			r.m[id] = nil // reserve
			r.mu.Unlock()
			return id, nil
		}
		r.mu.Unlock()
	}
	return 0, errors.New("no free address")
}

var reg = &registry{m: make(map[uint32]*session)}

var verbose bool

// --- minimal WebSocket (RFC 6455), binary frames only ----------------------

const wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

func acceptKey(key string) string {
	h := sha1.New()
	io.WriteString(h, key+wsGUID)
	return base64.StdEncoding.EncodeToString(h.Sum(nil))
}

func upgrade(w http.ResponseWriter, r *http.Request) (net.Conn, error) {
	if !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") {
		return nil, errors.New("not a websocket upgrade")
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		return nil, errors.New("missing Sec-WebSocket-Key")
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		return nil, errors.New("connection cannot be hijacked")
	}
	conn, buf, err := hj.Hijack()
	if err != nil {
		return nil, err
	}
	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\nConnection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + acceptKey(key) + "\r\n\r\n"
	if _, err := buf.WriteString(resp); err != nil {
		conn.Close()
		return nil, err
	}
	if err := buf.Flush(); err != nil {
		conn.Close()
		return nil, err
	}
	return conn, nil
}

// readFrame returns one message payload. Control frames are handled inline.
func readFrame(conn net.Conn) (opcode byte, payload []byte, err error) {
	var hdr [2]byte
	if _, err = io.ReadFull(conn, hdr[:]); err != nil {
		return 0, nil, err
	}
	opcode = hdr[0] & 0x0f
	masked := hdr[1]&0x80 != 0
	length := uint64(hdr[1] & 0x7f)

	switch length {
	case 126:
		var ext [2]byte
		if _, err = io.ReadFull(conn, ext[:]); err != nil {
			return 0, nil, err
		}
		length = uint64(binary.BigEndian.Uint16(ext[:]))
	case 127:
		var ext [8]byte
		if _, err = io.ReadFull(conn, ext[:]); err != nil {
			return 0, nil, err
		}
		length = binary.BigEndian.Uint64(ext[:])
	}
	if length > maxDatagram {
		return 0, nil, errors.New("frame too large")
	}

	var mask [4]byte
	if masked {
		if _, err = io.ReadFull(conn, mask[:]); err != nil {
			return 0, nil, err
		}
	}
	payload = make([]byte, length)
	if _, err = io.ReadFull(conn, payload); err != nil {
		return 0, nil, err
	}
	if masked {
		for i := range payload {
			payload[i] ^= mask[i%4]
		}
	}
	return opcode, payload, nil
}

func writeFrame(conn net.Conn, opcode byte, payload []byte) error {
	var hdr []byte
	n := len(payload)
	switch {
	case n < 126:
		hdr = []byte{0x80 | opcode, byte(n)}
	case n < 65536:
		hdr = []byte{0x80 | opcode, 126, byte(n >> 8), byte(n)}
	default:
		hdr = make([]byte, 10)
		hdr[0], hdr[1] = 0x80|opcode, 127
		binary.BigEndian.PutUint64(hdr[2:], uint64(n))
	}
	conn.SetWriteDeadline(time.Now().Add(writeTimeout))
	if _, err := conn.Write(hdr); err != nil {
		return err
	}
	_, err := conn.Write(payload)
	return err
}

// --- session handling ------------------------------------------------------

func handleNet(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrade(w, r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	id, err := reg.claim()
	if err != nil {
		log.Printf("refusing session: %v", err)
		conn.Close()
		return
	}
	s := &session{
		id:   id,
		ip:   0x0A000000 | (id & 0xffffff),
		conn: conn,
		out:  make(chan []byte, 256),
	}
	reg.add(s)
	log.Printf("session %s connected (%s)", roomCode(id), ipString(s.ip))

	// One writer goroutine per session; nothing else touches conn for writes.
	go func() {
		for msg := range s.out {
			if err := writeFrame(conn, 0x2, msg); err != nil {
				break
			}
		}
		conn.Close()
	}()

	defer func() {
		reg.remove(id)
		s.close()
		log.Printf("session %s disconnected", roomCode(id))
	}()

	expiry := time.Now().Add(maxSessionAge)
	for {
		now := time.Now()
		if now.After(expiry) {
			log.Printf("session %s expired", roomCode(id))
			return
		}
		deadline := now.Add(idleTimeout)
		if deadline.After(expiry) {
			deadline = expiry
		}
		conn.SetReadDeadline(deadline)
		opcode, payload, err := readFrame(conn)
		if err != nil {
			return
		}
		switch opcode {
		case 0x8: // close
			return
		case 0x9: // ping
			s.send(append([]byte{}, payload...), 0xA)
			continue
		case 0xA: // pong
			continue
		case 0x1, 0x2:
		default:
			continue
		}
		if len(payload) < 1 {
			continue
		}

		switch payload[0] {
		case cmdHello:
			if len(payload) >= 3 {
				s.port = binary.BigEndian.Uint16(payload[1:3])
			}
			welcome := make([]byte, 7)
			welcome[0] = cmdWelcome
			binary.BigEndian.PutUint32(welcome[1:5], s.ip)
			binary.BigEndian.PutUint16(welcome[5:7], s.port)
			s.send(welcome, 0x2)

		case cmdData:
			if len(payload) < 7 {
				continue
			}
			if !s.allow(now) {
				continue // over its rate; a real socket would drop these too
			}
			dstIP := binary.BigEndian.Uint32(payload[1:5])
			dst := reg.lookup(dstIP & 0xffffff)
			if verbose {
				log.Printf("%s -> %s  %d bytes  %s", roomCode(s.id), ipString(dstIP),
					len(payload)-7, map[bool]string{true: "relayed", false: "DROPPED (no such session)"}[dst != nil])
			}
			if dst == nil {
				continue // nobody at that address; a real UDP socket would drop it too
			}
			relay := make([]byte, len(payload))
			relay[0] = cmdRelay
			binary.BigEndian.PutUint32(relay[1:5], s.ip)
			binary.BigEndian.PutUint16(relay[5:7], s.port)
			copy(relay[7:], payload[7:])
			dst.send(relay, 0x2)
		}
	}
}

// send drops rather than blocks: this is a datagram relay, and the layers
// above it in Avara already handle loss.
func (s *session) send(msg []byte, opcode byte) {
	defer func() { recover() }() // out may be closed concurrently
	select {
	case s.out <- msg:
	default:
	}
}

func ipString(ip uint32) string {
	return net.IPv4(byte(ip>>24), byte(ip>>16), byte(ip>>8), byte(ip)).String()
}

const alphabet = "23456789abcdefghjkmnpqrstuvwxyz"

func roomCode(id uint32) string {
	out := make([]byte, 5)
	for i := 4; i >= 0; i-- {
		out[i] = alphabet[id%31]
		id /= 31
	}
	return string(out)
}

func main() {
	addr := flag.String("addr", envOr("AVARA_GW_ADDR", ":8088"), "listen address")
	flag.BoolVar(&verbose, "v", os.Getenv("AVARA_GW_VERBOSE") != "", "log every relayed datagram")
	flag.IntVar(&maxSessions, "max-sessions", maxSessions, "refuse new sessions past this many")
	flag.Float64Var(&sendRate, "rate", sendRate, "datagrams per second each session may relay")
	flag.DurationVar(&maxSessionAge, "max-age", maxSessionAge, "close a session after this long")
	flag.Parse()
	sendBurst = sendRate * 2

	mux := http.NewServeMux()
	mux.HandleFunc("/net", handleNet)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		reg.mu.RLock()
		n := len(reg.m)
		reg.mu.RUnlock()
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"status":"ok","sessions":` + itoa(n) + `}`))
	})

	srv := &http.Server{Addr: *addr, Handler: mux}
	log.Printf("avara-gw listening on %s", *addr)
	if err := srv.ListenAndServe(); err != nil {
		log.Fatal(err)
	}
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var b [20]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	return string(b[i:])
}
