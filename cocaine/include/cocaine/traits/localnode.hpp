/*
 * 2015+ Copyright (c) Ivan Chelyubeev <ivan.chelubeev@gmail.com>
 * 2014 Copyright (c) Asier Gutierrez <asierguti@gmail.com>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#ifndef LOCALNODE_SERIALIZATION_TRAITS_HPP
#define LOCALNODE_SERIALIZATION_TRAITS_HPP

#include <msgpack.hpp>
#include <cocaine/traits.hpp>
#include <elliptics/packet.h>
#include <elliptics/utils.hpp>

namespace cocaine { namespace io {

template<>
struct type_traits<ioremap::elliptics::data_pointer> {

    template<class Stream>
    static inline
    void
    pack(msgpack::packer<Stream>& target, const ioremap::elliptics::data_pointer& source) {
        target.pack_raw(source.size());
        target.pack_raw_body(static_cast<const char *>(source.data()), source.size());
    }

    static inline
    void
    unpack(const msgpack::object& source, ioremap::elliptics::data_pointer& target) {
        if (source.type != msgpack::type::RAW) {
            throw msgpack::type_error();
        }
        //XXX: Is it possible to avoid this copy?
        target = ioremap::elliptics::data_pointer::copy(
            source.via.raw.ptr,
            source.via.raw.size
        );
    }
};

}}

namespace msgpack {

//
// Msgpack serialization for structures used by service.
//
// Note: not using cocaine::io::type_traits<> here because input/output
// operators are more universal.
//

// struct dnet_raw_id {
//  uint8_t         id[DNET_ID_SIZE];
// } __attribute__ ((packed));
//
// Defined in elliptics/packet.h.
//
template <typename Stream>
inline msgpack::packer<Stream>& operator <<(msgpack::packer<Stream> &o, const dnet_raw_id &v)
{
    o.pack_array(1);
    o.pack_raw(sizeof(v.id));
    o.pack_raw_body(reinterpret_cast<const char *>(v.id), sizeof(v.id));
    return o;
}
inline dnet_raw_id& operator >>(const msgpack::object &o, dnet_raw_id &v)
{
    if (o.type != msgpack::type::ARRAY || o.via.array.size != 1) {
        throw msgpack::type_error();
    }
    {
        const auto &f = o.via.array.ptr[0];
        if (f.type != msgpack::type::RAW || f.via.raw.size != sizeof(v.id)) {
            throw msgpack::type_error();
        }
        memcpy(&v.id, f.via.raw.ptr, sizeof(v.id));
    }
    return v;
}

// struct dnet_addr
// {
//  uint8_t         addr[DNET_ADDR_SIZE];
//  uint16_t        addr_len;
//  uint16_t        family;
// } __attribute__ ((packed));
//
// Defined in elliptics/packet.h.
//
template <typename Stream>
inline msgpack::packer<Stream>& operator <<(msgpack::packer<Stream> &o, const dnet_addr &v)
{
    o.pack_array(3);
    o.pack_raw(sizeof(v.addr));
    o.pack_raw_body(reinterpret_cast<const char *>(v.addr), sizeof(v.addr));
    o.pack_uint16(v.addr_len);
    o.pack_uint16(v.family);
    return o;
}
inline dnet_addr& operator >>(const msgpack::object &o, dnet_addr &v)
{
    if (o.type != msgpack::type::ARRAY || o.via.array.size != 3) {
        throw msgpack::type_error();
    }
    {
        const auto &f = o.via.array.ptr[0];
        if (f.type != msgpack::type::RAW || f.via.raw.size != sizeof(v.addr)) {
            throw msgpack::type_error();
        }
        memcpy(&v.addr, f.via.raw.ptr, sizeof(v.addr));
    }
    v.addr_len = o.via.array.ptr[1].as<uint16_t>();
    v.family = o.via.array.ptr[2].as<uint16_t>();
    return v;
}

// struct dnet_time {
//  uint64_t        tsec, tnsec;
// };
//
// Defined in elliptics/packet.h.
//
template <typename Stream>
inline msgpack::packer<Stream>& operator <<(msgpack::packer<Stream> &o, const dnet_time &v)
{
    o.pack_array(2);
    o.pack_uint64(v.tsec);
    o.pack_uint64(v.tnsec);
    return o;
}
inline dnet_time& operator >>(const msgpack::object &o, dnet_time &v)
{
    if (o.type != msgpack::type::ARRAY || o.via.array.size != 2) {
        throw msgpack::type_error();
    }
    v.tsec = o.via.array.ptr[0].as<uint64_t>();
    v.tnsec = o.via.array.ptr[1].as<uint64_t>();
    return v;
}

// struct dnet_file_info {
//  int             flen;       /* filename length, which goes after this structure */
//  unsigned char   checksum[DNET_CSUM_SIZE];
//
//  uint64_t        record_flags;   /* combination of DNET_RECORD_FLAGS_* */
//  uint64_t        size;       /* size of file on disk */
//  uint64_t        offset;     /* offset of file on disk */
//
//  struct dnet_time    mtime;
// };
//
// Defined in elliptics/packet.h.
//
template <typename Stream>
inline msgpack::packer<Stream>& operator <<(msgpack::packer<Stream> &o, const dnet_file_info &v)
{
    o.pack_array(6);
    // There is no actual need in keeping dnet_file_info::flen --
    // -- its used to indicate length of file path tailing dnet_file_info
    // objects in replies from elliptics node, but we handle that file path
    // separately, so we could have dropped flen entirely.
    // But still its better to keep it and thus support symmetricity of
    // serialize/deserialize operations.
    o.pack_int(v.flen);
    o.pack_raw(sizeof(v.checksum));
    o.pack_raw_body(reinterpret_cast<const char *>(v.checksum), sizeof(v.checksum));
    o.pack_uint64(v.record_flags);
    o.pack_uint64(v.size);
    o.pack_uint64(v.offset);
    o.pack(v.mtime);
    return o;
}
inline dnet_file_info& operator >>(const msgpack::object &o, dnet_file_info &v)
{
    if (o.type != msgpack::type::ARRAY || o.via.array.size != 6) {
        throw msgpack::type_error();
    }
    int N = 0;
    o.via.array.ptr[N++] >> v.flen;
    {
        const auto &f = o.via.array.ptr[N++];
        if (f.type != msgpack::type::RAW || f.via.raw.size != sizeof(v.checksum)) {
            throw msgpack::type_error();
        }
        memcpy(&v.checksum, f.via.raw.ptr, sizeof(v.checksum));
    }
    o.via.array.ptr[N++] >> v.record_flags;
    o.via.array.ptr[N++] >> v.size;
    o.via.array.ptr[N++] >> v.offset;
    o.via.array.ptr[N++] >> v.mtime;
    return v;
}

// dnet_async_service_result
//
// Defined in cocaine/idl/localnode.hpp.
//
using ioremap::elliptics::dnet_async_service_result;

template <typename Stream>
inline msgpack::packer<Stream>& operator <<(msgpack::packer<Stream> &o, const dnet_async_service_result &v)
{
    o.pack_array(3);
    o.pack(v.addr);
    o.pack(v.file_info);
    o.pack(v.file_path);
    return o;
}
inline dnet_async_service_result& operator >>(const msgpack::object &o, dnet_async_service_result &v)
{
    if (o.type != msgpack::type::ARRAY || o.via.array.size != 3) {
        throw msgpack::type_error();
    }
    o.via.array.ptr[0] >> v.addr;
    o.via.array.ptr[1] >> v.file_info;
    o.via.array.ptr[2] >> v.file_path;
    return v;
}

} // namespace msgpack

#endif // LOCALNODE_SERIALIZATION_TRAITS_HPP
