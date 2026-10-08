#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h> 
#include <WiFiManager.h>      
#include <ArduinoJson.h>      
#include <MQTT.h>
#include <EEPROM.h>
#include <time.h>

// Definizione pin e costanti
#define BTNPIN D5           
#define IR_SENSOR D7        
#define LEDPIN D8           
#define PHOTORESISTOR A0    
#define DHTP1IN D6          
#define DHTTYPE DHT11
DHT dhtZ1 = DHT(DHTP1IN, DHTTYPE); 

#define DISPLAY_ADDR 0x27   
LiquidCrystal_I2C lcd(DISPLAY_ADDR, 16, 2);

// Parametri di configurazione di default
float sogliaMinTemp = 18.0;
float sogliaMaxTemp = 26.0;
float sogliaLucePerc = 58.0;
bool sistemaAttivo = true;
bool allarmeAttivo = false;
String statoTemperaturaPrecedente = "OK";
bool statoPrecedentePorta;            

// Intervalli di tempo
unsigned long previousMillis = 0;     
const long intervallo = 5000; 
unsigned long previousDiscoveryMillis = 0;
const long intervalloDiscovery = 60000;
float temperaturaEsterna = -999.0;
unsigned long previousWeatherMillis = 0;
const long intervalloWeather = 43200000;
String orarioAlba = "";
String orarioTramonto = "";

// Variabili per identificare univocamente la teca
String deviceID = "";
String topicTelemetry = "";
String topicEvents = "";
String topicStatus = "";
String topicDiscovery = "epms/network/join";

// Variabili per la configurazione MQTT dinamica
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
  inizializzaDisplay();
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
      visualizzaPausa();
      Serial.println(F("Teca in pausa"));
      
      
      if (mqttClient.connected()) {
        String payload = "{\"Stato_Sistema\": \"In pausa\"}";
        mqttClient.publish(topicTelemetry, payload);
      }
    }
    return;
  }
  
  gestisciAllarmeEReset();

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
      mqttClient.publish("epms/sun/request", "{}", false, 1);
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
  configTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
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
      if (doc.containsKey("min_t")) sogliaMinTemp = doc["min_t"];
      if (doc.containsKey("max_t")) sogliaMaxTemp = doc["max_t"];
      if (doc.containsKey("luce")) sogliaLucePerc = doc["luce"];
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
  pinMode(IR_SENSOR, INPUT);
  statoPrecedentePorta = digitalRead(IR_SENSOR);
  pinMode(BTNPIN, INPUT_PULLUP);
  pinMode(LEDPIN, OUTPUT);
  digitalWrite(LEDPIN, HIGH); 
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
}

void inizializzaDisplay() {
  Wire.begin();
  lcd.begin(16, 2);
  lcd.backlight();
}

void inizializzaSensoriTemperatura() {
  dhtZ1.begin(); 
}

void leggiSensoriTemperatura(float &t, float &h) {
  t = dhtZ1.readTemperature();
  h = dhtZ1.readHumidity();
  
  if (isnan(t) || isnan(h)) { 
    t = 0; h = 0; 
    Serial.println(F("Errore lettura sensore DHT!"));
  }
}

float leggiSensoreLuce() {
  int rawLdr = analogRead(PHOTORESISTOR);
  return (rawLdr / 1023.0) * 100.0;
}

int parseTimeStr(String timeStr) {
  int firstColon = timeStr.indexOf(':');
  int secondColon = timeStr.indexOf(':', firstColon + 1);
  int space = timeStr.indexOf(' ');

  if (firstColon == -1 || space == -1) return -1;

  int h = timeStr.substring(0, firstColon).toInt();
  int m = timeStr.substring(firstColon + 1, secondColon != -1 ? secondColon : space).toInt();
  String ampm = timeStr.substring(space + 1);

  if (ampm == "PM" && h != 12) h += 12;
  if (ampm == "AM" && h == 12) h = 0;

  return h * 3600 + m * 60;
}

