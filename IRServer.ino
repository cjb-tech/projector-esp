
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <FS.h>
#define CS_USE_SPIFFS true
#include <ConfigStorage.h>
#include <IRremote.h>
#define IR_SEND_PIN D3


#define DBG_OUTPUT_PORT Serial

const char* ap_ssid = "ESP8266-Projector";
const char* ap_pass = "12345678";
const char* host = "projector.local";
ConfigStorage prefs = NULL;

ESP8266WebServer server(80);
//holds the current upload
File fsUploadFile;

#define MAX_UID 10

const char* generateUID() {
  /* Change to allowable characters */
  const char possible[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  static char uid[MAX_UID + 1];
  for (int p = 0, i = 0; i < MAX_UID; i++) {
    int r = random(0, strlen(possible));
    uid[p++] = possible[r];
  }
  uid[MAX_UID] = '\0';
  return uid;
}

const char* tempKey = generateUID();
bool connectedToStation = false;


void powerBtn() {
  IrSender.sendNEC(0x1308, 0x87, 1);
  delay(1000);
  IrSender.sendNEC(0x1308, 0x87, 1);
  delay(2000);
}
//format bytes
String formatBytes(size_t bytes) {
  if (bytes < 1024) {
    return String(bytes) + "B";
  } else if (bytes < (1024 * 1024)) {
    return String(bytes / 1024.0) + "KB";
  } else if (bytes < (1024 * 1024 * 1024)) {
    return String(bytes / 1024.0 / 1024.0) + "MB";
  } else {
    return String(bytes / 1024.0 / 1024.0 / 1024.0) + "GB";
  }
}

String getContentType(String filename) {
  if (server.hasArg("download")) return "application/octet-stream";
  else if (filename.endsWith(".htm")) return "text/html";
  else if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".png")) return "image/png";
  else if (filename.endsWith(".gif")) return "image/gif";
  else if (filename.endsWith(".jpg")) return "image/jpeg";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".xml")) return "text/xml";
  else if (filename.endsWith(".pdf")) return "application/x-pdf";
  else if (filename.endsWith(".zip")) return "application/x-zip";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

bool handleRemoteFile() {
  String path = "/remote.html";
  if (!SPIFFS.exists(path)) {
    return false;
  }
  File file = SPIFFS.open(path, "r");
  String content = file.readString();
  content.replace("$TEMP_KEY", tempKey);
  server.send(200, getContentType(path), content);
  return true;
}

