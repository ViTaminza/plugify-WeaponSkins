// WeaponSkins — configuration parsing (config.toml, databases.cfg, database.jsonc, translations).
//
// Implements the contract in src/config.hpp:
//   * ParseConfig / LoadConfig      — the production config.toml subset reader;
//   * ParseDatabasesCfg / LoadDatabasesCfg / LoadPluginDatabase — the standard's connection file
//     (docs/PLUGIN_STANDARD.md §4: databases.cfg, KeyValues, section = connection name "skins");
//   * ParseDatabaseConfig / LoadDatabaseConfig — SwiftlyS2's configs/database.jsonc, the legacy
//     connection source kept for the transition period (§4 "переходный период");
//   * ReplaceToken                  — "{url}" / "{seconds}" substitution for the command messages;
//   * Translations                  — resources/translations/*.jsonc with the client/server fallback.
//
// Facts this file deliberately follows:
//   * The C++ port supports production only: StorageBackend = "pisex" and
//     InventoryUpdateBackend = "hook". The original treated anything else as a hard error
//     (WeaponSkins.Database/StorageService.cs:91 throws for an unknown backend,
//     WeaponSkins.Injections/InventoryServiceInjection.cs:20 for an unknown update backend),
//     so rejecting them here is faithful, not a narrowing.
//   * Defaults live in models.hpp (the production values). ParseConfig starts from a default
//     Config and only overwrites the keys present in the text, exactly like the original's
//     configuration binding did.
//   * Unknown keys and unknown tables are ignored (the original's binder ignored them too);
//     known keys with a wrong type, duplicate keys, malformed syntax and unsupported backends
//     are errors.
//   * Errors always name the offending key and the line number, and NEVER echo raw file
//     content — the same reader/parser pair is used for files that may contain credentials
//     (database.jsonc), where a leak into a log or a crash report would be unacceptable.
//   * database.jsonc layout = SwiftlyS2's ("connections": { name: object | "driver://..." URI }),
//     the same file and keys the C# port read (plugins_dotnet/WeaponSkins/src/Core/Core.cs:277).
//   * databases.cfg is read with pisexcompat's KeyValues reader (libs/pisexcompat) — the same
//     reader the vip/admin_system/levels_ranks ports use — and the path is the caller's business
//     (pisex::ResolvePath maps the Metamod spelling "addons/configs/databases.cfg" onto
//     <GetConfigsDir()>/WeaponSkins/databases.cfg), so nothing here touches the host.
//
// No third-party code: the TOML subset is parsed here; JSONC uses the nlohmann::json already
// shipped in sdk/include (parse with ignore_comments), plgdb::Config comes from libs/plgdb.

#include "config.hpp"

#include <nlohmann/json.hpp>

#include <pisex/keyvalues.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ws {
namespace {

// File-size guards: every file we read here is a small hand-written config (production
// config.toml is ~1 KB, database.jsonc ~2 KB, a translation file ~2 KB). A file bigger than
// this is not one of ours and must not be allowed to balloon memory.
constexpr std::size_t kMaxConfigBytes = 1u << 20;

std::string AsciiLower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

bool IsSpaceNoNewline(char c) {
    return c == ' ' || c == '\t';
}

bool IsBareKeyChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
}

// Redacts a config-supplied token for an error message: printable ASCII only, 32 bytes max.
// Used for values whose exact content is safe to show (an unsupported backend/driver name),
// never for free-form text.
std::string SanitizeToken(std::string_view text) {
    std::string out;
    out.reserve(std::min<std::size_t>(text.size(), 32));
    for (char c : text.substr(0, 32)) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back(u >= 0x20 && u < 0x7F ? c : '?');
    }
    return out;
}

std::string ErrorAt(int line, std::string_view message) {
    return "line " + std::to_string(line) + ": " + std::string(message);
}

// ---------------------------------------------------------------------------------------------
// TOML subset reader
// ---------------------------------------------------------------------------------------------

struct TomlValue {
    enum class Kind { String, Integer, Boolean, Array } kind = Kind::String;
    std::string string;
    long long integer = 0;
    bool boolean = false;
    std::vector<TomlValue> items; // Array
    int line = 0;
};

// A parsed document: full key ("Main.Commands.SiteUrl") -> value (with its line).
using KeyMap = std::map<std::string, TomlValue>;

class TomlReader {
public:
    explicit TomlReader(std::string_view text) : text_(text) {
        // Skip a UTF-8 BOM (Windows-authored configs); the original TOML binder did too.
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            pos_ = 3;
        }
    }

    bool Parse(KeyMap& keys, std::string& error) {
        while (true) {
            SkipSpaces();
            if (AtEnd()) {
                return true;
            }
            const char c = text_[pos_];
            if (c == '\n') {
                ++pos_;
                ++line_;
                continue;
            }
            if (c == '\r') {
                ++pos_;
                continue;
            }
            if (c == '#') {
                SkipComment();
                continue;
            }
            if (c == '[') {
                if (!ParseTableHeader(error)) {
                    return false;
                }
                continue;
            }
            if (!ParseAssignment(keys, error)) {
                return false;
            }
        }
    }

