/*
  matter_cert.cpp - Matter certificate to X.509 signed data conversion

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/matter/matter_cert.h"

#include "services/matter/matter_wire.h"
#include "support/number.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace newmota::matter {
namespace {
constexpr std::size_t kMaxNames = 8;
constexpr std::size_t kMaxExtensions = 8;
constexpr std::uint32_t kTagSerial = 1;
constexpr std::uint32_t kTagSignatureAlgorithm = 2;
constexpr std::uint32_t kTagIssuer = 3;
constexpr std::uint32_t kTagNotBefore = 4;
constexpr std::uint32_t kTagNotAfter = 5;
constexpr std::uint32_t kTagSubject = 6;
constexpr std::uint32_t kTagKeyAlgorithm = 7;
constexpr std::uint32_t kTagCurve = 8;
constexpr std::uint32_t kTagPublicKey = 9;
constexpr std::uint32_t kTagExtensions = 10;
constexpr std::uint32_t kDnPrintableBit = 0x80;
constexpr std::uint32_t kExtensionBasic = 1;
constexpr std::uint32_t kExtensionKeyUsage = 2;
constexpr std::uint32_t kExtensionExtendedKeyUsage = 3;
constexpr std::uint32_t kExtensionSubjectKey = 4;
constexpr std::uint32_t kExtensionAuthorityKey = 5;
constexpr std::uint32_t kExtensionFuture = 6; // Holds a complete DER extension.
constexpr std::uint32_t kDnNodeId = 17;
constexpr std::uint32_t kDnCat = 22;
constexpr std::uint32_t kDnVvsId = 23;
constexpr std::uint8_t kDerBoolean = 0x01;
constexpr std::uint8_t kDerInteger = 0x02;
constexpr std::uint8_t kDerBitString = 0x03;
constexpr std::uint8_t kDerOctetString = 0x04;
constexpr std::uint8_t kDerOid = 0x06;
constexpr std::uint8_t kDerUtf8 = 0x0C;
constexpr std::uint8_t kDerPrintable = 0x13;
constexpr std::uint8_t kDerUtcTime = 0x17;
constexpr std::uint8_t kDerGeneralizedTime = 0x18;
constexpr std::uint8_t kDerSequence = 0x30;
constexpr std::uint8_t kDerSet = 0x31;
constexpr std::uint8_t kDerContext0 = 0xA0;
constexpr std::uint8_t kDerContextPrimitive0 = 0x80;
constexpr std::uint8_t kDerContext3 = 0xA3;
constexpr std::uint8_t kOidEcdsaSha256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};
constexpr std::uint8_t kOidEcPublicKey[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
constexpr std::uint8_t kOidPrime256v1[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
constexpr std::uint8_t kOidMatterBase[] = {0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0xA2, 0x7C, 0x01};

/** @brief Writes DER from the end of a buffer, so lengths are known when they are written. */
class Der {
  public:
    Der(std::uint8_t* const output, const std::size_t capacity)
        : start_{output}, position_{output + capacity} {}

    [[nodiscard]] bool ok() const { return ok_; }

    [[nodiscard]] const std::uint8_t* data() const { return position_; }

    [[nodiscard]] std::size_t mark() const { return static_cast<std::size_t>(position_ - start_); }

    void bytes(const std::uint8_t* const data, const std::size_t size) {
        if (!ok_ || static_cast<std::size_t>(position_ - start_) < size) {
            ok_ = false;
            return;
        }
        position_ -= size;
        if (size != 0U)
            std::memcpy(position_, data, size);
    }

    void byte(const std::uint8_t value) { bytes(&value, 1); }

    /** @brief Closes an element whose content was written since mark. */
    void wrap(const std::uint8_t tag, const std::size_t mark) {
        const std::size_t length = mark - this->mark();
        if (length < 0x80U) {
            byte(static_cast<std::uint8_t>(length));
        } else if (length < 0x100U) {
            byte(static_cast<std::uint8_t>(length));
            byte(0x81);
        } else {
            byte(static_cast<std::uint8_t>(length));
            byte(static_cast<std::uint8_t>(length >> 8U));
            byte(0x82);
        }
        byte(tag);
    }