bool handleFileRead(String path) {
  DBG_OUTPUT_PORT.println("handleFileRead: " + path);
  if (path.endsWith("/")) {
    path = "/index.html";
  } else if (path.endsWith("/remote")) {
    return handleRemoteFile();
  }

  String contentType = getContentType(path);
  if (SPIFFS.exists(path)) {
    File file = SPIFFS.open(path, "r");
    size_t sent = server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

void handleFileUpload() {
  if (server.uri() != "/edit") return;
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    DBG_OUTPUT_PORT.print("handleFileUpload Name: "); DBG_OUTPUT_PORT.println(filename);
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    //DBG_OUTPUT_PORT.print("handleFileUpload Data: "); DBG_OUTPUT_PORT.println(upload.currentSize);
    if (fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (fsUploadFile)
      fsUploadFile.close();
    DBG_OUTPUT_PORT.print("handleFileUpload Size: "); DBG_OUTPUT_PORT.println(upload.totalSize);
  }
}

void handleFileDelete() {
  if (server.args() == 0) return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  DBG_OUTPUT_PORT.println("handleFileDelete: " + path);
  if (path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if (!SPIFFS.exists(path))
    return server.send(404, "text/plain", "FileNotFound");
  SPIFFS.remove(path);
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileCreate() {
  if (server.args() == 0)
    return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  DBG_OUTPUT_PORT.println("handleFileCreate: " + path);
  if (path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if (SPIFFS.exists(path))
    return server.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if (file)
    file.close();
  else
    return server.send(500, "text/plain", "CREATE FAILED");
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileList() {
  if (!server.hasArg("dir")) {
    server.send(500, "text/plain", "BAD ARGS");
    return;
  }

  String path = server.arg("dir");
  DBG_OUTPUT_PORT.println("handleFileList: " + path);
  Dir dir = SPIFFS.openDir(path);
  path = String();

  String output = "[";
  while (dir.next()) {
    File entry = dir.openFile("r");
    if (output != "[") output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir) ? "dir" : "file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }

  output += "]";
  server.send(200, "text/json", output);
}

void connectToWifiOrConfig() {
  WiFi.softAPdisconnect();

  DynamicJsonDocument configDoc = prefs.get();

  // change parameters
  String ssid = configDoc["ssid"];
  String pass = configDoc["pass"];

  long now = millis();

  //WIFI INIT
  DBG_OUTPUT_PORT.printf("Connecting to %s\n", ssid);
  if (String(WiFi.SSID()) != String(ssid)) {
    WiFi.hostname("Projector-ESP");
    WiFi.begin(ssid, pass);
  }
  bool connected = true;
  boolean result = WiFi.softAP(ap_ssid, ap_pass);
  if (result == true) {
  	DBG_OUTPUT_PORT.println("Successfully created AP");
	MDNS.begin(host);
  } else {
	DBG_OUTPUT_PORT.println("Failed to create access point");
  }
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    DBG_OUTPUT_PORT.print(".");
  }
  WiFi.softAPdisconnect();
  if (connected) {
    connectedToStation = true;
    DBG_OUTPUT_PORT.println("");
    DBG_OUTPUT_PORT.print("Connected! IP address: ");
    DBG_OUTPUT_PORT.println(WiFi.localIP());
  }
}

bool authNeeded() {
  DynamicJsonDocument configDoc = prefs.get();
  String authUsername = configDoc["authUsername"];
  String authPass = configDoc["authPass"];
  const char* authUsernameCh = authUsername.c_str();
  const char* authPassCh = authPass.c_str();
  Serial.println(authUsernameCh);
  Serial.println(authPassCh);
  if (!server.authenticate(authUsernameCh, authPassCh)) {
    server.requestAuthentication();
    return true;
  }
  return false;
}

bool redirectNeeded() {
  if (!connectedToStation) {
    server.sendHeader("Location", "/config");
    server.send(301, "text/plain", "");
    return true;
  }
  return false;
}

void initSettings() {
  char* path = "/config.json";
  ConfigStorage configStorage(path);
  prefs = configStorage;
}

void setup(void) {
  DBG_OUTPUT_PORT.begin(74880);
  DBG_OUTPUT_PORT.print("\n");
  DBG_OUTPUT_PORT.setDebugOutput(true);
  if (!SPIFFS.begin()) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  FSInfo fs_info;
  SPIFFS.info(fs_info);
  size_t totalBytes = fs_info.totalBytes;
  Serial.print("Total SPIFFS space: ");
  Serial.print(totalBytes);
  Serial.println(" bytes");

  Dir dir = SPIFFS.openDir("/");
  bool foundSomething = false;
  while (dir.next()) {
    foundSomething = true;
    String fileName = dir.fileName();
    size_t fileSize = dir.fileSize();
    DBG_OUTPUT_PORT.printf("FS File: %s, size: %s\n", fileName.c_str(), formatBytes(fileSize).c_str());
  }
  DBG_OUTPUT_PORT.printf("\n");
  if (!foundSomething) {
    Serial.println("FS dir is empty");
  }
  initSettings();
  connectToWifiOrConfig();

  server.on("/config", HTTP_GET, []() {
    String path = "/config.html";
    if (!SPIFFS.exists(path)) {
      server.send(200, "text/plain", "Error loading config.html");
      return;
    }
    if (connectedToStation && authNeeded()) {
      return;
    }

    File file = SPIFFS.open(path, "r");
    String configHtml = file.readString();
    String contentType = getContentType(path);

    DynamicJsonDocument configDoc = prefs.get();

    String ssid = configDoc["ssid"];
    String pass = configDoc["pass"];
    String apiSecret = configDoc["apiSecret"];
    String authUsername = configDoc["authUsername"];
    String authPass = configDoc["authPass"];

    configHtml.replace("$WIFI_SSID", ssid);
    configHtml.replace("$WIFI_PASS", pass);
    configHtml.replace("$API_SECRET", apiSecret);
    configHtml.replace("$AUTH_USERNAME", authUsername);
    configHtml.replace("$AUTH_PASS", authPass);

    server.send(200, contentType, configHtml);

  });
  server.on("/wifi_config", HTTP_POST, []() {
    if (connectedToStation && authNeeded()) {
      return;
    }
    String ssid = server.arg(0);
    String pass = server.arg(1);
    String apiSecret = server.arg(2);

    DynamicJsonDocument configDoc = prefs.get();
    configDoc["ssid"] = ssid;
    configDoc["pass"] = pass;
    configDoc["apiSecret"] = apiSecret;
    prefs.set(configDoc);
    prefs.save();
    server.sendHeader("Location", "/config");
    server.send(301, "text/plain", "");
    //connectToWifiOrConfig();
    ESP.reset();
  });

  server.on("/auth_config", HTTP_POST, []() {
    if (connectedToStation && authNeeded()) {
      return;
    }
    String username = server.arg(0);
    String pass = server.arg(1);

    DynamicJsonDocument configDoc = prefs.get();
    configDoc["authUsername"] = username;
    configDoc["authPass"] = pass;
    prefs.set(configDoc);
    prefs.save();
    server.sendHeader("Location", "/config");
    server.send(301, "text/plain", "");
  });

  server.on("/list", HTTP_GET, []() {
    if (redirectNeeded()) return;
    if (authNeeded()) return;

    handleFileList();
  });
  //load editor
  server.on("/edit", HTTP_GET, []() {
    if (redirectNeeded()) return;
    if (authNeeded()) return;
    if (!handleFileRead("/edit.html")) server.send(404, "text/plain", "FileNotFound");
  });
  //create file
  server.on("/edit", HTTP_PUT, []() {
    if (redirectNeeded()) return;
    if (authNeeded()) return;
    handleFileCreate();
  });
  //delete file
  server.on("/edit", HTTP_DELETE, []() {
    if (redirectNeeded()) return;
    if (authNeeded()) return;
    handleFileDelete();
  });

  server.on("/edit", HTTP_POST, []() {
    if (redirectNeeded()) return;
    if (authNeeded()) return;
    server.send(200, "text/plain", "");
  }, []() {
    if (redirectNeeded()) return;
    handleFileUpload();
  });

  //called when the url is not defined here
  //use it to load content from SPIFFS
  server.onNotFound([]() {
    if (redirectNeeded()) return;
    String uri = server.uri();
    Serial.println(uri);
    if (uri != "/" && uri != "/remote" && authNeeded()) {
      return;
    }
    if (!handleFileRead(server.uri()))
      server.send(404, "text/plain", "FileNotFound");
  });

  String tempKeyEndpoint = "/api/" + String(tempKey) + "/powerBtn";
  Serial.println(tempKeyEndpoint);
  server.on(tempKeyEndpoint, HTTP_GET, []() {
    if (redirectNeeded()) return;
    powerBtn();
    server.send(200, "text/json", "{\"status\": \"OK\"}");
  });
  DynamicJsonDocument configDoc = prefs.get();
  String apiSecret = configDoc["apiSecret"];
  String apiSecretEndpoint = "/api/" + apiSecret + "/powerBtn";
  Serial.println(apiSecretEndpoint);
  server.on(apiSecretEndpoint, HTTP_GET, []() {
    if (redirectNeeded()) return;
    powerBtn();
    server.send(200, "text/json", "{\"status\": \"OK\"}");
  });

  server.begin();
  DBG_OUTPUT_PORT.println("HTTP server started");

}

void loop(void) {
  server.handleClient();
}
