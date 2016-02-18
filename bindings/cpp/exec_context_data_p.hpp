/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 */

#include "elliptics/utils.hpp"
#include "elliptics/srw.h"
#include "elliptics/result_entry.hpp"

namespace ioremap { namespace elliptics {

struct exec_context_data
{
    data_pointer srw_data;
    std::string event;
    data_pointer data;

    static exec_context create_raw(const exec_context *other, const std::string &event, const argument_data &data)
    {
        std::shared_ptr<exec_context_data> p = std::make_shared<exec_context_data>();

        p->srw_data = data_pointer::allocate(sizeof(sph) + event.size() + data.size());

        sph *raw_sph = p->srw_data.data<sph>();
        if (other) {
            memcpy(p->srw_data.data<sph>(), other->m_data->srw_data.data<sph>(), sizeof(sph));
        } else {
            memset(raw_sph, 0, sizeof(sph));
            raw_sph->src_key = -1;
        }

        char *raw_event = reinterpret_cast<char *>(raw_sph + 1);
        memcpy(raw_event, event.data(), event.size());
        char *raw_data = raw_event + event.size();
        memcpy(raw_data, data.data(), data.size());

        raw_sph->event_size = event.size();
        raw_sph->data_size = data.size();

        p->event = event;
        p->data = data_pointer::from_raw(raw_data, raw_sph->data_size);

        return exec_context(p);
    }

    static exec_context create(const std::string &event, const argument_data &data)
    {
        return create_raw(NULL, event, data);
    }

    static exec_context copy(const exec_context &other, const std::string &event, const argument_data &data)
    {
        return create_raw(&other, event, data);
    }

    static exec_context copy(const sph &other, const std::string &event, const argument_data &data)
    {
        sph tmp = other;
        tmp.event_size = 0;
        tmp.data_size = 0;
        return copy(exec_context::from_raw(&tmp, sizeof(tmp)), event, data);
    }
};

}} // namespace ioremap::elliptics