    void element(const std::uint8_t tag, const std::uint8_t* const data, const std::size_t size) {
        const std::size_t end = mark();
        bytes(data, size);
        wrap(tag, end);
    }

  private:
    std::uint8_t* start_;
    std::uint8_t* position_;
    bool ok_{true};
};

/** @brief Fields of a certificate, borrowed from its TLV. */
struct Fields {
    const std::uint8_t* serial{};
    std::size_t serial_size{};
    std::size_t issuer{};
    std::size_t subject{};
    std::size_t extensions{};
    std::uint32_t not_before{};
    std::uint32_t not_after{};
    const std::uint8_t* key{};
    std::size_t key_size{};
    bool has_issuer{};
    bool has_subject{};
    bool has_extensions{};
    unsigned found{};
};

[[nodiscard]] bool parse_fields(
    const std::uint8_t* const data, const std::size_t size, Fields& out) {
    TlvElement top;
    if (!tlv_next(data, size, 0, top) || top.type != TlvType::structure)
        return false;
    std::size_t offset = top.next;
    while (true) {
        TlvElement element;
        if (!tlv_next(data, size, offset, element))
            return false;
        if (element.type == TlvType::end)
            return out.key != nullptr && out.serial != nullptr && out.has_issuer &&
                   out.has_subject && out.has_extensions;
        const auto is_number = element.type == TlvType::unsigned_integer;
        if (element.tag == kTagSerial && element.type == TlvType::octets) {
            out.serial = element.data;
            out.serial_size = element.length;
        } else if (element.tag == kTagSignatureAlgorithm || element.tag == kTagKeyAlgorithm ||
                   element.tag == kTagCurve) {
            if (!is_number || element.value != 1U)
                return false;
        } else if (element.tag == kTagIssuer && element.type == TlvType::list) {
            out.issuer = element.next;
            out.has_issuer = true;
        } else if (element.tag == kTagSubject && element.type == TlvType::list) {
            out.subject = element.next;
            out.has_subject = true;
        } else if (element.tag == kTagNotBefore && is_number) {
            out.not_before = static_cast<std::uint32_t>(element.value);
        } else if (element.tag == kTagNotAfter && is_number) {
            out.not_after = static_cast<std::uint32_t>(element.value);
        } else if (element.tag == kTagPublicKey && element.type == TlvType::octets) {
            out.key = element.data;
            out.key_size = element.length;
        } else if (element.tag == kTagExtensions && element.type == TlvType::list) {
            out.extensions = element.next;
            out.has_extensions = true;
        }
        if (!tlv_skip(data, size, offset, element))
            return false;
    }
}

/** @brief Maps a DN tag to its OID. @return The size, or 0 for an unknown attribute. */
[[nodiscard]] std::size_t dn_oid(const std::uint32_t tag, std::uint8_t (&oid)[12]) {
    if (tag >= kDnNodeId && tag <= kDnVvsId) {
        std::memcpy(oid, kOidMatterBase, sizeof(kOidMatterBase));
        oid[sizeof(kOidMatterBase)] = static_cast<std::uint8_t>(tag - kDnNodeId + 1U);
        return sizeof(kOidMatterBase) + 1U;
    }
    // X.520 attributes under 2.5.4, in the order of the Matter DN tags.
    static constexpr std::uint8_t kArcs[] = {3, 4, 5, 6, 7, 8, 10, 11, 12, 41, 42, 43, 44, 46, 65};
    if (tag == 0U || tag > sizeof(kArcs))
        return 0;
    oid[0] = 0x55;
    oid[1] = 0x04;
    oid[2] = kArcs[tag - 1U];
    return 3;
}

/** @brief Writes a whole number as DER text in a fixed-width upper-case hexadecimal form. */
[[nodiscard]] std::size_t hex_text(char* const text, const std::uint64_t value, const int digits) {
    // Not %0*llX: newlib-nano (Beken) has no long long conversions.
    // A negative width pads nothing, as snprintf's left-justified field would.
    const auto formatted =
        newmota::support::digits(value, 16, digits > 0 ? static_cast<unsigned>(digits) : 0U);
    const std::size_t length = std::min<std::size_t>(formatted.size, 19U);
    std::memcpy(text, formatted.text, length);
    text[length] = '\0';
    return length;
}

