#include <DHT.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h> 
#include <WiFiManager.h>      
#include <ArduinoJson.h>  
#include <MQTT.h>
#include <EEPROM.h>

// Definizione pin e costanti
#define LEDPIN D2           
#define DHTPIN D1         
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE); 

// Parametri di configurazione di default
float sogliaMaxTemp = 20.0;
bool sistemaAttivo = true;
bool allarmeAttivo = false;   

// Variabili per identificare univocamente la teca
String deviceID = "";
String topicTelemetry = "";
String topicEvents = "";
String topicStatus = "";
String topicDiscovery = "epms/network/join";
float temperaturaEsterna = -999.0;
String orarioAlba = "";
String orarioTramonto = "";

// Intervalli di tempo
unsigned long previousMillis = 0;     
const long intervallo = 5000;         
unsigned long previousDiscoveryMillis = 0;
const long intervalloDiscovery = 60000; 
unsigned long previousWeatherMillis = 0;
const long intervalloWeather = 43200000;

// variabili per la configurazione MQTT dinamica
char mqtt_server[40] = ""; 
char mqtt_user[40] = "";
char mqtt_pass[40] = "";
char nome_teca[40] = "Nuova Teca";
bool salvataggioConfigurazione = false;
void salvaConfigurazione() {
  salvataggioConfigurazione = true;
}

// Connessione MQTT e WiFi
#define MQTT_BUFFER_SIZE 4096
MQTTClient mqttClient(MQTT_BUFFER_SIZE);
WiFiClient networkClient;
ESP8266WebServer server(80); 

// Funzioni per la gestione dell'EEPROM
void scriviStringaEEPROM(int startAddr, String data) {
  for (int i = 0; i < data.length(); ++i) {
    EEPROM.write(startAddr + i, data[i]);
  }
  EEPROM.write(startAddr + data.length(), '\0');
}
String leggiStringaEEPROM(int startAddr) {
  String data = "";
  for (int i = 0; i < 40; i++) {
    char c = char(EEPROM.read(startAddr + i));
    if (c == '\0' || (byte)c == 255) break;
    data += c;
  }
  return data;
}

void messageReceived(String &topic, String &payload) {
  if (topic == "epms/weather/response") {
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, payload) && doc.containsKey("temperatura_esterna")) {
      temperaturaEsterna = doc["temperatura_esterna"];
    }
  }

  if (topic == "epms/sun/response") {
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, payload) && doc.containsKey("alba")) {
      orarioAlba = doc["alba"].as<String>();
      orarioTramonto = doc["tramonto"].as<String>();
    }
  }
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  delay(2000);
  Serial.println(F("\n\nAvvio della teca\n"));
  
  ricavaTopic();
  configuraPinI_O();
  inizializzaSensoriTemperatura();
  connettiWiFi();
  
  mqttClient.begin(mqtt_server, 1883, networkClient);
  mqttClient.onMessage(messageReceived);
  connessioneMQTT();
  
  // Chiamata api per la configurazione da remoto
  server.on("/api/configura", HTTP_POST, gestisciConfigurazioneREST);
  server.begin();
  
  Serial.println(F("Sistema pronto"));
}

void loop() {

  // gestione delle richieste REST
  server.handleClient(); 
  
  // Gestione connessione MQTT
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    static unsigned long ultimoTentativo = 0;
    if (millis() - ultimoTentativo > 5000) { 
      ultimoTentativo = millis();
      connessioneMQTT();
    }
  }
  mqttClient.loop();

  // Invio periodico della Thing description in modo che il master possa avere sempre le informazioni aggiornate 
  if (mqttClient.connected() && millis() - previousDiscoveryMillis >= intervalloDiscovery) {
    previousDiscoveryMillis = millis();
    inviaThingDescription();
  }

  // Gestione del sistema in pausa
  if (!sistemaAttivo) {
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 5000) {
      lastPrint = millis();
      Serial.println(F("Teca in pausa"));
      
      if (mqttClient.connected()) {
        String payload = "{\"Stato_Sistema\": \"In pausa\"}";
        mqttClient.publish(topicTelemetry, payload);
      }
    }
    return;
  }
  
  // Ciclo di monitoraggio periodico ogni 5 secondi
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= intervallo) {
    previousMillis = currentMillis;
    eseguiCicloMonitoraggio();
  }

  if (currentMillis - previousWeatherMillis >= intervalloWeather) {
    previousWeatherMillis = currentMillis;
    if (mqttClient.connected()) {
      mqttClient.publish("epms/weather/request", "{}", false, 1);
    }
  }
}

