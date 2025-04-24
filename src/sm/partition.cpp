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

/*<std-header orig-src='shore'>

 $Id: partition.cpp,v 1.3 2010/06/08 22:28:55 nhall Exp $

SHORE -- Scalable Heterogeneous Object REpository

Copyright (c) 1994-99 Computer Sciences Department, University of
                      Wisconsin -- Madison
All Rights Reserved.

Permission to use, copy, modify and distribute this software and its
documentation is hereby granted, provided that both the copyright
notice and this permission notice appear in all copies of the
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

#include "w_defines.h"

/*  -- do not edit anything above this line --   </std-header>*/

#define debug_log false

#define SM_SOURCE
#define PARTITION_C
#ifdef __GNUG__
#   pragma implementation
#endif

#include "sm_int_1.h"
#include "logtype_gen.h"
#include "log.h"
#include "log_core.h"
#include "w_rc.h"
#include "rdma_integration.h"
// DEAD #include "log_buf.h"

// needed for skip_log
#include "logdef_gen.cpp"

#include <sstream>   // For std::stringstream
#include <iomanip>   // For std::hex, std::setw, std::setfill
#include <string.h>  // For strerror
#include <algorithm> // For std::min
#include <cerrno>    // For errno

// Initialize on first access:
// block to be cleared upon first use.
class block_of_zeroes {
private:
    char _block[log_core::BLOCK_SIZE];
public:
    NORET block_of_zeroes() {
        memset(&_block[0], 0, log_core::BLOCK_SIZE);
    }
    char *block() { return _block; }
};

char *block_of_zeros() {

    static block_of_zeroes z;
    return z.block();
}

char *             
partition_t::_readbuf() { return _owner->readbuf(); }
#if W_DEBUG_LEVEL > 2
void              
partition_t::check_fhdl_rd() const {
    bool isopen = is_open_for_read();
    if(_fhdl_rd == invalid_fhdl) {
        w_assert3( !isopen );
    } else {
        w_assert3(isopen);
    }
}
void 
partition_t::check_fhdl_app() const {
    if(_fhdl_app != invalid_fhdl) {
        w_assert3(is_open_for_append());
    } else {
        w_assert3(! is_open_for_append());
    }
}
#endif

bool
partition_t::is_current()  const
{
    //  rd could be open
    if(index() == _owner->partition_index()) {
        w_assert3(num()>0);
        w_assert3(_owner->partition_num() == num());
        w_assert3(exists());
        w_assert3(_owner->curr_partition() == this);
        w_assert3(_owner->partition_index() == index());
        w_assert3(this->is_open_for_append());

        return true;
    }
#if W_DEBUG_LEVEL > 2
    if(num() == 0) {
        w_assert3(!this->exists());
    }
#endif 
    return false;
}


/*
 * open_for_append(num, end_hint)
 * "open" a file  for the given num for append, and
 * make it the current file.
 */
// MUTEX: flush, insert, partition
void
partition_t::open_for_append(partition_number_t __num, 
        const lsn_t& end_hint) 
{
    FUNC(partition::open_for_append);

    // shouldn't be calling this if we're already open
    w_assert3(!is_open_for_append());
    // We'd like to use this assertion, but in the
    // raw case, it's wrong: fhdl_app() is NOT synonymous
    // with is_open_for_append() and the same goes for ...rd()
    // w_assert3(fhdl_app() == 0);

    int         fd;

    DBG(<<"open_for_append num()=" << num()
            << "__num=" << __num
            << "_num=" << _num
            << " about to peek");

    /*
    if(num() == __num) {
        close_for_read();
        close_for_append();
        _num = 0; // so the peeks below
        // will work -- it'll get reset
        // again anyway.
   }
   */
    /* might not yet know its size - discover it now  */
    peek(__num, end_hint, true, &fd); // have to know its size
    w_assert3(fd);
    if(size() == nosize) {
        // we're opening a new partition
        set_size(0);
    }
        
    _num = __num;
    // size() was set in peek()
    w_assert1(size() != partition_t::nosize);

    _set_fhdl_app(fd);
    _set_state(m_flushed);
    _set_state(m_exists);
    _set_state(m_open_for_append);

    _owner->set_current( index(), num() );
    return ;
}

void
partition_t::clear()
{
    _num=0; 
    _size = nosize; 
    _mask=0; 
    _clr_state(m_open_for_read);
    _clr_state(m_open_for_append);
    DBGTHRD(<<"partition " << num() << " clear is clobbering " 
            << _fhdl_rd << " and " << _fhdl_app);
    _fhdl_rd = invalid_fhdl;
    _fhdl_app = invalid_fhdl;
}

void              
partition_t::init(log_core *owner) 
{
    _start = 0; // always
    _owner = owner;
    _eop = owner->limit(); // always
    clear();
}

/*
 * partition::flush(int fd, bool force)
 * flush to disk whatever's been buffered. 
 * Do this with a writev of 4 parts:
 * start->end1 where start is start1 rounded down to the beginning of a BLOCK
 * start2->end2
 * a skip record 
 * enough zeroes to make the entire write become a multiple of BLOCK_SIZE 
 */