/** @brief Writes a Name from the DN list starting at offset, in the original order. */
[[nodiscard]] bool write_name(
    Der& der, const std::uint8_t* const data, const std::size_t size, std::size_t offset) {
    TlvElement names[kMaxNames];
    std::size_t count = 0;
    while (true) {
        TlvElement element;
        if (!tlv_next(data, size, offset, element))
            return false;
        offset = element.next;
        if (element.type == TlvType::end)
            break;
        if (count == kMaxNames)
            return false;
        names[count++] = element;
    }
    const std::size_t name_end = der.mark();
    for (std::size_t index = count; index > 0; --index) {
        const TlvElement& name = names[index - 1U];
        const std::uint32_t tag = name.tag & ~kDnPrintableBit;
        std::uint8_t oid[12];
        const std::size_t oid_size = dn_oid(tag, oid);
        if (oid_size == 0U)
            return false;
        const std::size_t set_end = der.mark();
        const std::size_t sequence_end = der.mark();
        char text[20];
        if (name.type == TlvType::unsigned_integer) {
            // Matter identifiers are numbers in TLV and fixed-width hexadecimal text in X.509.
            const int digits = tag == kDnCat ? 8 : 16;
            if (tag < kDnNodeId || tag > kDnVvsId)
                return false;
            der.element(kDerUtf8, reinterpret_cast<const std::uint8_t*>(text),
                hex_text(text, name.value, digits));
        } else if (name.type == TlvType::text) {
            der.element((name.tag & kDnPrintableBit) != 0U ? kDerPrintable : kDerUtf8, name.data,
                name.length);
        } else {
            return false;
        }
        der.element(kDerOid, oid, oid_size);
        der.wrap(kDerSequence, sequence_end);
        der.wrap(kDerSet, set_end);
    }
    der.wrap(kDerSequence, name_end);
    return der.ok();
}

/** @brief Writes a Matter epoch second as UTCTime before 2050, else GeneralizedTime. */
void write_time(Der& der, const std::uint32_t epoch) {
    char text[20];
    constexpr std::int64_t kDaysFrom1970To2000 = 10957;
    const std::int64_t days =
        static_cast<std::int64_t>(epoch / 86400U) + kDaysFrom1970To2000 + 719468;
    const std::int64_t era = days / 146097;
    const std::int64_t day_of_era = days - era * 146097;
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t shifted_month = (5 * day_of_year + 2) / 153;
    const int day = static_cast<int>(day_of_year - (153 * shifted_month + 2) / 5 + 1);
    const int month = static_cast<int>(shifted_month < 10 ? shifted_month + 3 : shifted_month - 9);
    const int year = static_cast<int>(year_of_era + era * 400 + (month <= 2 ? 1 : 0));
    const std::uint32_t seconds = epoch % 86400U;
    const unsigned hour = seconds / 3600U;
    const unsigned minute = seconds / 60U % 60U;
    const unsigned second = seconds % 60U;
    if (year < 2050) {
        const int length = std::snprintf(text, sizeof(text), "%02d%02d%02d%02u%02u%02uZ",
            year % 100, month, day, hour, minute, second);
        der.element(kDerUtcTime, reinterpret_cast<const std::uint8_t*>(text),
            static_cast<std::size_t>(length));
    } else {
        const int length = std::snprintf(text, sizeof(text), "%04d%02d%02d%02u%02u%02uZ", year,
            month, day, hour, minute, second);
        der.element(kDerGeneralizedTime, reinterpret_cast<const std::uint8_t*>(text),
            static_cast<std::size_t>(length));
    }
}

