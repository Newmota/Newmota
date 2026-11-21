/*
  matter_dns_sd.cpp - minimal DNS-SD responder records for Matter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/matter/matter_dns_sd.h"

#include <cstdio>
#include <cstring>
#include <string_view>

namespace newmota::matter {
namespace {
constexpr std::uint16_t kTypeA6 = 28;
constexpr std::uint16_t kTypePtr = 12;
constexpr std::uint16_t kTypeTxt = 16;
constexpr std::uint16_t kTypeSrv = 33;
constexpr std::uint16_t kTypeAny = 255;
constexpr std::uint16_t kClassIn = 1;
constexpr std::uint16_t kCacheFlush = 0x8000;
constexpr std::uint32_t kTtlService = 4500;
constexpr std::uint32_t kTtlHost = 120;
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kNameCapacity = 256;
constexpr unsigned kMaxQuestions = 16;
constexpr unsigned kMaxPointers = 8;
constexpr std::string_view kEnumeration = "_services._dns-sd._udp.local";

/** @brief Returns the service type of an instance. */
[[nodiscard]] std::string_view type_of(const DnsService& service) {
    return service.commissionable ? "_matterc._udp.local" : "_matter._tcp.local";
}

/** @brief Compares DNS names, ignoring ASCII case. */
[[nodiscard]] bool same_name(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lower = [](const char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + 32) : value;
        };
        if (lower(left[index]) != lower(right[index]))
            return false;
    }
    return true;
}

/** @brief Appends records while they fit, counting those completed. */
class Writer {
  public:
    Writer(std::uint8_t* const output, const std::size_t capacity)
        : output_{output}, capacity_{capacity} {
        if (capacity_ >= kHeaderSize) {
            std::memset(output_, 0, kHeaderSize);
            output_[2] = 0x84; // Response, authoritative.
            size_ = kHeaderSize;
        }
    }

    /** @brief Writes a PTR record. */
    void ptr(const std::string_view name, const std::string_view target_prefix,
        const std::string_view target) {
        const std::size_t start = begin(name, kTypePtr, kClassIn, kTtlService);
        if (start == 0U)
            return;
        if (!target_prefix.empty())
            label_name(target_prefix, false);
        label_name(target, true);
        finish(start);
    }

