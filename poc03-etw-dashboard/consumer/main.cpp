/**
 * win-kernel-lab / PoC 03 — ETW → live web dashboard
 *
 * Subscribes to the Microsoft-Windows-Kernel-Process ETW provider with
 * KrabsETW (Microsoft's wrapper, see third_party/), and pushes process
 * start/stop events to browsers over Server-Sent Events via a tiny embedded
 * Winsock HTTP server. No frameworks, no dependencies beyond krabs headers.
 *
 * This is the "supported" counterpart to PoC 01's driver callback:
 * same signal (process lifecycle), zero driver code.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../third_party/krabs/krabs.hpp"

#define POC03_PORT 9180

/* ------------------------------------------------------------------ */
/* event bus: SSE clients                                              */
/* ------------------------------------------------------------------ */

static std::vector<SOCKET> g_clients;
static std::mutex g_clientsLock;
static std::atomic<bool> g_running{ true };

static void broadcast(const std::string& json)
{
    std::string frame = "data: " + json + "\n\n";
    std::lock_guard<std::mutex> lock(g_clientsLock);
    for (auto it = g_clients.begin(); it != g_clients.end();) {
        if (send(*it, frame.c_str(), (int)frame.size(), 0) == SOCKET_ERROR)
            it = g_clients.erase(it);      // client gone — drop it
        else
            ++it;
    }
}

static std::string escapeJson(const std::wstring& in)
{
    std::string out;
    out.reserve(in.size());
    for (wchar_t wc : in) {
        char buf[8];
        int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, buf, sizeof(buf), nullptr, nullptr);
        for (int i = 0; i < n; i++) {
            if (buf[i] == '"' || buf[i] == '\\') out += '\\';
            out += buf[i];
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* embedded dashboard                                                  */
/* ------------------------------------------------------------------ */

static const char* DASHBOARD_HTML = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>poc03 · ETW live</title>
<style>
body{background:#0d0f1a;color:#e7e9ee;font-family:ui-monospace,Consolas,monospace;margin:0;padding:2rem}
h1{font-size:1.1rem}h1 span{color:#8b5cf6}
#feed{white-space:pre-wrap;line-height:1.7;font-size:.9rem}
.s{color:#4ade80}.x{color:#f87171}
</style></head><body>
<h1>⚡ poc03 — process events, live via <span>ETW + SSE</span></h1>
<div id="feed"></div>
<script>
const feed = document.getElementById('feed');
new EventSource('/events').onmessage = e => {
  const d = JSON.parse(e.data);
  const el = document.createElement('div');
  el.className = d.type === 'start' ? 's' : 'x';
  el.textContent = `${d.type === 'start' ? '▶ START' : '■ STOP '}  pid=${d.pid}  ppid=${d.ppid}  ${d.image}`;
  feed.prepend(el);
  while (feed.childElementCount > 200) feed.lastChild.remove();
};
</script></body></html>)HTML";

/* ------------------------------------------------------------------ */
/* tiny HTTP server                                                    */
/* ------------------------------------------------------------------ */

static void serveClient(SOCKET client)
{
    std::array<char, 2048> buf{};
    int n = recv(client, buf.data(), (int)buf.size() - 1, 0);
    if (n <= 0) { closesocket(client); return; }
    buf[n] = 0;

    if (strstr(buf.data(), "GET /events") == buf.data()) {
        const char* hdr =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
        if (send(client, hdr, (int)strlen(hdr), 0) == SOCKET_ERROR) {
            closesocket(client);
            return;
        }
        std::lock_guard<std::mutex> lock(g_clientsLock);
        g_clients.push_back(client);       // kept open; closed on send failure
        return;
    }

    // any other GET → the dashboard
    const char* body = DASHBOARD_HTML;
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n\r\n",
             strlen(body));
    send(client, hdr, (int)strlen(hdr), 0);
    send(client, body, (int)strlen(body), 0);
    closesocket(client);
}

static void httpLoop()
{
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(POC03_PORT);
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        printf("[-] cannot bind :%d\n", POC03_PORT);
        return;
    }
    printf("[*] dashboard → http://localhost:%d\n", POC03_PORT);

    while (g_running) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::thread(serveClient, client).detach();
    }
    closesocket(listener);
}

/* ------------------------------------------------------------------ */
/* ETW consumer                                                        */
/* ------------------------------------------------------------------ */

static void etwLoop()
{
    krabs::kernel_trace trace(L"poc03-process");
    krabs::kernel::process_provider provider;

    provider.add_on_event_callback([](const EVENT_RECORD& record, const krabs::trace_context& ctx) {
        krabs::schema schema(record, ctx.schema_locator);
        int id = schema.event_id();
        if (id != 1 && id != 2) return;      // 1 = start, 2 = stop

        krabs::parser parser(schema);
        uint32_t pid  = parser.parse<uint32_t>(L"ProcessID");
        uint32_t ppid = 0;
        std::wstring image;
        try { ppid = parser.parse<uint32_t>(L"ParentID"); } catch (...) {}
        try { image = parser.parse<std::wstring>(L"ImageFileName"); } catch (...) {}

        char json[512];
        snprintf(json, sizeof(json),
                 "{\"type\":\"%s\",\"pid\":%u,\"ppid\":%u,\"image\":\"%s\"}",
                 id == 1 ? "start" : "stop", pid, ppid, escapeJson(image).c_str());
        broadcast(json);
    });

    trace.enable(provider);
    printf("[*] ETW session armed (Microsoft-Windows-Kernel-Process)\n");
    trace.start();   // blocks until stop
}

/* ------------------------------------------------------------------ */

int main()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        puts("[-] WSAStartup failed");
        return 1;
    }

    std::thread(httpLoop).detach();
    etwLoop();      // blocks on the main thread

    WSACleanup();
    return 0;
}
