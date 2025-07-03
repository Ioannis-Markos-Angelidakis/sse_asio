#include <asio.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

// Struct to hold connection data
struct Connection {
  Connection(asio::ip::tcp::socket socket) : socket(std::move(socket)) {
    std::ostringstream oss;
    oss << this->socket.remote_endpoint();
    endpoint = oss.str();

    // Generate a random username for the client
    static const char alphanum[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);

    username = "User";
    for (int i = 0; i < 6; ++i) {
      username += alphanum[dis(gen)];
    }
  }
  asio::ip::tcp::socket socket;
  std::string endpoint;
  std::string username;
  asio::streambuf buffer;
  std::array<char, 1> dummy_buffer;
};

// Struct to hold server data
struct Server {
  asio::io_context &io_context;
  asio::ip::tcp::acceptor acceptor;
  std::vector<std::shared_ptr<Connection>> connections;
};

// MIME type mapping
const std::unordered_map<std::string, std::string> MIME_TYPES = {
    {".html", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".json", "application/json"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    {".txt", "text/plain"},
    {".woff", "font/woff"},
    {".woff2", "font/woff2"}};

// Function declarations
void start_accept(Server &server);
void start_connection(Server &server, std::shared_ptr<Connection> conn);
void send_sse_headers(Server &server, std::shared_ptr<Connection> conn);
void send_event(Server &server, std::shared_ptr<Connection> conn,
                const std::string &event_type, const std::string &data);
void broadcast(Server &server, const std::string &event_type,
               const std::string &data);
void remove_connection(Server &server, std::shared_ptr<Connection> conn);
void start_disconnection_detection(Server &server,
                                   std::shared_ptr<Connection> conn);
void send_http_response(std::shared_ptr<Connection> conn,
                        const std::string &status, const std::string &content,
                        const std::string &content_type);
void serve_static_file(std::shared_ptr<Connection> conn,
                       const std::string &path);

void start_accept(Server &server) {
  auto socket = std::make_shared<asio::ip::tcp::socket>(server.io_context);
  server.acceptor.async_accept(*socket, [&server, socket](std::error_code ec) {
    if (!ec) {
      auto conn = std::make_shared<Connection>(std::move(*socket));
      start_connection(server, conn);
    }
    start_accept(server);
  });
}

void start_connection(Server &server, std::shared_ptr<Connection> conn) {
  asio::async_read_until(conn->socket, conn->buffer, "\r\n\r\n",
                         [&server, conn](std::error_code ec, std::size_t) {
                           if (!ec) {
                             std::istream is(&conn->buffer);
                             std::string request_line;
                             std::getline(is, request_line);

                             // Parse request method and path
                             std::istringstream iss(request_line);
                             std::string method, path, protocol;
                             iss >> method >> path >> protocol;

                             if (method == "GET") {
                               if (path == "/events") {
                                 send_sse_headers(server, conn);
                               } else {
                                 // Default to index.html for root
                                 if (path == "/")
                                   path = "/index.html";
                                 serve_static_file(conn, path);
                               }
                             } else {
                               send_http_response(
                                   conn, "405 Method Not Allowed",
                                   "Method not allowed", "text/plain");
                             }
                           } else {
                             conn->socket.close();
                           }
                         });
}

void send_http_response(std::shared_ptr<Connection> conn,
                        const std::string &status, const std::string &content,
                        const std::string &content_type) {
  std::string response = "HTTP/1.1 " + status +
                         "\r\n"
                         "Content-Type: " +
                         content_type +
                         "; charset=utf-8\r\n"
                         "Content-Length: " +
                         std::to_string(content.size()) +
                         "\r\n"
                         "Connection: close\r\n"
                         "Access-Control-Allow-Origin: *\r\n\r\n" +
                         content;

  asio::async_write(conn->socket, asio::buffer(response),
                    [conn](std::error_code ec, std::size_t) {
                      if (ec) {
                        std::println("Error sending HTTP response: {}",
                                     ec.message());
                      }
                      conn->socket.close();
                    });
}

void serve_static_file(std::shared_ptr<Connection> conn,
                       const std::string &path) {
  try {
    // Security: Prevent directory traversal
    if (path.find("..") != std::string::npos) {
      send_http_response(conn, "403 Forbidden", "Forbidden", "text/plain");
      return;
    }

    // Remove leading slash
    std::string file_path = path.substr(1);
    if (file_path.empty())
      file_path = "index.html";

    // Open file
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
      throw std::runtime_error("File not found");
    }

    // Read file content
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    // Determine MIME type
    std::string ext = fs::path(file_path).extension().string();
    std::string content_type = "text/plain";
    if (MIME_TYPES.find(ext) != MIME_TYPES.end()) {
      content_type = MIME_TYPES.at(ext);
    }

    // Send response
    send_http_response(conn, "200 OK", content, content_type);
  } catch (const std::exception &e) {
    std::println("Error serving {}: {}", path, e.what());
    std::string not_found = R"(
      <html>
        <head><title>404 Not Found</title></head>
        <body>
          <h1>404 Not Found</h1>
          <p>The requested resource was not found on this server.</p>
        </body>
      </html>
    )";
    send_http_response(conn, "404 Not Found", not_found, "text/html");
  }
}

