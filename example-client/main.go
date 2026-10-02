// Command argus-example-client subscribes to the Argus RTSP Watcher MQTT feeds
// and prints each message's raw JSON payload, one per line, on stdout (pipe to
// jq). Feeds are discovered from the wildcard topic; connection logs go to
// stderr so stdout stays pure JSON. See VISION.md.
package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

func main() {
	broker := flag.String("broker", "", "(required) MQTT broker host:port, e.g. 192.0.2.20:1883")
	topic := flag.String("topic", "bs/argus/#", "topic filter to subscribe to")
	once := flag.Bool("once", false, "exit after the first message")

	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "argus-example-client - print raw JSON from the Argus MQTT feeds.\n\n")
		fmt.Fprintf(os.Stderr, "Usage:\n  %s --broker host:port [--topic filter] [--once]\n\nFlags:\n", os.Args[0])
		flag.PrintDefaults()
		fmt.Fprintf(os.Stderr, "\nExample:\n  %s --broker 192.0.2.20:1883 | jq .\n", os.Args[0])
	}
	flag.Parse()

	log.SetFlags(0)

	if *broker == "" {
		fmt.Fprintln(os.Stderr, "error: --broker is required")
		flag.Usage()
		os.Exit(2)
	}

	done := make(chan struct{})
	var doneOnce sync.Once

	onMessage := func(_ mqtt.Client, m mqtt.Message) {
		fmt.Println(string(m.Payload())) // just the JSON, one per line
		if *once {
			doneOnce.Do(func() { close(done) })
		}
	}

	opts := mqtt.NewClientOptions().
		AddBroker("tcp://" + *broker).
		SetClientID(fmt.Sprintf("argus-example-client-%d", os.Getpid())).
		SetAutoReconnect(true).
		SetConnectTimeout(10 * time.Second).
		SetOnConnectHandler(func(c mqtt.Client) {
			if t := c.Subscribe(*topic, 0, onMessage); t.Wait() && t.Error() != nil {
				log.Fatalf("subscribe %q: %v", *topic, t.Error())
			}
			log.Printf("connected to %s, subscribed to %q", *broker, *topic)
		}).
		SetConnectionLostHandler(func(_ mqtt.Client, err error) {
			log.Printf("connection lost: %v (auto-reconnecting)", err)
		})

	client := mqtt.NewClient(opts)
	if t := client.Connect(); t.Wait() && t.Error() != nil {
		log.Fatalf("connect %s: %v", *broker, t.Error())
	}
	defer client.Disconnect(250)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	select {
	case <-sig:
	case <-done:
	}
}