private:
    bool AtEnd() const { return pos_ >= text_.size(); }
    char Cur() const { return text_[pos_]; }

    void SkipSpaces() {
        while (!AtEnd() && IsSpaceNoNewline(Cur())) {
            ++pos_;
        }
    }

    // '#' to the end of line. Only ever called outside a quoted string, which is what the
    // "comments only outside quotes" rule means.
    void SkipComment() {
        while (!AtEnd() && Cur() != '\n') {
            ++pos_;
        }
    }

    // Whitespace incl. newlines and comments (inside arrays / between statements).
    void SkipInsignificant() {
        while (!AtEnd()) {
            const char c = Cur();
            if (c == '\n') {
                ++line_;
                ++pos_;
            } else if (IsSpaceNoNewline(c) || c == '\r') {
                ++pos_;
            } else if (c == '#') {
                SkipComment();
            } else {
                break;
            }
        }
    }

    // After a value or a table header: only spaces, a comment and end-of-line may follow.
    bool ExpectLineEnd(std::string_view context, std::string& error) {
        SkipSpaces();
        if (AtEnd()) {
            return true;
        }
        if (Cur() == '#') {
            SkipComment();
            return true;
        }
        if (Cur() == '\n' || Cur() == '\r') {
            return true;
        }
        error = ErrorAt(line_, std::string("unexpected characters after ") + std::string(context));
        return false;
    }

    bool ReadBareKey(std::string& out) {
        out.clear();
        while (!AtEnd() && IsBareKeyChar(Cur())) {
            out.push_back(Cur());
            ++pos_;
        }
        return !out.empty();
    }

    // One key part: bare key or a basic/literal quoted key.
    bool ReadKeyPart(std::string& out, std::string& error) {
        if (AtEnd()) {
            error = ErrorAt(line_, "expected a key");
            return false;
        }
        if (Cur() == '"' || Cur() == '\'') {
            return ReadString(out, error);
        }
        if (!ReadBareKey(out)) {
            error = ErrorAt(line_, "expected a key");
            return false;
        }
        return true;
    }

    bool ParseTableHeader(std::string& error) {
        const int line = line_;
        ++pos_; // '['
        SkipSpaces();
        std::string table;
        while (true) {
            std::string part;
            if (!ReadKeyPart(part, error)) {
                return false;
            }
            if (!table.empty()) {
                table.push_back('.');
            }
            table += part;
            SkipSpaces();
            if (AtEnd() || Cur() != '.') {
                break;
            }
            ++pos_;
            SkipSpaces();
        }
        if (AtEnd() || Cur() != ']') {
            error = ErrorAt(line, "malformed table header: expected ']'");
            return false;
        }
        ++pos_;
        if (!ExpectLineEnd("the table header", error)) {
            return false;
        }
        currentTable_ = table;
        return true;
    }

    bool ParseAssignment(KeyMap& keys, std::string& error) {
        const int line = line_;
        std::string key;
        if (!ReadKeyPart(key, error)) {
            return false;
        }
        // TOML dotted keys: a.b = value.
        SkipSpaces();
        while (!AtEnd() && Cur() == '.') {
            ++pos_;
            SkipSpaces();
            std::string part;
            if (!ReadKeyPart(part, error)) {
                return false;
            }
            key.push_back('.');
            key += part;
            SkipSpaces();
        }

        if (AtEnd() || Cur() != '=') {
            error = ErrorAt(line, "expected '=' after the key '" + key + "'");
            return false;
        }
        ++pos_;
        SkipSpaces();

        TomlValue value;
        value.line = line;
        if (!ParseValue(value, error)) {
            return false;
        }
        if (!ExpectLineEnd("the value of '" + key + "'", error)) {
            return false;
        }

        const std::string full = currentTable_.empty() ? key : currentTable_ + "." + key;
        if (keys.count(full) != 0) {
            error = ErrorAt(line, "duplicate key '" + full + "'");
            return false;
        }
        keys.emplace(full, std::move(value));
        return true;
    }

    bool ParseValue(TomlValue& value, std::string& error) {
        if (AtEnd()) {
            error = ErrorAt(line_, "expected a value");
            return false;
        }
        const char c = Cur();
        if (c == '"' || c == '\'') {
            value.kind = TomlValue::Kind::String;
            return ReadString(value.string, error);
        }
        if (c == '[') {
            value.kind = TomlValue::Kind::Array;
            return ReadArray(value.items, error);
        }
        if (c == 't' || c == 'f') {
            std::string word;
            ReadBareKey(word);
            if (word == "true") {
                value.kind = TomlValue::Kind::Boolean;
                value.boolean = true;
                return true;
            }
            if (word == "false") {
                value.kind = TomlValue::Kind::Boolean;
                value.boolean = false;
                return true;
            }
            error = ErrorAt(line_, "unsupported value (only strings, integers, booleans and "
                                   "arrays are supported)");
            return false;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '+') {
            return ReadInteger(value, error);
        }
        error = ErrorAt(line_, "unsupported value (only strings, integers, booleans and arrays "
                               "are supported)");
        return false;
    }

    bool ReadInteger(TomlValue& value, std::string& error) {
        const std::size_t start = pos_;
        if (Cur() == '-' || Cur() == '+') {
            ++pos_;
        }
        const std::size_t digits = pos_;
        while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Cur())) != 0) {
            ++pos_;
        }
        if (pos_ == digits) {
            error = ErrorAt(line_, "unsupported value (expected an integer)");
            return false;
        }
        const std::string_view token = text_.substr(start, pos_ - start);
        long long parsed = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
        if (result.ec != std::errc() || result.ptr != token.data() + token.size()) {
            error = ErrorAt(line_, "integer value is out of range");
            return false;
        }
        value.kind = TomlValue::Kind::Integer;
        value.integer = parsed;
        return true;
    }

    bool ReadArray(std::vector<TomlValue>& items, std::string& error) {
        const int openLine = line_;
        ++pos_; // '['
        while (true) {
            SkipInsignificant();
            if (AtEnd()) {
                error = ErrorAt(openLine, "unterminated array");
                return false;
            }
            if (Cur() == ']') {
                ++pos_;
                return true;
            }
            TomlValue item;
            item.line = line_;
            if (!ParseValue(item, error)) {
                return false;
            }
            items.push_back(std::move(item));
            SkipInsignificant();
            if (AtEnd()) {
                error = ErrorAt(openLine, "unterminated array");
                return false;
            }
            if (Cur() == ',') {
                ++pos_;
                continue;
            }
            if (Cur() == ']') {
                ++pos_;
                return true;
            }
            error = ErrorAt(line_, "expected ',' or ']' in the array");
            return false;
        }
    }

    // Basic ("...", escapes) or literal ('...', no escapes) string. Newlines are not allowed
    // (the production files never use multiline strings).
    bool ReadString(std::string& out, std::string& error) {
        const char quote = Cur();
        ++pos_;
        out.clear();
        while (true) {
            if (AtEnd() || Cur() == '\n') {
                error = ErrorAt(line_, "unterminated string");
                return false;
            }
            const char c = Cur();
            ++pos_;
            if (c == quote) {
                return true;
            }
            if (quote == '\'' || c != '\\') {
                out.push_back(c);
                continue;
            }
            // Escape sequence (basic strings only).
            if (AtEnd()) {
                error = ErrorAt(line_, "unterminated string");
                return false;
            }
            const char esc = Cur();
            ++pos_;
            switch (esc) {
                case 'b': out.push_back('\b'); break;
                case 't': out.push_back('\t'); break;
                case 'n': out.push_back('\n'); break;
                case 'f': out.push_back('\f'); break;
                case 'r': out.push_back('\r'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'u':
                case 'U': {
                    const int digits = esc == 'u' ? 4 : 8;
                    if (!ReadUnicodeEscape(digits, out, error)) {
                        return false;
                    }
                    break;
                }
                default:
                    error = ErrorAt(line_, "invalid escape sequence in a string");
                    return false;
            }
        }
    }

    bool ReadHex(int digits, uint32_t& out) {
        if (pos_ + static_cast<std::size_t>(digits) > text_.size()) {
            return false;
        }
        out = 0;
        for (int i = 0; i < digits; ++i) {
            const char c = text_[pos_ + static_cast<std::size_t>(i)];
            uint32_t value = 0;
            if (c >= '0' && c <= '9') {
                value = static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value = static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value = static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
            out = (out << 4) | value;
        }
        pos_ += static_cast<std::size_t>(digits);
        return true;
    }

    // \uXXXX / \UXXXXXXXX -> UTF-8, with surrogate pairs.
    bool ReadUnicodeEscape(int digits, std::string& out, std::string& error) {
        uint32_t code = 0;
        if (!ReadHex(digits, code)) {
            error = ErrorAt(line_, "invalid unicode escape in a string");
            return false;
        }
        if (code >= 0xD800 && code <= 0xDBFF) {
            // High surrogate: require the low half right after.
            if (pos_ + 6 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                error = ErrorAt(line_, "invalid unicode escape in a string (unpaired surrogate)");
                return false;
            }
            pos_ += 2;
            uint32_t low = 0;
            if (!ReadHex(4, low) || low < 0xDC00 || low > 0xDFFF) {
                error = ErrorAt(line_, "invalid unicode escape in a string (unpaired surrogate)");
                return false;
            }
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            error = ErrorAt(line_, "invalid unicode escape in a string (unpaired surrogate)");
            return false;
        }
        if (code > 0x10FFFF) {
            error = ErrorAt(line_, "invalid unicode escape in a string");
            return false;
        }
        AppendUtf8(code, out);
        return true;
    }

    static void AppendUtf8(uint32_t code, std::string& out) {
        if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    int line_ = 1;
    std::string currentTable_;
};

// --- typed accessors over the parsed KeyMap ------------------------------------------------

bool ReadBool(const KeyMap& keys, const std::string& full, bool& target, std::string& error) {
    const auto it = keys.find(full);
    if (it == keys.end()) {
        return true;
    }
    if (it->second.kind != TomlValue::Kind::Boolean) {
        error = ErrorAt(it->second.line, "key '" + full + "' expects a boolean");
        return false;
    }
    target = it->second.boolean;
    return true;
}

bool ReadString(const KeyMap& keys, const std::string& full, std::string& target,
                std::string& error) {
    const auto it = keys.find(full);
    if (it == keys.end()) {
        return true;
    }
    if (it->second.kind != TomlValue::Kind::String) {
        error = ErrorAt(it->second.line, "key '" + full + "' expects a string");
        return false;
    }
    target = it->second.string;
    return true;
}

bool ReadInt(const KeyMap& keys, const std::string& full, int& target, std::string& error) {
    const auto it = keys.find(full);
    if (it == keys.end()) {
        return true;
    }
    if (it->second.kind != TomlValue::Kind::Integer) {
        error = ErrorAt(it->second.line, "key '" + full + "' expects an integer");
        return false;
    }
    if (it->second.integer < INT32_MIN || it->second.integer > INT32_MAX) {
        error = ErrorAt(it->second.line, "key '" + full + "' is out of the 32-bit range");
        return false;
    }
    target = static_cast<int>(it->second.integer);
    return true;
}

bool ReadStringArray(const KeyMap& keys, const std::string& full, std::vector<std::string>& target,
                     std::string& error) {
    const auto it = keys.find(full);
    if (it == keys.end()) {
        return true;
    }
    if (it->second.kind != TomlValue::Kind::Array) {
        error = ErrorAt(it->second.line, "key '" + full + "' expects an array of strings");
        return false;
    }
    std::vector<std::string> values;
    values.reserve(it->second.items.size());
    for (const TomlValue& item : it->second.items) {
        if (item.kind != TomlValue::Kind::String) {
            error = ErrorAt(item.line, "key '" + full + "' expects an array of strings");
            return false;
        }
        values.push_back(item.string);
    }
    target = std::move(values);
    return true;
}

bool ReadBackend(const KeyMap& keys, const std::string& full, const char* supported,
                 std::string& target, std::string& error) {
    const auto it = keys.find(full);
    if (it == keys.end()) {
        return true;
    }
    if (it->second.kind != TomlValue::Kind::String) {
        error = ErrorAt(it->second.line, "key '" + full + "' expects a string");
        return false;
    }
    if (AsciiLower(it->second.string) != supported) {
        error = ErrorAt(it->second.line,
                        "unsupported " + full + " '" + SanitizeToken(it->second.string) +
                            "' (the production port supports only '" + supported + "')");
        return false;
    }
    target = supported;
    return true;
}

bool ReadFileLimited(const std::filesystem::path& path, std::size_t limit, std::string& out,
                     std::string& error, std::string_view what) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && size > limit) {
        error = std::string(what) + " is too large to be a configuration file";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = std::string("cannot open the ") + std::string(what) + ": " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad()) {
        error = std::string("cannot read the ") + std::string(what) + ": " + path.string();
        return false;
    }
    if (out.size() > limit) {
        error = std::string(what) + " is too large to be a configuration file";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// database.jsonc
// ---------------------------------------------------------------------------------------------

const nlohmann::json* FindMemberCI(const nlohmann::json& object, std::string_view name) {
    if (!object.is_object()) {
        return nullptr;
    }
    const std::string wanted = AsciiLower(name);
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (AsciiLower(it.key()) == wanted) {
            return &it.value();
        }
    }
    return nullptr;
}

// Optional string field; `present` reports whether the key was found at all.
bool JsonString(const nlohmann::json& object, std::string_view name, std::string& out,
                bool& present) {
    const nlohmann::json* value = FindMemberCI(object, name);
    present = value != nullptr;
    if (!present) {
        return true;
    }
    if (value->is_string()) {
        out = value->get<std::string>();
        return true;
    }
    if (value->is_number_integer()) {
        out = std::to_string(value->get<long long>());
        return true;
    }
    return false;
}

bool JsonInt(const nlohmann::json& object, std::string_view name, long long& out, bool& present) {
    const nlohmann::json* value = FindMemberCI(object, name);
    present = value != nullptr;
    if (!present) {
        return true;
    }
    if (value->is_number_integer()) {
        out = value->get<long long>();
        return true;
    }
    if (value->is_string()) {
        const std::string text = value->get<std::string>();
        if (text.empty()) {
            return false;
        }
        long long parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
            return false;
        }
        out = parsed;
        return true;
    }
    return false;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Minimal percent-decoding for userinfo in a URI. Invalid escapes are kept verbatim
// (SwiftlyS2 writes plain credentials into this file, so this is a compatibility nicety).
std::string PercentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && HexDigit(text[i + 1]) >= 0 &&
            HexDigit(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(HexDigit(text[i + 1]) * 16 + HexDigit(text[i + 2])));
            i += 2;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

bool ApplyPort(long long port, std::string_view connection, plgdb::Config& out, std::string& error) {
    if (port < 1 || port > 65535) {
        error = "connection '" + std::string(connection) + "': the port must be in 1..65535";
        return false;
    }
    out.port = static_cast<uint16_t>(port);
    return true;
}

// "mysql://user:pass@host:3306/database" (SwiftlyS2's URI form). Errors never echo the URI:
// it contains the password.
bool ParseDatabaseUri(std::string_view uri, std::string_view connection, plgdb::Config& out,
                      std::string& error) {
    const std::size_t schemeEnd = uri.find("://");
    if (schemeEnd == std::string_view::npos) {
        error = "connection '" + std::string(connection) + "': malformed connection URI";
        return false;
    }
    const std::string scheme = AsciiLower(uri.substr(0, schemeEnd));
    if (scheme != "mysql" && scheme != "mariadb") {
        error = "connection '" + std::string(connection) + "': unsupported driver '" +
                SanitizeToken(scheme) + "' (only mysql/mariadb is supported)";
        return false;
    }
    std::string_view rest = uri.substr(schemeEnd + 3);

    // userinfo@host... — split at the LAST '@' so an unencoded '@' inside the password still
    // leaves a usable host part.
    const std::size_t at = rest.rfind('@');
    if (at != std::string_view::npos) {
        const std::string_view userinfo = rest.substr(0, at);
        rest = rest.substr(at + 1);
        const std::size_t colon = userinfo.find(':');
        if (colon == std::string_view::npos) {
            out.user = PercentDecode(userinfo);
        } else {
            out.user = PercentDecode(userinfo.substr(0, colon));
            out.password = PercentDecode(userinfo.substr(colon + 1));
        }
    }

    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
        error = "connection '" + std::string(connection) + "': the URI has no database name";
        return false;
    }
    const std::string_view hostPort = rest.substr(0, slash);
    const std::string_view database = rest.substr(slash + 1);
    if (database.empty()) {
        error = "connection '" + std::string(connection) + "': the URI has no database name";
        return false;
    }
    if (hostPort.empty()) {
        error = "connection '" + std::string(connection) + "': the URI has no host";
        return false;
    }

    std::string_view host = hostPort;
    long long port = 3306;
    if (!hostPort.empty() && hostPort.front() == '[') {
        const std::size_t close = hostPort.find(']');
        if (close == std::string_view::npos) {
            error = "connection '" + std::string(connection) + "': malformed host in the URI";
            return false;
        }
        host = hostPort.substr(1, close - 1);
        if (close + 1 < hostPort.size()) {
            if (hostPort[close + 1] != ':') {
                error = "connection '" + std::string(connection) + "': malformed host in the URI";
                return false;
            }
            const std::string_view portText = hostPort.substr(close + 2);
            if (!portText.empty()) {
                long long parsed = 0;
                const auto result =
                    std::from_chars(portText.data(), portText.data() + portText.size(), parsed);
                if (result.ec != std::errc() || result.ptr != portText.data() + portText.size()) {
                    error = "connection '" + std::string(connection) + "': malformed port in the URI";
                    return false;
                }
                port = parsed;
            }
        }
    } else {
        const std::size_t colon = hostPort.rfind(':');
        if (colon != std::string_view::npos) {
            host = hostPort.substr(0, colon);
            const std::string_view portText = hostPort.substr(colon + 1);
            if (!portText.empty()) {
                long long parsed = 0;
                const auto result =
                    std::from_chars(portText.data(), portText.data() + portText.size(), parsed);
                if (result.ec != std::errc() || result.ptr != portText.data() + portText.size()) {
                    error = "connection '" + std::string(connection) + "': malformed port in the URI";
                    return false;
                }
                port = parsed;
            }
        }
    }
    if (host.empty()) {
        error = "connection '" + std::string(connection) + "': the URI has no host";
        return false;
    }

    out.host = std::string(host);
    out.database = std::string(database);
    return ApplyPort(port, connection, out, error);
}

