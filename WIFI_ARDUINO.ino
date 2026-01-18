#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <Servo.h>

// --- WIFI CONFIG ---
const char* ssid     = "NOM_WIFI";     
const char* password = "MOT_DE_PASSE";    

const char* serverIP = "X.X.X.X"; // Mettre l'IP du serveur Python
const int serverPort = 5000; // Port par défaut

ESP8266WebServer server(80);

int MAX_ANGLE = 150;
int MIN_ANGLE = 0;
int DECALAGE = 30;

bool INVERSER_ORDRE = false; // false -> droite à gauche ; true -> gauche à droite

float SEUIL_TEMP_MIN = 10.0;
float SEUIL_TEMP_MAX = 25.0;

const int pinServo1 = D5;  // droite
const int pinServo2 = D6;  // gauche
const int buttonPin = D7;  // bouton

// Capteur ultrason
const int trigPin = D2;
const int echoPin = D3;

Servo servo1;
Servo servo2;

int pos1 = MIN_ANGLE;
int pos2 = MAX_ANGLE;
bool shutterOpen = false;
bool triggerOpening = false; 
bool operationInProcess = false;
bool modeManuel = false;

int nbDetection = 0;
float duration, distance;

String meteoActuelle = "Inconnue";
float tempActuelle = 0.0;

unsigned long dernierUpdateMeteo = 0;
const unsigned long INTERVALLE_METEO = 5000; // 5sec

unsigned long dernierCheckPlanning = 0;
const unsigned long INTERVALLE_PLANNING = 60000; // 1min

void recupererConfig() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    
    String url = "http://" + String(serverIP) + ":" + String(serverPort) + "/config";
    http.begin(client, url);
    
    int httpCode = http.GET();
    
    if (httpCode == 200) {
      String payload = http.getString();
      JsonDocument doc;
      deserializeJson(doc, payload);
      
      INVERSER_ORDRE = doc["inverser_ordre"];
      modeManuel = doc["mode_manuel"];
      SEUIL_TEMP_MIN = doc["seuil_temp_min"];
      SEUIL_TEMP_MAX = doc["seuil_temp_max"];
      shutterOpen = doc["is_opened"];
    }
    http.end();
  }
}

void envoyerModeManuel(bool actif) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    
    String url = "http://" + String(serverIP) + ":" + String(serverPort) + "/config/update";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    
    String jsonPayload = "{\"mode_manuel\": " + String(actif ? true : false) + "}";
    
    int httpCode = http.POST(jsonPayload);
    
    if (httpCode == 200) {
      Serial.println("SERVEUR PREVENU : Passage en mode " + String(actif ? "MANUEL" : "AUTO"));
      modeManuel = actif;
    } else {
      Serial.print("Echec envoi mode manuel. Code: ");
      Serial.println(httpCode);
    }
    http.end();
  }
}

void envoyerEtatVolet(bool is_opened) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    
    String url = "http://" + String(serverIP) + ":" + String(serverPort) + "/config/update";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    
    String jsonPayload = "{\"is_opened\": " + String(is_opened ? true : false) + "}";
    
    int httpCode = http.POST(jsonPayload);
    
    if (httpCode == 200) {
      shutterOpen = is_opened;
    } else {
      Serial.print("Echec envoi etat volet. Code: ");
      Serial.println(httpCode);
    }
    http.end();
  }
}

void recupererMeteo() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    String url = "http://" + String(serverIP) + ":" + String(serverPort) + "/meteo";
    http.begin(client, url);
    if (http.GET() == 200) {
      String payload = http.getString();
      JsonDocument doc;
      deserializeJson(doc, payload);
      tempActuelle = doc["temp"];
      meteoActuelle = String((const char*)doc["condition"]);
      Serial.print("Meteo: "); Serial.println(tempActuelle);
    }
    http.end();
  }
}

void verifierPlanning() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    String url = "http://" + String(serverIP) + ":" + String(serverPort) + "/planning/check";
    http.begin(client, url);
    if (http.GET() == 200) {
      String payload = http.getString();
      JsonDocument doc;
      deserializeJson(doc, payload);
      String actionStr = String((const char*)doc["execute"]);

      if (actionStr != "AUCUNE") {
          Serial.print("PLANNING: "); Serial.println(actionStr);
          if (actionStr == "OUVRIR" && shutterOpen == false){
              triggerOpening = true;
              envoyerModeManuel(true);
          } else if (actionStr == "FERMER" && shutterOpen == true){
              triggerOpening = true;
              envoyerModeManuel(true);
          }
      }
    }
    http.end();
  }
}

