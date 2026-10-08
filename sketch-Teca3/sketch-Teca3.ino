#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h> 
#include <WiFiManager.h>      
#include <ArduinoJson.h>      
#include <MQTT.h> 
#include <EEPROM.h>

// Definizione pin e costanti
#define TRIGGER D1   
#define ECHO D2    
#define LEDPIN D4    
#define SOUND_VELOCITY 0.034   

// Parametri di configurazione di default
float distanzaIdeale = 15.0;
float tolleranza = 2.0; 
bool sistemaAttivo = true;
bool allarmeAttivo = false;   

// Variabili per identificare univocamente la teca
String deviceID = "";
String topicTelemetry = "";
String topicEvents = "";
String topicStatus = "";
String topicDiscovery = "epms/network/join";

// Intervalli di tempo
unsigned long previousMillis = 0;     
const long intervallo = 5000;
unsigned long previousDiscoveryMillis = 0;
const long intervalloDiscovery = 60000;

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

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  delay(2000);
  Serial.println(F("\n\nAvvio della teca\n"));

  ricavaTopic();
  configuraPinI_O();
  connettiWiFi();
  
  mqttClient.begin(mqtt_server, 1883, networkClient);
  connessioneMQTT();
  
  // Chiamata api per la configurazione da remoto
  server.on("/api/configura", HTTP_POST, gestisciConfigurazioneREST);
  server.begin();
  
  Serial.println(F("Sistema Pronto!"));
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
      if (doc.containsKey("distanza")) distanzaIdeale = doc["distanza"];
      if (doc.containsKey("tolleranza")) tolleranza = doc["tolleranza"];
      if (doc.containsKey("active")) sistemaAttivo = doc["active"];
      
      Serial.println("Configurazione Aggiornata su teca " + deviceID);
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
  pinMode(ECHO, INPUT);
  pinMode(TRIGGER, OUTPUT);
  pinMode(LEDPIN, OUTPUT);
  digitalWrite(LEDPIN, HIGH);
}

// Funzione per leggere la distanza tramite il sensore ad ultrasuoni
float leggiDistanza() {
  digitalWrite(TRIGGER, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIGGER, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIGGER, LOW);

  long travelTime = pulseIn(ECHO, HIGH);
  return travelTime * SOUND_VELOCITY / 2.0;
}

void eseguiCicloMonitoraggio() {
  float distanzaAttuale = leggiDistanza();
  Serial.printf("Distanza Acqua: %.1f cm\n", distanzaAttuale);
  
  gestisciAllarmeLivello(distanzaAttuale);
  
  if (mqttClient.connected()) {
    String payload = "{";
    payload += "\"Stato_Sistema\":\"Attivo\",";
    payload += "\"Distanza_Acqua_cm\":" + String(distanzaAttuale, 1) + ",";
    
    // Manda lo stato formattato per la pagina Web
    if (allarmeAttivo) {
      if(distanzaAttuale < distanzaIdeale) {
        payload += "\"Stato_Vasca\":\"Livello dell'acqua alto\",";
      } else {
        payload += "\"Stato_Vasca\":\"Livello dell'acqua basso\",";
      }
    } else {
      payload += "\"Stato_Vasca\":\"Livello dell'acqua corretto\",";
    }
    
    payload += "\"Wifi-RSSI\":" + String(WiFi.RSSI());
    payload += "}";
    
    mqttClient.publish(topicTelemetry, payload);
  }
}

void gestisciAllarmeLivello(float dist) {
  bool fuoriRange = (dist > (distanzaIdeale + tolleranza)) || (dist < (distanzaIdeale - tolleranza));
  
  if (!allarmeAttivo && fuoriRange) {
    allarmeAttivo = true;
    digitalWrite(LEDPIN, LOW);
    Serial.println(F("!!! ALLARME: Livello Acqua Anomalo !!!"));
    if (mqttClient.connected()) mqttClient.publish(topicEvents, "{\"Stato_Vasca\": \"Livello dell'acqua anomalo\"}", false, 1);
  }
  else if (allarmeAttivo && !fuoriRange) {
    allarmeAttivo = false;
    digitalWrite(LEDPIN, HIGH);
    Serial.println(F("Livello dell'acqua corretto."));
    if (mqttClient.connected()) mqttClient.publish(topicEvents, "{\"Stato_Vasca\": \"Livello dell'acqua corretto\"}", false, 1);
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
  td += "\"Distanza_Acqua_cm\":{\"type\":\"number\",\"unit\":\"cm\"},";
  td += "\"Stato_Vasca\":{\"type\":\"string\"},";
  td += "\"Stato_Sistema\":{\"type\":\"string\"},";
  td += "\"Wifi-RSSI\":{\"type\":\"number\",\"unit\":\"dBm\"}";
  td += "},\"forms\":[{\"op\":\"observeproperty\",\"href\":\"mqtt://" + String(mqtt_server) + "/" + String(topicTelemetry) + "\",\"contentType\":\"application/json\"}]}";
  td += "},";
  td += "\"actions\":{";
  td += "\"configura\":{";
  td += "\"input\":{\"type\":\"object\",\"properties\":{";
  td += "\"distanza\":{\"type\":\"number\",\"title\":\"Distanza Target\",\"unit\":\"cm\",\"value\":" + String(distanzaIdeale, 1) + "},";
  td += "\"tolleranza\":{\"type\":\"number\",\"title\":\"Tolleranza\",\"unit\":\"cm\",\"value\":" + String(tolleranza, 1) + "},";
  td += "\"active\":{\"type\":\"boolean\",\"title\":\"Sistema Attivo\",\"value\":" + String(sistemaAttivo ? "true" : "false") + "}";
  td += "}},";
  td += "\"forms\":[{\"op\":\"invokeaction\",\"href\":\"http://" + ip + "/api/configura\",\"contentType\":\"application/json\",\"security\":\"nosec_sc\"}]";
  td += "}";
  td += "},";
  td += "\"events\":{";
  td += "\"allarme_livello\":{\"data\":{\"type\":\"object\",\"properties\":{\"Stato_Vasca\":{\"type\":\"string\"}}},";
  td += "\"forms\":[{\"op\":\"subscribeevent\",\"href\":\"mqtt://" + String(mqtt_server) + "/" + String(topicEvents) + "\",\"contentType\":\"application/json\"}]}";
  td += "}";
  td += "}"; 

  mqttClient.publish(topicDiscovery, td);
}