    /** @brief Writes the SRV and TXT records of an instance. */
    void instance(
        const DnsService& service, const std::string_view host, const std::uint16_t port) {
        char name[kNameCapacity];
        const std::string_view type = type_of(service);
        const int length = std::snprintf(name, sizeof(name), "%s.%.*s", service.instance,
            static_cast<int>(type.size()), type.data());
        if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(name))
            return;
        const std::string_view full{name, static_cast<std::size_t>(length)};
        std::size_t start = begin(full, kTypeSrv, kClassIn | kCacheFlush, kTtlHost);
        if (start != 0U) {
            u16(0);
            u16(0);
            u16(port);
            host_name(host);
            finish(start);
        }
        start = begin(full, kTypeTxt, kClassIn | kCacheFlush, kTtlService);
        if (start != 0U) {
            text(service.txt);
            finish(start);
        }
    }

    /** @brief Writes the host's AAAA records. */
    void addresses(const DnsRecords& records) {
        for (std::size_t index = 0; index < records.address_count; ++index) {
            char name[40];
            const int length = std::snprintf(name, sizeof(name), "%s.local", records.host);
            if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(name))
                return;
            const std::size_t start = begin({name, static_cast<std::size_t>(length)}, kTypeA6,
                kClassIn | kCacheFlush, kTtlHost);
            if (start == 0U)
                return;
            bytes(records.addresses[index].data(), 16);
            finish(start);
        }
    }

    /** @brief Stores the answer count; 0 when nothing was written. */
    [[nodiscard]] std::size_t complete() {
        if (answers_ == 0U)
            return 0;
        output_[6] = static_cast<std::uint8_t>(answers_ >> 8U);
        output_[7] = static_cast<std::uint8_t>(answers_);
        return size_;
    }

  private:
    /** @brief Writes a dotted name, ending it when last is set. */
    void label_name(const std::string_view name, const bool last) {
        std::size_t start = 0;
        while (start < name.size()) {
            std::size_t end = name.find('.', start);
            if (end == std::string_view::npos)
                end = name.size();
            const std::size_t length = end - start;
            if (length > 63U || !room(length + 1U)) {
                failed_ = true;
                return;
            }
            output_[size_++] = static_cast<std::uint8_t>(length);
            bytes(reinterpret_cast<const std::uint8_t*>(name.data() + start), length);
            start = end + 1U;
        }
        if (last && room(1U))
            output_[size_++] = 0;
        else if (last)
            failed_ = true;
    }

    /** @brief Writes the target of an SRV record. */
    void host_name(const std::string_view host) {
        label_name(host, false);
        label_name("local", true);
    }

    /** @brief Starts a record and returns its rollback position, or 0 when full. */
    [[nodiscard]] std::size_t begin(const std::string_view name, const std::uint16_t type,
        const std::uint16_t record_class, const std::uint32_t ttl) {
        if (size_ == 0U)
            return 0;
        failed_ = false;
        const std::size_t start = size_;
        label_name(name, true);
        u16(type);
        u16(record_class);
        u16(static_cast<std::uint16_t>(ttl >> 16U));
        u16(static_cast<std::uint16_t>(ttl));
        length_at_ = size_;
        u16(0);
        return failed_ ? rollback(start) : start;
    }

    /** @brief Fills the data length, or drops a record that did not fit. */
    void finish(const std::size_t start) {
        if (failed_) {
            (void)rollback(start);
            return;
        }
        const std::size_t length = size_ - length_at_ - 2U;
        output_[length_at_] = static_cast<std::uint8_t>(length >> 8U);
        output_[length_at_ + 1U] = static_cast<std::uint8_t>(length);
        ++answers_;
    }

    [[nodiscard]] std::size_t rollback(const std::size_t start) {
        size_ = start;
        return 0;
    }

    [[nodiscard]] bool room(const std::size_t count) const { return capacity_ - size_ >= count; }

    void u16(const std::uint16_t value) {
        const std::uint8_t data[2] = {
            static_cast<std::uint8_t>(value >> 8U), static_cast<std::uint8_t>(value)};
        bytes(data, 2);
    }

    void bytes(const std::uint8_t* const data, const std::size_t count) {
        if (!room(count)) {
            failed_ = true;
            return;
        }
        std::memcpy(output_ + size_, data, count);
        size_ += count;
    }

    /** @brief Writes newline-separated items as DNS character strings. */
    void text(const std::string_view items) {
        std::size_t start = 0;
        bool any = false;
        while (start < items.size()) {
            std::size_t end = items.find('\n', start);
            if (end == std::string_view::npos)
                end = items.size();
            const std::size_t length = end - start;
            if (length <= 255U && room(length + 1U)) {
                output_[size_++] = static_cast<std::uint8_t>(length);
                bytes(reinterpret_cast<const std::uint8_t*>(items.data() + start), length);
                any = true;
            } else {
                failed_ = true;
            }
            start = end + 1U;
        }
        if (!any && room(1U))
            output_[size_++] = 0;
    }

    std::uint8_t* output_;
    std::size_t capacity_;
    std::size_t size_{};
    std::size_t length_at_{};
    unsigned answers_{};
    bool failed_{};
};

/**
 * @brief Reads a possibly compressed name into dotted form.
 * Pointers only jump backward and their number is limited, so a hostile packet cannot loop.
 */
[[nodiscard]] bool read_name(const std::uint8_t* const data, const std::size_t size,
    std::size_t& offset, char* const name, std::size_t& name_size) {
    std::size_t position = offset;
    std::size_t limit = size;
    bool jumped = false;
    unsigned pointers = 0;
    name_size = 0;
    while (true) {
        if (position >= limit)
            return false;
        const std::uint8_t length = data[position];
        if (length == 0U) {
            if (!jumped)
                offset = position + 1U;
            return true;
        }
        if ((length & 0xC0U) == 0xC0U) {
            if (position + 1U >= size || ++pointers > kMaxPointers)
                return false;
            const std::size_t target =
                (static_cast<std::size_t>(length & 0x3FU) << 8U) | data[position + 1U];
            if (target >= position)
                return false;
            if (!jumped)
                offset = position + 2U;
            jumped = true;
            limit = position;
            position = target;
            continue;
        }
        if ((length & 0xC0U) != 0U || length > limit - position - 1U ||
            name_size + length + 2U > kNameCapacity)
            return false;
        if (name_size != 0U)
            name[name_size++] = '.';
        std::memcpy(name + name_size, data + position + 1U, length);
        name_size += length;
        position += length + 1U;
    }
}

/** @brief Marks what one question asks for. */
struct Selection {
    bool enumeration{};
    bool addresses{};
    bool pointer[kDnsServices]{};
    bool details[kDnsServices]{};
    const char* subtype[kDnsServices]{};
};