/** @brief Writes the value of one extension inside its OCTET STRING. */
[[nodiscard]] bool write_extension(
    Der& der, const std::uint8_t* const data, const std::size_t size, const TlvElement& element) {
    if (element.tag == kExtensionFuture) {
        if (element.type != TlvType::octets)
            return false;
        der.bytes(element.data, element.length);
        return der.ok();
    }
    const std::size_t extension_end = der.mark();
    bool critical = false;
    const std::uint8_t* oid = nullptr;
    static constexpr std::uint8_t kOidBasic[] = {0x55, 0x1D, 0x13};
    static constexpr std::uint8_t kOidKeyUsage[] = {0x55, 0x1D, 0x0F};
    static constexpr std::uint8_t kOidExtended[] = {0x55, 0x1D, 0x25};
    static constexpr std::uint8_t kOidSubjectKey[] = {0x55, 0x1D, 0x0E};
    static constexpr std::uint8_t kOidAuthorityKey[] = {0x55, 0x1D, 0x23};
    const std::size_t value_end = der.mark();
    switch (element.tag) {
    case kExtensionBasic: {
        if (element.type != TlvType::structure)
            return false;
        bool is_ca = false;
        bool has_path = false;
        std::uint64_t path = 0;
        std::size_t offset = element.next;
        while (true) {
            TlvElement inner;
            if (!tlv_next(data, size, offset, inner))
                return false;
            offset = inner.next;
            if (inner.type == TlvType::end)
                break;
            if (inner.tag == 1U && inner.type == TlvType::boolean)
                is_ca = inner.value != 0U;
            else if (inner.tag == 2U && inner.type == TlvType::unsigned_integer &&
                     inner.value < 0x80U)
                has_path = true, path = inner.value;
            else
                return false;
        }
        const std::size_t sequence_end = der.mark();
        if (has_path) {
            const auto value = static_cast<std::uint8_t>(path);
            der.element(kDerInteger, &value, 1);
        }
        if (is_ca) {
            const std::uint8_t value = 0xFF;
            der.element(kDerBoolean, &value, 1);
        }
        der.wrap(kDerSequence, sequence_end);
        critical = true;
        oid = kOidBasic;
        break;
    }
    case kExtensionKeyUsage: {
        if (element.type != TlvType::unsigned_integer || element.value == 0U ||
            element.value > 0x1FFU)
            return false;
        // Bit i of the value is bit i of the X.509 string, whose first bit is the top one.
        std::uint8_t first = 0;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            if ((element.value & (1U << bit)) != 0U)
                first = static_cast<std::uint8_t>(first | (0x80U >> bit));
        }
        std::uint8_t content[3] = {0, first, 0x80};
        std::size_t length = 2;
        if ((element.value & 0x100U) != 0U) {
            content[0] = 7;
            length = 3;
        } else {
            std::uint8_t unused = 0;
            while (((first >> unused) & 1U) == 0U)
                ++unused;
            content[0] = unused;
        }
        der.element(kDerBitString, content, length);
        critical = true;
        oid = kOidKeyUsage;
        break;
    }
    case kExtensionExtendedKeyUsage: {
        if (element.type != TlvType::array)
            return false;
        std::uint8_t purposes[6];
        std::size_t count = 0;
        std::size_t offset = element.next;
        while (true) {
            TlvElement inner;
            if (!tlv_next(data, size, offset, inner))
                return false;
            offset = inner.next;
            if (inner.type == TlvType::end)
                break;
            if (inner.type != TlvType::unsigned_integer || inner.value == 0U || inner.value > 6U ||
                count == sizeof(purposes))
                return false;
            purposes[count++] = static_cast<std::uint8_t>(inner.value);
        }
        static constexpr std::uint8_t kLast[] = {1, 2, 3, 4, 8, 9};
        const std::size_t sequence_end = der.mark();
        for (std::size_t index = count; index > 0; --index) {
            const std::uint8_t purpose[] = {
                0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, kLast[purposes[index - 1U] - 1U]};
            der.element(kDerOid, purpose, sizeof(purpose));
        }
        der.wrap(kDerSequence, sequence_end);
        critical = true;
        oid = kOidExtended;
        break;
    }
    case kExtensionSubjectKey:
        if (element.type != TlvType::octets)
            return false;
        der.element(kDerOctetString, element.data, element.length);
        oid = kOidSubjectKey;
        break;
    case kExtensionAuthorityKey: {
        if (element.type != TlvType::octets)
            return false;
        const std::size_t sequence_end = der.mark();
        der.element(kDerContextPrimitive0, element.data, element.length);
        der.wrap(kDerSequence, sequence_end);
        oid = kOidAuthorityKey;
        break;
    }
    default:
        return false;
    }
    der.wrap(kDerOctetString, value_end);
    if (critical) {
        const std::uint8_t value = 0xFF;
        der.element(kDerBoolean, &value, 1);
    }
    der.element(kDerOid, oid, 3);
    der.wrap(kDerSequence, extension_end);
    return der.ok();
}

