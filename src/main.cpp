#define CORE_DEBUG_LEVEL ARDUHAL_LOG_LEVEL_INFO

#include <Arduino.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>
#include <FS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include "api.h"
#include "config.h"
#include "lock.h"
#include "lockbox.h"
#include "memory.h"

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include "ESP8266mDNS.h"
#elif defined(ESP32)
#include <WiFi.h>
#include "ESPmDNS.h"
#endif

DNSServer *dns;
AsyncWebServer *api_server;
AsyncWebServer *frontend_server;
AsyncWiFiManager *wifiManager;
Memory *memory;
Lock *lock;
Lockbox *lockbox;

String api_host;
static unsigned long last_time = 0;

String processor(const String &var) {
    if (var == "API_HOST")
        return api_host;
    return String();
}

void check_emlalock_session() {
    WiFiClientSecure *client = new WiFiClientSecure;
    HTTPClient https;
    char api_user[32];
    char api_key[32];
    char url[100];
    JsonDocument info;

    memory->GetEmlalockApiUser(api_user, sizeof(api_user));
    memory->GetEmlalockApiKey(api_key, sizeof(api_user));
    sprintf(url, "https://api.emlalock.com/info?userid=%s&apikey=%s", api_user, api_key);

    log_d("checking %s", url);

    client->setInsecure();
    https.useHTTP10(true);
    bool ret = https.begin(*client, "api.emlalock.com", 443, url, true);
    if (!ret) {
        log_e("https.begin failed");
        return;
    }
    int httpCode = https.GET();
    if (httpCode != 200) {
        log_e("http error: %d", httpCode);
        return;
    }
    deserializeJson(info, https.getStream());
    https.end();

    const char *error = info["error"];
    if (error != NULL) {
        log_e("error: %s", error);
        return;
    }

    bool is_emlalocked = lockbox->GetVaultEmlalocked();
    const char *chastitysessionid = info["chastitysession"]["chastitysessionid"];

    log_i("is_emlalocked: %d", is_emlalocked);

    if (is_emlalocked && chastitysessionid == NULL) {
        /* no Emlalock session detected, but the vault is emlalocked. Unemlalock the vault. */
        lockbox->SetVaultUnemlalocked();
    } else if (!is_emlalocked && chastitysessionid != NULL) {
        /* sessionid isn't yet stored, time to lock up */
        lockbox->SetVaultEmlalocked(chastitysessionid);
    } else if (is_emlalocked && chastitysessionid != NULL) {
        /* vault is emlalocked, check if we are in a cleaningopening */
        lockbox->SetVaultEmlalockIncleaning(info["chastitysession"]["incleaning"]);
    }
}

void listDir(File dir, int level = 0) {
    while (File file = dir.openNextFile()) {
        if (file.isDirectory()) {
            log_i("%*s%s", level * 2, "", file.name());
            listDir(file, level + 1);
        } else {
            log_i(
                "%*s%-*s Größe: %7d Bytes", level * 2, "", 20 - level * 2, file.name(), file.size());
        }
        file.close();
    }
}

void setup() {
    Serial.begin(9600);
    delay(10);

    if (!LittleFS.begin()) {
        log_e("An Error has occurred while mounting LittleFS");
    } else {
        File root = LittleFS.open("/", "r");
        log_i("=== LittleFS Dateisystem Inhalt ===");
        listDir(root);
        root.close();
        log_i("================================");
    }

#if defined(ESP8266)
    pinMode(D3, INPUT_PULLUP);
#endif

    dns = new DNSServer;
    api_server = new AsyncWebServer(API_PORT);
    frontend_server = new AsyncWebServer(FRONTEND_PORT);

    memory = new Memory();
    lock = new Lock(PINSERVO, memory->GetOpenPosition(), memory->GetClosedPosition());
    lockbox = new Lockbox(lock, memory);

    char box_name[MAX_NAME_LENGTH] = "";
    if (!memory->GetName(box_name, MAX_NAME_LENGTH)) {
        snprintf(box_name, sizeof(box_name), "Lockbox");
    }
    log_i("box name: '%s'\n", box_name);
    WiFi.softAPdisconnect(true);
    wifiManager = new AsyncWiFiManager(frontend_server, dns);
    if (!wifiManager->autoConnect(box_name)) {
        log_e("Failed to connect and hit timeout");
        delay(3000);
        ESP.restart();
    }

    api_host = "";
    api_host.concat("http://");
    api_host.concat(WiFi.localIP().toString());
    api_host.concat(":");
    api_host.concat(API_PORT);

    // Wifi is connected, we can repurpose frontend server
    DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");
    DefaultHeaders::Instance().addHeader(
        "Content-Security-Policy",
        "default-src 'self'; style-src 'self' unpkg.com; script-src 'self';connect-src *;base-uri 'self';form-action 'self'");
    DefaultHeaders::Instance().addHeader("Referrer-Policy", "no-referrer");
    frontend_server->reset();
    frontend_server->begin();
    frontend_server->serveStatic("/", LittleFS, "/www");
    frontend_server->serveStatic("/templates", LittleFS, "/templates")
        .setTemplateProcessor(processor);
    StartServer(api_server, lockbox, wifiManager);

    MDNS.addService("ekilb", "tcp", API_PORT);
    if (!MDNS.begin(box_name)) {
        log_e("Error setting up MDNS responder!");
    } else {
        log_i("mDNS responder started");
    }
}

void loop() {
#if defined(ESP8266)
    MDNS.update();
#endif

#if defined(CONFIG_UNLOCK_PIN)
    if (!digitalRead(CONFIG_UNLOCK_PIN)) {
        log_i("unlock overwrite button pressed");
        lockbox->SetVaultUnemlalocked();
        memory->SetVaultUnlocked();
        ESP.restart();
    }
#endif

    if (last_time + 15000 < millis()) {
        last_time = millis();
        check_emlalock_session();
    }
}
