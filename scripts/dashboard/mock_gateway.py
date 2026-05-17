import time
import json
import random
import math
import paho.mqtt.client as mqtt

BROKER = "127.0.0.1"
PORT = 1883
GW_ID = "gw01"
DEVICES = ["0x58", "0x5A"]

def noise(amp):
    return (random.random() * 2 - 1) * amp

def on_connect(client, userdata, flags, rc):
    print(f"Connected to {BROKER} with result code {rc}")
    client.subscribe(f"pmbus/{GW_ID}/cmd/request")

def on_message(client, userdata, msg):
    if msg.topic == f"pmbus/{GW_ID}/cmd/request":
        try:
            req = json.loads(msg.payload.decode())
            print(f"Received command: {req}")
            
            # Extract fields from new schema
            req_id = req.get("id", "unknown")
            addr = req.get("addr", 0)
            wr = req.get("wr", [])
            rd_len = req.get("rd_len", 0)
            
            # mock response
            res = {
                "id": req_id,
                "addr": addr,
                "status": "OK",
                "data": [],
                "exec_ms": random.randint(2, 8)
            }
            
            if wr and len(wr) > 0 and wr[0] == 0xFF:
                res["status"] = "I2C_NACK"
            else:
                # Generate mock data if read requested
                if rd_len > 0:
                    if wr and len(wr) > 0:
                        cmd = wr[0]
                        if cmd == 0x88: # READ_VIN
                            res["data"] = [0x00, 0x18] # dummy bytes
                        elif cmd == 0x8B: # READ_VOUT
                            res["data"] = [0x50, 0x10] # dummy bytes
                        else:
                            res["data"] = [random.randint(0, 255) for _ in range(rd_len)]
                    else:
                        res["data"] = [random.randint(0, 255) for _ in range(rd_len)]

            client.publish(f"pmbus/{GW_ID}/cmd/response", json.dumps(res))
            print(f"-> Sent response: {res}")
        except Exception as e:
            print(f"Error parsing command: {e}")

client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

try:
    print(f"Connecting to MQTT broker at {BROKER}...")
    client.connect(BROKER, PORT, 60)
except Exception as e:
    print(f"Failed to connect: {e}")
    exit(1)

client.loop_start()

time_ms = 0
uptime = 0
last_metrics_time = time.time()
last_status_time = time.time()
last_events_time = time.time()
metrics_interval = 1.0
status_interval = 1.0
events_interval = 5.0
telemetry_interval = 0.25

print("Starting mock data streams... Press Ctrl+C to exit.")

try:
    while True:
        now = time.time()
        
        # 1. Telemetry
        time_ms += telemetry_interval * 1000
        for addr in DEVICES:
            phase = 0 if addr == "0x58" else math.pi
            voutBase = 12.0 if addr == "0x58" else 3.3
            ioutBase = 5.0
            
            obj = {
                "addr": addr,
                "ts_ms": int(now * 1000),
                "v": {
                    "vin": 48.0 + noise(1.0),
                    "vout": max(0, voutBase + math.sin(time_ms / 2000.0 + phase) * 0.5 + noise(0.05))
                },
                "a": {
                    "iout": max(0, ioutBase + abs(math.sin(time_ms / 3000.0 + phase)) * 3.0 + noise(0.2))
                },
                "c": {
                    "temp1": 45.0 + math.sin(time_ms / 10000.0 + phase) * 10.0 + noise(1.0)
                }
            }
            client.publish(f"pmbus/{GW_ID}/dev/{addr}/telemetry", json.dumps(obj))
            
        # 2. Status
        if now - last_status_time >= status_interval:
            last_status_time = now
            for addr in DEVICES:
                obj = {
                    "addr": addr,
                    "status_word": "0x0000",
                    "status_vout": "0x00",
                    "status_iout": "0x00",
                    "status_temperature": "0x00"
                }
                if addr == "0x5A" and random.random() < 0.05:
                    obj["status_word"] = "0x8000"
                client.publish(f"pmbus/{GW_ID}/dev/{addr}/status", json.dumps(obj))
                
        # 3. Metrics
        if now - last_metrics_time >= metrics_interval:
            last_metrics_time = now
            uptime += 1
            obj = {
                "timing_ms": {
                    "read_to_publish_p95": 15.0 + abs(noise(2.0)),
                    "read_to_publish_avg": 10.0 + abs(noise(1.0)),
                    "read_to_publish_max": 20.0 + abs(noise(5.0)),
                    "pmbus_txn_avg": 2.5 + abs(noise(0.5)),
                    "mqtt_publish_avg": 0.5 + abs(noise(0.1))
                },
                "timing_rolling_ms": {
                    "read_to_publish_p95": 16.0 + abs(noise(2.0)),
                    "read_to_publish_avg": 11.0 + abs(noise(1.0)),
                    "read_to_publish_max": 24.0 + abs(noise(5.0))
                },
                "timing_samples": {
                    "read_to_publish_window": 8,
                    "read_to_publish_rolling": 100
                },
                "rates": {
                    "telemetry_msgs_per_s": 8.0,
                    "pmbus_cmds_per_s": 1.0
                },
                "gauges": {
                    "buffer_depth_ram": int(100 + noise(20)),
                    "uptime_s": uptime,
                    "wifi_rssi_dbm": int(-60 + noise(5))
                },
                "counters_delta": {
                    "pmbus_reads_fail": 1 if random.random() < 0.1 else 0,
                    "pmbus_crc_pec_fail": 0,
                    "pmbus_nack": 0,
                    "pmbus_timeouts": 0,
                    "pmbus_retries": 1 if random.random() < 0.2 else 0,
                    "mqtt_reconnects": 0,
                    "i2c_controller_resets": 0,
                    "i2c_bus_recoveries": 0
                }
            }
            client.publish(f"pmbus/{GW_ID}/metrics", json.dumps(obj))
            
        # 4. Events
        if now - last_events_time >= events_interval:
            last_events_time = now
            if random.random() >= 0.7:
                isError = random.random() < 0.3
                obj = {
                    "type": "PMBUS_ERROR" if isError else "SYSTEM_OK",
                    "detail": "Timeout reading register 0x8B on 0x58" if isError else "Background sweep completed",
                    "ts_ms": int(now * 1000)
                }
                client.publish(f"pmbus/{GW_ID}/events", json.dumps(obj))
                
        time.sleep(telemetry_interval)

except KeyboardInterrupt:
    print("\nShutting down...")
    client.loop_stop()
    client.disconnect()
