/* -*- mode:C++; c-basic-offset:4 -*-
     Shore-MT -- Multi-threaded port of the SHORE storage manager

                       Copyright (c) 2007-2009
      Data Intensive Applications and Systems Labaratory (DIAS)
               Ecole Polytechnique Federale de Lausanne

                         All Rights Reserved.

   Permission to use, copy, modify and distribute this software and
   its documentation is hereby granted, provided that both the
   copyright notice and this permission notice appear in all copies of
   the software, derivative works or modified versions, and any
   portions thereof, and that both notices appear in supporting
   documentation.

   This code is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS
   DISCLAIM ANY LIABILITY OF ANY KIND FOR ANY DAMAGES WHATSOEVER
   RESULTING FROM THE USE OF THIS SOFTWARE.
*/

/*<std-header orig-src='shore' incl-file-exclusion='W_OPAQUE_H'>

 $Id: w_opaque.h,v 1.6.2.6 2010/03/19 22:19:19 nhall Exp $

SHORE -- Scalable Heterogeneous Object REpository

Copyright (c) 1994-99 Computer Sciences Department, University of
                      Wisconsin -- Madison
All Rights Reserved.

Permission to use, copy, modify and distribute this software and its
documentation is hereby granted, provided that both the copyright
notice and this permission notice appear in all copies of the the
software, derivative works or modified versions, and any portions
thereof, and that both notices appear in supporting documentation.

THE AUTHORS AND THE COMPUTER SCIENCES DEPARTMENT OF THE UNIVERSITY
OF WISCONSIN - MADISON ALLOW FREE USE OF THIS SOFTWARE IN ITS
"AS IS" CONDITION, AND THEY DISCLAIM ANY LIABILITY OF ANY KIND
FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.

This software was developed with support by the Advanced Research
Project Agency, ARPA order number 018 (formerly 8230), monitored by
the U.S. Army Research Laboratory under contract DAAB07-91-C-Q518.
Further funding for this work was provided by DARPA through
Rome Research Laboratory Contract No. F30602-97-2-0247.

*/

#ifndef W_OPAQUE_H
#define W_OPAQUE_H

#include "w_defines.h"

/*  -- do not edit anything above this line --   </std-header>*/

#include <cctype>
#include <cstring>

#ifndef W_BASE_H
#include <w_base.h>
#endif

#ifdef __GNUC__
/* XXX gcc-2.7.2.3 has some weird problem with forward template function
   declarations.   The easiest way (for now) is to just not do it.
   This should be moved to w_workaround once the other similar
   issues are accounted for. */
#if W_GCC_THIS_VER < W_GCC_VER(2,95)
#define    W_NO_TEMPLATE_FORWARD_DECLS
#endif
#endif

template <int LEN> class opaque_quantity;
#ifndef W_NO_TEMPLATE_FORWARD_DECLS
template <int LEN> ostream &operator<<(ostream &o,
                       const opaque_quantity<LEN> &r);
template <int LEN> bool operator==(const opaque_quantity<LEN> &l,
                   const opaque_quantity<LEN> &r);
#endif

/**\brief A set of untyped bytes. 
 *
 * \details 
 *
 * This is just a blob.  Not necessarily large object,
 * but it is an untyped group of bytes. Used for
 * global transaction IDs and server IDs for two-phase
 * commit.  The storage manager has to log this information
 * for preparing a 2PC transaction, so it has to flow
 * through the API.
 */
template <int LEN> 
class opaque_quantity 
{

private:

    w_base_t::uint4_t _length; // Use qualified name if needed, though uint4_t should be global
    unsigned char _opaque[LEN];

    public:
    opaque_quantity() {
        // Qualify set_length with this->
        (void) this->set_length(0);
#ifdef ZERO_INIT
        memset(this->_opaque, '\0', LEN); // Qualify _opaque with this->
#endif
    }
    opaque_quantity(const char* s)
    {
#ifdef ZERO_INIT
        memset(this->_opaque, '\0', LEN); // Qualify _opaque with this->
#endif
        *this = s;
    }

    // Explicitly define copy constructor and assignment operator
    // to follow the Rule of Three/Five and avoid deprecation warnings
    opaque_quantity(const opaque_quantity<LEN>& other)
        : _length(other._length) {
        memcpy(this->_opaque, other._opaque, LEN); // Copy all bytes
    }

    opaque_quantity<LEN>& operator=(const opaque_quantity<LEN>& other) {
        if (this != &other) {
            this->_length = other._length;
            memcpy(this->_opaque, other._opaque, LEN); // Copy all bytes
        }
        return *this;
    }


    friend bool
    operator== <LEN> (
        const opaque_quantity<LEN>    &l,
        const opaque_quantity<LEN>    &r); 

    friend ostream & 
    operator<< <LEN> (
        ostream &o, 
        const opaque_quantity<LEN>    &b);