bool ParseDatabaseObject(const nlohmann::json& entry, std::string_view connection,
                         plgdb::Config& out, std::string& error) {
    std::string driver = "mysql";
    bool present = false;
    if (!JsonString(entry, "driver", driver, present)) {
        error = "connection '" + std::string(connection) + "': 'driver' must be a string";
        return false;
    }
    const std::string driverLower = AsciiLower(driver);
    if (driverLower != "mysql" && driverLower != "mariadb") {
        error = "connection '" + std::string(connection) + "': unsupported driver '" +
                SanitizeToken(driver) + "' (only mysql/mariadb is supported)";
        return false;
    }

    if (!JsonString(entry, "host", out.host, present) || !present || out.host.empty()) {
        error = "connection '" + std::string(connection) + "': 'host' is missing";
        return false;
    }
    if (!JsonString(entry, "database", out.database, present) || !present ||
        out.database.empty()) {
        error = "connection '" + std::string(connection) + "': 'database' is missing";
        return false;
    }
    // The user key must be present (possibly empty only if the server really has no user);
    // a typo in its name would otherwise turn into an obscure auth failure at connect time.
    if (!JsonString(entry, "user", out.user, present) || !present) {
        error = "connection '" + std::string(connection) + "': 'user' is missing";
        return false;
    }

    // "pass" first, "password" as the fallback (the C# port read them in this order).
    std::string password;
    bool hasPass = false;
    if (!JsonString(entry, "pass", password, hasPass)) {
        error = "connection '" + std::string(connection) + "': 'pass' must be a string";
        return false;
    }
    if (!hasPass) {
        std::string legacy;
        bool hasLegacy = false;
        if (!JsonString(entry, "password", legacy, hasLegacy)) {
            error = "connection '" + std::string(connection) + "': 'password' must be a string";
            return false;
        }
        if (hasLegacy) {
            password = legacy;
        }
    }
    out.password = password;

    long long port = 3306;
    if (!JsonInt(entry, "port", port, present)) {
        error = "connection '" + std::string(connection) + "': 'port' must be an integer";
        return false;
    }
    if (!ApplyPort(port, connection, out, error)) {
        return false;
    }

    // SwiftlyS2's connection timeout; the C# port clamped it into [5, 30] seconds.
    long long timeout = 30;
    if (!JsonInt(entry, "timeout", timeout, present)) {
        error = "connection '" + std::string(connection) + "': 'timeout' must be an integer";
        return false;
    }
    out.connectTimeoutSec =
        static_cast<unsigned>(std::min<long long>(30, std::max<long long>(5, timeout)));

    std::string charset = "utf8mb4";
    if (!JsonString(entry, "charset", charset, present)) {
        error = "connection '" + std::string(connection) + "': 'charset' must be a string";
        return false;
    }
    if (present && charset.empty()) {
        error = "connection '" + std::string(connection) + "': 'charset' is empty";
        return false;
    }
    out.charset = charset;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

bool ParseConfig(std::string_view text, Config& config, std::string& error) {
    error.clear();
    KeyMap keys;
    TomlReader reader(text);
    if (!reader.Parse(keys, error)) {
        return false;
    }

    // Start from the contract defaults (models.hpp = production values); a failed parse must
    // not leave a half-filled config behind, so everything lands in a local copy.
    Config parsed{};

    bool ok = true;
    ok = ok && ReadBackend(keys, "Main.StorageBackend", "pisex", parsed.storageBackend, error);
    ok = ok && ReadBackend(keys, "Main.InventoryUpdateBackend", "hook", parsed.inventoryBackend,
                           error);
    ok = ok && ReadBool(keys, "Main.MusicKitMvpAnthem", parsed.musicKitMvpAnthem, error);
    ok = ok && ReadBool(keys, "Main.ResetGlovesOnDeath", parsed.resetGlovesOnDeath, error);
    ok = ok && ReadBool(keys, "Main.SyncFromDatabaseWhenPlayerJoin", parsed.syncOnJoin, error);
    ok = ok && ReadStringArray(keys, "Main.ItemLanguages", parsed.itemLanguages, error);

    ok = ok && ReadString(keys, "Main.Pisex.ConnectionName", parsed.pisex.connectionName, error);
    ok = ok && ReadString(keys, "Main.Pisex.TablePrefix", parsed.pisex.tablePrefix, error);
    ok = ok && ReadInt(keys, "Main.Pisex.ServerId", parsed.pisex.serverId, error);
    ok = ok && ReadBool(keys, "Main.Pisex.AutoCreatePlayer", parsed.pisex.autoCreatePlayer, error);
    ok = ok && ReadBool(keys, "Main.Pisex.UseExtTable", parsed.pisex.useExtTable, error);
    ok = ok && ReadBool(keys, "Main.Pisex.LazyLoadPerPlayer", parsed.pisex.lazyLoadPerPlayer,
                        error);
    ok = ok && ReadBool(keys, "Main.Pisex.PinsEnabled", parsed.pisex.pinsEnabled, error);

    ok = ok && ReadBool(keys, "Main.Commands.RedirectMenusToSite", parsed.commands.redirect,
                        error);
    ok = ok && ReadString(keys, "Main.Commands.SiteUrl", parsed.commands.siteUrl, error);
    ok = ok && ReadStringArray(keys, "Main.Commands.MenuAliases", parsed.commands.aliases, error);
    ok = ok && ReadString(keys, "Main.Commands.SyncCommand", parsed.commands.syncCommand, error);
    ok = ok && ReadInt(keys, "Main.Commands.SyncCooldownSeconds", parsed.commands.cooldownSeconds,
                       error);
    ok = ok && ReadString(keys, "Main.Commands.RedirectMessage", parsed.commands.redirectMessage,
                          error);
    ok = ok && ReadString(keys, "Main.Commands.SyncStartedMessage", parsed.commands.startedMessage,
                          error);
    ok = ok && ReadString(keys, "Main.Commands.SyncDoneMessage", parsed.commands.doneMessage,
                          error);
    ok = ok && ReadString(keys, "Main.Commands.SyncEmptyMessage", parsed.commands.emptyMessage,
                          error);
    ok = ok && ReadString(keys, "Main.Commands.SyncCooldownMessage", parsed.commands.cooldownMessage,
                          error);
    ok = ok && ReadString(keys, "Main.Commands.SyncErrorMessage", parsed.commands.errorMessage,
                          error);

    ok = ok && ReadString(keys, "Main.ItemPermissions.WeaponSkins", parsed.permissions.weapons,
                          error);
    ok = ok && ReadString(keys, "Main.ItemPermissions.KnifeSkins", parsed.permissions.knives,
                          error);
    ok = ok && ReadString(keys, "Main.ItemPermissions.GloveSkins", parsed.permissions.gloves,
                          error);
    ok = ok && ReadString(keys, "Main.ItemPermissions.Stickers", parsed.permissions.stickers,
                          error);
    ok = ok && ReadString(keys, "Main.ItemPermissions.Keychains", parsed.permissions.keychains,
                          error);
    ok = ok && ReadString(keys, "Main.ItemPermissions.Agents", parsed.permissions.agents, error);
    ok = ok && ReadString(keys, "Main.ItemPermissions.MusicKits", parsed.permissions.music, error);

    if (!ok) {
        return false;
    }

    config = std::move(parsed);
    return true;
}

bool LoadConfig(const std::filesystem::path& path, Config& config, std::string& error) {
    error.clear();
    std::string text;
    if (!ReadFileLimited(path, kMaxConfigBytes, text, error, "config file")) {
        return false;
    }
    return ParseConfig(text, config, error);
}

std::optional<plgdb::Config> ParseDatabaseConfig(std::string_view jsonc,
                                                 std::string_view connection,
                                                 std::string& error) {
    error.clear();
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(std::string(jsonc), nullptr, /*allow_exceptions=*/true,
                                     /*ignore_comments=*/true);
    } catch (const nlohmann::json::parse_error& e) {
        // e.what() can quote the offending input (i.e. the password): report the byte offset
        // only, never the text.
        error = "invalid JSONC in the database config (parse error at byte " +
                std::to_string(e.byte) + ")";
        return std::nullopt;
    } catch (const nlohmann::json::exception&) {
        error = "invalid JSONC in the database config";
        return std::nullopt;
    }

    if (!root.is_object()) {
        error = "the database config must be a JSON object";
        return std::nullopt;
    }
    const nlohmann::json* connections = FindMemberCI(root, "connections");
    if (connections == nullptr || !connections->is_object()) {
        error = "the database config has no 'connections' section";
        return std::nullopt;
    }
    const nlohmann::json* entry = FindMemberCI(*connections, connection);
    if (entry == nullptr) {
        error = "connection '" + SanitizeToken(connection) + "' not found in the database config";
        return std::nullopt;
    }

    plgdb::Config out;
    out.driver = plgdb::Driver::MySQL;
    if (entry->is_string()) {
        if (!ParseDatabaseUri(entry->get<std::string>(), connection, out, error)) {
            return std::nullopt;
        }
        return out;
    }
    if (entry->is_object()) {
        if (!ParseDatabaseObject(*entry, connection, out, error)) {
            return std::nullopt;
        }
        return out;
    }
    error = "connection '" + SanitizeToken(connection) +
            "' must be an object or a connection URI string";
    return std::nullopt;
}

