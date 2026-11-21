/*
  path_module.cpp - Bounded Berry filesystem path helpers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#include "core/storage/filesystem.h"
extern "C" {
#include "berry.h"
#include "be_exec.h"
}

namespace {
namespace fs = newmota::filesystem;
constexpr std::size_t kEntryLimit = 128;
constexpr std::size_t kNameBytesLimit = 8192;

/** @brief Reads a bounded filesystem path without losing embedded NUL bytes. */
bool path_at(bvm* vm, int index, std::string_view& path) {
    if (!be_isstring(vm, index))
        return false;
    path = {be_tostring(vm, index), static_cast<std::size_t>(be_strlen(vm, index))};
    return !path.empty() && path.size() <= 191 && path.find('\0') == std::string_view::npos;
}

/** @brief Returns whether a path identifies an existing entry. */
int Exists(bvm* vm) {
    std::string_view path;
    fs::EntryType type{};
    std::uint32_t size{};
    be_pushbool(
        vm, path_at(vm, 1, path) && fs::stat(path, type, size) == fs::ReadResult::succeeded);
    be_return(vm);
}

/** @brief Returns a directory flag, or nil when no entry can be inspected. */
int Isdir(bvm* vm) {
    std::string_view path;
    fs::EntryType type{};
    std::uint32_t size{};
    if (path_at(vm, 1, path) && fs::stat(path, type, size) == fs::ReadResult::succeeded)
        be_pushbool(vm, type == fs::EntryType::directory);
    else
        be_pushnil(vm);
    be_return(vm);
}

struct Listing final {
    bvm* vm;
    int list;
    std::string_view name;
    std::size_t count{};
    std::size_t bytes{};
    int error{};
    bool overflow{};
};

/** @brief Allocates and appends one name inside a Berry exception boundary. */
void append_name(bvm* vm, void* opaque) {
    const auto* listing = static_cast<Listing*>(opaque);
    be_pushnstring(vm, listing->name.data(), listing->name.size());
    be_data_push(vm, listing->list);
    be_pop(vm, 1);
}

/** @brief Stops traversal before overflow or after a protected Berry failure. */
bool visit(void* opaque, const fs::DirectoryEntry& entry) {
    auto* listing = static_cast<Listing*>(opaque);
    if (listing->count == kEntryLimit || entry.name.size() > kNameBytesLimit - listing->bytes) {
        listing->overflow = true;
        return false;
    }
    listing->name = entry.name;
    listing->error = be_execprotected(listing->vm, append_name, listing);
    if (listing->error)
        return false;
    ++listing->count;
    listing->bytes += entry.name.size();
    return true;
}

/** @brief Lists child basenames, closing the filesystem iterator before throwing. */
int Listdir(bvm* vm) {
    std::string_view path;
    const bool valid = path_at(vm, 1, path);
    be_newobject(vm, "list");
    Listing listing{vm, be_absindex(vm, -1), {}};
    if (valid) {
        const auto result = fs::list(path, visit, &listing);
        if (listing.error)
            be_throw(vm, listing.error);
        if (listing.overflow)
            be_raise(vm, "value_error", "path.listdir capacity exceeded");
        if (result != fs::DirectoryResult::listed) {
            be_pop(vm, 2);
            be_newobject(vm, "list");
        }
    }
    be_pop(vm, 1); // Return the public list instance rather than its backing list.
    be_return(vm);
}

/** @brief Creates a new directory and reports existing entries as failure. */
int Mkdir(bvm* vm) {
    std::string_view path;
    fs::EntryType type{};
    std::uint32_t size{};
    be_pushbool(vm, path_at(vm, 1, path) &&
                        fs::stat(path, type, size) == fs::ReadResult::not_found &&
                        fs::ensure_directory(path) == fs::MutationResult::succeeded);
    be_return(vm);
}

/** @brief Removes only an empty directory. */
int Rmdir(bvm* vm) {
    std::string_view path;
    fs::EntryType type{};
    std::uint32_t size{};
    be_pushbool(vm,
        path_at(vm, 1, path) && fs::stat(path, type, size) == fs::ReadResult::succeeded &&
            type == fs::EntryType::directory && fs::remove(path) == fs::MutationResult::succeeded);
    be_return(vm);
}

/** @brief Removes a file or empty directory using the shared owner. */
int Remove(bvm* vm) {
    std::string_view path;
    be_pushbool(vm, path_at(vm, 1, path) && fs::remove(path) == fs::MutationResult::succeeded);
    be_return(vm);
}

/** @brief Renames an entry without allowing truncated source or destination paths. */
int Rename(bvm* vm) {
    std::string_view source, destination;
    be_pushbool(vm, path_at(vm, 1, source) && path_at(vm, 2, destination) &&
                        fs::rename(source, destination) == fs::MutationResult::succeeded);
    be_return(vm);
}
} // namespace

extern "C" {
be_native_module_attr_table(path){
    be_native_module_function("exists", Exists),
    be_native_module_function("isdir", Isdir),
    be_native_module_function("listdir", Listdir),
    be_native_module_function("mkdir", Mkdir),
    be_native_module_function("rmdir", Rmdir),
    be_native_module_function("remove", Remove),
    be_native_module_function("rename", Rename),
};
extern const bntvmodule_t be_native_module_path = {
    "path", path_attrs, sizeof(path_attrs) / sizeof(path_attrs[0]), nullptr};
}