void
partition_t::flush(
        int fd, // not necessarily fhdl_app() since flush is called from
        // skip, when peeking and this might be in recovery.
        lsn_t lsn,  // needed so that we can set the lsn in the skip_log record
        const char* const buf,
        long start1,
        long end1,
        long start2,
        long end2)
{
    long size = (end2 - start2) + (end1 - start1);
    long write_size = size;
    fileoff_t where;

    { // sync log: Seek the file to the right place.
        DBGTHRD( << "Sync-ing log lsn " << lsn
                << " start1 " << start1
                << " end1 " << end1
                << " start2 " << start2
                << " end2 " << end2 );

        // This change per e-mail from Ippokratis, 16 Jun 09:
        // long file_offset = _owner->floor(lsn.lo(), log_core::BLOCK_SIZE);
        // works because BLOCK_SIZE is always a power of 2
        long file_offset = log_core::floor2(lsn.lo(), log_core::BLOCK_SIZE);
        // offset is rounded down to a block_size

        long delta = lsn.lo() - file_offset;

        // adjust down to the nearest full block
        w_assert1(start1 >= delta); // really offset - delta >= 0,
                                    // but works for unsigned...
        write_size += delta; // account for the extra (clean) bytes
        start1 -= delta;

        /* FRJ: This seek is safe (in theory) because only one thread
           can flush at a time and all other accesses to the file use
           pread/pwrite (which doesn't change the file pointer).
         */
        where = start() + file_offset;
        RdmaSyscallResponse seekResponse = rdmaLseekFile((unsigned int)fd, where, SEEK_SET);
        if (seekResponse.status < 0) {
            w_rc_t e = RC(smlevel_0::eOS);
            W_FATAL_MSG(e.err_num(), << "ERROR: could not rdma seek to "
                                    << file_offset
                                    << " + " << start()
                                    << " to write log record"
                                    << endl);
        }
    } // end sync log

    /*
       stolen from log_buf::write_to
    */
    { // Copy a skip record to the end of the buffer.
        skip_log* s = _owner->get_skip_log();
        s->set_lsn_ck(lsn+size);

#ifdef W_TRACE
        RdmaSyscallResponse posResponse = rdmaLseekFile((unsigned int)fd, 0, SEEK_CUR);
        off_t position = (posResponse.status >= 0) ? posResponse.offset : -1; // Get position, handle error
        if (posResponse.status < 0) {
            w_rc_t e = RC(smlevel_0::eOS); // Log error if lseek fails (debug only, maybe not fatal)
            smlevel_0::errlog->clog << error_prio << "rdmaLseekFile(SEEK_CUR) failed in partition_t::flush trace." << endl << e << endl;
        }
		DBGTHRD(<<"setting lsn_ck in skip_log at pos "
				<< position << " with lsn "
                << s->get_lsn_ck()
                << "and size " << s->length()
                );
#endif

        // Hopefully the OS is smart enough to coalesce the writes
        // before sending them to disk. If not, and it's a problem
        // (e.g. for direct I/O), the alternative is to assemble the last
        // block by copying data out of the buffer so we can append the
        // skiplog without messing up concurrent inserts. However, that
        // could mean copying up to BLOCK_SIZE bytes.
        long total = write_size + s->length();

        // This change per e-mail from Ippokratis, 16 Jun 09:
        // long grand_total = _owner->ceil(total, log_core::BLOCK_SIZE);
        // works because BLOCK_SIZE is always a power of 2
        long grand_total = log_core::ceil2(total, log_core::BLOCK_SIZE);
        // take it up to multiple of block size
        w_assert2(grand_total % log_core::BLOCK_SIZE == 0);

        // --- Start Modification: Replace iovec and me()->writev with contiguous buffer and rdmaWalWrite ---
        // Allocate a contiguous buffer large enough for the combined data.
        char* contiguous_buffer = new char[grand_total];
        if (!contiguous_buffer) {
            W_FATAL(fcOUTOFMEMORY); // Handle allocation failure
            return; // Exit function on allocation failure
        }
        w_auto_delete_array_t<char> ad_buffer(contiguous_buffer); // Auto cleanup for the allocated buffer

        // Copy data from each part into the contiguous buffer sequentially.
        char* current_pos = contiguous_buffer;
        memcpy(current_pos, buf + start1, end1 - start1); current_pos += (end1 - start1);
        memcpy(current_pos, buf + start2, end2 - start2); current_pos += (end2 - start2);
        memcpy(current_pos, s, s->length()); current_pos += s->length(); // Assuming skip_log is a contiguous struct
        long padding_size = grand_total - total; memset(current_pos, 0, padding_size);


        // Call rdmaWalWrite to write the entire contiguous buffer.
        // - Use the RDMA file handle 'fd'.
        // - Pass the LSN of the skip record (&skip_lsn_for_flush) for the server to track the durable point.
        // - Use the calculated offset 'where' as the starting position for this write.
        // - The size is the total size including padding ('grand_total').
        // - Based on the user's clarification, this *single* rdmaWalWrite call represents a complete
        //   buffer flush operation by the thread (e.g., the flush daemon thread).
        //   Therefore, both start and end flags should be true to signal batch completion to the server.
        lsn_t skip_lsn_for_flush = lsn + size; // LSN of the first byte *after* the flushed data
        ssize_t write_result = rdmaWalWrite(contiguous_buffer, (unsigned int)fd, reinterpret_cast<lsn_t_c*>(&skip_lsn_for_flush), where, grand_total, true, true); // Use calculated offset 'where', total size, skip LSN, true/true flags - Use reinterpret_cast for lsn_t* to lsn_t_c*

        // Check the return value of rdmaWalWrite (ssize_t). Full success is writing exactly 'grand_total' bytes.
        if (write_result != (ssize_t)grand_total) {
            w_rc_t e = RC(smlevel_0::eOS); // Use smlevel_0::eOS for OS error
            if (write_result < 0) { // Explicit error return (-1)
                smlevel_0::errlog->clog << fatal_prio
                    << "ERROR: rdmaWalWrite failed writing log buffer contents at offset " << where
                    << " for fd " << fd << ". Return value: "
                    << write_result << "..." << flushl; // Add return value to log
                W_COERCE(e); // Handle fatal error (Exits or throws)
            } else { // Short write occurred (0 <= result < grand_total)
                smlevel_0::errlog->clog << fatal_prio
                    << "ERROR: rdmaWalWrite short write for log buffer contents at offset " << where
                    << " for fd " << fd << ". Wrote " << write_result << " of " << grand_total << " bytes." << flushl; // Add details
                W_COERCE(e); // Treat short write as fatal error
            }
            return; // Exit function on write failure
        }
        // --- End Modification: Replace iovec and me()->writev ---
    } // end prepare data and write


    // --- Start Modification: Replace original fsync step with RDMA flush completion ---
    // The original code would have called this->flush(fd) here to do the fsync.
    // Now we use the RDMA flush completion mechanism to wait for durability.
    // We wait for the LSN corresponding to the end of the written data (the skip record's LSN).
    lsn_t lsn_to_wait_for = lsn + (end2 - start2) + (end1 - start1); // LSN of the first byte *after* the flushed data, recalculate size here for clarity
    int fsync_result = isRdmaFlushCompleted(reinterpret_cast<lsn_t_c*>(&lsn_to_wait_for)); // Call C wrapper - Pass LSN to wait for

    if (fsync_result < 0) { // Check for error return from C wrapper (-1 indicates error)
        w_rc_t e = RC(smlevel_0::eOS); // Use smlevel_0::eOS for OS error
         smlevel_0::errlog->clog << fatal_prio
             << "   isRdmaFlushCompleted failed for LSN " << lsn_to_wait_for << " after flushing buffer for fd " << fd << "." << flushl; // Log the LSN and fd
         W_COERCE(e); // Handle fatal error
    } else {
         DBGTHRD(<<"isRdmaFlushCompleted successful for LSN " << lsn_to_wait_for << " after flushing buffer for fd " << fd << "."); // Debug on success
    }
    // --- End Modification: Replace fsync step ---

    // --- Start Modification: Add state update after successful flush confirmation ---
    // Although not in the original snippets, setting this state here is logical
    // in the RDMA context after confirming durability via isRdmaFlushCompleted.
    _set_state(m_flushed); // Set the partition state to flushed
    // --- End Modification: Add state update ---
}

/*
 *  partition_t::_peek(num, peek_loc, whole_size,
        recovery, fd) -- used by both -- contains
 *   the guts
 *
 *  Peek at a partition num() -- see what num it represents and
 *  if it's got anything other than a skip record in it.
 *
 *  If recovery==true,
 *  determine its size, if it already exists (has something
 *  other than a skip record in it). In this case its num
 *  had better match num().
 *
 *  If it's just a skip record, consider it not to exist, and
 *  set _num to 0, leave it "closed"
 *
 *********************************************************************/
