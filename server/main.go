package main

import (
	"context"
	"flag"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

func main() {
	port := flag.Int("port", 19533, "UDP listen port (signaling for desktop clients)")
	httpAddr := flag.String("http", ":8080",
		"HTTP listen address for the WebSocket relay used by the web build (/ws); empty disables it")
	origins := flag.String("origins", os.Getenv("WS_ORIGINS"),
		"extra allowed Origin hosts for /ws, comma-separated (same-host origins are always allowed)")
	flag.Parse()

	addr := &net.UDPAddr{Port: *port}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		log.Fatalf("Failed to listen on :%d: %v", *port, err)
	}
	defer conn.Close()

	log.Printf("Signaling server listening on :%d", *port)
	if os.Getenv("TURN_SECRET") != "" {
		log.Printf("TURN credential generation enabled")
	}

	srv := NewServer(conn)

	var httpSrv *http.Server
	if *httpAddr != "" {
		mux := http.NewServeMux()
		mux.Handle("/ws", NewWSHub(splitList(*origins)))
		mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
			w.Write([]byte("ok\n"))
		})
		httpSrv = &http.Server{Addr: *httpAddr, Handler: mux, ReadHeaderTimeout: 10 * time.Second}
		go func() {
			log.Printf("WebSocket relay listening on %s (/ws)", *httpAddr)
			if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
				log.Fatalf("HTTP server: %v", err)
			}
		}()
	}

	// Graceful shutdown
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sig
		log.Println("Shutting down...")
		if httpSrv != nil {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			httpSrv.Shutdown(ctx)
			cancel()
		}
		conn.Close()
	}()

	srv.Run()
}

func splitList(s string) []string {
	var out []string
	for _, part := range strings.Split(s, ",") {
		if part = strings.TrimSpace(part); part != "" {
			out = append(out, part)
		}
	}
	return out
}