void send_sse_headers(Server &server, std::shared_ptr<Connection> conn) {
  std::string headers = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/event-stream\r\n"
                        "Cache-Control: no-cache\r\n"
                        "Connection: keep-alive\r\n"
                        "Access-Control-Allow-Origin: *\r\n\r\n";

  asio::async_write(conn->socket, asio::buffer(headers),
                    [&server, conn](std::error_code ec, std::size_t) {
                      if (!ec) {
                        server.connections.push_back(conn);
                        std::println("Client connected: {} ({})",
                                     conn->username, conn->endpoint);

                        // Send initial list of connected users
                        std::string user_list = "[";
                        for (size_t i = 0; i < server.connections.size(); ++i) {
                          user_list +=
                              "\"" + server.connections[i]->username + "\"";
                          if (i < server.connections.size() - 1) {
                            user_list += ",";
                          }
                        }
                        user_list += "]";

                        std::string event =
                            "event: connect\n"
                            "data: {\"type\":\"connect\",\"user\":\"" +
                            conn->username + "\",\"users\":" + user_list +
                            "}\n\n";

                        asio::write(conn->socket, asio::buffer(event));

                        // Broadcast new connection to other clients
                        broadcast(server, "user_connected",
                                  "{\"user\":\"" + conn->username + "\"}");

                        start_disconnection_detection(server, conn);
                      } else {
                        conn->socket.close();
                      }
                    });
}

void send_event(Server &server, std::shared_ptr<Connection> conn,
                const std::string &event_type, const std::string &data) {
  std::string event = "event: " + event_type +
                      "\n"
                      "data: " +
                      data + "\n\n";

  asio::async_write(conn->socket, asio::buffer(event),
                    [&server, conn](std::error_code ec, std::size_t) {
                      if (ec) {
                        remove_connection(server, conn);
                      }
                    });
}

void broadcast(Server &server, const std::string &event_type,
               const std::string &data) {
  for (auto &c : server.connections) {
    send_event(server, c, event_type, data);
  }
}

void remove_connection(Server &server, std::shared_ptr<Connection> conn) {
  auto it =
      std::find(server.connections.begin(), server.connections.end(), conn);
  if (it != server.connections.end()) {
    std::println("Client disconnected: {} ({})", conn->username,
                 conn->endpoint);
    server.connections.erase(it);

    // Broadcast disconnection to all clients
    broadcast(server, "user_disconnected",
              "{\"user\":\"" + conn->username + "\"}");
  }
}

void start_disconnection_detection(Server &server,
                                   std::shared_ptr<Connection> conn) {
  conn->socket.async_read_some(
      asio::buffer(conn->dummy_buffer),
      [&server, conn](std::error_code ec, std::size_t bytes_transferred) {
        if (ec) {
          remove_connection(server, conn);
        } else {
          // Restart detection if data is unexpectedly received
          start_disconnection_detection(server, conn);
        }
      });
}

int32_t main() {
  try {
    asio::io_context io_context;
    Server server{
        io_context,
        asio::ip::tcp::acceptor(
            io_context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 3000)),
        {}};

    start_accept(server);
    std::println("Server running on port 3000...");
    std::println("Serving static files from: {}", fs::current_path().string());
    std::println("Frontend available at: http://192.168.0.13:3000/");
    io_context.run();
  } catch (const std::exception &e) {
    std::println("Exception: {}", e.what());
  }
}
// clang++ -Wall -Wextra -Wpedantic -Wconversion -fsanitize=address
// sse_server.cpp -o sse_server -std=c++26 -lws2_32 -lgdi32 -lwsock32