void
partition_t::_peek(
    partition_number_t num_wanted,
    fileoff_t        peek_loc,
    fileoff_t        whole_size,
    bool recovery,
    int fd
)
{
    FUNC(partition_t::_peek);
    w_assert3(num() == 0 || num() == num_wanted);
    clear();

    _clr_state(m_exists);
    _clr_state(m_flushed);
    _clr_state(m_open_for_read);

    w_assert3(fd);

    logrec_t        *l = NULL;

    // seek to start of partition or to the location given
    // in peek_loc -- that's a location we suspect might
    // be the end of-the-log skip record.
    //
    // the lsn passed to read(rec,lsn) is not
    // inspected for its hi() value
    //
    bool  peeked_high = false;
    if(    (peek_loc != partition_t::nosize)
        && (peek_loc <= this->_eop) 
        && (peek_loc < whole_size) ) {
        peeked_high = true;
    } else {
        peek_loc = 0;
        peeked_high = false;
    }
again:
    lsn_t pos = lsn_t(uint4_t(num()), sm_diskaddr_t(peek_loc));

    lsn_t lsn_ck = pos ;
    w_rc_t rc;

    while(pos.lo() < this->_eop) {
        DBGTHRD("pos.lo() = " << pos.lo()
                << " and eop=" << this->_eop);
        if(recovery) {
            // increase the starting point as much as possible.
            // to decrease the time for recovery
            if(pos.hi() == _owner->master_lsn().hi() &&
               pos.lo() < _owner->master_lsn().lo())  {
                  if(!debug_log) {
                      pos = _owner->master_lsn();
                  }
            }
        }
        DBGTHRD( <<"reading pos=" << pos <<" eop=" << this->_eop);

        rc = read(l, pos, fd);
        DBGTHRD(<<"POS " << pos << ": tx." << *l);

        if(rc.err_num() == smlevel_0::eEOF) {
            // eof or record -- wipe it out
            DBGTHRD(<<"EOF--Skipping!");
            _skip(pos, fd);
            break;
        }

        w_assert1(l != NULL);
                
        DBGTHRD(<<"peek index " << _index 
            << " l->length " << l->length() 
            << " l->type " << int(l->type()));

        w_assert1(l->length() >= logrec_t::hdr_sz);
        {
            // check lsn
            lsn_ck = l->get_lsn_ck();
            int err = 0;

            DBGTHRD( <<"lsnck=" << lsn_ck << " pos=" << pos
                <<" l.length=" << l->length() );


            if( ( l->length() < logrec_t::hdr_sz )
                ||
                ( l->length() > sizeof(logrec_t) )
                ||
                ( lsn_ck.lo() !=  pos.lo() )
                ||
                (num_wanted  && (lsn_ck.hi() != num_wanted) )
                ) {
                err++;
            }

            if( num_wanted  && (lsn_ck.hi() != num_wanted) ) {
                // Wrong partition - break out/return
                DBGTHRD(<<"NOSTASH because num_wanted="
                        << num_wanted
                        << " lsn_ck="
                        << lsn_ck
                    );
                return;
            }

            DBGTHRD( <<"type()=" << int(l->type())
                << " index()=" << this->index() 
                << " lsn_ck=" << lsn_ck
                << " err=" << err );

            /*
            // if it's a skip record, and it's the first record
            // in the partition, its lsn might be null.
            //
            // A skip record that's NOT the first in the partiton
            // will have a correct lsn.
            */

#if W_DEBUG_LEVEL > 1
            if( l->type() == logrec_t::t_skip ) {
                smlevel_0::errlog->clog << info_prio <<
                "Found skip record " << " at " << pos
                << flushl;
            }
#endif
            if( l->type() == logrec_t::t_skip   && 
                pos == first_lsn()) {
                // it's a skip record and it's the first rec in partition
                if( lsn_ck != lsn_t::null )  {
                    DBGTHRD( <<" first rec is skip and has lsn " << lsn_ck );
                    err = 1; 
                }
            } else {
                // ! skip record or ! first in the partition
                if ( (lsn_ck.hi()-1) % PARTITION_COUNT != (uint4_t)this->index()) {
                    DBGTHRD( <<"unexpected end of log");
                    err = 2;
                }
            }
            if(err > 0) {
                // bogus log record, 
                // consider end of log to be previous record

                if(err > 1) {
                    smlevel_0::errlog->clog << error_prio <<
                    "Found unexpected end of log --"
                    << " probably due to a previous crash." 
                    << flushl;
                }

                if(peeked_high) {
                    // set pos to 0 and start this loop all over
                    DBGTHRD( <<"Peek high failed at loc " << pos);
                    peek_loc = 0;
                    peeked_high = false;
                    goto again;
                }

                /*
                // Incomplete record -- wipe it out
                */
#if W_DEBUG_LEVEL > 2
                if(pos.hi() != 0) {
                   w_assert3(pos.hi() == num_wanted);
                }
#endif 

                // assign to lsn_ck so that the when
                // we drop out the loop, below, pos is set
                // correctly.
                lsn_ck = lsn_t(num_wanted, pos.lo());
                _skip(lsn_ck, fd);
                break;
            }
        }
        // DBGTHRD(<<" changing pos from " << pos << " to " << lsn_ck );
        pos = lsn_ck;

        DBGTHRD(<< " recovery=" << recovery
            << " master=" << _owner->master_lsn()
        );
        if( l->type() == logrec_t::t_skip 
            || !recovery) {
            /*
             * IF 
             *  we hit a skip record 
             * or 
             *  if we're not in recovery (i.e.,
             *  we aren't trying to find the last skip log record
             *  or check each record's legitimacy)
             * THEN 
             *  we've seen enough
             */
            DBGTHRD(<<" BREAK EARLY ");
            break;
        }
        pos.advance(l->length());
    }

    // pos == 0 if the first record
    // was a skip or if we don't care about the recovery checks.

    w_assert1(l != NULL);
    DBGTHRD(<<"pos= " << pos << "l->type()=" << int(l->type()));

#if W_DEBUG_LEVEL > 2
    if(pos.lo() > first_lsn().lo()) {
        w_assert3(l!=0);
    }
#endif 

    if( pos.lo() > first_lsn().lo() || l->type() != logrec_t::t_skip ) {
        // we care and the first record was not a skip record
        _num = pos.hi();

        // let the size *not* reflect the skip record
        // and let us *not* set it to 0 (had we not read
        // past the first record, which is the case when
        // we're peeking at a partition that's earlier than
        // that containing the master checkpoint
        // 
        if(pos.lo()> first_lsn().lo()) set_size(pos.lo());

        // OR first rec was a skip so we know
        // size already
        // Still have to figure out if file exists

        _set_state(m_exists);

        DBGTHRD(<<"STASHED num()=" << num()
                << " size()=" << size()
            );
    } else { 
        w_assert3(num() == 0);
        w_assert3(size() == nosize || size() == 0);
        // size can be 0 if the partition is exactly
        // a skip record
        DBGTHRD(<<"SIZE NOT STASHED ");
    }
}


// Helper for _peek
void
partition_t::_skip(const lsn_t &ll, int fd)
{
    FUNC(partition_t::skip);

    // Current partition should flush(), not skip()
    w_assert1(_num == 0 || _num != _owner->partition_num());
    
    DBGTHRD(<<"skip at " << ll);

    char* _skipbuf = new char[log_core::BLOCK_SIZE*2];
    // FRJ: We always need to prime() partition ops (peek, open, etc)
    // always use a different buffer than log inserts.
    long offset = _owner->prime(_skipbuf, fd, start(), ll);
    
    // Make sure that flush writes a skip record
    this->flush(fd, ll, _skipbuf, offset, offset, offset, offset);
    delete [] _skipbuf;
    DBGTHRD(<<"wrote and flushed skip record at " << ll);

    _set_last_skip_lsn(ll);
}