[[nodiscard]] bool write_extensions(
    Der& der, const std::uint8_t* const data, const std::size_t size, std::size_t offset) {
    TlvElement extensions[kMaxExtensions];
    std::size_t count = 0;
    while (true) {
        TlvElement element;
        if (!tlv_next(data, size, offset, element))
            return false;
        if (element.type == TlvType::end)
            break;
        if (count == kMaxExtensions)
            return false;
        extensions[count++] = element;
        if (!tlv_skip(data, size, offset, element))
            return false;
    }
    const std::size_t outer_end = der.mark();
    const std::size_t list_end = der.mark();
    for (std::size_t index = count; index > 0; --index) {
        if (!write_extension(der, data, size, extensions[index - 1U]))
            return false;
    }
    der.wrap(kDerSequence, list_end);
    der.wrap(kDerContext3, outer_end);
    return der.ok();
}
} // namespace

std::size_t certificate_signed_data(const std::uint8_t* const certificate, const std::size_t size,
    std::uint8_t* const output, const std::size_t capacity, const std::uint8_t** const der_out) {
    Fields fields;
    if (!parse_fields(certificate, size, fields))
        return 0;
    if (fields.key_size != 65U || fields.serial_size == 0U || fields.serial_size > 20U)
        return 0;
    Der der{output, capacity};
    const std::size_t tbs_end = der.mark();
    if (!write_extensions(der, certificate, size, fields.extensions))
        return 0;
    // SubjectPublicKeyInfo.
    const std::size_t key_info_end = der.mark();
    const std::size_t bits_end = der.mark();
    der.bytes(fields.key, fields.key_size);
    der.byte(0);
    der.wrap(kDerBitString, bits_end);
    const std::size_t algorithm_end = der.mark();
    der.element(kDerOid, kOidPrime256v1, sizeof(kOidPrime256v1));
    der.element(kDerOid, kOidEcPublicKey, sizeof(kOidEcPublicKey));
    der.wrap(kDerSequence, algorithm_end);
    der.wrap(kDerSequence, key_info_end);
    if (!write_name(der, certificate, size, fields.subject))
        return 0;
    const std::size_t validity_end = der.mark();
    if (fields.not_after == 0U) { // No well-defined expiration.
        static constexpr char kForever[] = "99991231235959Z";
        der.element(kDerGeneralizedTime, reinterpret_cast<const std::uint8_t*>(kForever),
            sizeof(kForever) - 1U);
    } else {
        write_time(der, fields.not_after);
    }
    write_time(der, fields.not_before);
    der.wrap(kDerSequence, validity_end);
    if (!write_name(der, certificate, size, fields.issuer))
        return 0;
    const std::size_t signature_end = der.mark();
    der.element(kDerOid, kOidEcdsaSha256, sizeof(kOidEcdsaSha256));
    der.wrap(kDerSequence, signature_end);
    der.element(kDerInteger, fields.serial, fields.serial_size);
    const std::size_t version_end = der.mark();
    const std::uint8_t version = 2;
    der.element(kDerInteger, &version, 1);
    der.wrap(kDerContext0, version_end);
    der.wrap(kDerSequence, tbs_end);
    if (!der.ok())
        return 0;
    *der_out = der.data();
    return tbs_end - der.mark();
}

std::size_t certificate_cats(const std::uint8_t* const certificate, const std::size_t size,
    std::uint32_t* const cats, const std::size_t capacity) {
    Fields fields;
    if (!parse_fields(certificate, size, fields))
        return 0;
    std::size_t count = 0;
    std::size_t offset = fields.subject;
    while (count < capacity) {
        TlvElement element;
        if (!tlv_next(certificate, size, offset, element) || element.type == TlvType::end)
            break;
        offset = element.next;
        if (element.tag == kDnCat && element.type == TlvType::unsigned_integer)
            cats[count++] = static_cast<std::uint32_t>(element.value);
    }
    return count;
}

} // namespace newmota::matter
