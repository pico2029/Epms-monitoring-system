import paho.mqtt.client as mqtt
from flask import Flask, jsonify, request, render_template
import requests
import threading
import json
import time
import queue
import os
from pathlib import Path
from dotenv import load_dotenv
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS

load_dotenv(Path(__file__).with_name(".env"))

INFLUX_URL = os.getenv("INFLUX_URL", "")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN", "")
INFLUX_ORG = os.getenv("INFLUX_ORG", "")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "")

# Variabili globali di configurazione MQTT e Flask
MQTT_BROKER = os.getenv("MQTT_BROKER", "")
FLASK_PORT = 5000
MQTT_USER = os.getenv("MQTT_USER", "")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD", "")
TELEGRAM_TOKEN = os.getenv("TELEGRAM_TOKEN", "")
TELEGRAM_CHAT_ID = os.getenv("TELEGRAM_CHAT_ID", "")
TELEGRAM_COOLDOWN = 60
TELEGRAM_JOIN_COOLDOWN = 300
TELEGRAM_TELEMETRY_INTERVAL = 5
TELEGRAM_EVENT_COOLDOWN = 5
TELEGRAM_JOIN_BATCH_DELAY = 2
TELEGRAM_FIRST_TELEMETRY_DELAY = 5
OWM_API_KEY = os.getenv("OWM_API_KEY", "")
OWM_CITY = "Milano,IT"
LATITUDE = 45.4642
LONGITUDE = 9.1900
API_CACHE_FILE = "api_cache.json"

# Strutture dati per tenere traccia delle teche, telemetria ed eventi
teche_connesse = {}  
dati_telemetria = {}
eventi_teche = {}
last_seen = {}
last_alert = {}
telegram_queue = queue.Queue()
join_in_attesa = {}
join_lock = threading.Lock()
join_timer = None
telemetria_abilitata_da = {}
telemetria_lock = threading.Lock()
ultima_temperatura_esterna = None
ultimo_aggiornamento_meteo = 0
meteo_update = 3600
dati_sole = None
ultimo_aggiornamento_sole = 0
sole_update = 43200
api_lock = threading.Lock()


try:
    client_influx = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
    write_api = client_influx.write_api(write_options=SYNCHRONOUS)
    print("[INFLUXDB] Motore di salvataggio Database: ATTIVO")
except Exception as e:
    print(f"[INFLUXDB] Attenzione, errore avvio database: {e}")

app = Flask(__name__, template_folder=".")


def invia_telegram(messaggio):
    if not TELEGRAM_TOKEN or not TELEGRAM_CHAT_ID:
        return

    telegram_queue.put(messaggio)


def telegram_worker():
    while True:
        messaggio = telegram_queue.get()
        try:
            invia_telegram_http(messaggio)
        finally:
            telegram_queue.task_done()


def invia_telegram_http(messaggio):
    if not TELEGRAM_TOKEN or not TELEGRAM_CHAT_ID:
        return

    url = f"https://api.telegram.org/bot{TELEGRAM_TOKEN}/sendMessage"
    payload = {
        "chat_id": TELEGRAM_CHAT_ID,
        "text": messaggio
    }

    try:
        requests.post(url, json=payload, timeout=5)
    except requests.exceptions.RequestException as e:
        print(f"[TELEGRAM] Errore invio messaggio: {e}")


def notifica_join(teca_id):
    global join_timer

    chiave_alert = f"join_{teca_id}"
    ora = time.time()
    ultimo_alert = last_alert.get(chiave_alert, 0)

    if ora - ultimo_alert < TELEGRAM_JOIN_COOLDOWN:
        return

    last_alert[chiave_alert] = ora

    with join_lock:
        join_in_attesa[teca_id] = nome_teca(teca_id)

        if join_timer is None or not join_timer.is_alive():
            join_timer = threading.Timer(TELEGRAM_JOIN_BATCH_DELAY, invia_join_batch)
            join_timer.daemon = True
            join_timer.start()