/*
 * partition_t::read(logrec_t *&rp, lsn_t &ll, int fd)
 * 
 * expect ll to be correct for this partition.
 * if we're reading this for the first time,
 * for the sake of peek(), we expect ll to be
 * lsn_t(0,0), since we have no idea what
 * its lsn is supposed to be, but in fact, we're
 * trying to find that out.
 *
 * If a non-zero fd is given, the read is to be done
 * on that fd. Otherwise it is assumed that the
 * read will be done on the fhdl_rd().
 */
// MUTEX: partition
w_rc_t
partition_t::read(logrec_t *&rp, lsn_t &ll, int fd)
{
    FUNC(partition::read); // Standard Shore-MT tracing macro
    INC_TSTAT(log_fetches); // Keep stats for the logical fetch operation

    // Use the provided fd, or the partition's read handle if fd is invalid.
    // invalid_fhdl should be compatible with int.
    if(fd == invalid_fhdl) fd = fhdl_rd();

#if W_DEBUG_LEVEL > 2
    // Debug assertions based on partition state and input LSN/fd.
    w_assert3(fd >= 0); // Assert fd is a valid handle (>= 0)
    if(exists()) {
        // If the partition exists, it should be open for read if using its handle.
        if(fd == fhdl_rd()) w_assert3(is_open_for_read());
        // The partition number should match the high part of the LSN.
        w_assert3(num() == ll.hi());
    }
#endif

    // Calculate the offset within the partition file corresponding to the LSN.
    fileoff_t pos = ll.lo(); // Offset within the partition data stream

    // Calculate the starting offset of the XFERSIZE block containing pos.
    fileoff_t lower = pos / XFERSIZE;
    lower *= XFERSIZE;
    // Calculate the offset within the XFERSIZE block where the log record starts.
    fileoff_t off = pos - lower;

    DBGTHRD(<<"Reading log record at lsn " << ll
        << " index=" << _index << " fd=" << fd
        << " pos=" << pos
        << " lower=" << lower  << " + " << start() // start() is 0 for file partitions
        << " fd=" << fd
    );

    /*
     * read & inspect header size and see
     * and see if there's more to read
     *
     * We read data in chunks of XFERSIZE bytes into the _readbuf().
     * The log record starts at _readbuf() + off.
     */
    int b = 0; // Offset into _readbuf() for the current read chunk (increments by XFERSIZE)
    fileoff_t leftover = logrec_t::hdr_sz; // Initially need at least the header size
    bool first_time_read = true; // Flag to know when the first XFERSIZE chunk (containing header) is read

    // Set rp to point to where the log record *should* start within _readbuf(), relative to 'off'.
    // The actual data will be read into _readbuf() starting from offset 0 relative to _readbuf().
    // rp will point into this buffer at the correct offset.
    rp = (logrec_t *)(_readbuf() + off);

    DBGTHRD(<< "Record starts at offset " << ((int)off) << " within the XFERSIZE block buffer (_readbuf())."
        << "_readbuf()@ " << W_ADDR(_readbuf())
        << " rp@ " << W_ADDR(rp)
    );

    // Loop to read XFERSIZE chunks until the entire log record is in the buffer.
    // 'b' tracks the cumulative offset into _readbuf() where the current chunk is placed.
    // 'start() + lower + b' is the corresponding absolute file offset for the read.
    while (leftover > 0) {

        DBGTHRD(<<"Reading chunk. leftover=" << int(leftover) << " b=" << b << " File Offset=" << start() + lower + b);

        // --- Start Modification: Replace me()->pread with rdmaWalRead ---
        // Read XFERSIZE bytes from the file into _readbuf() at offset 'b'.
        // The file offset is start() + lower + b.
        ssize_t read_result = rdmaWalRead((unsigned int)fd, start() + lower + b, XFERSIZE, _readbuf() + b);

        // Check the return value of rdmaWalRead (ssize_t).
        // We expect to read XFERSIZE bytes in each iteration.
        if (read_result != (ssize_t)XFERSIZE) {
            // Handle error or short read. Treat as end of log or fatal error depending on context.
            // During log scanning (_peek), a read failure or short read indicates the end of the valid log.
             w_rc_t e = RC(smlevel_0::eOS); // Use smlevel_0::eOS for OS error
             std::stringstream err_msg;
             err_msg << "ERROR: rdmaWalRead failed or short read while reading log record at LSN " << ll << " in partition_t::read."
                     << " FD: " << fd << ", File Offset: " << start() + lower + b
                     << ", Requested Size: " << XFERSIZE << ", Bytes Read: " << read_result;

             if (read_result >= 0) {
                 // Short read (read less than requested, but no explicit error). Indicates end of file or corruption.
                 smlevel_0::errlog->clog << error_prio << err_msg.str() << endl << e << flushl; // Log as error
                 // Treat this as end of valid log for the scanning process.
                 return RC(smlevel_0::eEOF); // Return eEOF on short read
             } else { // read_result < 0 (explicit error return from rdmaWalRead)
                  // Explicit read error is typically fatal.
                  err_msg << ". System Error: " << strerror(errno); // Append system error message
                  e = RC_AUGMENT(e); // Augment w_rc_t with errno
                  smlevel_0::errlog->clog << fatal_prio << err_msg.str() << endl << e << flushl; // Log as fatal
                  W_FATAL(e.err_num()); // Handle fatal error
                  // Original W_COERCE allows execution to continue potentially.
                  // If you prefer W_COERCE's behavior, use it instead of W_FATAL + return eEOF.
                  // W_COERCE(e); return e.reset(); // Alternative if W_COERCE is desired
                  return RC(smlevel_0::eEOF); // Still return eEOF to signal end of log to caller (_peek)
             }
        }
        // --- End Modification: Replace me()->pread ---

        b += XFERSIZE; // Move the buffer offset for the next read chunk

        // This logic processes the first XFERSIZE block read (when first_time_read is true).
        // It verifies the header and calculates the total record length.
        if (first_time_read) {
            // After the first XFERSIZE read, the header should be available in the buffer at _readbuf() + off.
            // Check the log record length from the header pointed to by rp.
            if( rp->length() > sizeof(logrec_t) || rp->length() < logrec_t::hdr_sz ) {
                // Invalid header length found. This indicates corruption or end of valid log.
                // This assertion from the original code is expected during peek() when scanning.
                w_assert1(ll.hi() == 0 || num() == ll.hi()); // Added check for partition number
                DBGTHRD(<<"Invalid log record length found at LSN " << ll << ". Length: " << rp->length() << ".");
                return RC(smlevel_0::eEOF); // Return eEOF, matching original logic for bad length
            }
            first_time_read = false; // Header has been processed

            // Calculate how much more data is needed *after* the first XFERSIZE bytes have been read into the buffer.
            // total_bytes_needed is the record length (rp->length()).
            // bytes_already_in_buffer_for_record = b - off.
            // leftover is total_bytes_needed - bytes_already_in_buffer_for_record.
            leftover = rp->length() - (b - off);

            DBGTHRD(<<"Calculated total record length: " << rp->length()
                << ". Bytes read into buffer for record: " << (b - off)
                << ". Leftover bytes to read: " << leftover);

        } else {
            // For subsequent reads (after the first block), subtract the XFERSIZE bytes just read from leftover.
            leftover -= XFERSIZE;
            // The original code had an assertion here:
            // w_assert3(leftover == (int)rp->length() - (b - off)); // This assertion should still hold if logic is correct

            DBGTHRD(<<"Leftover bytes after reading chunk: " << leftover);
        }
    }

    // After the loop, the entire log record (header + body) should be present in _readbuf(),
    // starting at the memory location pointed to by rp (_readbuf() + off).

    DBGTHRD( << "_readbuf()@ " << W_ADDR(_readbuf())
        << " Record at LSN " << ll << " starts at offset " << off << " in buffer."
        << " First few bytes at " << W_ADDR(rp) << ": " // Log bytes from rp, not start of buffer
        << std::hex << std::setw(2) << std::setfill('0') << (int)((unsigned char*)rp)[0] << " "
        << std::hex << std::setw(2) << std::setfill('0') << (int)((unsigned char*)rp)[1] << " "
        << std::hex << std::setw(2) << std::setfill('0') << (int)((unsigned char*)rp)[2] << " "
        << std::hex << std::setw(2) << std::setfill('0') << (int)((unsigned char*)rp)[3] << "..."
    );

    w_assert1(rp != NULL); // Ensure rp is not null

    // The input lsn 'll' should not be modified by read(). The caller (_peek)
    // is responsible for advancing the LSN based on the record's lsn_ck.

    return RCOK; // Return success
}


