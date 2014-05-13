#ifndef TMWA_NET_VOMIT_HPP
#define TMWA_NET_VOMIT_HPP
//    vomit.hpp - sickening socket accessors
//
//    Copyright © ????-2004 Athena Dev Teams
//    Copyright © 2004-2011 The Mana World Development Team
//    Copyright © 2011-2014 Ben Longbons <b.r.longbons@gmail.com>
//
//    This file is part of The Mana World (Athena server)
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.

# include "fwd.hpp"

# include "socket.hpp"

template<class T>
uint8_t *pod_addressof_m(T& structure)
{
    static_assert(is_trivially_copyable<T>::value, "Can only byte-copy POD-ish structs");
    return &reinterpret_cast<uint8_t&>(structure);
}

template<class T>
const uint8_t *pod_addressof_c(const T& structure)
{
    static_assert(is_trivially_copyable<T>::value, "Can only byte-copy POD-ish structs");
    return &reinterpret_cast<const uint8_t&>(structure);
}

/// Read from the queue
inline
const void *RFIFOP(Session *s, size_t pos)
{
    return &s->rdata[s->rdata_pos + pos];
}
inline
uint8_t RFIFOB(Session *s, size_t pos)
{
    return *static_cast<const uint8_t *>(RFIFOP(s, pos));
}
inline
uint16_t RFIFOW(Session *s, size_t pos)
{
    return *static_cast<const uint16_t *>(RFIFOP(s, pos));
}
inline
uint32_t RFIFOL(Session *s, size_t pos)
{
    return *static_cast<const uint32_t *>(RFIFOP(s, pos));
}
template<class T>
void RFIFO_STRUCT(Session *s, size_t pos, T& structure)
{
    really_memcpy(pod_addressof_m(structure), static_cast<const uint8_t *>(RFIFOP(s, pos)), sizeof(T));
}
inline
IP4Address RFIFOIP(Session *s, size_t pos)
{
    IP4Address o;
    RFIFO_STRUCT(s, pos, o);
    return o;
}
template<uint8_t len>
inline
VString<len-1> RFIFO_STRING(Session *s, size_t pos)
{
    const char *const begin = static_cast<const char *>(RFIFOP(s, pos));
    const char *const end = begin + len-1;
    const char *const mid = std::find(begin, end, '\0');
    return XString(begin, mid, nullptr);
}
inline
AString RFIFO_STRING(Session *s, size_t pos, size_t len)
{
    const char *const begin = static_cast<const char *>(RFIFOP(s, pos));
    const char *const end = begin + len;
    const char *const mid = std::find(begin, end, '\0');
    return XString(begin, mid, nullptr);
}
inline
void RFIFO_BUF_CLONE(Session *s, uint8_t *buf, size_t len)
{
    really_memcpy(buf, static_cast<const uint8_t *>(RFIFOP(s, 0)), len);
}

/// Read from an arbitrary buffer
inline
const void *RBUFP(const uint8_t *p, size_t pos)
{
    return p + pos;
}
inline
uint8_t RBUFB(const uint8_t *p, size_t pos)
{
    return *static_cast<const uint8_t *>(RBUFP(p, pos));
}
inline
uint16_t RBUFW(const uint8_t *p, size_t pos)
{
    return *static_cast<const uint16_t *>(RBUFP(p, pos));
}
inline
uint32_t RBUFL(const uint8_t *p, size_t pos)
{
    return *static_cast<const uint32_t *>(RBUFP(p, pos));
}
template<class T>
void RBUF_STRUCT(const uint8_t *p, size_t pos, T& structure)
{
    really_memcpy(pod_addressof_m(structure), p + pos, sizeof(T));
}
inline
IP4Address RBUFIP(const uint8_t *p, size_t pos)
{
    IP4Address o;
    RBUF_STRUCT(p, pos, o);
    return o;
}
template<uint8_t len>
inline
VString<len-1> RBUF_STRING(const uint8_t *p, size_t pos)
{
    const char *const begin = static_cast<const char *>(RBUFP(p, pos));
    const char *const end = begin + len-1;
    const char *const mid = std::find(begin, end, '\0');
    return XString(begin, mid, nullptr);
}
inline
AString RBUF_STRING(const uint8_t *p, size_t pos, size_t len)
{
    const char *const begin = static_cast<const char *>(RBUFP(p, pos));
    const char *const end = begin + len;
    const char *const mid = std::find(begin, end, '\0');
    return XString(begin, mid, nullptr);
}


