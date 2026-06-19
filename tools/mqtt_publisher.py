#!/usr/bin/env python3
"""Publish deterministic IoT messages to an external MQTT broker."""

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


def main():
    parser = argparse.ArgumentParser(description="Publish synthetic IoT MQTT traffic for ForgeLSM.")
    parser.add_argument("--broker", default="localhost", help="MQTT broker host")
    parser.add_argument("--port", type=int, default=1883, help="MQTT broker port")
    parser.add_argument("--events", type=int, default=10000, help="Total MQTT messages to publish")
    parser.add_argument("--devices", type=int, default=100, help="Number of simulated devices")
    parser.add_argument("--site", default="site_a", help="Factory/site label")
    parser.add_argument("--qos", type=int, default=0, choices=(0, 1, 2), help="MQTT publish QoS")
    parser.add_argument("--delay-ms", type=float, default=0.0, help="Delay between messages")
    parser.add_argument("--seed", type=int, default=42, help="Deterministic random seed")
    parser.add_argument("--client-id", default="forgelsm-mqtt-publisher", help="MQTT client id")
    args = parser.parse_args()

    random.seed(args.seed)
    client = make_client(args.client_id)
    client.connect(args.broker, args.port, keepalive=60)
    client.loop_start()

    counts = {"telemetry_put": 0, "state_update": 0, "delete": 0, "get": 0}
    live_telemetry_keys = []

    try:
        for sequence in range(1, args.events + 1):
            device = f"dev_{((sequence - 1) % args.devices) + 1:04d}"
            bucket = (sequence - 1) % 100

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
                counts["telemetry_put"] += 1
            elif bucket < 90:
                family = ["firmware", "sampling", "threshold", "mode"][sequence % 4]
                topic = f"factory/{args.site}/device/{device}/state/{family}"
                payload = {
                    "op": "update",
                    "device_id": device,
                    "sequence": sequence,
                    "state_family": family,
                    "value": f"{family}-{sequence // max(args.devices, 1)}",
                }
                counts["state_update"] += 1
            elif bucket < 95:
                topic = f"factory/{args.site}/device/{device}/delete"
                target = live_telemetry_keys.pop(0) if live_telemetry_keys else telemetry_key(args.site, device, sequence)
                payload = {
                    "op": "delete",
                    "device_id": device,
                    "sequence": sequence,
                    "target_key": target,
                }
                counts["delete"] += 1
            else:
                topic = f"factory/{args.site}/device/{device}/query"
                target = live_telemetry_keys[-1] if live_telemetry_keys else telemetry_key(args.site, device, sequence)
                payload = {
                    "op": "get",
                    "device_id": device,
                    "sequence": sequence,
                    "target_key": target,
                }
                counts["get"] += 1

            publish_json(client, topic, payload, args.qos)

            if args.delay_ms > 0:
                time.sleep(args.delay_ms / 1000.0)

            if sequence % 1000 == 0 or sequence == args.events:
                print(f"Published {sequence}/{args.events} messages", flush=True)
    finally:
        client.loop_stop()
        client.disconnect()

    print(
        "MQTT publisher summary: "
        f"events={args.events}, devices={args.devices}, telemetry_put={counts['telemetry_put']}, "
        f"state_update={counts['state_update']}, delete={counts['delete']}, get={counts['get']}",
        flush=True,
    )


if __name__ == "__main__":
    main()