w_rc_t
partition_t::open_for_read(
    partition_number_t  __num,
    bool err // = true.  if true, it's an error for the partition not to exist
)
{
    FUNC(partition_t::open_for_read); // Standard Shore-MT tracing macro
    // protected w_assert2(_owner->_partition_lock.is_mine()==true); // Keep assertion if still relevant

    DBGTHRD(<<"start open for part " << __num << " err=" << err);

    w_assert1(__num != 0); // Cannot open partition 0

    // If the partition is not already open for read (check internal handle).
    if(fhdl_rd() == invalid_fhdl) {
        char *fname = new char[smlevel_0::max_devname];
        if (!fname)
                W_FATAL(fcOUTOFMEMORY); // Handle allocation failure
        w_auto_delete_array_t<char> ad_fname(fname); // Auto cleanup for filename buffer

        // Generate the log partition filename.
        log_m::make_log_name(__num, fname, smlevel_0::max_devname);

        //int fd; // Original variable to hold FD from open
        //w_rc_t e; // Original variable to hold RC from open

        DBGTHRD(<< "partition " << __num << " open_for_read OPEN " << fname);

        // Set flags for read-only open. Assumes smthread_t::OPEN_RDONLY maps to OS flags compatible with rdmaOpenFile.
        int flags = smthread_t::OPEN_RDONLY;
        int mode = 0; // Permissions mode, 0 implies default or inherited

        // --- Start Modification: Replace me()->open with rdmaOpenFile ---
        // Call rdmaOpenFile to open the file on the remote machine.
        RdmaSyscallResponse openResponse = rdmaOpenFile(fname, flags, mode);

        // The file descriptor/handle is returned in openResponse.status.
        int fd = openResponse.status; // Use fd to hold the returned handle

        DBGTHRD(<< " rdmaOpenFile " << fname << " returned handle " << fd);

        // Check the status from the response. < 0 indicates an error.
        if (fd < 0) { // Check status for error (handle < 0 as error)
            w_rc_t e = RC(smlevel_0::eOS); // Use smlevel_0::eOS for OS error

            // Log detailed error message.
            std::stringstream err_msg;
            err_msg << "ERROR: rdmaOpenFile failed for log partition number " << __num
                    << ", filename: " << fname
                    << ". rdmaOpenFile status: " << fd; // fd holds the error status here

            // Attempt to map the error status to a system error if possible
            // (depends on what rdmaOpenFile returns on error).
            // If status < 0 is just an internal code, logging the status is enough.
            // If status maps to errno, you might add strerror.
            // Let's assume for now logging the status is sufficient or requires custom mapping.

            if(err) {
                // If err is true, treat open failure as fatal.
                smlevel_0::errlog->clog << fatal_prio << err_msg.str() << endl << e << flushl;
                // Original used W_DO(e), which logs and returns the error.
                // Let's construct the w_rc_t and return it.
                // e.err_num() will be smlevel_0::eOS, can add more info if needed.
                return e.reset(); // Return the error code
            } else {
                // If err is false, it's not an error for the file *not* to exist.
                // This might happen during scanning for existing partitions.
                // Original code cleaned up and returned RCOK, implying file not found/needed.
                smlevel_0::errlog->clog << error_prio << err_msg.str() << endl << e << flushl; // Log as error, but not fatal

                // Ensure state reflects that it's not open for read.
                w_assert3(! exists()); // If it didn't exist, exists() should be false
                w_assert3(_fhdl_rd == invalid_fhdl); // Handle should still be invalid
                _clr_state(m_open_for_read); // Ensure state flag is clear
                DBGTHRD(<<"fhdl_app() is " << _fhdl_app << " (not open for read).");
                return RCOK; // Return RCOK, indicating graceful handling (file not needed/found)
            }
        }

        // If we reached here, open was successful (fd >= 0).
        // Store the successfully opened RDMA file handle.
        w_assert3(_fhdl_rd == invalid_fhdl); // Assert handle was invalid before storing
        _fhdl_rd = fd; // Store the RDMA file handle

        // --- End Modification: Replace me()->open ---


        DBGTHRD(<<"size is " << size());
        // size might not be known at this point (size() == partition_t::nosize),
        // especially if this is an old partition being opened for the first time.
        // The size will be determined later if needed (e.g., during peek).


        // Set partition state flags.
        _set_state(m_exists); // File was found and opened, so it exists.
        _set_state(m_open_for_read); // Partition is now open for read.
    }
    // If we fall through the if, the partition was already open for read.

    // Update the partition number. This should match the __num requested.
    _num = __num; // Assign the partition number

    // Assert consistent state after opening or if already open.
    w_assert3(exists()); // Should exist if open or just opened
    w_assert3(is_open_for_read()); // Should be open for read now
    // The flushed state is not guaranteed here.
    // w_assert3(flushed()); // Removed/commented out as in original block

    // Assert the stored file handle is valid.
    w_assert3(_fhdl_rd != invalid_fhdl);
    DBGTHRD(<<"_fhdl_rd = " <<_fhdl_rd );

    return RCOK; // Return success
}