def invia_join_batch():
    global join_timer

    with join_lock:
        teche = list(join_in_attesa.values())
        join_in_attesa.clear()
        join_timer = None

    if not teche:
        return

    if len(teche) == 1:
        messaggio = f"[CONNESSIONE]\nTeca: {teche[0]}\nStato: online"
    else:
        lista_teche = "\n".join(f"- {teca}" for teca in teche)
        messaggio = f"[CONNESSIONE]\nTeche online:\n{lista_teche}"

    invia_telegram(messaggio)


def alert_con_cooldown(chiave_alert, messaggio, cooldown=TELEGRAM_COOLDOWN):
    ora = time.time()
    ultimo_alert = last_alert.get(chiave_alert, 0)

    if ora - ultimo_alert >= cooldown:
        invia_telegram(messaggio)
        last_alert[chiave_alert] = ora


def carica_cache_api():
    global ultima_temperatura_esterna, ultimo_aggiornamento_meteo
    global dati_sole, ultimo_aggiornamento_sole

    try:
        with open(API_CACHE_FILE, "r", encoding="utf-8") as file_cache:
            cache = json.load(file_cache)
    except FileNotFoundError:
        return
    except (OSError, json.JSONDecodeError) as e:
        print(f"[CACHE] Errore lettura cache API: {e}")
        return

    ultima_temperatura_esterna = cache.get("temperatura_esterna")
    ultimo_aggiornamento_meteo = float(cache.get("ultimo_aggiornamento_meteo", 0) or 0)
    dati_sole = cache.get("sole")
    ultimo_aggiornamento_sole = float(cache.get("ultimo_aggiornamento_sole", 0) or 0)
    print("[CACHE] Dati API esterne caricati da file")


def salva_cache_api():
    cache = {
        "temperatura_esterna": ultima_temperatura_esterna,
        "ultimo_aggiornamento_meteo": ultimo_aggiornamento_meteo,
        "sole": dati_sole,
        "ultimo_aggiornamento_sole": ultimo_aggiornamento_sole
    }
    file_temporaneo = f"{API_CACHE_FILE}.tmp"

    try:
        with open(file_temporaneo, "w", encoding="utf-8") as file_cache:
            json.dump(cache, file_cache, ensure_ascii=False, indent=2)
        os.replace(file_temporaneo, API_CACHE_FILE)
    except OSError as e:
        print(f"[CACHE] Errore salvataggio cache API: {e}")


def get_temperatura_esterna():
    global ultima_temperatura_esterna, ultimo_aggiornamento_meteo

    with api_lock:
        ora = time.time()

        if ora - ultimo_aggiornamento_meteo >= meteo_update or ultima_temperatura_esterna is None:
            if not OWM_API_KEY or OWM_API_KEY == "INSERISCI_LA_TUA_API_KEY":
                return None

            url = f"https://api.openweathermap.org/data/2.5/weather?q={OWM_CITY}&appid={OWM_API_KEY}&units=metric"
            try:
                risposta = requests.get(url, timeout=5)
                if risposta.status_code == 200:
                    dati = risposta.json()
                    ultima_temperatura_esterna = dati["main"]["temp"]
                    ultimo_aggiornamento_meteo = ora
                    salva_cache_api()
                    print(f"[METEO] Temperatura esterna a {OWM_CITY} aggiornata: {ultima_temperatura_esterna} C")
                else:
                    print(f"[METEO] Errore API OWM: {risposta.status_code}")
            except requests.exceptions.RequestException as e:
                print(f"[METEO] Errore richiesta HTTP a OWM: {e}")

    return ultima_temperatura_esterna


def get_alba_tramonto():
    global dati_sole, ultimo_aggiornamento_sole

    with api_lock:
        ora = time.time()

        if ora - ultimo_aggiornamento_sole >= sole_update or dati_sole is None:
            url = f"https://api.sunrise-sunset.org/json?lat={LATITUDE}&lng={LONGITUDE}&tzid=Europe/Rome&formatted=1"
            try:
                risposta = requests.get(url, timeout=5)
                if risposta.status_code == 200:
                    dati = risposta.json()
                    if dati.get("status") == "OK":
                        dati_sole = {
                            "alba": dati["results"]["sunrise"],
                            "tramonto": dati["results"]["sunset"]
                        }
                        ultimo_aggiornamento_sole = ora
                        salva_cache_api()
                        print(f"[SOLE] Dati aggiornati: Alba {dati_sole['alba']} - Tramonto {dati_sole['tramonto']}")
                else:
                    print(f"[SOLE] Errore API Sunrise-Sunset: {risposta.status_code}")
            except requests.exceptions.RequestException as e:
                print(f"[SOLE] Errore richiesta HTTP a Sunrise-Sunset: {e}")

    return dati_sole