void gestisciGiornoNotte() {
  if (orarioAlba == "" || orarioTramonto == "") return;

  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  if (timeinfo->tm_year < 120) return;

  int currentSecs = timeinfo->tm_hour * 3600 + timeinfo->tm_min * 60 + timeinfo->tm_sec;
  int secAlba = parseTimeStr(orarioAlba);
  int secTramonto = parseTimeStr(orarioTramonto);

  if (secAlba != -1 && secTramonto != -1) {
    if (currentSecs >= secTramonto || currentSecs < secAlba) {
      digitalWrite(LED_BUILTIN, LOW);
    } else {
      digitalWrite(LED_BUILTIN, HIGH);
    }
  }
}

void eseguiCicloMonitoraggio() {
  float temp, hum;
  leggiSensoriTemperatura(temp, hum);
  float luce = leggiSensoreLuce();

  String statoTemperatura = "OK";

  if (temp > sogliaMaxTemp) {
    statoTemperatura = "Temperatura Alta";
  } else if (temp < sogliaMinTemp) {
    statoTemperatura = "Temperatura Bassa";
  }
  
  if (!allarmeAttivo) {
    visualizzaDatiLocale(temp, hum, luce);
  }
  gestisciGiornoNotte();
  
  if (mqttClient.connected()) {
    String payload = "{";
    payload += "\"Stato_Sistema\":\"Attivo\",";
    payload += "\"temperatura\":" + String(temp, 1) + ",";
    payload += "\"Stato_Temperatura\":\"" + statoTemperatura + "\",";
    payload += "\"umidita\":" + String(hum, 0) + ",";
    payload += "\"luce\":" + String(luce, 1) + ",";
    
    if (allarmeAttivo) {
      payload += "\"Allarme_Porta\":\"APERTA\",";
    } else {
      payload += "\"Allarme_Porta\":\"CHIUSA\",";
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

    if (orarioAlba != "" && orarioTramonto != "") {
      payload += "\"Alba\":\"" + orarioAlba + "\",";
      payload += "\"Tramonto\":\"" + orarioTramonto + "\",";
    }
    
    payload += "\"Wifi-RSSI\":" + String(WiFi.RSSI());
    payload += "}";

    if (statoTemperatura != statoTemperaturaPrecedente) {
      mqttClient.publish(topicEvents, "{\"Stato_Temperatura\":\"" + statoTemperatura + "\"}", false, 1);
      statoTemperaturaPrecedente = statoTemperatura;
    }
    mqttClient.publish(topicTelemetry, payload);
  }
}

void gestisciAllarmeEReset() {
  bool statoAttualePorta = digitalRead(IR_SENSOR);
  
  if (!allarmeAttivo && statoAttualePorta == LOW && statoPrecedentePorta == HIGH) {
    allarmeAttivo = true;
    digitalWrite(LEDPIN, LOW); 
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("!!! PERICOLO !!!");
    lcd.setCursor(0, 1); lcd.print("  PORTA APERTA  ");
    
    // Invia l'evento istantaneo formattato al Master
    if (mqttClient.connected()) mqttClient.publish(topicEvents, "{\"Allarme_Porta\": \"🚨 APERTA\"}", false, 1);
  }
  statoPrecedentePorta = statoAttualePorta;
  
  if (allarmeAttivo && digitalRead(BTNPIN) == LOW) {
    allarmeAttivo = false;
    digitalWrite(LEDPIN, HIGH); 

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Allarme resettato");
    lcd.setCursor(0, 1); lcd.print("Sistema OK");
    
    if (mqttClient.connected()) mqttClient.publish(topicEvents, "{\"Allarme_Porta\": \"✅ CHIUSA\"}", false, 1);
  }
}

void visualizzaDatiLocale(float t1, float h1, float luce) {
  static int schermata = 0;
  lcd.clear();
  if (schermata == 0) {
    lcd.setCursor(0, 0); lcd.printf("Temp: %.1fC", t1);
    lcd.setCursor(0, 1); lcd.printf("Umid: %d%%", (int)h1);
    schermata = 1;
  } else if (schermata == 1) {
    lcd.setCursor(0, 0); lcd.printf("Luce: %.1f%%", luce);
    lcd.setCursor(0, 1); lcd.printf("MaxT:%.1f MinT:%.1f", sogliaMaxTemp, sogliaMinTemp);
    if (orarioAlba != "" && orarioTramonto != "") {
      schermata = 2;
    } else {
      schermata = 0;
    }
  } else if (schermata == 2) {
    lcd.setCursor(0, 0); lcd.print("Alba: " + orarioAlba);
    lcd.setCursor(0, 1); lcd.print("Tram: " + orarioTramonto);
    schermata = 0;
  }
}

void visualizzaPausa() {
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate >= 2000) {
    lastUpdate = millis();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("SISTEMA IN PAUSA");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
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
  td += "\"Stato_Temperatura\":{\"type\":\"string\"},";
  td += "\"umidita\":{\"type\":\"number\",\"unit\":\"%\"},";
  td += "\"luce\":{\"type\":\"number\",\"unit\":\"%\"},";
  td += "\"Allarme_Porta\":{\"type\":\"string\"},";
  td += "\"Avviso_Esterno\":{\"type\":\"string\"},";
  td += "\"Alba\":{\"type\":\"string\"},";
  td += "\"Tramonto\":{\"type\":\"string\"},";
  td += "\"Stato_Sistema\":{\"type\":\"string\"},";
  td += "\"Wifi-RSSI\":{\"type\":\"number\",\"unit\":\"dBm\"}";
  td += "},\"forms\":[{\"op\":\"observeproperty\",\"href\":\"mqtt://" + String(mqtt_server) + "/" + String(topicTelemetry) + "\",\"contentType\":\"application/json\"}]}";
  td += "},";
  td += "\"actions\":{";
  td += "\"configura\":{";
  td += "\"input\":{\"type\":\"object\",\"properties\":{";
  td += "\"min_t\":{\"type\":\"number\",\"title\":\"Soglia Temp Min\",\"unit\":\"C\",\"value\":" + String(sogliaMinTemp, 1) + "},";
  td += "\"max_t\":{\"type\":\"number\",\"title\":\"Soglia Temp Max\",\"unit\":\"C\",\"value\":" + String(sogliaMaxTemp, 1) + "},";
  td += "\"luce\":{\"type\":\"number\",\"title\":\"Soglia Luce\",\"unit\":\"%\",\"value\":" + String(sogliaLucePerc, 1) + "},";
  td += "\"active\":{\"type\":\"boolean\",\"title\":\"Sistema Attivo\",\"value\":" + String(sistemaAttivo ? "true" : "false") + "}";
  td += "}},";
  td += "\"forms\":[{\"op\":\"invokeaction\",\"href\":\"http://" + ip + "/api/configura\",\"contentType\":\"application/json\",\"security\":\"nosec_sc\"}]";
  td += "}";
  td += "},";
  td += "\"events\":{";
  td += "\"allarme_porta\":{\"data\":{\"type\":\"object\",\"properties\":{\"Allarme_Porta\":{\"type\":\"string\"}}},";
  td += "\"forms\":[{\"op\":\"subscribeevent\",\"href\":\"mqtt://" + String(mqtt_server) + "/" + String(topicEvents) + "\",\"contentType\":\"application/json\"}]},";
  td += "\"allarme_temp\":{\"data\":{\"type\":\"object\",\"properties\":{\"Stato_Temperatura\":{\"type\":\"string\"}}},";
  td += "\"forms\":[{\"op\":\"subscribeevent\",\"href\":\"mqtt://" + String(mqtt_server) + "/" + String(topicEvents) + "\",\"contentType\":\"application/json\"}]}";
  td += "}";
  td += "}"; 

  mqttClient.publish(topicDiscovery, td, true, 1);
}
