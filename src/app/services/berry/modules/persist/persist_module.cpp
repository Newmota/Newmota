/*
  persist_module.cpp - Berry persist singleton storage bindings

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#include "services/berry/modules/persist/persist_module.h"
#include "core/storage/filesystem.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
#include "berry.h"
#include "be_exec.h"
#include "be_gc.h"
#include "be_map.h"
#include "be_string.h"
#include "be_vm.h"
}
#include "support/number.h"

namespace {
namespace fs = newmota::filesystem;
constexpr std::size_t kCapacity = 8192;
constexpr unsigned kDepth = 16;
constexpr char kPath[] = "/_persist.json";
constexpr char kTemporary[] = "/_persist.json.tmp";

struct Buffer {
    char* bytes;
    std::size_t size{};
    const void* ancestors[kDepth]{};

    bool append(const char* text, std::size_t count) {
        if (count > kCapacity - size)
            return false;
        std::memcpy(bytes + size, text, count);
        size += count;
        return true;
    }

    bool character(char c) { return append(&c, 1); }
};

bool string_json(bvm* vm, int index, Buffer& out) {
    const char* text = be_tostring(vm, index);
    const auto size = static_cast<std::size_t>(be_strlen(vm, index));
    if (!out.character('"'))
        return false;
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < size; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            if (!out.character('\\') || !out.character(static_cast<char>(c)))
                return false;
        } else if (c < 32) {
            const char escape[] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            if (!out.append(escape, sizeof escape))
                return false;
        } else if (!out.character(static_cast<char>(c)))
            return false;
    }
    return out.character('"');
}

/** @brief Encodes directly into bounded storage; only ancestors count as cycles. */
bool encode(bvm* vm, int index, Buffer& out, unsigned depth) {
    index = be_absindex(vm, index);
    be_stack_require(vm, 6);
    if (be_isnil(vm, index))
        return out.append("null", 4);
    if (be_isbool(vm, index))
        return be_tobool(vm, index) ? out.append("true", 4) : out.append("false", 5);
    if (be_isstring(vm, index))
        return string_json(vm, index, out);
    if (be_isint(vm, index) || be_isreal(vm, index)) {
        if (be_isint(vm, index)) {
            const auto number = newmota::support::decimal(std::int64_t(be_toint(vm, index)));
            return out.append(number.text, number.size);
        }
        const auto value = static_cast<double>(be_toreal(vm, index));
        if (!std::isfinite(value))
            return false;
        char number[64];
        const int size = std::snprintf(number, sizeof number, "%.17g", value);
        return size > 0 && static_cast<std::size_t>(size) < sizeof number &&
               out.append(number, static_cast<std::size_t>(size));
    }
    const bool map = be_ismapinstance(vm, index);
    if ((!map && !be_islistinstance(vm, index)) || depth == kDepth)
        return false;
    const void* identity = var_toobj(be_indexof(vm, index));
    for (unsigned i = 0; i < depth; ++i)
        if (out.ancestors[i] == identity)
            return false;
    out.ancestors[depth] = identity;
    be_getmember(vm, index, ".p");
    if (map ? !be_ismap(vm, -1) : !be_islist(vm, -1)) {
        be_pop(vm, 1);
        return false;
    }
    const int backing = be_absindex(vm, -1);
    be_pushiter(vm, backing);
    bool valid = out.character(map ? '{' : '[');
    bool first = true;
    while (valid && be_iter_hasnext(vm, backing)) {
        const int count = be_iter_next(vm, backing);
        if (!first)
            valid = out.character(',');
        first = false;
        if (valid && map) {
            valid = be_isstring(vm, -2) && string_json(vm, -2, out) && out.character(':');
        }
        if (valid)
            valid = encode(vm, -1, out, depth + 1);
        be_pop(vm, count);
    }
    be_pop(vm, 2);
    return valid && out.character(map ? '}' : ']');
}

void encode_protected(bvm* vm, void* opaque) {
    auto& out = *static_cast<Buffer*>(opaque);
    if (!be_ismapinstance(vm, 1) || !encode(vm, 1, out, 0))
        be_raise(vm, "value_error",
            "persist requires JSON values, string keys, at most 16 container levels "
            "and 8192 bytes");
}