void
partition_t::close(bool both)
{
    bool err_encountered=false; // Flag to track if any close operation failed
    w_rc_t last_error_rc; // Variable to store the last error encountered as w_rc_t

    // protected member: w_assert2(_owner->_partition_lock.is_mine()==true); // Keep assertion if still relevant
    // assert is done by callers (as per original comment)

    // If this partition is currently the active partition, unset it in the owner.
    if(is_current()) {
        _owner->unset_current(); // Update owner's state
    }

    // If 'both' flag is true, attempt to close the read file handle.
    if (both) {
        // Check if the read handle is currently valid (partition is open for read).
        if (fhdl_rd() != invalid_fhdl) {
            int handle_to_close = fhdl_rd(); // Get the read handle

            DBGTHRD(<< " CLOSE read handle: " << handle_to_close << " for partition " << num()); // Log which handle is being closed

            // --- Start Modification: Replace me()->close with rdmaCloseFile ---
            // Call rdmaCloseFile to close the handle on the remote machine.
            RdmaSyscallResponse closeResponse = rdmaCloseFile((unsigned int)handle_to_close);

            // Check the status from the response. < 0 indicates an error.
            if (closeResponse.status < 0) { // Check status for error (handle < 0 as error)
                err_encountered = true; // Set the error flag
                // Create a w_rc_t for the error. Use smlevel_0::eOS for OS error.
                // May need to map closeResponse.status to a more specific errno or w_rc_t code if possible.
                last_error_rc = RC(smlevel_0::eOS);
                // Log the error.
                smlevel_0::errlog->clog << error_prio
                        << "ERROR: rdmaCloseFile failed for read handle " << handle_to_close
                        << " (partition " << num() << "). Status: " << closeResponse.status
                        << "." << endl << last_error_rc << endl << flushl;
            }
            // --- End Modification: Replace me()->close ---
        }
        // Regardless of close success/failure, mark the read handle as invalid
        // and clear the read state flag to transition the object state to "not open for read".
        _fhdl_rd = invalid_fhdl; // Invalidate the internal read handle
        _clr_state(m_open_for_read); // Clear the state flag
    }

    // Attempt to close the append file handle if the partition is open for append.
    if (is_open_for_append()) {
        int handle_to_close = fhdl_app(); // Get the append handle

        // The original code had DBGTHRD(<< " CLOSE " << fhdl_rd()); here, likely a copy-paste error.
        DBGTHRD(<< " CLOSE append handle: " << handle_to_close << " for partition " << num()); // Corrected log message

        // --- Start Modification: Replace me()->close with rdmaCloseFile ---
        // Call rdmaCloseFile to close the handle on the remote machine.
        RdmaSyscallResponse closeResponse = rdmaCloseFile((unsigned int)handle_to_close);

        // Check the status from the response. < 0 indicates an error.
        if (closeResponse.status < 0) { // Check status for error (handle < 0 as error)
            err_encountered = true; // Set the error flag
            // Create a w_rc_t for the error. Use smlevel_0::eOS for OS error.
            last_error_rc = RC(smlevel_0::eOS);
            // Log the error.
            smlevel_0::errlog->clog << error_prio
            << "ERROR: rdmaCloseFile failed for append handle " << handle_to_close
            << " (partition " << num() << "). Status: " << closeResponse.status
            << "." << endl << last_error_rc << endl << flushl;
        }
        // --- End Modification: Replace me()->close ---

        // Regardless of close success/failure, mark the append handle as invalid
        // and clear the append state flag to transition the object state to "not open for append".
        _fhdl_app = invalid_fhdl; // Invalidate the internal append handle
        _clr_state(m_open_for_append); // Clear the state flag
        DBGTHRD(<<"fhdl_app() is " << _fhdl_app << " (after close attempt)."); // Log the state
    }

    // Clear the flushed state flag. This happens regardless of closing handles or errors.
    _clr_state(m_flushed); // Clear the flushed state

    // If any close operation encountered an error, make the overall function fatal.
    if (err_encountered) {
        // The last error encountered is stored in last_error_rc.
        W_COERCE(last_error_rc); // Use W_COERCE to handle the fatal error
    }
    // If no errors were encountered, the function implicitly returns RCOK (void function).
}


void 
partition_t::sanity_check() const
{
    if(num() == 0) {
       // initial state
       w_assert3(size() == nosize);
       w_assert3(!is_open_for_read());
       w_assert3(!is_open_for_append());
       w_assert3(!exists());
       // don't even ask about flushed
    } else {
       w_assert3(exists());
       (void) is_open_for_read();
       (void) is_open_for_append();
    }
    if(is_current()) {
       w_assert3(is_open_for_append());
    }
}



/**********************************************************************
 *
 *  partition_t::destroy()
 *
 *  Destroy a log file.
 *
 *********************************************************************/
void
partition_t::destroy()
{
    w_assert3(num() < _owner->global_min_lsn().hi());

    if(num()>0) {
        w_assert3(exists());
        w_assert3(! is_current() );
        w_assert3(! is_open_for_read() );
        w_assert3(! is_open_for_append() );

        log_core::destroy_file(num(), true);
        _clr_state(m_exists);
        // _num = 0;
        DBG(<< " calling clear");
        clear();
    }
    w_assert3( !exists());
    sanity_check();
}

/*
 * partition_t::peek(num, peek_loc, whole_size,
        recovery, fdp) -- used by both -- contains
 * the guts
 *
 * Peek at a partition num() -- see what num it represents and
 * if it's got anything other than a skip record in it.
 *
 * If recovery==true,
 * determine its size, if it already exists (has something
 * other than a skip record in it). In this case its num
 * had better match num().
 *
 * If it's just a skip record, consider it not to exist, and
 * set _num to 0, leave it "closed"
 *
 * Modified to use RDMA file operations (open, fstat, ftruncate, fsync, close).
 */
