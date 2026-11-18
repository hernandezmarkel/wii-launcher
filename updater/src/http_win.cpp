// HTTP con WinHTTP (TLS y certificados del sistema). Un salto por petición; las redirecciones las sigue fetch()
// validando cada destino contra la lista de hosts permitidos.
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

#include "wl/update.h"

namespace wl::upd {

std::string HttpResponse::header(const std::string& name) const {
    auto it = headers.find(name);
    return it == headers.end() ? std::string() : it->second;
}

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

struct Url {
    std::string scheme, host, path;
    int port = 0;
};

// Solo http/https, host en minúsculas sin usuario, puerto explícito o por defecto. Nada de '\\' ni espacios.
bool parse_url(const std::string& url, Url& out) {
    if (url.size() > 2048) return false;
    for (unsigned char c : url)
        if (c <= 0x20 || c == '\\' || c >= 0x7f) return false;
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    out.scheme = url.substr(0, p);
    std::transform(out.scheme.begin(), out.scheme.end(), out.scheme.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    size_t start = p + 3;
    size_t slash = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    out.path = slash == std::string::npos ? "/" : url.substr(slash);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    size_t colon = authority.rfind(':');
    out.port = out.scheme == "https" ? 443 : out.scheme == "http" ? 80 : 0;
    if (colon != std::string::npos) {
        std::string port = authority.substr(colon + 1);
        authority = authority.substr(0, colon);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return false;
        out.port = std::stoi(port);
    }
    out.host = authority;
    std::transform(out.host.begin(), out.host.end(), out.host.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}

class WinHttpClient : public HttpClient {
public:
    WinHttpClient() {
        std::wstring agent = widen("WiiLauncher/" + current_version());
        session_ = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) session_ = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_) {
            WinHttpSetTimeouts(session_, 10000, 10000, 10000, 30000);
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
            protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
            if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
                protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
                WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
            }
        }
    }
    ~WinHttpClient() override {
        if (session_) WinHttpCloseHandle(session_);
    }