    opaque_quantity<LEN>    &
    operator=(const char* s)
    {
        w_assert9(strlen(s) <= LEN);
        // Qualify set_length with this->
        (void) this->set_length(0);
        // Qualify length and _opaque with this->
        while ((this->_opaque[this->length()] = *s++))
            (void) this->set_length(this->length() + 1);
        return *this;
    }
    opaque_quantity<LEN>    &
    operator+=(const char* s)
    {
        // Qualify length with this->
        w_assert9(strlen(s) + this->length() <= LEN);
        // Qualify set_length, length, and _opaque with this->
        while ((this->_opaque[this->set_length(this->length() + 1)] = *s++))
            ;
        return *this;
    }
    opaque_quantity<LEN>    &
    operator-=(w_base_t::uint4_t len) // Corrected declaration syntax and type
    {
        // Qualify length and set_length with this->
        w_assert9(len <= this->length());
        (void) this->set_length(this->length() - len);
        return *this;
    }
    opaque_quantity<LEN>    &
    append(const void* data, w_base_t::uint4_t len) // Use w_base_t::uint4_t
    {
        // Qualify length and _opaque with this->
        w_assert9(len + this->length() <= LEN);
        memcpy((void*)&this->_opaque[this->length()], data, len);
        // Qualify set_length and length with this->
        (void) this->set_length(this->length() + len);
        return *this;
    }
    opaque_quantity<LEN>    &
    zero()
    {
        // Qualify set_length and _opaque with this->
        (void) this->set_length(0);
        memset(this->_opaque, 0, LEN);
        return *this;
    }
    opaque_quantity<LEN>    &
    clear()
    {
        // Qualify set_length with this->
        (void) this->set_length(0);
        return *this;
    }
    void *
    data_at_offset(unsigned i)  const
    {
        // Qualify length and _opaque with this->
        w_assert9(i < this->length());
        return (void*)&this->_opaque[i];
    }
    w_base_t::uint4_t wholelength() const { // Use w_base_t::uint4_t
        // Qualify length with this->
        return (sizeof(this->_length) + this->length()); // Qualify _length with this->
    }
    w_base_t::uint4_t set_length(w_base_t::uint4_t l) { // Use w_base_t::uint4_t
        // Qualify _length with this->
        if(this->is_aligned()) { // Qualify is_aligned with this->
            this->_length = l;
        } else {
            char *m = (char *)&this->_length; // Qualify _length with this->
            memcpy(m, &l, sizeof(this->_length)); // Qualify _length with this->
        }
        return l;
    }
    w_base_t::uint4_t length() const { // Use w_base_t::uint4_t
        // Qualify is_aligned with this->
        if(this->is_aligned()) return this->_length; // Qualify _length with this->
        else {
            w_base_t::uint4_t l; // Use w_base_t::uint4_t
            char *m = (char *)&this->_length; // Qualify _length with this->
            memcpy(&l, m, sizeof(this->_length)); // Qualify _length with this->
            return l;
        }
    }

    void          ntoh()  {
        // Qualify is_aligned, _length, and length with this->
        if(this->is_aligned()) {
            this->_length = w_base_t::w_ntohl(this->_length);
        } else {
            w_base_t::uint4_t l = w_base_t::w_ntohl(this->length()); // Use w_base_t::uint4_t
            char *m = (char *)&l;
            memcpy(&this->_length, m, sizeof(this->_length)); // Qualify _length with this->
        }
    }
    void          hton()  {
        // Qualify is_aligned, _length, and length with this->
        if(this->is_aligned()) {
            this->_length = w_base_t::w_htonl(this->_length);
        } else {
            w_base_t::uint4_t l = w_base_t::w_htonl(this->length()); // Use w_base_t::uint4_t
            char *m = (char *)&l;
            memcpy(&this->_length, m, sizeof(this->_length)); // Qualify _length with this->
        }
    }

    /* XXX why doesn't this use the aligned macros? */
    bool          is_aligned() const  {
        // Qualify _length with this->
        return (((ptrdiff_t)(&this->_length) & (sizeof(this->_length) - 1)) == 0);
    }

    ostream        &print(ostream & o) const {
        // Qualify length with this->
        o << "opaque[" << this->length() << "]" ;

        // Qualify length with this-> and use w_base_t::uint4_t
        w_base_t::uint4_t print_length = this->length();
        if (print_length > LEN) {
            o << "[TRUNC TO LEN=" << LEN << "!!]";
            print_length = LEN;
        }
        o << '"';
        const unsigned char *cp = &this->_opaque[0]; // Qualify _opaque with this->
        // Use w_base_t::uint4_t for loop counter
        for (w_base_t::uint4_t i = 0; i < print_length; i++, cp++) {
            if (isprint(*cp))
                o << *cp;
            else {
                W_FORM(o)("\\x%02X", *cp);
            }
        }

        return o << '"';
    }
};


template <int LEN>
bool operator==(const opaque_quantity<LEN> &a,
    const opaque_quantity<LEN>    &b) 
{
    return ((a.length()==b.length()) &&
        (memcmp(a._opaque,b._opaque,a.length())==0));
}

template <int LEN>
ostream & 
operator<<(ostream &o, const opaque_quantity<LEN>    &b) 
{
    return b.print(o);
}

/*<std-footer incl-file-exclusion='W_OPAQUE_H'>  -- do not edit anything below this line -- */

#endif          /*</std-footer>*/