void
partition_t::peek(
    partition_number_t  __num,
    const lsn_t&        end_hint, // Used for peek_loc calculation if part_size > 0 and recovery
    bool                 recovery, // Flag for recovery mode scanning
    int * fdp       // Optional pointer to return the opened file descriptor/handle
)
{
    FUNC(partition_t::peek); // Standard Shore-MT tracing macro
    // w_assert2(_owner->_partition_lock.is_mine()==true); // Keep assertion if still relevant

    int fd; // Variable to hold the opened file descriptor/handle

    // If this partition object already represents an open partition (__num),
    // close its existing handles and clear its state before proceeding.
    // This seems to handle reusing a partition_t object.
    if( num() ) { // Checks if _num is non-zero
        close_for_read(); // Calls method, assume ported
        close_for_append(); // Calls method, assume ported
        DBG(<< " calling clear");
        clear(); // Calls method, assume ported
    }

    // Clear state flags related to existence and flushed status before checking/opening the file.
    _clr_state(m_exists);
    _clr_state(m_flushed);

    // Allocate buffer for the log partition filename and generate the name.
    char *fname = new char[smlevel_0::max_devname];
    if (!fname)
        W_FATAL(fcOUTOFMEMORY); // Handle allocation failure
    w_auto_delete_array_t<char> ad_fname(fname); // Auto cleanup for filename buffer
    log_m::make_log_name(__num, fname, smlevel_0::max_devname);

    // Variable to store the size of the partition file obtained from stat.
    smlevel_0::fileoff_t part_size = fileoff_t(0);

    DBGTHRD(<<"partition " << __num << " peek opening " << fname);

    // --- Start Modification: Replace me()->open with rdmaOpenFile ---
    // Open the partition file. Use RDWR, SYNC, and CREATE flags.
    // Assumes smthread_t::OPEN_... flags map correctly to OS flags compatible with rdmaOpenFile.
    int flags = smthread_t::OPEN_RDWR | smthread_t::OPEN_SYNC | smthread_t::OPEN_CREATE;
    int mode = 0744; // Permissions mode

    RdmaSyscallResponse openResponse = rdmaOpenFile(fname, flags, mode);

    // Get the file descriptor/handle from the response.
    fd = openResponse.status; // Use fd to hold the returned handle

    // Check the status from the response. < 0 indicates an error.
    if (fd < 0) { // Check status for error
        w_rc_t e = RC(smlevel_0::eOS); // Use smlevel_0::eOS for OS error
        std::stringstream err_msg;
        err_msg << "ERROR: rdmaOpenFile failed for log partition number " << __num
                << ", filename: " << fname
                << ". rdmaOpenFile status: " << fd; // fd holds the error status here

        // In peek(), open failure is typically a fatal error.
        smlevel_0::errlog->clog << fatal_prio << err_msg.str() << endl << e << flushl;
        W_COERCE(e); // Handle fatal error
        return; // Exit function
    }
    // If we reached here, open was successful (fd >= 0).
    DBGTHRD(<<"partition " << __num << " peek  opened " << fname << " with handle " << fd);
    // --- End Modification: Replace me()->open ---

    // --- Start Modification: Replace me()->fstat with rdmaFstatCall ---
    // Get the size of the opened file using fstat.
    RdmaSyscallResponse fstatResponse = rdmaFstatCall((unsigned int)fd);

    // Check the status from the response. < 0 indicates an error.
    if (fstatResponse.status < 0) { // Check status for error
        w_rc_t e = RC(smlevel_0::eOS); // Use smlevel_0::eOS for OS error
        std::stringstream err_msg;
        err_msg << " ERROR: rdmaFstatCall failed for fd " << fd
                << " (partition " << __num << "). Status: " << fstatResponse.status;

        // In peek(), fstat failure on an opened file is typically a fatal error.
        smlevel_0::errlog->clog << fatal_prio << err_msg.str() << endl << e << flushl;
        W_COERCE(e); // Handle fatal error
        // Need to close the FD before exiting on error? Original code didn't show explicit close here.
        // If W_COERCE might not exit, consider adding rdmaCloseFile(fd); here.
        return; // Exit function
    }
    // If successful, get the size from the response. Assumes RdmaSyscallResponse has a 'size' member for stat results.
    part_size = fstatResponse.statbuf.st_size;
    DBGTHRD(<< "partition " << __num << " peek size of " << fname << " is " << part_size);
    // --- End Modification: Replace me()->fstat ---


    // We will eventually want to write a record with the durable
    // lsn.  But if this is start-up and we've initialized
    // with a partial partition, we have to prime the
    // buf with the last block in the partition.
    //
    // If this was a pre-existing partition (part_size > 0), we have to scan it
    // to find the *real* end of the file.
    if( part_size > 0 ) {
        // If the file has content, call _peek to scan it.
        // _peek will determine the actual end of valid log and update partition state.
        // It uses the end_hint LSN offset (__num == end_hint.hi() is asserted).
        w_assert3(__num == end_hint.hi() || end_hint.hi() == 0); // Assertions from original code
        _peek(__num, end_hint.lo(), part_size, recovery, fd); // Call helper, assume ported dependencies (_peek, read, _skip)
    } else {
        // If the file is empty (part_size == 0), initialize it with a skip record.
        DBGTHRD(<<" peek INITIALIZING EMPTY PARTITION " << __num << " on fd " << fd); // Updated log message

        // --- Start Modification: Replace me()->ftruncate with rdmaFtruncateFile ---
        // Truncate the file to BLOCK_SIZE (or ensure it's at least BLOCK_SIZE, though empty is 0).
        // This prepares space for the initial skip record.
        RdmaSyscallResponse ftruncateResponse = rdmaFtruncateFile((unsigned int)fd, log_core::BLOCK_SIZE);

        // Check status from the response. < 0 indicates an error.
        if (ftruncateResponse.status < 0) { // Check status for error
             w_rc_t e = RC(smlevel_0::eOS); // Use smlevel_0::eOS for OS error
             std::stringstream err_msg;
             err_msg << "ERROR: rdmaFtruncateFile failed for fd " << fd
                     << " (partition " << __num << ") to size " << log_core::BLOCK_SIZE
                     << ". Status: " << ftruncateResponse.status;
             smlevel_0::errlog->clog << fatal_prio << err_msg.str() << flushl;
            W_COERCE(e); // Handle fatal error
            // Need to close the FD before exiting?
            return; // Exit function
        }
        // --- End Modification: Replace me()->ftruncate ---

        /* write the lsn of the up-coming skip record */

        // Write the initial skip record and flush it to disk.
        // _skip calls partition_t::flush, which handles rdmaWalWrite and isRdmaFlushCompleted.
        _skip(first_lsn(__num), fd); // Call helper, assume ported dependencies (_skip, prime, flush)

        // The original code had a separate fsync here after writing the skip record.
        // The durability is now handled within _skip -> partition_t::flush
        // via the isRdmaFlushCompleted call based on the skip record's LSN.
        // So, the explicit me()->fsync call here is no longer needed.
        // --- Removed: e = me()->fsync(fd); if (e.is_error()) { ... } ---


        // Size is 0 for a newly initialized partition (size() is the size of valid data).
        set_size(0); // Update partition object's size state
    }

    // Decide whether to return the opened file descriptor/handle or close it.
    if (fdp) {
        // If fdp is provided, return the opened handle.
        DBGTHRD(<< "partition " << __num << " SAVED, NOT CLOSED fd " << fd);
        *fdp = fd; // Return the RDMA file handle via the pointer
    } else {
        // If fdp is null, close the opened file handle.
        DBGTHRD(<< " CLOSE fd " << fd << " for partition " << __num); // Log which handle is being closed

        // --- Start Modification: Replace me()->close with rdmaCloseFile ---
        // Call rdmaCloseFile to close the handle on the remote machine.
        RdmaSyscallResponse closeResponse = rdmaCloseFile((unsigned int)fd);

        // Check the status from the response. < 0 indicates an error.
        if (closeResponse.status < 0) { // Check status for error
            w_rc_t e = RC(smlevel_0::eOS); // Use smlevel_0::eOS for OS error
            std::stringstream err_msg;
             err_msg << "ERROR: rdmaCloseFile failed for fd " << fd
                     << " (partition " << __num << ") at end of peek. Status: " << closeResponse.status;
            smlevel_0::errlog->clog << fatal_prio << err_msg.str() << flushl;
            W_COERCE(e); // Handle fatal error
        }
        // --- End Modification: Replace me()->close ---
    }
    // The function is void, implicit return.
}
void
partition_t::flush(int fd)
{
    FUNC(partition::flush); // Standard Shore-MT tracing macro

    // Original code had: INC_TSTAT(log_fsync_cnt);
    // This counter is typically removed as the operation is no longer a local fsync.

    // --- Start Modification: Replace me()->fsync with LSN-based flush completion ---
    // The original function performed fsync(fd). With the LSN-based API,
    // we need to find the partition associated with this fd and wait for its
    // last written LSN to be flushed.

    partition_t* p = nullptr;
    // Find the partition_t object corresponding to the input file descriptor.
    // This requires searching the _partition array within the log_core instance (_owner).
    // Assumes _owner has a way to access its partition array and PARTITION_COUNT.
    // Assumes partition_t has public methods like fhdl_app() and fhdl_rd() to get handles.

    // --- Start Added Logic: Find the partition matching the file descriptor ---
    // Iterate through all partitions managed by the log_core owner.
    for (int i = 0; i < PARTITION_COUNT; ++i) {
        // Access the partition_t object at the current index.
        // We can access _part directly because partition_t is a friend of log_core.
        partition_t* current_p = &_owner->_part[i];

        // Check if this partition object is valid (i.e., currently in use/open).
        // Assuming a partition_t has a way to indicate if it's active, e.g., check its file handles.
        // Assuming fhdl_app() returns the append file descriptor for the partition.
        // Adjust the check if a different handle (like fhdl_rd()) is relevant, or if
        // partition_t has a dedicated 'is_active()' method.
        if (current_p && current_p->fhdl_app() == fd) {
            // Found the partition that matches the input file descriptor.
            p = current_p;
            break; // Exit the loop once the matching partition is found.
        }
         // Optional: Check read handle if necessary, though flush usually applies to writes.
         // if (current_p && current_p->fhdl_rd() == fd) {
         //     p = current_p;
         //     break;
         // }
    }
    // --- End Added Logic ---


    if (!p) {
        // Error: Could not find a partition object associated with the provided file descriptor.
        // This could happen if the fd is invalid or doesn't belong to an active partition.
        w_rc_t e = RC(smlevel_0::eOS); // Use smlevel_0::eOS for OS error
        smlevel_0::errlog->clog << fatal_prio
            << "ERROR: partition_t::flush(int fd) called with unknown or invalid file descriptor: " << fd
            << ". Could not find corresponding partition object." << endl << e << flushl;
        W_FATAL(e.err_num()); // Treat this as a fatal error, as we cannot proceed without a partition.
        return; // Exit function
    }

    // Determine the LSN up to which durability is required for this partition.
    // In an fsync-like operation, this typically means ensuring everything written
    // to the file so far is durable.
    // With the LSN-based API, this should be the LSN of the last data block/record
    // that has been successfully sent for writing for this partition.
    // Assuming partition_t::_flush_lsn (or a similar internal state) holds the LSN
    // up to which data has been submitted for flushing for this partition.
    // This _flush_lsn would be updated in the first flush overload after rdmaWalWrite.
    lsn_t lsn_to_wait_for = _owner->_flush_lsn; // Access _flush_lsn from the owner log_core

    // If _flush_lsn is the initial LSN (e.g., 0.0 for a new file), isRdmaFlushCompleted(0.0)
    // should likely succeed immediately as nothing needs flushing.

    // Call the RDMA flush completion mechanism to wait for the determined LSN to be durable.
    int fsync_result = isRdmaFlushCompleted(reinterpret_cast<lsn_t_c*>(&lsn_to_wait_for)); // Call C wrapper with the determined LSN

    if (fsync_result < 0) { // Check for error return from C wrapper (-1 indicates error)
        w_rc_t e = RC(smlevel_0::eOS); // Use smlevel_0::eOS for OS error
         smlevel_0::errlog->clog << fatal_prio
             << "   isRdmaFlushCompleted failed for LSN " << lsn_to_wait_for << " after flushing partition associated with fd " << fd << "." << flushl; // Log the LSN and fd
         W_COERCE(e); // Handle fatal error
    } else {
         DBGTHRD(<<"isRdmaFlushCompleted successful for LSN " << lsn_to_wait_for << " after flushing partition associated with fd " << fd << "."); // Debug on success
    }
    // --- End Modification: Replace me()->fsync ---

    _set_state(m_flushed);
}