def aggiorna_dati_esterni_periodicamente():
    print("[BACKGROUND] Avvio thread per aggiornamento dati esterni (meteo, sole).")

    while True:
        get_temperatura_esterna()
        get_alba_tramonto()
        time.sleep(300)


def estrai_teca_id(topic):
    parti = topic.split("/")

    if len(parti) >= 3 and parti[1] in ["nodes", "teche"]:
        return parti[2]

    if len(parti) >= 2:
        return parti[1]

    return None


def nome_teca(teca_id):
    teca = teche_connesse.get(teca_id, {})
    titolo = teca.get("title")

    if titolo:
        return titolo

    return teca_id


def registra_teca_minima(teca_id):
    if teca_id not in teche_connesse:
        teche_connesse[teca_id] = {
            "id": f"urn:epms:{teca_id}",
            "title": teca_id,
            "online": True,
            "actions": {}
        }
        telemetria_abilitata_da[teca_id] = time.time() + TELEGRAM_FIRST_TELEMETRY_DELAY

    return teche_connesse[teca_id]


def formatta_label(chiave):
    etichette = {
        "temperatura": "Temperatura",
        "umidita": "Umidita",
        "luce": "Luce",
        "Distanza_Acqua_cm": "Distanza acqua",
        "Wifi-RSSI": "WiFi RSSI",
        "Stato_Sistema": "Sistema",
        "Stato_Temperatura": "Stato temperatura",
        "Stato_Vasca": "Stato vasca",
        "Allarme_Porta": "Porta"
    }

    return etichette.get(chiave, chiave.replace("_", " ").replace("-", " "))


def formatta_valore(chiave, valore):
    unita = {
        "temperatura": " C",
        "umidita": " %",
        "luce": " %",
        "Distanza_Acqua_cm": " cm",
        "Wifi-RSSI": " dBm"
    }

    return f"{valore}{unita.get(chiave, '')}"


def formatta_dati_telegram(dati):
    righe = []

    for chiave, valore in dati.items():
        if chiave != "rssi":
            if "PORTA" in chiave.upper():
                continue
            righe.append(f"- {formatta_label(chiave)}: {formatta_valore(chiave, valore)}")

    return "\n".join(righe)


def formatta_evento_telegram(dati):
    righe = []

    for chiave, valore in dati.items():
        righe.append(f"- {formatta_label(chiave)}: {valore}")

    return "\n".join(righe)


def marca_teca_offline(teca_id, motivo="offline"):
    teca = teche_connesse.get(teca_id)

    if teca is None:
        return

    if teca.get("online") is False and last_seen.get(teca_id) == -1:
        return

    teca["online"] = False
    last_seen[teca_id] = -1

    with telemetria_lock:
        dati_telemetria.pop(teca_id, None)

    nome = nome_teca(teca_id)
    print(f"[MQTT] {nome} Offline")
    alert_con_cooldown(
        f"offline_{teca_id}",
        f"[DISCONNESSIONE]\nTeca: {nome}\nStato: Offline",
        TELEGRAM_COOLDOWN
    )


def invia_telemetrie_periodiche():
    prossimo_invio = time.monotonic()

    while True:
        prossimo_invio += TELEGRAM_TELEMETRY_INTERVAL
        time.sleep(max(0, prossimo_invio - time.monotonic()))
        ora = time.time()

        with telemetria_lock:
            snapshot = {
                teca_id: dict(dati)
                for teca_id, dati in dati_telemetria.items()
                if teca_id in teche_connesse
                and teche_connesse[teca_id].get("online", True)
                and ora >= telemetria_abilitata_da.get(teca_id, 0)
            }

        for teca_id, dati in snapshot.items():
            corpo_messaggio = formatta_dati_telegram(dati)

            if corpo_messaggio:
                invia_telegram(f"[TELEMETRIA]\nTeca: {nome_teca(teca_id)}\n\n{corpo_messaggio}")