std::optional<plgdb::Config> LoadDatabaseConfig(const std::filesystem::path& path,
                                                std::string_view connection,
                                                std::string& error) {
    error.clear();
    std::string text;
    if (!ReadFileLimited(path, kMaxConfigBytes, text, error, "database config")) {
        return std::nullopt;
    }
    return ParseDatabaseConfig(text, connection, error);
}

// ---------------------------------------------------------------------------------------------
// databases.cfg (the standard's connection file)
// ---------------------------------------------------------------------------------------------

std::optional<plgdb::Config> ParseDatabasesCfg(std::string_view text, std::string_view connection,
                                               std::string& error) {
    error.clear();
    pisex::KeyValues kv("Databases");
    if (!kv.LoadFromBuffer(text)) {
        error = "invalid KeyValues in databases.cfg";
        return std::nullopt;
    }
    const std::string name(connection);
    const pisex::KeyValues* section = kv.FindKey(name.c_str());
    if (section == nullptr) {
        error = "databases.cfg has no '" + SanitizeToken(connection) + "' section";
        return std::nullopt;
    }

    // The keys the vip/admin_system/levels_ranks ports read, with the same defaults: an absent
    // section means "no database server here", never a broken one, so a MySQL block without a
    // database name fails at connect time rather than silently pointing at a foreign schema.
    plgdb::Config out;
    const std::string driver = section->GetString("driver", "mysql");
    out.driver = driver == "sqlite" ? plgdb::Driver::SQLite : plgdb::Driver::MySQL;
    out.host = section->GetString("host", "127.0.0.1");
    out.port = static_cast<uint16_t>(
        std::max(1, std::min(65535, section->GetInt("port", 3306))));
    out.user = section->GetString("user", "");
    out.password = section->GetString("pass", "");
    out.database = section->GetString("database", "");
    if (out.driver == plgdb::Driver::SQLite) {
        out.sqlitePath = section->GetString("sqlite_path", "WeaponSkins.sqlite3");
    }
    return out;
}

