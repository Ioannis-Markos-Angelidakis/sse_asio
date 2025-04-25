#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <print>
#include <vector>
#include <string>
#include <memory>

using asio::ip::tcp;
using namespace asio::experimental::awaitable_operators;

// Global message store
std::vector<std::string> messages;

// Global list of active SSE client sockets (using unique_ptr)
std::vector<std::unique_ptr<tcp::socket>> sse_clients;

// Buffer for reading requests
std::array<char, 1024> buffer;

// Check if request is for SSE
bool is_sse_request(const std::string& request) {
    return request.find("GET /events") != std::string::npos && request.find("Accept: text/event-stream") != std::string::npos;
}

// Check if request is for homepage
bool is_home_request(const std::string& request) {
    return request.find("GET / ") != std::string::npos || request.find("GET /index.html") != std::string::npos;
}

// Check if request is a POST for messages
bool is_post_request(const std::string& request) {
    return request.find("POST /post") != std::string::npos;
}

// Send HTTP response
asio::awaitable<void> send_response(tcp::socket& socket, const std::string& response) {
    auto [ec, _] = co_await asio::async_write(socket, asio::buffer(response), asio::as_tuple(asio::use_awaitable));
    if (ec) {
        // Socket will be closed automatically by unique_ptr
    }
}

// Send SSE event to a single client
asio::awaitable<void> send_sse_event(tcp::socket& socket, const std::string& event) {
    auto [ec, _] = co_await asio::async_write(socket, asio::buffer(event), asio::as_tuple(asio::use_awaitable));
    if (ec) {
        auto it = std::find_if(sse_clients.begin(), sse_clients.end(),
            [&socket](const auto& ptr) { return ptr.get() == &socket; });
        if (it != sse_clients.end()) {
            sse_clients.erase(it); // unique_ptr handles deletion
        }
    }
}

// Broadcast message to all SSE clients
void broadcast_message(const std::string& message) {
    std::string event = "data: " + message + "\n\n";
    for (auto& client : sse_clients) {
        asio::co_spawn(client->get_executor(), send_sse_event(*client, event), asio::detached);
    }
}

// Handle client connection
asio::awaitable<void> handle_client(std::unique_ptr<tcp::socket> socket) {
    // Read request
    auto [ec, length] = co_await socket->async_read_some(asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
    if (ec) {
        co_return; // socket is automatically deleted by unique_ptr
    }

    std::string request(buffer.data(), length);
    if (is_sse_request(request)) {
        std::string response = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/event-stream\r\n"
                              "Cache-Control: no-cache\r\n"
                              "Connection: keep-alive\r\n\r\n";
        sse_clients.emplace_back(std::move(socket)); // Transfer ownership to sse_clients
        co_await send_response(*sse_clients.back(), response);
        // Send existing messages to new client
        for (const std::string& msg : messages) {
            co_await send_sse_event(*sse_clients.back(), "data: " + msg + "\n\n");
        }
        // Keep connection open for SSE (socket managed by sse_clients)
    } else if (is_home_request(request)) {
        std::string response = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/html\r\n\r\n"
                              "<!DOCTYPE html>"
                              "<html lang='en'>"
                              "<head><meta charset='UTF-8'><title>Message Board</title></head>"
                              "<body>"
                              "<h1>Message Board</h1>"
                              "<input id='message' type='text' placeholder='Enter your message'>"
                              "<button onclick='postMessage()'>Post</button>"
                              "<ul id='messages'></ul>"
                              "<script>"
                              "const source = new EventSource('/events');"
                              "source.onmessage = function(event) {"
                              "  const li = document.createElement('li');"
                              "  li.textContent = event.data;"
                              "  document.getElementById('messages').appendChild(li);"
                              "};"
                              "function postMessage() {"
                              "  const msg = document.getElementById('message').value;"
                              "  if (msg) {"
                              "    fetch('/post', {"
                              "      method: 'POST',"
                              "      headers: {'Content-Type': 'text/plain'},"
                              "      body: msg"
                              "    }).then(() => {"
                              "      document.getElementById('message').value = '';"
                              "    });"
                              "  }"
                              "}"
                              "</script>"
                              "</body></html>";
        co_await send_response(*socket, response);
        // socket is automatically deleted by unique_ptr
    } else if (is_post_request(request)) {
        // Extract message from request body
        size_t pos = request.find("\r\n\r\n");
        if (pos != std::string::npos) {
            std::string body = request.substr(pos + 4);
            if (!body.empty()) {
                messages.emplace_back(body);
                broadcast_message(body);
            }
        }
        std::string response = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/plain\r\n\r\n"
                              "Message received";
        co_await send_response(*socket, response);
        // socket is automatically deleted by unique_ptr
    }
    // No delete needed; socket is managed by unique_ptr
}

// Accept new connections
asio::awaitable<void> accept(tcp::acceptor& acceptor) {
    while (true) {
        auto socket = std::make_unique<tcp::socket>(acceptor.get_executor());
        auto [ec] = co_await acceptor.async_accept(*socket, asio::as_tuple(asio::use_awaitable));
        if (!ec) {
            asio::co_spawn(acceptor.get_executor(), handle_client(std::move(socket)), asio::detached);
        }
        // socket is automatically deleted by unique_ptr if accept fails
    }
}

int32_t main() {
    try {
        asio::io_context io_context;
        tcp::acceptor acceptor(io_context, tcp::endpoint(asio::ip::address::from_string("192.168.0.21"), 3000));
        std::println("SSE Server running on 192.168.0.21:3000");
        asio::co_spawn(io_context, accept(acceptor), asio::detached);
        io_context.run();
    } catch (std::exception& e) {
        std::println("Exception: {}", e.what());
    }
}
//clang++ -Wall -Wextra -fsanitize=address sse_asio.cpp -o sse_asio -std=c++23 -lwsock32 -lws2_32