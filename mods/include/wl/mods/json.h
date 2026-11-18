#pragma once
// JSON mínimo y acotado para manifiestos de mods, perfiles y salas.
// Pensado para datos NO confiables (anfitrión remoto, proveedor): límites de tamaño, profundidad y
// elementos, sin recursión ilimitada. La salida es canónica (claves ordenadas, sin espacios), así que
// sirve para calcular fingerprints estables.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wl::mods {

class Json {
public:
    enum class Type { Null, Bool, Int, String, Array, Object };

    Json() = default;
    Json(bool b) : type_(Type::Bool), b_(b) {}
    Json(int v) : type_(Type::Int), i_(v) {}
    Json(int64_t v) : type_(Type::Int), i_(v) {}
    Json(uint64_t v) : type_(Type::Int), i_(static_cast<int64_t>(v)) {}
    Json(const char* s) : type_(Type::String), s_(s) {}
    Json(std::string s) : type_(Type::String), s_(std::move(s)) {}
    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_string() const { return type_ == Type::String; }
    bool is_int() const { return type_ == Type::Int; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    // Lectores tolerantes: devuelven el valor por defecto si el tipo no coincide.
    std::string str(const std::string& def = {}) const { return is_string() ? s_ : def; }
    int64_t num(int64_t def = 0) const { return is_int() ? i_ : def; }
    bool boolean(bool def = false) const { return is_bool() ? b_ : def; }

    // Objeto
    const Json& operator[](const std::string& key) const;
    Json& set(const std::string& key, Json value);
    bool has(const std::string& key) const { return is_object() && o_.count(key) != 0; }
    const std::map<std::string, Json>& items() const { return o_; }

    // Array
    const Json& operator[](size_t index) const;
    Json& push(Json value);
    size_t size() const { return is_array() ? a_.size() : is_object() ? o_.size() : 0; }
    const std::vector<Json>& elements() const { return a_; }

    // Serialización canónica: claves ordenadas por bytes UTF-8, sin espacios, escapes mínimos.
    std::string dump() const;
    // Igual pero con sangría, para archivos que lee una persona (perfiles, mod.json).
    std::string pretty() const;

private:
    void dump_to(std::string& out, int indent, int level) const;
    Type type_ = Type::Null;
    bool b_ = false;
    int64_t i_ = 0;
    std::string s_;
    std::vector<Json> a_;
    std::map<std::string, Json> o_;
};

struct JsonLimits {
    size_t max_bytes = 1 << 20;     // 1 MiB
    int max_depth = 16;
    size_t max_string = 64 * 1024;
    size_t max_elements = 20000;    // total de valores en el documento
    bool allow_fractions = false;   // solo respuestas de proveedores: 1.5 / 1e3 se guardan truncados a entero
};

// false + error (en español) si no es JSON válido o supera los límites. Solo enteros (sin decimales
// ni exponentes): ningún dato de mods los necesita y así no hay ambigüedad al comparar.
bool parse_json(const std::string& text, Json& out, std::string& error, const JsonLimits& limits = {});

}  // namespace wl::mods