carica_cache_api()

threading.Thread(target=telegram_worker, daemon=True).start()
threading.Thread(target=aggiorna_dati_esterni_periodicamente, daemon=True).start()
threading.Thread(target=invia_telemetrie_periodiche, daemon=True).start()


# Funzione che permette di iscriversi ai canali corretti una volta connessi al broker MQTT
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[MQTT] Connesso al Broker {MQTT_BROKER} con successo!")
        client.subscribe("epms/#", qos=1)
    else:
        print(f"[MQTT] ERRORE DI CONNESSIONE! Codice rifiuto: {rc}")

# Funzione che gestisce i messaggi ricevuti dal broker MQTT
def on_message(client, userdata, msg):
    topic = msg.topic
    payload_raw = msg.payload.decode('utf-8')

    if topic == "epms/weather/request":
        temp_esterna = get_temperatura_esterna()
        if temp_esterna is not None:
            client.publish("epms/weather/response", json.dumps({"temperatura_esterna": temp_esterna}))
        return

    if topic == "epms/sun/request":
        sole = get_alba_tramonto()
        if sole:
            client.publish("epms/sun/response", json.dumps(sole))
        return

    parti_topic = topic.split("/")
    if len(parti_topic) >= 3 and parti_topic[2] == "status":
        teca_id = parti_topic[1]
        try:
            stato_payload = json.loads(payload_raw)
            stato = str(stato_payload.get("status", "")).strip().lower()
        except json.JSONDecodeError:
            stato = payload_raw.strip().lower()

        teca = registra_teca_minima(teca_id)

        if stato == "offline":
            marca_teca_offline(teca_id, "offline (LWT)")
        elif stato == "online":
            teca["online"] = True
            last_seen[teca_id] = time.time()
            print(f"[MQTT] Teca {teca_id} tornata online")
        return

    if getattr(msg, "retain", False) and ("telemetry" in topic or "events" in topic):
        return

    try:
        dati = json.loads(payload_raw)
    except Exception as e:
        print(f"JSON non valido! Motivo: {e}")
        return 
    
    # Rileva una nuova teca che si unisce alla rete (messaggio di discovery)
    if topic == "epms/network/join":
        raw_id = dati.get("id", "Sconosciuto")
        teca_id = raw_id.rsplit(":", 1)[-1] if raw_id.startswith("urn:epms:") else raw_id
        gia_connessa = teca_id in teche_connesse
        teche_connesse[teca_id] = dati
        teche_connesse[teca_id]["online"] = True
        last_seen[teca_id] = time.time()
        telemetria_abilitata_da[teca_id] = time.time() + TELEGRAM_FIRST_TELEMETRY_DELAY
        print(f"Teca Rilevata -> {dati.get('title')}")
        if not gia_connessa:
            notifica_join(teca_id)
        return

    teca_id = estrai_teca_id(topic)
    if not teca_id:
        return

    if teca_id not in teche_connesse:
        if "telemetry" not in topic and "events" not in topic:
            return
        registra_teca_minima(teca_id)
    elif teche_connesse[teca_id].get("online") is False and ("telemetry" in topic or "events" in topic):
        return

    last_seen[teca_id] = time.time()
    teche_connesse[teca_id]["online"] = True

    if "telemetry" in topic or "events" in topic:
        if "telemetry" in topic:
            with telemetria_lock:
                if teca_id not in dati_telemetria:
                    dati_telemetria[teca_id] = {}
                dati_telemetria[teca_id].update(dati)

        if "events" in topic:
            eventi_teche[teca_id] = dati
            corpo_evento = formatta_evento_telegram(dati)
            evento_key = json.dumps(dati, sort_keys=True)

            if corpo_evento:
                alert_con_cooldown(
                    f"event_{teca_id}_{evento_key}",
                    f"[EVENTO]\nTeca: {nome_teca(teca_id)}\n\n{corpo_evento}",
                    TELEGRAM_EVENT_COOLDOWN
                )
        
        print(f"Nuovi dati ricevuti da {teca_id} -> Salvataggio in DB...")

        if 'write_api' in globals():
            try:
                punto = Point("telemetria").tag("teca", teca_id)

                for chiave, valore in dati.items():
                    
                    if isinstance(valore, (int, float)):
                        punto.field(chiave, float(valore))
                        
                    elif isinstance(valore, str):
                        punto.field(chiave, valore)

                # Scrive i dati su InfluxDB
                write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=punto)
                
            except Exception as e:
                print(f"[Errore DB] Impossibile salvare i dati di {teca_id}: {e}")