void connettiWiFi() {

  WiFiManager wifiManager;

  if (EEPROM.read(500) == 42) {
    strcpy(mqtt_server, leggiStringaEEPROM(0).c_str());
    strcpy(mqtt_user, leggiStringaEEPROM(40).c_str());
    strcpy(mqtt_pass, leggiStringaEEPROM(80).c_str());
    strcpy(nome_teca, leggiStringaEEPROM(120).c_str());
    Serial.println(F("Dati MQTT recuperati dalla EEPROM."));
  } else {
    Serial.println(F("Nuova teca: nessuna configurazione trovata."));
    wifiManager.resetSettings();
  }

  wifiManager.setSaveConfigCallback(salvaConfigurazione);
  wifiManager.setConfigPortalTimeout(180);

  WiFiManagerParameter custom_mqtt_server("server", "Indirizzo IP Master", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_user("user", "Username MQTT", mqtt_user, 40);
  WiFiManagerParameter custom_mqtt_pass("pass", "Password MQTT", mqtt_pass, 40);
  WiFiManagerParameter custom_nome_teca("nome", "Nome Teca", nome_teca, 40);

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);
  wifiManager.addParameter(&custom_nome_teca);

  String reteWifiNome = "EPMS_" + deviceID + "_Config";
  if (!wifiManager.autoConnect(reteWifiNome.c_str())) {
    Serial.println(F("Timeout. Riavvio..."));
    delay(3000);
    ESP.restart(); 
  }

  if (salvataggioConfigurazione) {
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_user, custom_mqtt_user.getValue());
    strcpy(mqtt_pass, custom_mqtt_pass.getValue());
    strcpy(nome_teca, custom_nome_teca.getValue());

    scriviStringaEEPROM(0, String(mqtt_server));
    scriviStringaEEPROM(40, String(mqtt_user));
    scriviStringaEEPROM(80, String(mqtt_pass));
    scriviStringaEEPROM(120, String(nome_teca));
    EEPROM.write(500, 42);
    
    if (EEPROM.commit()) {
      Serial.println(F("Configurazione salvata permanentemente!"));
    }
  }
  Serial.println(F("WiFi OK!"));
}

void connessioneMQTT() {
  Serial.println(F("\nTentativo di connessione MQTT"));
  Serial.print(F("IP Server : [")); Serial.print(mqtt_server); Serial.println(F("]"));
  Serial.print(F("Username  : [")); Serial.print(mqtt_user); Serial.println(F("]"));
  Serial.println(F("Password  : [hidden]"));
  Serial.print(F("Client ID : [")); Serial.print(deviceID); Serial.println(F("]"));

  mqttClient.setWill(topicStatus.c_str(), "{\"status\":\"offline\"}", true, 1);

  if (mqttClient.connect(deviceID.c_str(), mqtt_user, mqtt_pass)) {
    Serial.println(F("Connesso al Broker MQTT!"));
    mqttClient.publish(topicStatus, "{\"status\":\"online\"}", true, 1);
    mqttClient.subscribe("epms/weather/response");
    mqttClient.subscribe("epms/sun/response");
    mqttClient.publish("epms/weather/request", "{}", false, 1);
    mqttClient.publish("epms/sun/request", "{}", false, 1);
    previousWeatherMillis = millis();
    inviaThingDescription(); 
  } else {
    Serial.println(F("Connessione RIFIUTATA dal Broker!"));
  }
}

void gestisciConfigurazioneREST() {
  if (server.hasArg("plain")) {
    String payload = server.arg("plain");
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      if (doc.containsKey("max_t")) sogliaMaxTemp = doc["max_t"];
      if (doc.containsKey("active")) sistemaAttivo = doc["active"];
      
      Serial.println("Configurazione Aggiornata via REST su Teca " + deviceID + "!");
      server.send(200, "application/json", "{\"status\":\"Configurazione Teca " + deviceID + " applicata\"}");
      inviaThingDescription();
    } else {
      server.send(400, "application/json", "{\"error\":\"JSON non valido\"}");
    }
  } else {
    server.send(400, "application/json", "{\"error\":\"Nessun dato\"}");
  }
}

// Funzione per ricavare i topic MQTT e l'ID univoco della teca basandosi sul MAC address
void ricavaTopic() {
  WiFi.mode(WIFI_STA);
  String macAddress = WiFi.macAddress();
  macAddress.replace(":", ""); 
  deviceID = "teca_" + macAddress;
  topicTelemetry = "epms/" + deviceID + "/telemetry";
  topicEvents = "epms/" + deviceID + "/events";
  topicStatus = "epms/" + deviceID + "/status";
}

void configuraPinI_O() {
  pinMode(LEDPIN, OUTPUT);
  digitalWrite(LEDPIN, HIGH);
}

void inizializzaSensoriTemperatura() {
  dht.begin();
}

void leggiSensoriTemperatura(float &t, float &h) {
  t = dht.readTemperature();
  h = dht.readHumidity();
  
  if (isnan(t) || isnan(h)) { 
    t = 0; h = 0; 
    Serial.println(F("Errore lettura sensore DHT!"));
  }
}