std::optional<plgdb::Config> LoadDatabasesCfg(const std::filesystem::path& path,
                                              std::string_view connection, std::string& error) {
    error.clear();
    std::string text;
    if (!ReadFileLimited(path, kMaxConfigBytes, text, error, "databases.cfg")) {
        return std::nullopt;
    }
    return ParseDatabasesCfg(text, connection, error);
}

std::optional<plgdb::Config> LoadPluginDatabase(std::string_view databasesCfgPath,
                                                std::string_view legacyJsoncPath,
                                                std::string_view connection, bool& usedLegacy,
                                                std::string& error) {
    usedLegacy = false;
    error.clear();
    std::string cfgError;
    if (const std::optional<plgdb::Config> standard =
            LoadDatabasesCfg(std::filesystem::path(std::string(databasesCfgPath)), connection,
                             cfgError)) {
        return standard;
    }
    std::string legacyError;
    if (const std::optional<plgdb::Config> legacy =
            LoadDatabaseConfig(std::filesystem::path(std::string(legacyJsoncPath)), connection,
                               legacyError)) {
        usedLegacy = true;
        return legacy;
    }
    error = "databases.cfg not usable (" + cfgError + "), legacy database.jsonc not usable (" +
            legacyError + ")";
    return std::nullopt;
}