    HttpResponse get(const HttpRequest& req) override {
        HttpResponse r;
        Url u;
        if (!session_) {
            r.error = "No se pudo iniciar la conexión de red.";
            return r;
        }
        if (!parse_url(req.url, u) || (u.scheme != "https" && u.scheme != "http")) {
            r.error = "Dirección no válida.";
            return r;
        }
        HINTERNET conn = WinHttpConnect(session_, widen(u.host).c_str(), (INTERNET_PORT)u.port, 0);
        HINTERNET hreq = conn ? WinHttpOpenRequest(conn, L"GET", widen(u.path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   u.scheme == "https" ? WINHTTP_FLAG_SECURE : 0)
                              : nullptr;
        auto close = [&] {
            if (hreq) WinHttpCloseHandle(hreq);
            if (conn) WinHttpCloseHandle(conn);
        };
        if (!hreq) {
            r.error = net_error(GetLastError());
            close();
            return r;
        }
        DWORD no_redirect = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(hreq, WINHTTP_OPTION_DISABLE_FEATURE, &no_redirect, sizeof no_redirect);
        std::wstring headers;
        for (const auto& [k, v] : req.headers) headers += widen(k + ": " + v + "\r\n");
        if (!WinHttpSendRequest(hreq, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(hreq, nullptr)) {
            r.error = net_error(GetLastError());
            close();
            return r;
        }
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        r.status = (int)status;
        read_headers(hreq, r);

        uint64_t total = 0;
        std::string cl = r.header("content-length");
        if (!cl.empty() && cl.size() < 15 && cl.find_first_not_of("0123456789") == std::string::npos) total = std::stoull(cl);
        if (r.status == 200 && total > req.max_bytes) {
            r.status = 0;  // se rechaza antes de escribir un solo byte
            r.error = "El archivo es mayor de lo permitido; no se descarga.";
            close();
            return r;
        }
        std::ofstream file;
        bool to_file = !req.to_file.empty() && r.status == 200;
        if (to_file) {
            file.open(req.to_file, std::ios::binary | std::ios::trunc);
            if (!file) {
                r.error = "No se pudo escribir la descarga (¿disco lleno o sin permisos?).";
                close();
                return r;
            }
        }
        uint64_t got = 0;
        std::vector<char> buf(1 << 16);
        for (;;) {
            DWORD n = 0;
            if (!WinHttpReadData(hreq, buf.data(), (DWORD)buf.size(), &n)) {
                r.error = net_error(GetLastError());
                r.status = 0;  // respuesta incompleta: se trata como fallo de red
                break;
            }
            if (n == 0) break;
            got += n;
            uint64_t limit = r.status == 200 ? req.max_bytes : 64 * 1024;
            if (got > limit) {
                r.error = "La respuesta supera el tamaño permitido.";
                r.status = 0;
                break;
            }
            if (to_file) {
                file.write(buf.data(), n);
                if (!file) {
                    r.error = "No se pudo escribir la descarga (¿disco lleno?).";
                    r.status = 0;
                    break;
                }
            } else {
                r.body.append(buf.data(), n);
            }
            if (req.progress && !req.progress(got, total)) {
                r.cancelled = true;
                r.error = "Descarga cancelada.";
                r.status = 0;
                break;
            }
        }
        if (r.status == 200 && total && got != total) {
            r.error = "La descarga se cortó a medias.";
            r.status = 0;
        }
        close();
        return r;
    }

private:
    HINTERNET session_ = nullptr;

    static std::string net_error(DWORD code) {
        switch (code) {
            case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "Sin conexión a Internet o DNS no disponible.";
            case ERROR_WINHTTP_CANNOT_CONNECT: return "No se pudo conectar con el servidor de actualizaciones.";
            case ERROR_WINHTTP_TIMEOUT: return "El servidor de actualizaciones no responde (tiempo agotado).";
            case ERROR_WINHTTP_CONNECTION_ERROR: return "Se cortó la conexión.";
            case ERROR_WINHTTP_SECURE_FAILURE:
            case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
            case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
            case ERROR_WINHTTP_SECURE_INVALID_CA:
            case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
                return "Conexión segura rechazada (certificado no válido o red interceptada).";
            default: return "Error de red (" + std::to_string(code) + ").";
        }
    }

    static void read_headers(HINTERNET h, HttpResponse& r) {
        DWORD len = 0;
        WinHttpQueryHeaders(h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len, WINHTTP_NO_HEADER_INDEX);
        if (!len || len > 256 * 1024) return;
        std::wstring raw(len / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &len, WINHTTP_NO_HEADER_INDEX)) return;
        std::string all = narrow(raw);
        size_t pos = all.find("\r\n");  // primera línea = estado
        while (pos != std::string::npos) {
            size_t next = all.find("\r\n", pos + 2);
            std::string line = all.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
            size_t c = line.find(':');
            if (c != std::string::npos) {
                std::string k = line.substr(0, c), v = line.substr(c + 1);
                std::transform(k.begin(), k.end(), k.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
                while (!v.empty() && v[0] == ' ') v.erase(0, 1);
                r.headers[k] = v;
            }
            pos = next;
        }
    }
};

}  // namespace

std::unique_ptr<HttpClient> make_winhttp_client() { return std::make_unique<WinHttpClient>(); }

bool allowed_url(const std::string& url) {
    Url u;
    if (!parse_url(url, u)) return false;
    if (u.scheme == "https" && u.port == 443) {
        static const char* hosts[] = {"github.com", "api.github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"};
        for (const char* h : hosts)
            if (u.host == h) return true;
        return false;
    }
    // Feed de staging local: solo en builds de prueba y solo loopback.
    return testing_build() && u.scheme == "http" && u.host == "127.0.0.1";
}

HttpResponse fetch(HttpClient& http, HttpRequest request, int max_redirects) {
    for (int hop = 0;; ++hop) {
        if (!allowed_url(request.url)) {
            HttpResponse r;
            r.error = hop ? "Redirección a un origen no permitido. Se rechaza la actualización." : "Origen de actualizaciones no permitido.";
            log("rechazada URL no permitida (salto " + std::to_string(hop) + ")");
            return r;
        }
        HttpResponse r = http.get(request);
        bool redirect = r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308;
        if (!redirect) return r;
        std::string location = r.header("location");
        if (location.empty() || hop >= max_redirects) {
            HttpResponse bad;
            bad.error = "Demasiadas redirecciones o redirección sin destino.";
            return bad;
        }
        if (location[0] == '/') {  // relativa: mismo origen
            Url u;
            parse_url(request.url, u);
            location = u.scheme + "://" + u.host + ((u.scheme == "https" && u.port == 443) || (u.scheme == "http" && u.port == 80) ? "" : ":" + std::to_string(u.port)) + location;
        }
        request.url = location;
        // Las cabeceras condicionales y de API no deben viajar a la CDN.
        request.headers.erase(std::remove_if(request.headers.begin(), request.headers.end(),
                                             [](const auto& h) { return h.first == "If-None-Match" || h.first == "Accept"; }),
                              request.headers.end());
    }
}

}  // namespace wl::upd
