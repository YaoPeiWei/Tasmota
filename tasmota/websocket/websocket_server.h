/*
  websocket_server.h - WebSocket server for Tasmota

  Copyright (C) 2024  Theo Arends and Mathieu Carbou

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef CUBE_WEBSERVER

#pragma once

#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include "../include/tasmota.h"
#include <vector>
#include <ArduinoJson.h>
#include <LittleFS.h>

#define WS_PORT 80
#define WS_MAX_CLIENTS 8
#define WS_PING_INTERVAL 30000  // 30 seconds
#define WS_TIMEOUT 60000        // 60 seconds
#define WS_QUEUE_SIZE 32
#define WS_MAX_MESSAGE_SIZE 4096

extern void AddLog(uint32_t loglevel, PGM_P formatP, ...);
extern void CommandHandler(char* topicBuf, char* dataBuf, uint32_t data_len);
extern void ExecuteCommand(const char *cmnd, uint32_t source);


class TasmotaWebSocketServer {
public:
  TasmotaWebSocketServer() : server(nullptr), ws(nullptr) {}
  
  void begin() {
    if (server != nullptr) {
      return;
    }
    
    AddLog(LOG_LEVEL_INFO, PSTR(" CUBE_WS ==> Starting on port %d"), WS_PORT);
    
    server = new AsyncWebServer(WS_PORT);
    if (!server) {
      AddLog(LOG_LEVEL_ERROR, PSTR("CUBE_WS ==> Server creation failed"));
      return;
    }
    
    // set uri
    server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LittleFS, "/index.html");
    });
    server->on("/ws.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LittleFS, "/ws.html");
    });
    server->on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LittleFS, "/style.css");
    });
    server->onNotFound([](AsyncWebServerRequest *request) {
        request->send(LittleFS, "/index.html");
    });
    
    ws = new AsyncWebSocket("/ws");
    if (!ws) {
      AddLog(LOG_LEVEL_ERROR, PSTR("CUBE_WS ==> WebSocket creation failed"));
      delete server;
      server = nullptr;
      return;
    }
    
    // Configure WebSocket
    ws->onEvent([this](AsyncWebSocket* server, 
                      AsyncWebSocketClient* client,
                      AwsEventType type,
                      void* arg, 
                      uint8_t* data,
                      size_t len) {
      handleWebSocketEvent(type, client, arg, data, len);
    });
    
    // Add WebSocket handler
    server->addHandler(ws);
    
    // Start server
    server->begin();
    
    AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Server started successfully on port %d"), WS_PORT);
    AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Max clients: %d, Queue size: %d"), WS_MAX_CLIENTS, WS_QUEUE_SIZE);
    AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> WebSocket endpoint: ws://<ip>:%d/ws"), WS_PORT);

    // 启动心跳检测
    startHeartbeat();
  }

  void loop() {
    if (!ws) return;

    // Handle heartbeat
    handleHeartbeat();

    // Clean inactive clients
    cleanInactiveClients();

    // 处理消息队列
    processMessageQueue();
  }

  void stop() {
    AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Stopping WebSocket server..."));
    
    if (ws) {
      uint32_t clientCount = clients.size();
      if (clientCount > 0) {
        AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Closing %d active connections"), clientCount);
      }
      ws->closeAll();
      delete ws;
      ws = nullptr;
    }
    
    if (server) {
      server->end();
      delete server;
      server = nullptr;
    }

    clients.clear();
    messageQueue.clear();
    
    AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Server stopped"));
  }

  // Broadcast message to all authenticated clients
  void broadcast(const String& message, bool binary = false) {
    if (!ws) return;

    // 直接广播消息给所有客户端
    for (const auto& client : clients) {
      if (binary) {
        ws->binary(client.id, message.c_str(), message.length());
      } else {
        AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> Broadcast to client %u: %s"), client.id, message.c_str());
        ws->text(client.id, message.c_str());
      }
    }
  }

  // Send message to specific client
  void sendTo(uint32_t clientId, const String& message, bool binary = false) {
    if (!ws) return;

    // 直接发送消息给指定客户端
    if (binary) {
      ws->binary(clientId, message.c_str(), message.length());
    } else {
      AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> Send to client %u: %s"), clientId, message.c_str());
      ws->text(clientId, message.c_str());
    }
  }

private:
  struct WSClient {
    uint32_t id;
    uint32_t lastPing;
    bool authenticated;
  };

  struct WSMessage {
    String message;
    uint32_t clientId;
    bool binary;
    bool isText;  // 新增字段，标识是文本消息还是二进制消息
  };

  AsyncWebServer* server;
  AsyncWebSocket* ws;
  std::vector<WSClient> clients;
  std::vector<WSMessage> messageQueue;
  uint32_t lastCleanup = 0;
  uint32_t lastHeartbeatTime = 0;

  void startHeartbeat() {
    lastHeartbeatTime = millis();
  }

  void handleHeartbeat() {
    uint32_t now = millis();
    if (now - lastHeartbeatTime >= WS_PING_INTERVAL) {
      lastHeartbeatTime = now;
      
      for (const auto& client : clients) {
        ws->ping(client.id);
      }
    }
  }

  void handleWebSocketEvent(AwsEventType type, 
                          AsyncWebSocketClient* client,
                          void* arg, 
                          uint8_t* data,
                          size_t len) {
    switch (type) {
      case WS_EVT_CONNECT:
        handleConnect(client);
        break;
        
      case WS_EVT_DISCONNECT:
        handleDisconnect(client);
        break;
        
      case WS_EVT_DATA:
        handleData(client, arg, data, len);
        break;
        
      case WS_EVT_ERROR:
        handleError(client, arg);
        break;
        
      case WS_EVT_PONG:
        handlePong(client);
        break;
    }
  }

  void handleConnect(AsyncWebSocketClient* client) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> Client trying to connect from %s"), 
           client->remoteIP().toString().c_str());
    
    if (clients.size() >= WS_MAX_CLIENTS) {
        AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Rejected - max clients reached"));
        AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Rejected client %u - max clients reached (%d)"), 
               client->id(), WS_MAX_CLIENTS);
        client->close(1000, "Max clients reached");
        return;
    }

    WSClient wsClient;
    wsClient.id = client->id();
    wsClient.lastPing = millis();
    wsClient.authenticated = true;
    clients.push_back(wsClient);

    AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Client %u connected successfully"), client->id());
    AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Client %u connected from %s"), 
           client->id(), client->remoteIP().toString().c_str());
    AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> Active clients: %d"), clients.size());

    // Send welcome message
    DynamicJsonDocument doc(128);
    doc["type"] = "welcome";
    doc["id"] = client->id();
    
    String response;
    serializeJson(doc, response);
    client->text(response);
  }

  void handleDisconnect(AsyncWebSocketClient* client) {
    bool found = false;
    for (auto it = clients.begin(); it != clients.end(); ++it) {
      if (it->id == client->id()) {
        found = true;
        clients.erase(it);
        break;
      }
    }
    
    if (found) {
      AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Client %u disconnected, remaining clients: %d"), 
             client->id(), clients.size());
    }
  }

  void handleData(AsyncWebSocketClient* client, void* arg, uint8_t* data, size_t len) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    
    if (len > WS_MAX_MESSAGE_SIZE) {
      sendError(client, "Message too large");
      return;
    }
    
    if (info->final && info->index == 0 && info->len == len) {
      if (info->opcode == WS_TEXT) {
        data[len] = 0;
        handleMessage(client, (char*)data);
      }
    }
  }

  void handleMessage(AsyncWebSocketClient* client, const char* message) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> Received message from client %u: %s"), 
           client->id(), message);

    // 首先判断是否是JSON格式
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, message);
    DynamicJsonDocument rsp(1024);  // 正确初始化 DynamicJsonDocument
    String response;
    if (!error && doc.containsKey("req") && doc["req"].is<JsonObject>()) {
        JsonObject req = doc["req"].as<JsonObject>();
        if (req.containsKey("topic") && req.containsKey("payload")) {
          rsp["code"] = 0;
          rsp["req"] = String(message);
          rsp["from"] = "server";
          serializeJson(rsp, response);
          client->text(response);
          AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> Sent JSON response to client %u: %s"), 
                 client->id(), response.c_str());
          WSMessage msg;
          msg.message = message;
          msg.clientId = client->id();
          msg.binary = false;
          msg.isText = false;
          messageQueue.push_back(msg);
        } else {
          rsp["code"] = 1;
          rsp["req"] = String(message);
          rsp["msg"] = "Parameter format error";
          rsp["from"] = "server";
          serializeJson(rsp, response);
          client->text(response);
          AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> Sent JSON response to client %u: %s"), 
                 client->id(), response.c_str());
        }
    } else {
        if (strcmp(message, "ping") == 0) {
          client->text("pong");
          return;
        }
        rsp["code"] = 0;
        rsp["req"] = String(message);
        rsp["from"] = "server";
        serializeJson(rsp, response);
        client->text(response);
        AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> Sent text response to client %u: %s"), 
        client->id(), response.c_str());
        WSMessage msg;
        msg.message = message;
        msg.clientId = client->id();
        msg.binary = false;
        msg.isText = true;  // 标识为文本消息
        messageQueue.push_back(msg);
    }
  }

  void processMessageQueue() {
    while (!messageQueue.empty()) {
      WSMessage msg = messageQueue.front();
      messageQueue.erase(messageQueue.begin());

      if (!msg.isText) {
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, msg.message);
        JsonObject req = doc["req"].as<JsonObject>();
        char* topic = strdup(req["topic"]);
        char* payload = strdup(req["payload"]);
        CommandHandler(topic, payload, strlen(payload));
        free(topic);
        free(payload);
      } else {
        ExecuteCommand((char*)msg.message.c_str(), SRC_CUBE);
      }
    }
  }

  void handleError(AsyncWebSocketClient* client, void* arg) {
    uint16_t* code = (uint16_t*)arg;
    AddLog(LOG_LEVEL_ERROR, PSTR("CUBE_WS ==> Client %u error %u"), client->id(), *code);
  }

  void handlePong(AsyncWebSocketClient* client) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> Received pong from client %u"), client->id());
    
    for (auto& c : clients) {
      if (c.id == client->id()) {
        c.lastPing = millis();  // 更新最后响应时间
        break;
      }
    }
  }

  void sendError(AsyncWebSocketClient* client, const char* message) {
    DynamicJsonDocument doc(128);
    doc["type"] = "error";
    doc["message"] = message;
    
    String response;
    serializeJson(doc, response);
    client->text(response);
  }

  void cleanInactiveClients() {
    uint32_t now = millis();
    
    if (now - lastCleanup < WS_TIMEOUT) return;
    lastCleanup = now;
    
    uint32_t initialCount = clients.size();
    uint32_t removedCount = 0;
    
    for (auto it = clients.begin(); it != clients.end();) {
      if (now - it->lastPing >= WS_TIMEOUT) {
        AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Client %u timed out after %d ms"), 
               it->id, WS_TIMEOUT);
        ws->close(it->id, 1000, "Timeout");
        it = clients.erase(it);
        removedCount++;
      } else {
        ++it;
      }
    }
    
    if (removedCount > 0) {
      AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Cleaned up %d inactive clients, remaining: %d"), 
             removedCount, clients.size());
    }
  }
};

// 在文件末尾定义全局实例
TasmotaWebSocketServer WebSocketServer; 
#endif