std::string ReplaceToken(std::string text, std::string_view token, std::string_view value) {
    // Accept both "url" and "{url}"; the production config messages use "{url}"/"{seconds}".
    if (token.size() >= 2 && token.front() == '{' && token.back() == '}') {
        token.remove_prefix(1);
        token.remove_suffix(1);
    }
    if (token.empty()) {
        return text;
    }
    const std::string needle = "{" + std::string(token) + "}";
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), value);
        pos += value.size();
    }
    return text;
}

// ---------------------------------------------------------------------------------------------
// Translations
// ---------------------------------------------------------------------------------------------

namespace {

// CS2 language names (the ones the original mapped for econ data) and the file codes.
// File stems are stored lowercased ("zh-cn"), so the lookup is case-insensitive.
const char* const kLanguageAliases[][2] = {
    {"arabic", "ar"},       {"bulgarian", "bg"},   {"schinese", "zh-cn"},
    {"chinese", "zh-cn"},   {"tchinese", "zh-tw"}, {"czech", "cs"},
    {"danish", "da"},       {"dutch", "nl"},       {"english", "en"},
    {"finnish", "fi"},      {"french", "fr"},      {"german", "de"},
    {"greek", "el"},        {"hungarian", "hu"},   {"indonesian", "id"},
    {"italian", "it"},      {"japanese", "ja"},    {"koreana", "ko"},
    {"korean", "ko"},       {"norwegian", "no"},   {"polish", "pl"},
    {"portuguese", "pt"},   {"brazilian", "pt-br"},{"romanian", "ro"},
    {"russian", "ru"},      {"spanish", "es"},     {"latam", "es-419"},
    {"swedish", "sv"},      {"thai", "th"},        {"turkish", "tr"},
    {"ukrainian", "uk"},    {"vietnamese", "vn"},
};

std::string NormalizeLanguage(std::string_view language) {
    std::string code = AsciiLower(Trim(language));
    if (code.empty()) {
        return "en";
    }
    for (const auto& alias : kLanguageAliases) {
        if (code == alias[0]) {
            return alias[1];
        }
    }
    return code;
}

// "{0}", "{1}", ... placeholders; anything else (including "{{0}}", "{x}", "{url}") is copied
// verbatim, and a placeholder without a matching argument stays literal — the C# localizer
// caught the FormatException and returned the raw text, this is the same behaviour.
std::string FormatPhrase(const std::string& text, const std::vector<std::string>& args) {
    if (args.empty() || text.find('{') == std::string::npos) {
        return text;
    }
    std::string out;
    out.reserve(text.size() + 16);
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '{') {
            out.push_back(text[i]);
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        std::size_t index = 0;
        bool digits = false;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])) != 0) {
            index = index * 10 + static_cast<std::size_t>(text[j] - '0');
            digits = true;
            ++j;
            if (index > 10000) {
                break; // keep the scan bounded; falls through to the literal branch
            }
        }
        if (digits && index <= 10000 && j < text.size() && text[j] == '}') {
            if (index < args.size()) {
                out += args[index];
            } else {
                out.append(text, i, j - i + 1);
            }
            i = j + 1;
            continue;
        }
        out.push_back('{');
        ++i;
    }
    return out;
}

} // namespace