void checkAndPauseIfObstacle() {
    digitalWrite(trigPin, LOW); delayMicroseconds(2);
    digitalWrite(trigPin, HIGH); delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    duration = pulseIn(echoPin, HIGH, 25000); 
    if (duration == 0) distance = 999; 
    else distance = (duration * .0343) / 2;

    if(distance < 5 && distance > 0){
      nbDetection++;
      if(nbDetection > 2){
        Serial.println("OBSTACLE DETECTE !");
        delay(3000); 
        nbDetection = 0;
      }
    } else nbDetection = 0;
}

void handleRoot() {
  String html = "<html><head><meta charset='utf-8'></head><body style='font-family:sans-serif; text-align:center;'>";
  html += "<h1>Controle Volets</h1>";
  if(modeManuel) html += "<h3 style='color:red'>MODE MANUEL</h3>";
  else html += "<h3 style='color:green'>MODE AUTO</h3>";
  
  html += "<p>Météo: " + String(tempActuelle) + "C (" + meteoActuelle + ")</p>";
  html += "<p>Etat: " + String(shutterOpen ? "OUVERTS" : "FERMES") + "</p>";
  html += "<a href='/trigger'><button style='padding:15px;background:#2196F3;color:white;'>ACTIVER</button></a><br><br>";
  if(modeManuel) html += "<a href='/auto'><button style='padding:10px;background:#4CAF50;color:white;'>RETOUR AUTO</button></a>";
  server.send(200, "text/html", html);
}

void handleTrigger() {
  if (!operationInProcess) {
    triggerOpening = true; 
    if (!modeManuel) {
        envoyerModeManuel(true);
    }
    server.send(200, "text/plain", "OK");
  } else server.send(200, "text/plain", "Busy");
}

void handleResetAuto() {
  if (modeManuel) {
      envoyerModeManuel(false);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(9600);
  servo1.attach(pinServo1, 544, 2400); 
  servo2.attach(pinServo2, 544, 2400); 
  servo1.write(pos1); servo2.write(pos2);
  
  pinMode(buttonPin, INPUT); 
  pinMode(trigPin, OUTPUT); pinMode(echoPin, INPUT);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/trigger", handleTrigger);
  server.on("/auto", handleResetAuto);
  server.begin();

  recupererConfig();
  recupererMeteo();
  dernierUpdateMeteo = millis();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // Météo
  if (now - dernierUpdateMeteo >= INTERVALLE_METEO) {
      dernierUpdateMeteo = now;
      recupererMeteo(); 
      recupererConfig();
      if (!modeManuel) {
          bool ideal = (tempActuelle >= SEUIL_TEMP_MIN && tempActuelle <= SEUIL_TEMP_MAX);
          if (ideal && !shutterOpen) triggerOpening = true;
          else if (!ideal && shutterOpen) triggerOpening = true;
      }
  }

  // Planning
  if (now - dernierCheckPlanning >= INTERVALLE_PLANNING) {
      dernierCheckPlanning = now;
      if (!operationInProcess) verifierPlanning();
  }

  // Bouton
  if(digitalRead(buttonPin) == HIGH && !operationInProcess){
    triggerOpening = true;
    if (!modeManuel) {
        envoyerModeManuel(true);
    }
    delay(200); 
  }

  // Mouvement
  if(triggerOpening){
    operationInProcess = true;
    triggerOpening = false;

    bool leaderMoves, followerMoves;

    if(shutterOpen){ 
      Serial.println("Fermeture...");
      for(int i = 1; i <= (MAX_ANGLE + DECALAGE); i++){
        
        bool tempsPourLeader = (i <= MAX_ANGLE && i > MIN_ANGLE);
        bool tempsPourSuiveur = (i <= (MAX_ANGLE + DECALAGE) && i > DECALAGE);
        
        if (!INVERSER_ORDRE) {
             if(tempsPourLeader) pos2++; 
             if(tempsPourSuiveur) pos1--;
        } else {
             if(tempsPourLeader) pos1--; 
             if(tempsPourSuiveur) pos2++;
        }

        servo1.write(pos1); servo2.write(pos2);
        delay(15); 
        checkAndPauseIfObstacle();
      }
      envoyerEtatVolet(false);
      
    } else {
      Serial.println("Ouverture...");
      for(int i = 1; i <= MAX_ANGLE + DECALAGE; i++){
        
        bool tempsPourLeader = (i <= MAX_ANGLE && i > MIN_ANGLE);
        bool tempsPourSuiveur = (i <= (MAX_ANGLE + DECALAGE) && i > DECALAGE);

        if (!INVERSER_ORDRE) {
             if(tempsPourLeader) pos1++; 
             if(tempsPourSuiveur) pos2--; 
        } else {
             if(tempsPourLeader) pos2--; 
             if(tempsPourSuiveur) pos1++; 
        }

        servo1.write(pos1); servo2.write(pos2);
        delay(15);
        checkAndPauseIfObstacle();
      }
      envoyerEtatVolet(true);
    }
    operationInProcess = false;
  }
}