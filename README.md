# EPMS - IoT Showcase Monitoring

An educational IoT project for monitoring display cases using three ESP8266 sensor nodes and a Python master node. The system collects environmental measurements, detects events, displays device status in a web dashboard, and supports remote configuration.

The master integrates MQTT, Flask, InfluxDB, Telegram notifications, and external weather and sunrise/sunset services. Each sensor node publishes a Web of Things Thing Description describing its properties, events, and configuration action.

## System overview

- **ESP8266 nodes** read sensors, manage local alarms, publish telemetry and events over MQTT, and receive configuration through HTTP endpoints.
- **Python master** discovers nodes, keeps their latest state in memory, stores telemetry in InfluxDB when configured, and forwards dashboard commands to devices.
- **Flask dashboard** displays connected nodes and their measurements and provides controls for changing device settings.
- **External integrations** provide Telegram notifications, outdoor temperature, and sunrise/sunset information.

MQTT carries discovery, telemetry, and events. HTTP connects the dashboard to the master and the master to each node's configuration endpoint. The master must be able to reach the devices on the network, and all components must use the same MQTT broker.

## Repository contents

| File or directory | Purpose |
| --- | --- |
| `master_node.py` | MQTT subscriber, Flask application, device coordination, storage, and external integrations. |
| `dashboard.html` | Web interface served by Flask. Keep it beside `master_node.py`. |
| `sketch-Teca1/` | Temperature, humidity, light, door status, alarm LED, and LCD node. |
| `sketch-Teca2/` | Temperature, humidity, and temperature alarm node. |
| `sketch-Teca3/` | Ultrasonic sensor node for monitoring water level through distance measurements. |
| `requirements.txt` | Python dependencies. |
| `.env.example` | Empty configuration template to copy locally. |
| `.gitignore` | Excludes credentials, generated cache files, environments, and build artifacts. |

`api_cache.json` is generated locally to cache external API responses and is not part of the repository.

## Requirements

- Python 3 and the packages in `requirements.txt`.
- Three ESP8266 boards and the sensors and peripherals used by their sketches.
- Arduino IDE with ESP8266 board support, or an equivalent compatible development environment.
- An MQTT broker reachable by the master and all three nodes.
- An InfluxDB instance, organization, bucket, and write token for persistent telemetry storage.
- Optional Telegram bot and OpenWeather credentials.
- Internet access for external integrations; the sunrise/sunset request does not require an API key in this implementation.

The sketches use the ESP8266 core libraries plus WiFiManager, ArduinoJson, MQTT, DHT, and LiquidCrystal_I2C where applicable. `Wire`, `EEPROM`, and time support are provided by the corresponding Arduino core. Exact dependency versions were not recorded, so compatibility should be checked when reproducing the project.

## Run the master

Open a terminal in the directory containing `master_node.py`:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
cp .env.example .env
```

On Windows, activate the environment with `.venv\Scripts\activate` and copy `.env.example` to `.env` using your file manager or `copy .env.example .env`.

Edit `.env` with your own service settings:

| Setting | Purpose |
| --- | --- |
| `MQTT_BROKER` | Broker hostname or IP address; MQTT uses port 1883. |
| `MQTT_USER`, `MQTT_PASSWORD` | Broker credentials; required by the master. |
| `INFLUX_URL` | InfluxDB server URL. |
| `INFLUX_TOKEN`, `INFLUX_ORG`, `INFLUX_BUCKET` | Telemetry storage credentials and destination. |
| `TELEGRAM_TOKEN`, `TELEGRAM_CHAT_ID` | Optional bot token and destination chat; leave empty to disable notifications. |
| `OWM_API_KEY` | Optional OpenWeather key; leave empty to disable outdoor-temperature requests. |

The default weather location and sunrise/sunset coordinates refer to Milan, Italy. To use a different location, change `OWM_CITY`, `LATITUDE`, and `LONGITUDE` in `master_node.py`.

Start the master:

```bash
python master_node.py
```

Open **http://localhost:5000** on the computer running the master. The dashboard depends on the Flask server and should not be opened as a standalone HTML page. Persistent telemetry requires valid InfluxDB settings. The master validates MQTT settings before connecting; the broker must be running and accessible.

## Set up the ESP8266 nodes

1. Install ESP8266 board support and the libraries used by the selected sketch.
2. Wire the sensors and peripherals according to that sketch's pin definitions.
3. Open and upload each `.ino` file from its matching directory to the corresponding board.
4. On first setup, use the WiFiManager configuration portal to enter the Wi-Fi and MQTT settings and the node name. The portal has a 180-second timeout.
5. Configure all nodes with the same broker and credentials used by the master. MQTT settings are saved in EEPROM.
6. Start the master and check that the nodes appear in the dashboard and publish measurements.

### Sensor pin assignments

Pin names below follow the aliases used in the sketches; check their mapping for your particular ESP8266 board.

| Node | Connections defined in the sketch |
| --- | --- |
| Teca 1 | Button `D5`, infrared door sensor `D7`, alarm LED `D8`, photoresistor `A0`, DHT11 `D6`, I2C LCD at address `0x27` with 16 columns and 2 rows. |
| Teca 2 | Alarm LED `D2`, DHT11 `D1`. |
| Teca 3 | Ultrasonic trigger `D1`, echo `D2`, alarm LED `D4`. |

These assignments are a reference to the code, not a complete electrical wiring diagram. Match power supplies and signal levels to the boards and sensors used.

## Master API

| Endpoint | Purpose |
| --- | --- |
| `GET /` | Serve the dashboard. |
| `GET /api/dati` | Return current node state, telemetry, and external information. |
| `POST /api/configura/<teca_id>` | Forward configuration parameters to a discovered node. |
| `GET /thing-description` | Return the master's Web of Things Thing Description. |

The devices expose `POST /api/configura` for configuration. Their advertised Thing Descriptions provide the MQTT topics and HTTP addresses used by the master.

## Credentials and deployment scope

Use your own credentials and keep `.env` local. Publish `.env.example`, which contains no credentials. When uploading files through the GitHub website, select files explicitly: `.gitignore` does not filter a browser upload.

If any credential has already been exposed, revoke or replace it at the service provider. Removing it from the current source does not invalidate it or remove it from earlier commits. After changing the broker password, update the configuration stored on every ESP8266 node.

This implementation is a prototype intended for a trusted local network. MQTT uses an unencrypted connection, and the HTTP configuration endpoints do not authenticate callers. Internet deployment would require transport security, authentication, and appropriate network restrictions.

## Validation status

Python syntax and Git ignore rules have been checked. The current configuration changes have not been verified with connected boards or live service credentials. To validate a deployment, check sensor readings, node discovery and reconnection, dashboard commands, database writes, and any enabled Telegram notifications.
