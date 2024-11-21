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

#define WS_PORT 8080
#define WS_MAX_CLIENTS 8
#define WS_PING_INTERVAL 30000  // 30 seconds
#define WS_TIMEOUT 60000        // 60 seconds
#define WS_QUEUE_SIZE 32
#define WS_MAX_MESSAGE_SIZE 4096

class TasmotaWebSocketServer {
public:
  TasmotaWebSocketServer() : server(nullptr), ws(nullptr) {}
  
  void begin() {
    if (server != nullptr) {
      AddLog(LOG_LEVEL_INFO, PSTR("CUBE_WS ==> Already running"));
      return;
    }
    
    AddLog(LOG_LEVEL_INFO, PSTR(" CUBE_WS ==> Starting on port %d"), WS_PORT);
    
    server = new AsyncWebServer(WS_PORT);
    if (!server) {
      AddLog(LOG_LEVEL_ERROR, PSTR("CUBE_WS ==> Server creation failed"));
      return;
    }
    
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

    // Process message queue
    processQueue();

    // Clean inactive clients
    cleanInactiveClients();
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

    if (messageQueue.size() >= WS_QUEUE_SIZE) {
      messageQueue.erase(messageQueue.begin());
    }

    WSMessage msg;
    msg.message = message;
    msg.clientId = 0; // 0 means broadcast
    msg.binary = binary;
    messageQueue.push_back(msg);
  }

  // Send message to specific client
  void sendTo(uint32_t clientId, const String& message, bool binary = false) {
    if (!ws) return;

    if (messageQueue.size() >= WS_QUEUE_SIZE) {
      messageQueue.erase(messageQueue.begin());
    }

    WSMessage msg;
    msg.message = message;
    msg.clientId = clientId;
    msg.binary = binary;
    messageQueue.push_back(msg);
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
    
    if (!error) {
        // 是JSON格式，添加from字段后返回
        doc["from"] = "server";
        
        String response;
        serializeJson(doc, response);
        client->text(response);
        
        AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> Sent JSON response to client %u: %s"), 
               client->id(), response.c_str());
    } else {
        // 不是JSON格式，直接返回文本消息
        String response = String(message) + " (Response from server)";
        client->text(response);
        
        AddLog(LOG_LEVEL_DEBUG, PSTR("CUBE_WS ==> Sent text response to client %u: %s"), 
               client->id(), response.c_str());
    }
  }

  void handleCommand(AsyncWebSocketClient* client, const JsonDocument& doc) {
    const char* command = doc["command"];
    if (!command) {
      sendError(client, "Missing command");
      return;
    }

    // Execute Tasmota command
    // ExecuteCommand((char*)command, SRC_WEBGUI);
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

  void processQueue() {
    if (messageQueue.empty()) return;

    for (const auto& msg : messageQueue) {
      if (msg.clientId == 0) {
        // Broadcast to all clients without checking authentication
        for (const auto& client : clients) {
          if (msg.binary) {
            ws->binary(client.id, msg.message.c_str(), msg.message.length());
          } else {
            ws->text(client.id, msg.message);
          }
        }
      } else {
        // Send to specific client
        if (msg.binary) {
          ws->binary(msg.clientId, msg.message.c_str(), msg.message.length());
        } else {
          ws->text(msg.clientId, msg.message);
        }
      }
    }
    
    messageQueue.clear();
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