/*
 * Close the append file handle if it is valid.
 * Modified to use rdmaCloseFile.
 */
void
partition_t::close_for_append()
{
    // Get the append file handle.
    int f = fhdl_app();

    // Check if the handle is valid (partition is open for append).
    if (f != invalid_fhdl)  {
        //w_rc_t e; // Original variable for error code

        DBGTHRD(<< " CLOSE append handle: " << f); // Log which handle is being closed

        // --- Start Modification: Replace me()->close with rdmaCloseFile ---
        // Call rdmaCloseFile to close the handle on the remote machine.
        RdmaSyscallResponse closeResponse = rdmaCloseFile((unsigned int)f);

        // Check the status from the response. < 0 indicates an error.
        if (closeResponse.status < 0) { // Check status for error (handle < 0 as error)
             // Log the error at warning level, as in the original code.
            smlevel_0::errlog->clog << warning_prio
                << "warning: rdmaCloseFile failed for append handle " << f
                << " (partition " << num() << "). Status: " << closeResponse.status
                << "." << endl; // No w_rc_t variable needed for this log format
        }
        // --- End Modification: Replace me()->close ---

        // Regardless of close success/failure, mark the handle as invalid
        // to transition the object state to "not open for append".
        _fhdl_app = invalid_fhdl; // Invalidate the internal append handle
        // Note: The original code did NOT clear the m_open_for_append state flag here.
        // It was cleared in partition_t::close(bool both). We follow the original behavior for this function.
    }
    // If handle was invalid, do nothing.
}

/*
 * Close the read file handle if it is valid.
 * Modified to use rdmaCloseFile.
 */
void
partition_t::close_for_read()
{
    // Get the read file handle.
    int f = fhdl_rd();

    // Check if the handle is valid (partition is open for read).
    if (f != invalid_fhdl)  {
        //w_rc_t e; // Original variable for error code

        DBGTHRD(<< " CLOSE read handle: " << f); // Log which handle is being closed

        // --- Start Modification: Replace me()->close with rdmaCloseFile ---
        // Call rdmaCloseFile to close the handle on the remote machine.
        RdmaSyscallResponse closeResponse = rdmaCloseFile((unsigned int)f);

        // Check the status from the response. < 0 indicates an error.
        if (closeResponse.status < 0) { // Check status for error (handle < 0 as error)
            // Log the error at warning level, as in the original code.
            smlevel_0::errlog->clog << warning_prio
                << "warning: rdmaCloseFile failed for read handle " << f
                << " (partition " << num() << "). Status: " << closeResponse.status
                << "." << endl; // No w_rc_t variable needed for this log format
        }
        // --- End Modification: Replace me()->close ---

        // Regardless of close success/failure, mark the handle as invalid
        // to transition the object state to "not open for read".
        _fhdl_rd = invalid_fhdl; // Invalidate the internal read handle
        // Note: The original code did NOT clear the m_open_for_read state flag here.
        // It was cleared in partition_t::close(bool both). We follow the original behavior for this function.
    }
    // If handle was invalid, do nothing.
}