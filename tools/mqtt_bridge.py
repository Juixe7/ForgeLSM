#!/usr/bin/env python3
"""Bridge an external MQTT broker into the ForgeLSM HTTP API.

The C++ engine remains the source of truth. This process only translates MQTT
messages into /api/put, /api/delete, and /api/get calls.
"""

import argparse
import hashlib
import json
import signal
import sys
import time
import urllib.error
import urllib.request

MQTT_MODULE = None


def import_mqtt():
    global MQTT_MODULE
    if MQTT_MODULE is not None:
        return MQTT_MODULE
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print(
            "Missing dependency: paho-mqtt. Install with: python -m pip install -r requirements-mqtt.txt",
            file=sys.stderr,
        )
        sys.exit(2)
    MQTT_MODULE = mqtt
    return mqtt


def make_client(client_id):
    mqtt = import_mqtt()
    if hasattr(mqtt, "CallbackAPIVersion"):
        return mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    return mqtt.Client(client_id=client_id)


def parse_json(raw):
    try:
        value = json.loads(raw)
        if isinstance(value, dict):
            return value
    except json.JSONDecodeError:
        pass
    return {}


def stable_suffix(topic, payload, sequence):
    obj = parse_json(payload)
    for field in ("storage_key", "event_id", "sequence", "seq", "timestamp", "ts", "id"):
        if field in obj:
            return str(obj[field])
    digest = hashlib.sha1(f"{topic}\0{payload}\0{sequence}".encode("utf-8")).hexdigest()
    return digest[:16]


def key_for_message(topic, payload, sequence):
    obj = parse_json(payload)
    if obj.get("storage_key"):
        return str(obj["storage_key"])
    if obj.get("key"):
        return str(obj["key"])
    if topic.endswith("/telemetry"):
        return f"mqtt:{topic}:{stable_suffix(topic, payload, sequence)}"
    if "/state/" in topic or topic.endswith("/state") or "/config/" in topic or topic.endswith("/config"):
        return f"mqtt:{topic}"
    return f"mqtt:{topic}:{stable_suffix(topic, payload, sequence)}"


def target_key_for_message(topic, payload, sequence):
    obj = parse_json(payload)
    for field in ("target_key", "key", "storage_key"):
        if obj.get(field):
            return str(obj[field])
    return key_for_message(topic, payload, sequence)


class Bridge:
    def __init__(self, args):
        self.args = args
        self.engine = args.engine.rstrip("/")
        self.received = 0
        self.puts = 0
        self.deletes = 0
        self.gets = 0
        self.errors = 0
        self.stopped = False

    def post_json(self, path, payload):
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        request = urllib.request.Request(
            self.engine + path,
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=self.args.http_timeout) as response:
            text = response.read().decode("utf-8")
            return json.loads(text) if text else {}

    def handle_message(self, topic, payload):
        self.received += 1
        obj = parse_json(payload)
        op = str(obj.get("op", "")).lower()

        if topic.endswith("/delete") or op == "delete":
            key = target_key_for_message(topic, payload, self.received)
            response = self.post_json("/api/delete", {"key": key})
            self.deletes += 1
            self.log("DELETE", topic, key, response)
            return

        if topic.endswith("/query") or op == "get":
            key = target_key_for_message(topic, payload, self.received)
            response = self.post_json("/api/get", {"key": key})
            self.gets += 1
            self.log("GET", topic, key, response)
            return

        key = key_for_message(topic, payload, self.received)
        value = payload
        response = self.post_json("/api/put", {"key": key, "value": value})
        self.puts += 1
        self.log("PUT", topic, key, response)

    def log(self, op, topic, key, response):
        if not self.args.verbose:
            return
        status = response.get("status", "ok")
        print(f"[{op}] topic={topic} key={key} status={status}", flush=True)

    def print_summary(self):
        print(
            "MQTT bridge summary: "
            f"received={self.received}, puts={self.puts}, deletes={self.deletes}, "
            f"gets={self.gets}, errors={self.errors}",
            flush=True,
        )


def main():
    parser = argparse.ArgumentParser(description="Bridge an external MQTT broker into ForgeLSM.")
    parser.add_argument("--broker", default="localhost", help="MQTT broker host")
    parser.add_argument("--port", type=int, default=1883, help="MQTT broker port")
    parser.add_argument("--topic", default="factory/+/device/+/+/#", help="MQTT subscription topic")
    parser.add_argument("--engine", default="http://localhost:8080", help="ForgeLSM HTTP base URL")
    parser.add_argument("--client-id", default="forgelsm-mqtt-bridge", help="MQTT client id")
    parser.add_argument("--qos", type=int, default=0, choices=(0, 1, 2), help="MQTT subscription QoS")
    parser.add_argument("--limit", type=int, default=0, help="Stop after N received messages; 0 means forever")
    parser.add_argument("--http-timeout", type=float, default=10.0, help="HTTP request timeout in seconds")
    parser.add_argument("--verbose", action="store_true", help="Print every translated operation")
    args = parser.parse_args()

    bridge = Bridge(args)
    client = make_client(args.client_id)

    def on_connect(client, userdata, flags, reason_code, properties=None):
        print(f"Connected to MQTT broker {args.broker}:{args.port}; subscribing to {args.topic}", flush=True)
        client.subscribe(args.topic, qos=args.qos)

    def on_message(client, userdata, msg):
        try:
            payload = msg.payload.decode("utf-8", errors="replace")
            bridge.handle_message(msg.topic, payload)
            if args.limit and bridge.received >= args.limit:
                bridge.stopped = True
                client.disconnect()
        except (urllib.error.URLError, TimeoutError, OSError, ValueError) as exc:
            bridge.errors += 1
            print(f"[ERROR] topic={msg.topic} error={exc}", file=sys.stderr, flush=True)

    def stop(signum, frame):
        bridge.stopped = True
        client.disconnect()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.broker, args.port, keepalive=60)

    try:
        client.loop_forever()
    finally:
        time.sleep(0.1)
        bridge.print_summary()


if __name__ == "__main__":
    main()