void eseguiCicloMonitoraggio() {
  float temp, hum;
  leggiSensoriTemperatura(temp, hum);
  
  gestisciAllarmeTermico(temp);
  
  if (mqttClient.connected()) {
    String payload = "{";
    payload += "\"Stato_Sistema\":\"Attivo\",";
    payload += "\"temperatura\":" + String(temp, 1) + ",";
    payload += "\"umidita\":" + String(hum, 0) + ",";

    if (allarmeAttivo) {
      payload += "\"Stato_Temperatura\":\"Temperatura Alta\","; // La parola "Allarme" lo farà diventare rosso
    } else {
      payload += "\"Stato_Temperatura\":\"Temperatura Normale\","; // La parola "OK" lo farà diventare verde
    }

    if (temperaturaEsterna != -999.0) {
      if (temperaturaEsterna > 28.0) {
        payload += "\"Avviso_Esterno\":\"Temperatura esterna alta (" + String(temperaturaEsterna, 1) + " C), valutare modifiche\",";
      } else if (temperaturaEsterna < 15.0) {
        payload += "\"Avviso_Esterno\":\"Temperatura esterna bassa (" + String(temperaturaEsterna, 1) + " C), valutare modifiche\",";
      } else {
        payload += "\"Avviso_Esterno\":\"Temperatura esterna ok (" + String(temperaturaEsterna, 1) + " C)\",";
      }
    }

    payload += "\"Wifi-RSSI\":" + String(WiFi.RSSI());
    payload += "}";
    
    mqttClient.publish(topicTelemetry, payload);
  }
}

void gestisciAllarmeTermico(float temperaturaAttuale) {
  if (!allarmeAttivo && temperaturaAttuale > sogliaMaxTemp) {
    attivaAllarme();
  }
  else if (allarmeAttivo && temperaturaAttuale <= sogliaMaxTemp) {
    disattivaAllarme();
  }
}

void attivaAllarme() {
  allarmeAttivo = true;
  digitalWrite(LEDPIN, LOW); 
  Serial.println("!!! ALLARME TECA " + deviceID + ": Temperatura Troppo Alta !!!");
  
  if (mqttClient.connected()) {
    mqttClient.publish(topicEvents, "{\"Stato_Temperatura\":\"Temperatura Alta\"}", false, 1);
  }
}

void disattivaAllarme() {
  allarmeAttivo = false;
  digitalWrite(LEDPIN, HIGH); 
  Serial.println("Allarme Teca " + deviceID + " rientrato. Temperatura OK.");
  
  if (mqttClient.connected()) {
    mqttClient.publish(topicEvents, "{\"Stato_Temperatura\":\"OK\"}", false, 1);
  }
}

void inviaThingDescription() {
  String ip = WiFi.localIP().toString();
  
  String td = "{";
  td += "\"@context\":[\"https://www.w3.org/2022/wot/td/v1.1\",{\"@language\":\"it\"}],";
  td += "\"id\":\"urn:epms:" + deviceID + "\",";
  td += "\"title\": \"" + String(nome_teca) + "\",";
  td += "\"description\":\"Nodo sensore EPMS controllabile da remoto\",";
  td += "\"securityDefinitions\":{\"mqtt_basic_sc\":{\"scheme\":\"basic\"},\"nosec_sc\":{\"scheme\":\"nosec\"}},";
  td += "\"security\":\"mqtt_basic_sc\",";
  td += "\"properties\":{";
  td += "\"telemetria\":{\"type\":\"object\",\"readOnly\":true,\"properties\":{";
  td += "\"temperatura\":{\"type\":\"number\",\"unit\":\"C\"},";
  td += "\"umidita\":{\"type\":\"number\",\"unit\":\"%\"},";
  td += "\"Stato_Temperatura\":{\"type\":\"string\"},";
  td += "\"Avviso_Esterno\":{\"type\":\"string\"},";
  td += "\"Stato_Sistema\":{\"type\":\"string\"},";
  td += "\"Wifi-RSSI\":{\"type\":\"number\",\"unit\":\"dBm\"}";
  td += "},\"forms\":[{\"op\":\"observeproperty\",\"href\":\"mqtt://" + String(mqtt_server) + "/" + String(topicTelemetry) + "\",\"contentType\":\"application/json\"}]}";
  td += "},";
  td += "\"actions\":{";
  td += "\"configura\":{";
  td += "\"input\":{\"type\":\"object\",\"properties\":{";
  td += "\"max_t\":{\"type\":\"number\",\"title\":\"Soglia Temp Max\",\"unit\":\"C\",\"value\":" + String(sogliaMaxTemp, 1) + "},";
  td += "\"active\":{\"type\":\"boolean\",\"title\":\"Sistema Attivo\",\"value\":" + (sistemaAttivo ? String("true") : String("false")) + "}";
  td += "}},";
  td += "\"forms\":[{\"op\":\"invokeaction\",\"href\":\"http://" + ip + "/api/configura\",\"contentType\":\"application/json\",\"security\":\"nosec_sc\"}]";
  td += "}";
  td += "},";
  td += "\"events\":{";
  td += "\"allarme_temp\":{\"data\":{\"type\":\"object\",\"properties\":{\"Stato_Temperatura\":{\"type\":\"string\"}}},";
  td += "\"forms\":[{\"op\":\"subscribeevent\",\"href\":\"mqtt://" + String(mqtt_server) + "/" + String(topicEvents) + "\",\"contentType\":\"application/json\"}]}";
  td += "}";
  td += "}"; 

  mqttClient.publish(topicDiscovery, td, true, 1);
}