/** @brief Tests whether name is exactly prefix.suffix. */
[[nodiscard]] bool has_form(
    const std::string_view name, const std::string_view prefix, const std::string_view suffix) {
    return name.size() == prefix.size() + 1U + suffix.size() && name[prefix.size()] == '.' &&
           same_name(name.substr(0, prefix.size()), prefix) &&
           same_name(name.substr(prefix.size() + 1U), suffix);
}

void select(const std::string_view name, const std::uint16_t type, const DnsRecords& records,
    Selection& selection) {
    const bool ptr = type == kTypePtr || type == kTypeAny;
    if (ptr && same_name(name, kEnumeration))
        selection.enumeration = true;
    if ((type == kTypeA6 || type == kTypeAny) && has_form(name, records.host, "local"))
        selection.addresses = true;
    for (std::size_t index = 0; index < records.service_count; ++index) {
        const DnsService& service = records.services[index];
        const std::string_view service_type = type_of(service);
        if (ptr && same_name(name, service_type))
            selection.pointer[index] = selection.details[index] = true;
        if ((type == kTypeSrv || type == kTypeTxt || type == kTypeAny) &&
            has_form(name, service.instance, service_type))
            selection.details[index] = true;
        if (!ptr)
            continue;
        char subtype_type[40];
        const int length = std::snprintf(subtype_type, sizeof(subtype_type), "_sub.%.*s",
            static_cast<int>(service_type.size()), service_type.data());
        if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(subtype_type))
            continue;
        for (const auto& subtype : service.subtypes) {
            if (subtype[0] != 0 &&
                has_form(name, subtype, {subtype_type, static_cast<std::size_t>(length)})) {
                selection.subtype[index] = subtype;
                selection.details[index] = true;
            }
        }
    }
}

/** @brief Writes what a selection asks for, addresses last. */
[[nodiscard]] std::size_t write_selection(const Selection& selection, const DnsRecords& records,
    std::uint8_t* const output, const std::size_t capacity) {
    Writer writer{output, capacity};
    bool any_details = false;
    for (std::size_t index = 0; index < records.service_count; ++index) {
        const DnsService& service = records.services[index];
        const std::string_view type = type_of(service);
        if (selection.enumeration)
            writer.ptr(kEnumeration, {}, type);
        if (selection.pointer[index])
            writer.ptr(type, service.instance, type);
        if (selection.subtype[index] != nullptr) {
            char name[64];
            const int length = std::snprintf(name, sizeof(name), "%s._sub.%.*s",
                selection.subtype[index], static_cast<int>(type.size()), type.data());
            if (length > 0 && static_cast<std::size_t>(length) < sizeof(name))
                writer.ptr({name, static_cast<std::size_t>(length)}, service.instance, type);
        }
        if (selection.details[index]) {
            writer.instance(service, records.host, records.port);
            any_details = true;
        }
    }
    if (selection.addresses || any_details)
        writer.addresses(records);
    return writer.complete();
}
} // namespace

std::size_t dns_sd_respond(const std::uint8_t* const query, const std::size_t query_size,
    const DnsRecords& records, std::uint8_t* const output, const std::size_t capacity) {
    if (query_size < kHeaderSize || (query[2] & 0x80U) != 0U || (query[2] & 0x78U) != 0U)
        return 0;
    const unsigned questions = (static_cast<unsigned>(query[4]) << 8U) | query[5];
    if (questions == 0U || questions > kMaxQuestions)
        return 0;
    Selection selection;
    std::size_t offset = kHeaderSize;
    for (unsigned index = 0; index < questions; ++index) {
        char name[kNameCapacity];
        std::size_t name_size = 0;
        if (!read_name(query, query_size, offset, name, name_size) || query_size - offset < 4U)
            return 0;
        const auto type =
            static_cast<std::uint16_t>((unsigned{query[offset]} << 8U) | query[offset + 1U]);
        const auto record_class = static_cast<std::uint16_t>(
            ((unsigned{query[offset + 2U]} << 8U) | query[offset + 3U]) & 0x7FFFU);
        offset += 4U;
        if (record_class == kClassIn || record_class == kTypeAny)
            select({name, name_size}, type, records, selection);
    }
    return write_selection(selection, records, output, capacity);
}

std::size_t dns_sd_announce(
    const DnsRecords& records, std::uint8_t* const output, const std::size_t capacity) {
    Selection selection;
    selection.addresses = true;
    for (std::size_t index = 0; index < records.service_count; ++index) {
        selection.pointer[index] = true;
        selection.details[index] = true;
    }
    return write_selection(selection, records, output, capacity);
}

} // namespace newmota::matter