# Configurazione del client MQTT e avvio del loop in un thread separato
missing_mqtt = [name for name in ("MQTT_BROKER", "MQTT_USER", "MQTT_PASSWORD") if not os.getenv(name)]
if missing_mqtt:
    raise RuntimeError(f"Configurazione MQTT mancante in .env: {', '.join(missing_mqtt)}")

mqtt_client = mqtt.Client()
mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message
mqtt_client.connect(MQTT_BROKER, 1883, 60)

threading.Thread(target=mqtt_client.loop_forever, daemon=True).start()


# Endpoint per fornire i dati attuali delle teche e della telemetria
@app.route("/api/dati")
def get_dati():
    return jsonify({
        "teche": teche_connesse, 
        "telemetria": dati_telemetria,
        "temperatura_esterna": {"temperatura": ultima_temperatura_esterna, "citta": OWM_CITY} if ultima_temperatura_esterna is not None else {},
        "sole": dati_sole if dati_sole is not None else {}
    })

# Endpoint per ricevere i comandi di configurazione dalla dashboard e inoltrarli alla teca corrispondente
@app.route("/api/configura/<teca_id>", methods=["POST"])
def configura(teca_id): 
    if teca_id not in teche_connesse:
        return jsonify({"error": "Teca non trovata"}), 404

    nuovi_parametri = request.json
    
    try:
        url_teca = teche_connesse[teca_id]["actions"]["configura"]["forms"][0]["href"]
    except KeyError:
        return jsonify({"error": "La teca non ha un URL di configurazione valido"}), 400

    try:
        risposta = requests.post(url_teca, json=nuovi_parametri, timeout=5)
        if risposta.status_code == 200:
            return jsonify({"status": "OK", "msg": "Configurazione applicata!"})
        else:
            return jsonify({"error": f"Errore ESP: {risposta.status_code}"}), 500
    except requests.exceptions.RequestException as e:
        return jsonify({"error": "Teca irraggiungibile. Controlla il WiFi."}), 503


@app.route("/thing-description")
def master_td():
    base = request.host_url.rstrip("/")
    return jsonify({
        "@context": "https://www.w3.org/2022/wot/td/v1.1",
        "id": "urn:epms:master-node",
        "title": "EPMS Master Node",
        "description": "Nodo master per discovery, dashboard e controllo remoto delle teche",
        "securityDefinitions": {"nosec_sc": {"scheme": "nosec"}},
        "security": "nosec_sc",
        "properties": {
            "nodes": {
                "type": "object",
                "readOnly": True,
                "forms": [{
                    "op": "readproperty",
                    "href": f"{base}/api/dati",
                    "contentType": "application/json"
                }]
            }
        },
        "actions": {
            "configureNode": {
                "input": {"type": "object"},
                "uriVariables": {"teca_id": {"type": "string"}},
                "forms": [{
                    "op": "invokeaction",
                    "href": f"{base}/api/configura/{{teca_id}}",
                    "contentType": "application/json"
                }]
            }
        }
    })



@app.route("/")
def index():
    return render_template("dashboard.html")

if __name__ == "__main__":
    print(f"Server web locale attivo sulla porta {FLASK_PORT} ")
    app.run(host="0.0.0.0", port=FLASK_PORT, debug=False, use_reloader=False)
