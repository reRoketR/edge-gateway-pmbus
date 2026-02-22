# Mosquitto MQTT Broker (Docker)

One-command setup for the MQTT broker used by the PMBus-MQTT Edge Gateway.

## Quick Start

```bash
cd scripts/mqtt_broker
docker-compose up -d
```

## Verify

```bash
# Check container is running
docker ps | grep pmbus-mqtt-broker

# Subscribe to all gateway topics (in another terminal)
docker exec pmbus-mqtt-broker mosquitto_sub -t "pmbus/#" -v
```

## Configuration

| Parameter | Value | Notes |
|-----------|-------|-------|
| MQTT port | 1883 | Plain TCP, no TLS |
| WebSocket port | 9001 | Optional, for browser-based MQTT clients |
| Authentication | Anonymous | No username/password required |
| Persistence | Enabled | Survives container restart |

## Stop

```bash
docker-compose down
```

## Alternative: Native Install

If Docker is not available, install Mosquitto natively:

```bash
# Windows (winget)
winget install EclipseMosquitto.Mosquitto

# Linux (apt)
sudo apt install mosquitto mosquitto-clients

# macOS (brew)
brew install mosquitto
```

Then use the provided `mosquitto.conf`:
```bash
mosquitto -c scripts/mqtt_broker/mosquitto.conf
```