int Write(bvm* vm) {
    Buffer out{static_cast<char*>(std::malloc(kCapacity))};
    if (!out.bytes)
        be_raise(vm, "memory_error", "persist buffer allocation failed");
    const int error = be_execprotected(vm, encode_protected, &out);
    bool written = false;
    if (!error) {
        written =
            fs::write_file(kTemporary, out.bytes, out.size) == fs::MutationResult::succeeded &&
            fs::rename(kTemporary, kPath) == fs::MutationResult::succeeded;
    }
    std::free(out.bytes);
    if (error)
        be_throw(vm, error);
    if (!written)
        be_raise(vm, "io_error", "persist save failed");
    be_return_nil(vm);
}

struct Input {
    char* bytes;
    std::size_t size;
};

void decode_protected(bvm* vm, void* opaque) {
    const auto& input = *static_cast<Input*>(opaque);
    be_getmodule(vm, "json");
    be_getmember(vm, -1, "load");
    be_pushnstring(vm, input.bytes, input.size);
    be_call(vm, 1);
    be_pop(vm, 1);
    be_remove(vm, -2);
    if (!be_ismapinstance(vm, -1))
        be_raise(vm, "value_error", "persist file must contain a JSON object");
    // Validate parsed values (including finite reals) without changing the committed file.
    Buffer out{input.bytes};
    if (!encode(vm, -1, out, 0))
        be_raise(vm, "value_error", "persist file contains unsupported values");
}

int Read(bvm* vm) {
    Input input{static_cast<char*>(std::malloc(kCapacity + 1)), 0};
    if (!input.bytes)
        be_raise(vm, "memory_error", "persist buffer allocation failed");
    const auto result = fs::read_file(kPath, input.bytes, kCapacity, input.size);
    bool valid = result == fs::ReadResult::succeeded;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (std::size_t i = 0; valid && i < input.size; ++i) {
        const char c = input.bytes[i];
        if (!c) {
            valid = false;
            break;
        }
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '[' || c == '{')
            valid = ++depth <= kDepth;
        else if (c == ']' || c == '}') {
            if (!depth)
                valid = false;
            else
                --depth;
        }
    }
    int error = 0;
    if (valid) {
        input.bytes[input.size] = 0;
        error = be_execprotected(vm, decode_protected, &input);
    }
    std::free(input.bytes);
    if (error)
        be_throw(vm, error);
    if (result == fs::ReadResult::not_found) {
        be_newobject(vm, "map");
        be_pop(vm, 1);
    } else if (!valid)
        be_raise(vm, "io_error", "persist file unreadable, oversized or invalid");
    be_return(vm);
}

constexpr char kSource[] = R"berry(
return def(m)
  class Persist
    var _data, _dirty
    def init() self._data = m._read() self._dirty = false end
    def member(key) return self._data.find(key) end
    def setmember(key, value)
      if type(key) != 'string' raise 'type_error', 'persist keys must be strings' end
      self._data[key] = value self._dirty = true
    end
    def has(key) return self._data.contains(key) end
    def contains(key) return self.has(key) end
    def find(key, default) return self._data.find(key, default) end
    def remove(key)
      self._data.remove(key) self._dirty = true
    end
    def zero() self._data = {} self._dirty = true end
    def dirty() self._dirty = true end
    def load()
      var data = m._read()
      self._data = data self._dirty = false
    end
    def save(force)
      if force self._dirty = true end
      if self._dirty m._write(self._data) self._dirty = false end
    end
    def tostring() return str(self._data) end
  end
  return Persist()
end
)berry";

int Init(bvm* vm) {
    int error = be_loadbuffer(vm, "persist", kSource, sizeof kSource - 1);
    if (!error)
        error = be_pcall(vm, 0);
    if (error)
        be_throw(vm, error);
    be_pushvalue(vm, 1);
    be_call(vm, 1);
    be_pop(vm, 1);
    be_return(vm);
}
} // namespace

namespace newmota {
int save_imported_persist(bvm* vm) {
    if (vm->module.loaded) {
        auto* value = be_map_findstr(vm, vm->module.loaded, be_newstr(vm, "persist"));
        if (value) {
            const bvalue instance = *value;
            be_pushnil(vm);
            *be_indexof(vm, -1) = instance;
            be_getmember(vm, -1, "save");
            be_pushvalue(vm, -2);
            be_call(vm, 1);
            be_pop(vm, 1);
        }
    }
    be_return_nil(vm);
}
} // namespace newmota

extern "C" {
be_native_module_attr_table(persist){
    be_native_module_function("init", Init),
    be_native_module_function("_read", Read),
    be_native_module_function("_write", Write),
};
extern const bntvmodule_t be_native_module_persist = {
    "persist", persist_attrs, sizeof(persist_attrs) / sizeof(persist_attrs[0]), nullptr};
}
