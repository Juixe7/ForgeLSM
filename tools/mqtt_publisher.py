#!/usr/bin/env python3
"""Publish deterministic or live-random IoT messages to an external MQTT broker."""

import argparse
import json
import random
import sys
import time

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


def publish_json(client, topic, payload, qos):
    body = json.dumps(payload, separators=(",", ":"))
    result = client.publish(topic, body, qos=qos)
    result.wait_for_publish()


def telemetry_key(site, device, sequence):
    return f"mqtt:factory/{site}/device/{device}/telemetry:{sequence}"


def choose_device(sequence, devices, random_mode):
    if random_mode:
        return f"dev_{random.randint(1, devices):04d}"
    return f"dev_{((sequence - 1) % devices) + 1:04d}"


def choose_bucket(sequence, random_mode):
    if random_mode:
        return random.randint(0, 99)
    return (sequence - 1) % 100


def build_message(args, sequence, live_telemetry_keys):
    device = choose_device(sequence, args.devices, args.random)
    bucket = choose_bucket(sequence, args.random)

    if bucket < 80:
        topic = f"factory/{args.site}/device/{device}/telemetry"
        key = telemetry_key(args.site, device, sequence)
        payload = {
            "op": "put",
            "storage_key": key,
            "device_id": device,
            "sequence": sequence,
            "timestamp_ms": 1_700_000_000_000 + sequence * 1000,
            "temperature_c": round(24.0 + random.random() * 7.0, 3),
            "vibration_mm_s": round(random.random() * 4.0, 3),
            "status": "ok" if random.random() > 0.02 else "warn",
        }
        live_telemetry_keys.append(key)
        return topic, payload, "telemetry_put"

    if bucket < 90:
        family = random.choice(["firmware", "sampling", "threshold", "mode"]) if args.random else ["firmware", "sampling", "threshold", "mode"][sequence % 4]
        topic = f"factory/{args.site}/device/{device}/state/{family}"
        payload = {
            "op": "update",
            "device_id": device,
            "sequence": sequence,
            "state_family": family,
            "value": f"{family}-{random.randint(1, 100000) if args.random else sequence // max(args.devices, 1)}",
        }
        return topic, payload, "state_update"

    if bucket < 95:
        topic = f"factory/{args.site}/device/{device}/delete"
        if live_telemetry_keys:
            index = random.randrange(len(live_telemetry_keys)) if args.random else 0
            target = live_telemetry_keys.pop(index)
        else:
            target = telemetry_key(args.site, device, sequence)
        payload = {
            "op": "delete",
            "device_id": device,
            "sequence": sequence,
            "target_key": target,
        }
        return topic, payload, "delete"

    topic = f"factory/{args.site}/device/{device}/query"
    target = random.choice(live_telemetry_keys) if args.random and live_telemetry_keys else (live_telemetry_keys[-1] if live_telemetry_keys else telemetry_key(args.site, device, sequence))
    payload = {
        "op": "get",
        "device_id": device,
        "sequence": sequence,
        "target_key": target,
    }
    return topic, payload, "get"


def main():
    parser = argparse.ArgumentParser(description="Publish synthetic IoT MQTT traffic for ForgeLSM.")
    parser.add_argument("--broker", default="localhost", help="MQTT broker host")
    parser.add_argument("--port", type=int, default=1883, help="MQTT broker port")
    parser.add_argument("--events", type=int, default=10000, help="Total MQTT messages to publish")
    parser.add_argument("--devices", type=int, default=100, help="Number of simulated devices")
    parser.add_argument("--site", default="site_a", help="Factory/site label")
    parser.add_argument("--qos", type=int, default=0, choices=(0, 1, 2), help="MQTT publish QoS")
    parser.add_argument("--delay-ms", type=float, default=0.0, help="Delay between messages")
    parser.add_argument("--rate", type=float, default=0.0, help="Messages per second for live/random demos")
    parser.add_argument("--live", action="store_true", help="Publish forever until Ctrl+C")
    parser.add_argument("--random", action="store_true", help="Choose devices and operation types randomly")
    parser.add_argument("--seed", type=int, default=42, help="Deterministic random seed")
    parser.add_argument("--client-id", default="forgelsm-mqtt-publisher", help="MQTT client id")
    args = parser.parse_args()

    random.seed(args.seed)
    client = make_client(args.client_id)
    client.connect(args.broker, args.port, keepalive=60)
    client.loop_start()

    counts = {"telemetry_put": 0, "state_update": 0, "delete": 0, "get": 0}
    live_telemetry_keys = []

    sequence = 0
    delay = args.delay_ms / 1000.0 if args.delay_ms > 0 else (1.0 / args.rate if args.rate > 0 else 0.0)
    target_events = None if args.live else args.events

    try:
        while target_events is None or sequence < target_events:
            sequence += 1
            topic, payload, kind = build_message(args, sequence, live_telemetry_keys)
            counts[kind] += 1
            publish_json(client, topic, payload, args.qos)

            if delay > 0:
                time.sleep(delay)

            if sequence % 1000 == 0 or (target_events is not None and sequence == target_events):
                suffix = "live" if target_events is None else str(target_events)
                print(f"Published {sequence}/{suffix} messages", flush=True)
    except KeyboardInterrupt:
        print("\nStopping live MQTT publisher...", flush=True)
    finally:
        client.loop_stop()
        client.disconnect()

    print(
        "MQTT publisher summary: "
        f"events={sequence}, devices={args.devices}, telemetry_put={counts['telemetry_put']}, "
        f"state_update={counts['state_update']}, delete={counts['delete']}, get={counts['get']}",
        flush=True,
    )


if __name__ == "__main__":
    main()
