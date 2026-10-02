package broker

import (
	"net"
	"testing"
	"time"
)

func TestWaitForBrokerSucceedsWhenListening(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	if err := WaitForBroker(ln.Addr().String(), time.Second); err != nil {
		t.Errorf("expected ready, got %v", err)
	}
}

func TestWaitForBrokerTimesOut(t *testing.T) {
	if err := WaitForBroker("127.0.0.1:1", 200*time.Millisecond); err == nil {
		t.Error("expected timeout error")
	}
}