/// Unused - check how much data can be written
// the existence of this seems scary
inline
size_t WFIFOSPACE(Session *s)
{
    return s->max_wdata - s->wdata_size;
}
/// Write to the queue
inline
void *WFIFOP(Session *s, size_t pos)
{
    return &s->wdata[s->wdata_size + pos];
}
inline
uint8_t& WFIFOB(Session *s, size_t pos)
{
    return *static_cast<uint8_t *>(WFIFOP(s, pos));
}
inline
uint16_t& WFIFOW(Session *s, size_t pos)
{
    return *static_cast<uint16_t *>(WFIFOP(s, pos));
}
inline
uint32_t& WFIFOL(Session *s, size_t pos)
{
    return *static_cast<uint32_t *>(WFIFOP(s, pos));
}
template<class T>
void WFIFO_STRUCT(Session *s, size_t pos, T& structure)
{
    really_memcpy(static_cast<uint8_t *>(WFIFOP(s, pos)), pod_addressof_c(structure), sizeof(T));
}
inline
IP4Address& WFIFOIP(Session *s, size_t pos)
{
    static_assert(is_trivially_copyable<IP4Address>::value, "That was the whole point");
    return *static_cast<IP4Address *>(WFIFOP(s, pos));
}
inline
void WFIFO_STRING(Session *s, size_t pos, XString str, size_t len)
{
    char *const begin = static_cast<char *>(WFIFOP(s, pos));
    char *const end = begin + len;
    char *const mid = std::copy(str.begin(), str.end(), begin);
    std::fill(mid, end, '\0');
}
inline
void WFIFO_ZERO(Session *s, size_t pos, size_t len)
{
    uint8_t *b = static_cast<uint8_t *>(WFIFOP(s, pos));
    uint8_t *e = b + len;
    std::fill(b, e, '\0');
}
inline
void WFIFO_BUF_CLONE(Session *s, const uint8_t *buf, size_t len)
{
    really_memcpy(static_cast<uint8_t *>(WFIFOP(s, 0)), buf, len);
}

/// Write to an arbitrary buffer
inline
void *WBUFP(uint8_t *p, size_t pos)
{
    return p + pos;
}
inline
uint8_t& WBUFB(uint8_t *p, size_t pos)
{
    return *static_cast<uint8_t *>(WBUFP(p, pos));
}
inline
uint16_t& WBUFW(uint8_t *p, size_t pos)
{
    return *static_cast<uint16_t *>(WBUFP(p, pos));
}
inline
uint32_t& WBUFL(uint8_t *p, size_t pos)
{
    return *static_cast<uint32_t *>(WBUFP(p, pos));
}
template<class T>
void WBUF_STRUCT(uint8_t *p, size_t pos, T& structure)
{
    really_memcpy(p + pos, pod_addressof_c(structure), sizeof(T));
}
inline
IP4Address& WBUFIP(uint8_t *p, size_t pos)
{
    return *static_cast<IP4Address *>(WBUFP(p, pos));
}
inline
void WBUF_STRING(uint8_t *p, size_t pos, XString s, size_t len)
{
    char *const begin = static_cast<char *>(WBUFP(p, pos));
    char *const end = begin + len;
    char *const mid = std::copy(s.begin(), s.end(), begin);
    std::fill(mid, end, '\0');
}
inline
void WBUF_ZERO(uint8_t *p, size_t pos, size_t len)
{
    uint8_t *b = static_cast<uint8_t *>(WBUFP(p, pos));
    uint8_t *e = b + len;
    std::fill(b, e, '\0');
}

inline
void RFIFO_WFIFO_CLONE(Session *rs, Session *ws, size_t len)
{
    really_memcpy(static_cast<uint8_t *>(WFIFOP(ws, 0)),
            static_cast<const uint8_t *>(RFIFOP(rs, 0)), len);
}

#endif // TMWA_NET_VOMIT_HPP