bool Translations::Load(const std::filesystem::path& directory, std::string& error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        languages_.clear();
        error = "translations directory not found: " + directory.string();
        return false;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string extension = AsciiLower(entry.path().extension().string());
        if (extension == ".jsonc" || extension == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        languages_.clear();
        error = "no translation files in " + directory.string();
        return false;
    }

    // Best effort like the C# port: one broken file does not throw the rest away, but the
    // caller is told about it (it returns false together with the first error).
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> loaded;
    bool ok = true;

    for (const auto& file : files) {
        std::string text;
        std::string fileError;
        if (!ReadFileLimited(file, kMaxConfigBytes, text, fileError, "translation file")) {
            if (ok) {
                error = fileError;
            }
            ok = false;
            continue;
        }
        nlohmann::json root;
        try {
            root = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/true,
                                         /*ignore_comments=*/true);
        } catch (const nlohmann::json::exception&) {
            if (ok) {
                error = "failed to parse the translation file '" + file.filename().string() + "'";
            }
            ok = false;
            continue;
        }
        if (!root.is_object()) {
            if (ok) {
                error = "the translation file '" + file.filename().string() +
                        "' is not a JSON object";
            }
            ok = false;
            continue;
        }

        auto& table = loaded[AsciiLower(file.stem().string())];
        for (auto it = root.begin(); it != root.end(); ++it) {
            std::string value;
            if (it.value().is_string()) {
                value = it.value().get<std::string>();
            } else if (it.value().is_number_integer()) {
                value = std::to_string(it.value().get<long long>());
            } else if (it.value().is_number_float()) {
                value = it.value().dump();
            } else if (it.value().is_boolean()) {
                value = it.value().get<bool>() ? "true" : "false";
            } else {
                if (ok) {
                    error = "the translation value for key '" + SanitizeToken(it.key()) +
                            "' in '" + file.filename().string() + "' is not a string";
                }
                ok = false;
                continue;
            }
            table[AsciiLower(it.key())] = std::move(value);
        }
    }

    languages_ = std::move(loaded);
    return ok;
}

std::string Translations::Get(std::string_view language, std::string_view key,
                              const std::vector<std::string>& args) const {
    const std::string code = NormalizeLanguage(language);
    const std::string wanted = AsciiLower(key);

    // Fallback chain: the requested language, then its main subtag ("zh-hans" -> "zh"),
    // then "en", then the key itself (the original returned the key for a missing phrase).
    const std::string* phrase = nullptr;
    const auto find = [&](const std::string& candidate) -> const std::string* {
        const auto languageIt = languages_.find(candidate);
        if (languageIt == languages_.end()) {
            return nullptr;
        }
        const auto phraseIt = languageIt->second.find(wanted);
        return phraseIt == languageIt->second.end() ? nullptr : &phraseIt->second;
    };

    phrase = find(code);
    if (phrase == nullptr) {
        const std::size_t dash = code.find('-');
        if (dash != std::string::npos && dash > 0) {
            phrase = find(code.substr(0, dash));
        }
    }
    if (phrase == nullptr && code != "en") {
        phrase = find("en");
    }
    if (phrase == nullptr) {
        return std::string(key);
    }
    return FormatPhrase(*phrase, args);
}

} // namespace ws
