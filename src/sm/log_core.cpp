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

 $Id: log_core.cpp,v 1.4 2010/06/15 17:30:07 nhall Exp $

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
#define LOG_CORE_C
#ifdef __GNUG__
#   pragma implementation
#endif

#ifdef __SUNPRO_CC
#include <stdio.h>
#else
#include <cstdio>        /* XXX for log recovery */
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <os_interface.h>
#include <largefile_aware.h>
#include <fcntl.h>
#include <unistd.h>

#include "sm_int_1.h"
#include "logtype_gen.h"
// DEAD #include "log_buf.h"
#include "log.h"
#include "log_core.h"

// chkpt.h needed to kick checkpoint thread
#include "chkpt.h"

#include <sstream>
#include <w_strstream.h>

// needed for skip_log
#include "logdef_gen.cpp"

#include <map>
#include <math.h>

// RDMA operations
#include "rdma_integration.h"
#include "c_list.h"

bool       log_core::_initialized = false;

// Once the log is created, this points to it. This is the
// implementation of log_m.
log_core *log_core::THE_LOG(NULL); // me


long         
log_core::partition_size(long psize) {
     long p = psize - BLOCK_SIZE;
     return _floor(p, SEGMENT_SIZE) + BLOCK_SIZE; 
}
long         
log_core::min_partition_size() { 
     return _floor(SEGMENT_SIZE, SEGMENT_SIZE) + BLOCK_SIZE; 
}
/*********************************************************************
 *
 *  log_core *log_core::new_log_m(logdir,segid)
 *
 *  CONSTRUCTOR.  Returns one or the other log types.
 *
 *********************************************************************/

w_rc_t
log_core::new_log_m(
    log_m        *&log_p,
    int          wrbufsize,
    bool         reformat
)
{
    rc_t        rc;

    if(THE_LOG != NULL) {
        // not mt-safe
        smlevel_0::errlog->clog << error_prio  << "Log already created. " 
                << endl << flushl;
        return RC(eINTERNAL); // server programming error
    }

    if(rc.is_error()) {
        // not mt-safe, but this is not going to happen in concurrency scenario
        smlevel_0::errlog->clog << error_prio  
                << "Error: cannot open the log file(s) " << dir_name()
                << ":" << endl << rc << flushl;
        return rc;
    }

    /* The log created here is deleted by the ss_m. */
    log_core *l = 0;
    {
        DBGTHRD(<<" log is unix file" );
        if (max_logsz == 0)  {
            // not mt-safe, but this is not going to happen in 
            // concurrency scenario
            smlevel_0::errlog->clog << fatal_prio
                << "Error: log size must be non-zero for log devices"
                << flushl;
            /* XXX should genertae invalid log size of something instead? */
            return RC(eOUTOFLOGSPACE);
        }

        l = new log_core(wrbufsize, reformat);
    }
    if (rc.is_error())
        return rc;    

    log_p = l;
    THE_LOG = l;
    return RCOK;
}

void
log_core::_acquire() 
{
    _partition_lock.acquire(&me()->get_log_me_node());
}
void
log_core::release() 
{
    _partition_lock.release(me()->get_log_me_node());
}


partition_index_t
log_core::_get_index(uint4_t n) const
{
    const partition_t        *p;
    for(int i=0; i<PARTITION_COUNT; i++) {
        p = _partition(i);
        if(p->num()==n) return i;
    }
    return -1;
}

partition_t *
log_core::_n_partition(partition_number_t n) const
{
    partition_index_t i = _get_index(n);
    return (i<0)? (partition_t *)0 : _partition(i);
}


partition_t *
log_core::curr_partition() const
{
    w_assert3(partition_index() >= 0);
    return _partition(partition_index());
}

/*********************************************************************
 *
 *  log_core::scavenge(min_rec_lsn, min_xct_lsn)
 *
 *  Scavenge (free, reclaim) unused log files. 
 *  We can scavenge all log files with index less 
 *  than the minimum of the three lsns: 
 *  the two arguments  
 *  min_rec_lsn,  : minimum recovery lsn computed by checkpoint
 *  min_xct_lsn,  : first log record written by any uncommitted xct
 *  and 
 *  global_min_lsn: the smaller of :
 *     min chkpt rec lsn: min_rec_lsn computed by the last checkpoint
 *     master_lsn: lsn of the last completed checkpoint-begin 
 * (so the min chkpt rec lsn is in here twice - that's ok)
 *
 *********************************************************************/
rc_t
log_core::scavenge(lsn_t min_rec_lsn, lsn_t min_xct_lsn)
{
    FUNC(log_core::scavenge);
    CRITICAL_SECTION(cs, _partition_lock);
    DO_PTHREAD(pthread_mutex_lock(&_scavenge_lock));

#if W_DEBUG_LEVEL > 2
    _sanity_check();
#endif 
    partition_t        *p;

    lsn_t lsn = global_min_lsn(min_rec_lsn,min_xct_lsn);
    partition_number_t min_num;
    {
        /* 
         *  find min_num -- the lowest of all the partitions
         */
        min_num = partition_num();
        for (uint i = 0; i < PARTITION_COUNT; i++)  {
            p = _partition(i);
            if( p->num() > 0 &&  p->num() < min_num )
                min_num = p->num();
        }
    }

    DBGTHRD( << "scavenge until lsn " << lsn << ", min_num is " 
         << min_num << endl );

    /*
     *  recycle all partitions  whose num is less than
     *  lsn.hi().
     */
    int count=0;
    for ( ; min_num < lsn.hi(); ++min_num)  {
        p = _n_partition(min_num); 
        w_assert3(p);
        if (durable_lsn() < p->first_lsn() )  {
            W_FATAL(fcINTERNAL); // why would this ever happen?
            //            set_durable(first_lsn(p->num() + 1));
        }
        w_assert3(durable_lsn() >= p->first_lsn());
        DBGTHRD( << "scavenging log " << p->num() << endl );
        count++;
        p->close(true);
        p->destroy();
    }
    if(count > 0) {
        /* LOG_RESERVATIONS

           reinstate the log space from the reclaimed partitions. We
           can put back the entire partition size because every log
           insert which finishes off a partition will consume whatever
           unused space was left at the end.

           Skim off the top of the released space whatever it takes to
           top up the log checkpoint reservation.
         */
        fileoff_t reclaimed = recoverable_space(count);
        fileoff_t max_chkpt = max_chkpt_size();
        while(!verify_chkpt_reservation() && reclaimed > 0) {
            long skimmed = std::min(max_chkpt, reclaimed);
            atomic_add_64((uint64_t*)&_space_rsvd_for_chkpt, skimmed);
            reclaimed -= skimmed;
        }
        release_space(reclaimed);
        DO_PTHREAD(pthread_cond_signal(&_scavenge_cond));
    }
    DO_PTHREAD(pthread_mutex_unlock(&_scavenge_lock));

    return RCOK;
}


/*********************************************************************
 *
 * log_core::_flushX(start_lsn, end_lsn, start1, end1, start2, end2)
 * @param[in] start_lsn    starting lsn of the data being flushed
 * @param[in] end_lsn      lsn of the first byte AFTER the data being flushed (skip record LSN)
 * @param[in] start1
 * @param[in] end1
 * @param[in] start2
 * @param[in] end2
 *
 * helper for flush_daemon_work - Flushes log buffer segments to disk.
 *
 * Modified to use the RDMA-ported partition_t::flush and handle new partitions.
 *********************************************************************/
void
log_core::_flushX(lsn_t start_lsn, lsn_t end_lsn,
        long start1, long end1, long start2, long end2)
{
    // time to open a new partition? (used to be in log_core::insert,
    // now called by log flush daemon)
    // This will open a new file when the given start_lsn has a
    // different file() portion from the current partition()'s
    // partition number, so the start_lsn is the clue.
    partition_t* p = curr_partition();
    if(start_lsn.file() != p->num()) {
        partition_number_t n = p->num();
        w_assert3(start_lsn.file() == n+1);
        w_assert3(n != 0);

        {
            /* FRJ: before starting into the CS below we have to be
               sure an empty partition waits for us (otherwise we
               deadlock because partition scavenging is protected by
               the _partition_lock as well).
             */
            DO_PTHREAD(pthread_mutex_lock(&_scavenge_lock));
        retry:
            bf->activate_background_flushing();
            smlevel_1::chkpt->wakeup_and_take();
            u_int oldest = log->global_min_lsn().hi();
            if(oldest + PARTITION_COUNT == start_lsn.file()) {
                // If the partition we need to open is still within the range of partitions in use,
                // wait for scavenging to free up space.
                fprintf(stderr, "Can't open partition %d until partition %d is reclaimed\n",
                    start_lsn.file(), oldest);
                DO_PTHREAD(pthread_cond_wait(&_scavenge_cond, &_scavenge_lock)); // Wait for scavenge signal
                goto retry; // Retry checking after waking up
            }
            DO_PTHREAD(pthread_mutex_unlock(&_scavenge_lock));
            
            // grab the lock -- we're about to mess with partitions
            CRITICAL_SECTION(cs, _partition_lock);
            p->close();  
            unset_current();
            DBG(<<" about to open " << n+1 << "for append");
            //                                  end_hint, existing, recovery
            p = _open_partition_for_append(n+1, lsn_t::null, false, false);
        }
        
        // it's a new partition -- size is now 0
        w_assert3(curr_partition()->size()== 0);
        w_assert3(partition_num() != 0);
    }

    // Flush the log buffer segments to the file for this partition.
    // This is the key call to the RDMA-ported partition_t::flush function.
    // Use the partition's append file handle (p->fhdl_app()).
    // Pass the END LSN (end_lsn) as the second parameter, as this is the LSN
    // that partition_t::flush uses for the skip record and isRdmaFlushCompleted.
    // Pass the log buffer base pointer (_buf).
    // Pass the buffer segment offsets (start1, end1, start2, end2).
    p->flush(p->fhdl_app(), end_lsn, _buf, start1, end1, start2, end2); // Call the ported partition_t::flush overload

    // Update the logical size of the partition object.
    long written = (end2 - start2) + (end1 - start1); // Calculate the total data bytes written
    // The new size is the original start_lsn.lo() offset plus the total bytes written.
    p->set_size(start_lsn.lo()+written); // Update partition object's logical size

#if W_DEBUG_LEVEL > 2
    _sanity_check();
#endif
}


// See that the log buffer contains whatever partial log record
// might have been written to the tail of the file fd.
// Used when recovery finds a not-full partition file.
void
log_core::_prime(int fd, fileoff_t start, lsn_t next) 
{
    w_assert1(_durable_lsn == _curr_lsn); // better be startup/recovery!
    long boffset = prime(_buf, fd, start, next);
    _durable_lsn = _flush_lsn = _curr_lsn = next;

    /* FRJ: the new code assumes that the buffer is always aligned
       with some buffer-sized multiple of the partition, so we need to
       return how far into the current segment we are.
     */
    long offset = next.lo() % segsize();
    long base = next.lo() - offset;
    lsn_t start_lsn(next.hi(), base);

    // This should happend only in recovery/startup case.  So let's assert
    // that there is no log daemon running yet. If we ever this this
    // assert, we'd better see why and it means we might have to protect
    // _cur_epoch and _start/_end with a critical section on _insert_lock.
    w_assert1(_flush_daemon_running == false);
    _buf_epoch = _cur_epoch = epoch(start_lsn, base, offset, offset);
    _end = _start = next.lo();

    // move the primed data where it belongs (watch out, it might overlap)
    memmove(_buf+offset-boffset, _buf, boffset);
}

// Prime buf with the partial block ending at 'next'; 
// return the size of that partial block (possibly 0)
//
// We are about to write a record for a certain lsn(next).
// If we haven't been appending to this file (e.g., it's
// startup), we need to make sure the first part of the buffer
// contains the last partial block in the file, so that when
// we append that block to the file, we aren't clobbering the
// tail of the file (partition).
//
// This reads from the given file descriptor, the necessary
// block to cover the lsn.
//
// The start argument (offset from beginning of file (fd) of
// start of partition) is for support on raw devices; for unix
// files, it's always zero, since the beginning of the partition
// is the beginning of the file (fd).
//
// This method is public to allow calling from partition_t, which
// uses this to prime its own buffer for writing a skip record.
// It is called from the private _prime to prime the segment-sized
// log buffer _buf.
long
log_core::prime(char* buf, int fd, fileoff_t start, lsn_t next)
{
    FUNC(log_core::prime);

    // This assertion is likely still valid even with RDMA, assuming
    // partitions are treated as starting at offset 0 within the file.
    w_assert1(start == 0); // unless we are on a raw device, which is
    // no longer supported for the log.

    // Calculate the starting offset of the block containing 'next'.
    fileoff_t b = _floor(next.lo(), BLOCK_SIZE);
    // get the first lsn in the block to which "next" belongs.
    lsn_t first = lsn_t(uint4_t(next.hi()), sm_diskaddr_t(b));

    // If the "next" lsn is not at the beginning of a block,
    // we need to read the block it is contained in.
    if(first != next) {
        w_assert3(first.lo() < next.lo());
        // The offset in the file where this block starts.
        // Since start is asserted to be 0, this is just b.
        fileoff_t read_offset = start + first.lo(); // Effectively just b

        DBG(<<" reading " << int(BLOCK_SIZE) << " bytes on fd " << fd << " at offset " << read_offset );

        // --- Start Modification: Replace me()->pread with rdmaWalRead ---
        // The original code was:
        // int n = 0; // n was initialized to 0, used in error message
        // w_rc_t e = me()->pread(fd, buf, BLOCK_SIZE, offset); // uses offset, should be read_offset
        // if (e.is_error()) { ... W_FATAL_MSG(e.err_num(), << "pread() returns " << n) ... }

        ssize_t bytes_read = rdmaWalRead((unsigned int)fd, read_offset, BLOCK_SIZE, buf); // Use rdmaWalRead

        // Check the return value of rdmaWalRead (ssize_t)
        // We expect to read a full BLOCK_SIZE here.
        if (bytes_read != (ssize_t)BLOCK_SIZE) {
            // Read failed (-1) or was a short read (0 <= bytes_read < BLOCK_SIZE).
            w_rc_t e = RC(eOS); // Use generic OS error code

            // Craft a more informative error message
            std::stringstream err_msg;
            err_msg << "ERROR: rdmaWalRead failed or short read in log_core::prime."
                    << " File Descriptor: " << fd
                    << ", Offset: " << read_offset
                    << ", Requested Size: " << BLOCK_SIZE
                    << ", Bytes Read: " << bytes_read;

            if (bytes_read < 0) { // Explicit error return from rdmaWalRead
                // errno should be set by rdmaWalRead on -1 return
                err_msg << ". System Error: " << strerror(errno);
                // Use the error number from errno if available, or a generic one.
                // Original code used e.err_num(), let's try to map errno if possible, or use eOS.
                e = RC_AUGMENT(e); // Augment the error code with errno
            } else { // Short read (0 <= bytes_read < BLOCK_SIZE)
                // Short reads for full blocks are typically unexpected and indicate a problem.
                e = RC_AUGMENT(e); // Treat as I/O error
            }

            smlevel_0::errlog->clog << fatal_prio << err_msg.str() << endl << e << flushl;
            W_FATAL(e.err_num()); // Use the augmented error number
        }
        // --- End Modification: Replace me()->pread ---

    } else { // first == next (the next lsn is at the beginning of a block)
        // No read is necessary, the buffer should already be empty or zeroed.
        // Ensure the buffer is zeroed in this case for consistency, although
        // the memorymove below might handle it if boffset is 0.
        // Let's explicitly zero just the block if no read happens.
        // This prevents stale data from previous reads or uninitialized buffer.
        memset(buf, 0, BLOCK_SIZE);
    }

    // The size of the partial block ending at 'next'.
    // This is the number of bytes from the start of the block (offset b)
    // up to the offset of 'next' within the file.
    return next.lo() - first.lo();
}

void 
log_core::_sanity_check() const
{
    if(!_initialized) return;

#if W_DEBUG_LEVEL > 1
    partition_index_t   i;
    const partition_t*  p;
    bool                found_current=false;
    bool                found_min_lsn=false;

    // we should not be calling this when
    // we're in any intermediate state, i.e.,
    // while there's no current index
    
    if( _curr_index >= 0 ) {
        w_assert1(_curr_num > 0);
    } else {
        // initial state: _curr_num == 1
        w_assert1(_curr_num == 1);
    }
    w_assert1(durable_lsn() <= curr_lsn());
    w_assert1(durable_lsn() >= first_lsn(1));

    for(i=0; i<PARTITION_COUNT; i++) {
        p = _partition(i);
        p->sanity_check();

        w_assert1(i ==  p->index());

        // at most one open for append at any time
        if(p->num()>0) {
            w_assert1(p->exists());
            w_assert1(i ==  _get_index(p->num()));
            w_assert1(p ==  _n_partition(p->num()));

            if(p->is_current()) {
                w_assert1(!found_current);
                found_current = true;

                w_assert1(p ==  curr_partition());
                w_assert1(p->num() ==  partition_num());
                w_assert1(i ==  partition_index());

                w_assert1(p->is_open_for_append());
            } else if(p->is_open_for_append()) {
                // FRJ: not always true with concurrent inserts
                //w_assert1(p->flushed());
            }

            // look for global_min_lsn 
            if(global_min_lsn().hi() == p->num()) {
                //w_assert1(!found_min_lsn);
                // don't die in case global_min_lsn() is null lsn
                found_min_lsn = true;
            }
        } else {
            w_assert1(!p->is_current());
            w_assert1(!p->exists());
        }
    }
    w_assert1(found_min_lsn || (global_min_lsn()== lsn_t::null));
#endif 
}

/*********************************************************************
 *
 *  log_core::fetch(lsn, rec, nxt)
 * 
 *  used in rollback and log_i
 *
 *  Fetch a record at lsn, and return it in rec. Optionally, return
 *  the lsn of the next record in nxt.  The lsn parameter also returns
 *  the lsn of the log record actually fetched.  This is necessary
 *  since it is possible while scanning to specify an lsn
 *  that points to the end of a log file and therefore is actually
 *  the first log record in the next file.
 *
 * NOTE: caller must call release() 
 *********************************************************************/
rc_t
log_core::fetch(lsn_t& ll, logrec_t*& rp, lsn_t* nxt)
{
    FUNC(log_core::fetch);

    DBGTHRD(<<"fetching lsn " << ll 
        << " , _curr_lsn = " << curr_lsn()
        << " , _durable_lsn = " << durable_lsn());

#if W_DEBUG_LEVEL > 0
    _sanity_check();
#endif 

    // it's not sufficient to flush to ll, since
    // ll is at the *beginning* of what we want
    // to read...
    W_DO(flush(ll+sizeof(logrec_t)));

    // protect against double-acquire
    _acquire(); // caller must release the _partition_lock mutex

    /*
     *  Find and open the partition
     */

    partition_t        *p = 0;
    uint4_t        last_hi=0;
    while (!p) {
        if(last_hi == ll.hi()) {
            // can happen on the 2nd or subsequent round
            // but not first
            DBGTHRD(<<"no such partition " << ll  );
            return RC(eEOF);
        }
        if (ll >= curr_lsn())  {
            /*
             *  This would constitute a
             *  read beyond the end of the log
             */
            DBGTHRD(<<"fetch at lsn " << ll  << " returns eof -- _curr_lsn=" 
                    << curr_lsn());
            return RC(eEOF);
        }
        last_hi = ll.hi();

        DBG(<<" about to open " << ll.hi());
        //                                 part#, end_hint, existing, recovery
        if ((p = _open_partition_for_read(ll.hi(), lsn_t::null, true, false))) {

            // opened one... is it the right one?
            DBGTHRD(<<"opened... p->size()=" << p->size());

            if ( ll.lo() >= p->size() ||
                (p->size() == partition_t::nosize && ll.lo() >= limit()))  {
                DBGTHRD(<<"seeking to " << ll.lo() << ";  beyond p->size() ... OR ...");
                DBGTHRD(<<"limit()=" << limit() << " & p->size()==" 
                        << int(partition_t::nosize));

                ll = first_lsn(ll.hi() + 1);
                DBGTHRD(<<"getting next partition: " << ll);
                p = 0; continue;
            }
        }
    }

    W_COERCE(p->read(rp, ll));
    {
        logrec_t        &r = *rp;

        if (r.type() == logrec_t::t_skip && r.get_lsn_ck() == ll) {

            DBGTHRD(<<"seeked to skip" << ll );
            DBGTHRD(<<"getting next partition.");
            ll = first_lsn(ll.hi() + 1);
            // FRJ: BUG? Why are we so certain this partition is even
            // open, let alone open for read?
            p = _n_partition(ll.hi());
            if(!p)
                p = _open_partition_for_read(ll.hi(), lsn_t::null, false, false);

            // re-read
            
            W_COERCE(p->read(rp, ll));
        } 
    }
    logrec_t        &r = *rp;

    if (r.lsn_ck().hi() != ll.hi()) {
        W_FATAL_MSG(fcINTERNAL,
            << "Fatal error: log record " << ll 
            << " is corrupt in lsn_ck().hi() " 
            << r.get_lsn_ck()
            << endl);
    } else if (r.lsn_ck().lo() != ll.lo()) {
        W_FATAL_MSG(fcINTERNAL,
            << "Fatal error: log record " << ll 
            << "is corrupt in lsn_ck().lo()" 
            << r.get_lsn_ck()
            << endl);
    }

    if (nxt) {
        lsn_t tmp = ll;
        *nxt = tmp.advance(r.length());
    }

#ifdef UNDEF
    int saved = r._checksum;
    r._checksum = 0;
    for (int c = 0, i = 0; i < r.length(); c += ((char*)&r)[i++]);
    w_assert1(c == saved);
#endif

    DBGTHRD(<<"fetch at lsn " << ll  << " returns " << r);
#if W_DEBUG_LEVEL > 2
    _sanity_check();
#endif 

    // caller must release the _partition_lock mutex
    return RCOK;
}

/*********************************************************************
 * 
 *  log_core::close_min(n)
 *
 *  Close the partition with the smallest index(num) or an unused
 *  partition, and 
 *  return a ptr to the partition
 *
 *  The argument n is the partition number for which we are going
 *  to use the free partition.
 *
 *********************************************************************/
// MUTEX: partition
partition_t        *
log_core::_close_min(partition_number_t n)
{
    // kick the cleaner thread(s)
    bf->activate_background_flushing();
    
    FUNC(log_core::close_min);
    
    /*
     *  If a free partition exists, return it.
     */

    /*
     * first try the slot that is n % PARTITION_COUNT
     * That one should be free.
     */
    int tries=0;
 again:
    partition_index_t    i =  (int)((n-1) % PARTITION_COUNT);
    partition_number_t   min = min_chkpt_rec_lsn().hi();
    partition_t         *victim;

    victim = _partition(i);
    if((victim->num() == 0)  ||
        (victim->num() < min)) {
        // found one -- doesn't matter if it's the "lowest"
        // but it should be
    } else {
        victim = 0;
    }

    if (victim)  {
        w_assert3( victim->index() == (partition_index_t)((n-1) % PARTITION_COUNT));
    }
    /*
     *  victim is the chosen victim partition.
     */
    if(!victim) {
        /*
         * uh-oh, no space left. Kick the page cleaners, wait a bit, and 
         * try again. Do this no more than 8 times.
         *
         */
        {
            w_ostrstream msg;
            msg << error_prio 
            << "Thread " << me()->id << " "
            << "Out of log space  (" 
            << space_left()
            << "); No empty partitions."
            << endl;
            fprintf(stderr, "%s\n", msg.c_str());
        }
        
        if(tries++ > 8) W_FATAL(smlevel_0::eOUTOFLOGSPACE);
        bf->activate_background_flushing();
        me()->sleep(1000);
        goto again;
    }
    w_assert1(victim);
    // num could be 0

    /*
     *  Close it.
     */
    if(victim->exists()) {
        /*
         * Cannot close it if we need it for recovery.
         */
        if(victim->num() >= min_chkpt_rec_lsn().hi()) {
            w_ostrstream msg;
            msg << " Cannot close min partition -- still in use!" << endl;
            // not mt-safe
            smlevel_0::errlog->clog << error_prio  << msg.c_str() << flushl;
        }
        w_assert1(victim->num() < min_chkpt_rec_lsn().hi());

        victim->close(true);
        victim->destroy();

    } else {
        w_assert3(! victim->is_open_for_append());
        w_assert3(! victim->is_open_for_read());
    }
    w_assert1(! victim->is_current() );
    
    victim->clear();

    return victim;
}

/*********************************************************************
 * 
 *  log_core::_open_partition_for_append() calls _open_partition with
 *                            forappend=true)
 *  log_core::_open_partition_for_read() calls _open_partition with
 *                            forappend=false)
 *
 *  log_core::_open_partition(num, end_hint, existing, 
 *                           forappend, during_recovery)
 *
 *  This partition structure is free and usable.
 *  Open it as partition num. 
 *
 *  if existing==true, the partition "num" had better already exist,
 *  else it had better not already exist.
 * 
 *  if forappend==true, making this the new current partition.
 *    and open it for appending was well as for reading
 *
 *  if during_recovery==true, make sure the entire partition is 
 *   checked and its size is recorded accurately.
 *
 *  end_hint is used iff during_recovery is true.
 *
 *********************************************************************/

// MUTEX: partition
partition_t        *
log_core::_open_partition(partition_number_t  __num, 
        const lsn_t&  end_hint,
        bool existing, 
        bool forappend, 
        bool during_recovery
)
{
    w_assert3(__num > 0);

#if W_DEBUG_LEVEL > 2
    // sanity checks for arguments:
    {
        // bool case1 = (existing  && forappend && during_recovery);
        bool case2 = (existing  && forappend && !during_recovery);
        // bool case3 = (existing  && !forappend && during_recovery);
        // bool case4 = (existing  && !forappend && !during_recovery);
        // bool case5 = (!existing  && forappend && during_recovery);
        // bool case6 = (!existing  && forappend && !during_recovery);
        bool case7 = (!existing  && !forappend && during_recovery);
        bool case8 = (!existing  && !forappend && !during_recovery);

        w_assert3( ! case2);
        w_assert3( ! case7);
        w_assert3( ! case8);
    }

#endif 

    // see if one's already opened with the given __num
    partition_t *p = _n_partition(__num);

#if W_DEBUG_LEVEL > 2
    if(forappend) {
        w_assert3(partition_index() == -1);
        // there should now be *no open partition*
        partition_t *c;
        int i;
        for (i = 0; i < PARTITION_COUNT; i++)  {
            c = _partition(i);
            w_assert3(! c->is_current());
        }
    }
#endif 

    if(!p) {
        /*
         * find an empty partition to use
         */
        DBG(<<"find a new partition structure  to use " );
        p = _close_min(__num);
        w_assert1(p);
        p->peek(__num, end_hint, during_recovery);
    }



    if(existing && !forappend) {
        DBG(<<"about to open for read");
        w_rc_t err = p->open_for_read(__num);
        if(err.is_error()) {
            // Try callback to recover this file
            if(smlevel_0::log_archived_callback) {
                static char buf[max_devname];
                make_log_name(__num, buf, max_devname);
                err = (*smlevel_0::log_archived_callback)( 
                        buf,
                        __num
                        );
                if(!err.is_error()) {
                    // Try again, just once.
                    err = p->open_for_read(__num);
                }
            }
        }
        if(err.is_error()) {
            fprintf(stderr, 
                    "Could not open partition %d for reading.\n",
                    __num);
            W_FATAL(eINTERNAL);
        }


        w_assert3(p->is_open_for_read());
        w_assert3(p->num() == __num);
        w_assert3(p->exists());
    }


    if(forappend) {
        /*
         *  This becomes the current partition.
         */
        p->open_for_append(__num, end_hint);
        if(during_recovery) {
          // We will eventually want to write a record with the durable
          // lsn.  But if this is start-up and we've initialized
          // with a partial partition, we have to prime the
          // buf with the last block in the partition.
          w_assert1(durable_lsn() == curr_lsn());
          _prime(p->fhdl_app(), p->start(), durable_lsn());
        }
        w_assert3(p->exists());
        w_assert3(p->is_open_for_append());

        // The idea here is to checkpoint at the beginning of every
        // new partition because it seems we aren't taking enough
        // checkpoints; then we were making the user threads do an emergency
        // checkpoint to scavenge log space.  Short-tx workloads should never
        // encounter this.    Don't do this if shutting down or starting
        // up because in those 2 cases, the chkpt_m might not exist yet/anymore
        if(smlevel_1::chkpt != NULL) smlevel_1::chkpt->wakeup_and_take();
    }
    return p;
}

void
log_core::unset_current()
{
    _curr_index = -1;
    _curr_num = 0;
}

void
log_core::set_current(
        partition_index_t i, 
        partition_number_t num
)
{
    w_assert3(_curr_index == -1);
    w_assert3(_curr_num  == 0 || _curr_num == 1);
    _curr_index = i;
    _curr_num = num;
}


class flush_daemon_thread_t : public smthread_t {
    log_core* _log;
public:
    flush_daemon_thread_t(log_core* log) : 
         smthread_t(t_regular, "flush_daemon", WAIT_NOT_USED), _log(log) { }

    virtual void run() { _log->flush_daemon(); }
};

// Does not get called until after the 
// log is fully constructed:
void log_core::start_flush_daemon() 
{
    _flush_daemon_running = true;
    _flush_daemon->fork();
}

void
log_core::shutdown() 
{ 
    // gnats 52:  RACE: We set _shutting_down and signal between the time
    // the daemon checks _shutting_down (false) and waits.
    //
    // So we need to notice if the daemon got the message.
    // It tells us it did by resetting the flag after noticing
    // that the flag is true.
    // There should be no interference with these two settings
    // of the flag since they happen strictly in that sequence.
    //
    _shutting_down = true;
    while (*&_shutting_down) {
        CRITICAL_SECTION(cs, _wait_flush_lock);
        // Use signal since the only thread that should be waiting 
        // on the _flush_cond is the log flush daemon.
        DO_PTHREAD(pthread_cond_broadcast(&_flush_cond));
    }
    _flush_daemon->join();
    _flush_daemon_running = false;
    delete _flush_daemon;
    _flush_daemon=NULL;
}

// used to access the _waiting and _dummy nodes together
struct hacked_qnode 
{
    mcs_lock::qnode* _next;
    uint64_t _state;
};

static union {
    mcs_lock::qnode q;
    hacked_qnode hq;
} const WAITING = {{0,1,0}}, ABORT_ME = {{0,1,1}};


enum { SLOT_ARRAY_SIZE=256 };
enum { SLOT_ACTIVE_COUNT=5 };
enum { SLOT_AVAILABLE=0,
       SLOT_UNUSED=-1,
       SLOT_PENDING=-2,
       SLOT_FINISHED=-4
};
struct log_core::insert_info {
    lsn_t lsn;		// where will we end up on disk?
    long old_end;	// end point of our predecessor
    long start_pos;	// start point for thread groups
    long pos;		// how much of the allocation already claimed?
    long new_end;	// eventually assigned to _cur_epoch 
    long new_base;	// positive if we started a new partition

    // these are used by the combination array
    long count;
    long error;
    mcs_lock::qnode me;
    union {
	mcs_lock::qnode me2;
	hacked_qnode me2h;
    };
    mcs_lock::qnode* pred2;

    insert_info volatile* vthis() { return this; }

    insert_info() : count(SLOT_UNUSED), error(0) { }
};

struct log_core::insert_info_array {
    long _total_slots;
    long _slot_mark;
    insert_info* _slot_array;
    
    insert_info_array(long count=16)
	: _total_slots(count)
	, _slot_mark(0)
	, _slot_array(new insert_info[count])
    {
    }
    
    ~insert_info_array() 
    {
	// make sure all slots are free before continuing...
	for(long i=0; i < _total_slots; i++)
        {
	    allocate(false);
        }
	delete [] _slot_array;
    }

    long indexof(insert_info const* info) const {
	return info - _slot_array;
    }

    insert_info* allocate(bool be_patient=true) 
    {
        long orig_slot_mark = _slot_mark;
	while(SLOT_UNUSED != _slot_array[_slot_mark].count) 
        {
	    if(++_slot_mark == _total_slots)
            {
		_slot_mark = 0;
            }
            w_assert0(be_patient || _slot_mark != orig_slot_mark);
	}
	insert_info* i2 = &_slot_array[_slot_mark];
	i2->count = SLOT_AVAILABLE;
	return i2;
    }
};

pthread_mutex_t global_histo_lock = PTHREAD_MUTEX_INITIALIZER;

struct histo {
    typedef std::map<int, long> bucket_map;
    bucket_map _buckets;

    histo &operator+=(histo const &other) {
	for(bucket_map::const_iterator it=other._buckets.begin(); it != other._buckets.end(); ++it) {
	    _buckets[it->first] += it->second;
	}
	return *this;
    }
    long &operator[](long idx) {
	// log base 2 indexing ...
	return _buckets[ilogb(double(idx))];
    }
    
    void print() const {
	fprintf(stderr, "Log working set histogram (log-2 buckets):\n");
	for(bucket_map::const_iterator it=_buckets.begin(); it != _buckets.end(); ++it) {
	    fprintf(stderr, "\t%d: %ld\n", it->first, it->second);
	}
    }
    
    static histo global_histo;
    
    ~histo() {
	pthread_mutex_lock(&global_histo_lock);
	global_histo += *this;
	pthread_mutex_unlock(&global_histo_lock);
    }
};

histo histo::global_histo;

DECLARE_TLS(log_core::insert_info_array, tls_info_array);
DECLARE_TLS(histo, tls_histo);


/*********************************************************************
 *
 *  log_core::log_core(bufsize, reformat)
 *
 *  Hidden constructor. 
 *  Create log flush daemon thread.
 *
 *  Open and scan logdir for master lsn and last log file. 
 *  Truncate last incomplete log record (if there is any)
 *  from the last log file.
 *
 *********************************************************************/

// Make sure to allocate enough extra space that log wraps can always fit
NORET
log_core::log_core(
    long bsize,
    bool reformat) 

    : 
      _reservations_active(false), 
      _waiting_for_space(false), 
      _waiting_for_flush(false),
      _start(0), 
      _end(0),
      _needs_flushed(0),
      _segsize(_ceil(bsize, SEGMENT_SIZE)), 
      // _blocksize(BLOCK_SIZE),
      _buf(new char[_segsize]),
      _shutting_down(false),
      _flush_daemon_running(false),
      _slot_array(new insert_info_array(SLOT_ARRAY_SIZE)),
      _active_slots(SLOT_ACTIVE_COUNT),
      _slots(new insert_info* volatile[SLOT_ACTIVE_COUNT]),
      _curr_index(-1),
      _curr_num(1),
      _readbuf(new char[BLOCK_SIZE*4]),
      _skip_log(new skip_log)
{
    FUNC(log_core::log_core);
    DO_PTHREAD(pthread_mutex_init(&_wait_flush_lock, NULL));
    DO_PTHREAD(pthread_cond_init(&_wait_cond, NULL));
    DO_PTHREAD(pthread_cond_init(&_flush_cond, NULL));
    DO_PTHREAD(pthread_mutex_init(&_scavenge_lock, NULL));
    DO_PTHREAD(pthread_cond_init(&_scavenge_cond, NULL));
    
    for(int i=0; i < _active_slots; i++) 
	_allocate_slot(i);
    
    /* Create thread o flush the log */
    _flush_daemon = new flush_daemon_thread_t(this);

    if (bsize < 64 * 1024) {
        // not mt-safe, but this is not going to happen in 
        // concurrency scenario
        errlog->clog << error_prio 
        << "Log buf size (sm_logbufsize) too small: "
        << bsize << ", require at least " << 64 * 1024 
        << endl; 
        errlog->clog << error_prio << endl;
        fprintf(stderr,
            "Log buf size (sm_logbufsize) too small: %ld, need %d\n",
            bsize, 64*1024);
        W_FATAL(OPT_BadValue);
    }

    w_assert1(is_aligned(_readbuf));

    // By the time we get here, the max_logsize has already been
    // adjusted by the sm options-handling code, so it should be
    // a legitimate value now.
    _set_size(max_logsz);


    // FRJ: we don't actually *need* this (no trx around yet), but we
    // don't want to trip the assertions that watch for it.
    CRITICAL_SECTION(cs, _partition_lock);


    if(!rdmaContextCreate()) {
      //error
      smlevel_0::errlog->clog << fatal_prio
      	    << "Error: Could not create RDMA context" <<flushl;
        W_FATAL(eINTERNAL);
    }

    rdmaInitMessage("Test message");

	bool error = false;
    c_list_handle list = rdmaGetDirContentsList(&error);

    if (error) {
        w_rc_t e = RC(eOS);
        smlevel_0::errlog->clog << fatal_prio
      	    << "Error: Could not retrieve files from the log directory " << dir_name() <<flushl;
        smlevel_0::errlog->clog << fatal_prio
            << "\tNote: the log directory is specified using\n"
            "\t      the sm_logdir option." << flushl;
        W_COERCE(e);
    }

    partition_number_t  last_partition = partition_num();
    bool                last_partition_exists = false;
    /*
     * make sure there's room for the log names
     */
    fileoff_t eof= fileoff_t(0);

//    os_dirent_t *dd=0;
//    os_dir_t ldir = os_opendir(dir_name());
    c_list_iterator_handle iter = c_list_begin(list);
    c_list_iterator_handle iterEnd = c_list_end(list);
//    if (! ldir)
//    {
//        w_rc_t e = RC(eOS);
//        smlevel_0::errlog->clog << fatal_prio
//            << "Error: could not open the log directory " << dir_name() <<flushl;
//        smlevel_0::errlog->clog << fatal_prio
//            << "\tNote: the log directory is specified using\n"
//            "\t      the sm_logdir option." << flushl;
//        W_COERCE(e);
//    }
    DBGTHRD(<<"opendir " << dir_name() << " succeeded");

    /*
     *  scan directory for master lsn and last log file 
     */

    _master_lsn = null_lsn;

    uint4_t min_index = max_uint4;

    char *fname = new char [smlevel_0::max_devname];
    if (!fname)
        W_FATAL(fcOUTOFMEMORY);
    w_auto_delete_array_t<char> ad_fname(fname);

    /* Create a list of lsns for the partitions - this
     * will be used to store any hints about the last
     * lsns of the partitions (stored with checkpoint meta-info
     */ 
    lsn_t lsnlist[PARTITION_COUNT];
    int   listlength=0;
    {
        /*
         *  initialize partition table
         */
        partition_index_t i;
        for (i = 0; i < PARTITION_COUNT; i++)  {
            _part[i].init_index(i);
            _part[i].init(this);
        }
    }

    DBGTHRD(<<"reformat= " << reformat 
            << " last_partition "  << last_partition
            << " last_partition_exists "  << last_partition_exists
            );
    if (reformat) 
    {
        smlevel_0::errlog->clog << emerg_prio 
            << "Reformatting logs..." << endl;

        while (!c_list_iterator_is_equal(iter, iterEnd)) // ((dd = os_readdir(ldir)))
        {
            RdmaDirContents contents;
            if (c_list_iterator_get_data(iter, &contents) != 0) {
            	smlevel_0::errlog->clog << fatal_prio
              		<< "Error: Reformatting log failed - failed to retrieve log file name from list" << endl;
            	W_FATAL(eINTERNAL);
            }
            DBGTHRD(<<"master_prefix= " << master_prefix());

            unsigned int namelen = strlen(log_prefix());
            namelen = namelen > strlen(master_prefix())? namelen :
                                        strlen(master_prefix());

//            const char *d = dd->d_name;
            const char *d = contents.name;
            unsigned int orig_namelen = strlen(d);
            namelen = namelen > orig_namelen ? namelen : orig_namelen;

            char *name = new char [namelen+1];
            w_auto_delete_array_t<char>  cleanup(name);

            memset(name, '\0', namelen+1);
            strncpy(name, d, namelen);
            name[namelen] = '\0';
            DBGTHRD(<<"name= " << name);

            bool parse_ok = (strncmp(name,master_prefix(),strlen(master_prefix()))==0);
            if(!parse_ok) {
                parse_ok = (strncmp(name,log_prefix(),strlen(log_prefix()))==0);
            }
            if(parse_ok) {
                smlevel_0::errlog->clog << debug_prio 
                    << "\t" << name << "..." << endl;

                {
                    w_ostrstream s(fname, (int) smlevel_0::max_devname);
                    s << dir_name() << _SLASH << name << ends;
                    w_assert1(s);
                    RdmaSyscallResponse response = rdmaUnlinkFile(fname, NORMAL);
//                    if( unlink(fname) < 0) {
                    if (response.status != 0) {
                        w_rc_t e = RC(fcOS);
                        smlevel_0::errlog->clog << debug_prio 
                            << "unlink(" << fname << "):"
                            << endl << e << endl;
                    }

                    // delete this file from the list
                    iter = c_list_erase(list, iter);
                }
            }
        } 

        //  os_closedir(ldir);
        w_assert3(!last_partition_exists);
    }

    DBGTHRD(<<"about to readdir"
            << " last_partition "  << last_partition
            << " last_partition_exists "  << last_partition_exists
            );

    c_list_destroy_iterator(iter);
    iter = c_list_begin(list);

//    while ((dd = os_readdir(ldir)))
    while (!c_list_iterator_is_equal(iter, iterEnd))
    {
      	RdmaDirContents contents;
        if (c_list_iterator_get_data(iter, &contents) != 0) {
            smlevel_0::errlog->clog << fatal_prio
            	<< "Error: Failed to read file name from list" << endl;
            W_FATAL(eINTERNAL);;
        }
//        DBGTHRD(<<"dd->d_name=" << dd->d_name);
		DBGTHRD(<<"contents.name=" << contents.name);

        // XXX should abort on name too long earlier, or size buffer to fit
        const unsigned int prefix_len = strlen(master_prefix());
        w_assert3(prefix_len < smlevel_0::max_devname);

        char *buf = new char[smlevel_0::max_devname+1];
        if (!buf)
                W_FATAL(fcOUTOFMEMORY);
        w_auto_delete_array_t<char>  ad_buf(buf);

        unsigned int         namelen = prefix_len;
        const char *         dn = contents.name; // dd->d_name;
        unsigned int         orig_namelen = strlen(dn);

        namelen = namelen > orig_namelen ? namelen : orig_namelen;
        char *                name = new char [namelen+1];
        w_auto_delete_array_t<char>  cleanup(name);

        memset(name, '\0', namelen+1);
        strncpy(name, dn, namelen);
        name[namelen] = '\0';

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
        strncpy(buf, name, prefix_len);
        buf[prefix_len] = '\0';
#pragma GCC diagnostic pop

        DBGTHRD(<<"name= " << name);

        bool parse_ok = ((strlen(buf)) == prefix_len);

        DBGTHRD(<<"parse_ok  = " << parse_ok
                << " buf = " << buf
                << " prefix_len = " << prefix_len
                << " strlen(buf) = " << strlen(buf));
        if (parse_ok) {
            lsn_t tmp;
            if (strcmp(buf, master_prefix()) == 0)  
            {
                DBGTHRD(<<"found log file " << buf);
                /*
                 *  File name matches master prefix.
                 *  Extract master lsn & lsns of skip-records
                 */
                lsn_t tmp1;
                bool old_style=false;
                rc_t rc = _read_master(name, prefix_len,
                        tmp, tmp1, lsnlist, listlength,
                        old_style);
                W_COERCE(rc);

                if (tmp < master_lsn())  {
                    /* 
                     *  Swap tmp <-> _master_lsn, tmp1 <-> _min_chkpt_rec_lsn
                     */
                    std::swap(_master_lsn, tmp);
                    std::swap(_min_chkpt_rec_lsn, tmp1);
                }
                /*
                 *  Remove the older master record.
                 */
                if (_master_lsn != lsn_t::null) {
                    _make_master_name(_master_lsn,
                                      _min_chkpt_rec_lsn,
                                      fname,
                                      smlevel_0::max_devname);

                    RdmaSyscallResponse response = rdmaUnlinkFile(fname, NORMAL);
                    if (response.status != 0) {
                        smlevel_0::errlog->clog << fatal_prio
                        	<< "unlink(" << fname << "): failed" << endl;
                        W_FATAL(eINTERNAL);
                    }
                    // (void) unlink(fname);
                }
                /*
                 *  Save the new master record
                 */
                _master_lsn = tmp;
                _min_chkpt_rec_lsn = tmp1;
                DBG(<<" _master_lsn=" << _master_lsn
                 <<" _min_chkpt_rec_lsn=" << _min_chkpt_rec_lsn);

                DBG(<<"parse_ok = " << parse_ok);

            } else if (strcmp(buf, log_prefix()) == 0)  {
                DBGTHRD(<<"found log file " << buf);
                /*
                 *  File name matches log prefix
                 */

                w_istrstream s(name + prefix_len);
                uint4_t curr;
                if (! (s >> curr))  {
                    smlevel_0::errlog->clog << fatal_prio 
                    << "bad log file \"" << name << "\"" << flushl;
                    W_FATAL(eINTERNAL);
                }

                DBGTHRD(<<"curr " << curr
                        << " partition_num()==" << partition_num() 
                        << " last_partition_exists " << last_partition_exists
                        );

                if (curr >= last_partition) {
                    last_partition = curr;
                    last_partition_exists = true;
                    DBGTHRD(<<"new last_partition " << curr
                        << " exits=true" );
                }
                if (curr < min_index) {
                    min_index = curr;
                }
            } else {
                DBGTHRD(<<"NO MATCH");
                DBGTHRD(<<"_master_prefix= " << master_prefix());
                DBGTHRD(<<"_log_prefix= " << log_prefix());
                DBGTHRD(<<"buf= " << buf);
                parse_ok = false;
            }
        } 

        /*
         *  if we couldn't parse the file name and it was not "." or ..
         *  then print an error message
         */
        if (!parse_ok && ! (strcmp(name, ".") == 0 || 
                                strcmp(name, "..") == 0)) {
            smlevel_0::errlog->clog << fatal_prio
                << "log_core: cannot parse " << name << flushl;
            W_FATAL(fcINTERNAL);
        }
        iter = c_list_erase(list, iter); // always remove the first element of the list (clean up list by the end)
    }

    c_list_destroy_iterator(iter);
    c_list_destroy_iterator(iterEnd);
    c_list_destroy(list);

//    os_closedir(ldir); (not needed, already handled in the server)

    DBGTHRD(<<"after closedir  " 
            << " last_partition "  << last_partition
            << " last_partition_exists "  << last_partition_exists
            );

#if W_DEBUG_LEVEL > 2
    if(reformat) {
        w_assert3(partition_num() == 1);
        w_assert3(_min_chkpt_rec_lsn.hi() == 1);
        w_assert3(_min_chkpt_rec_lsn.lo() == first_lsn(1).lo());
    } else {
       // ??
    }
    w_assert3(partition_index() == -1);
#endif 

    DBGTHRD(<<"Last partition is " << last_partition
        << " existing = " << last_partition_exists
     );

    /*
     *  Destroy all partitions less than _min_chkpt_rec_lsn
     *  Open the rest and close them.
     *  There might not be an existing last_partition,
     *  regardless of the value of "reformat"
     */
    {
        partition_number_t n;
        partition_t        *p;

        DBGTHRD(<<" min_chkpt_rec_lsn " << min_chkpt_rec_lsn() 
                << " last_partition " << last_partition);
        w_assert3(min_chkpt_rec_lsn().hi() <= last_partition);

        for (n = min_index; n < min_chkpt_rec_lsn().hi(); n++)  {
            // not an error if we can't unlink (probably doesn't exist)
            DBGTHRD(<<" destroy_file " << n << "false"); 
            destroy_file(n, false);
        }
        for (n = _min_chkpt_rec_lsn.hi(); n <= last_partition; n++)  {
            // Find out if there's a hint about the length of the 
            // partition (from the checkpoint).  This lsn serves as a
            // starting point from which to search for the skip_log record
            // in the file.  It's a performance thing...
            lsn_t lasthint;
            for(int q=0; q<listlength; q++) {
                if(lsnlist[q].hi() == n) {
                    lasthint = lsnlist[q];
                }
            }

            // open and check each file (get its size)
            DBGTHRD(<<" open " << n << "true, false, true"); 

            // last argument indicates "in_recovery" more accurately,
            // we should say "at-startup"
            p = _open_partition_for_read(n, lasthint, true, true);
            w_assert3(p == _n_partition(n));
            p->close();
            unset_current();
            DBGTHRD(<<" done w/ open " << n );
        }
    }

    /* XXXX :  Don't have a static method on 
     * partition_t for start() 
    */
    /* end of the last valid log record / start of invalid record */
    fileoff_t pos = 0;

    { // Truncate at last complete log rec
    DBGTHRD(<<" truncate last complete log rec "); 

    /*
     *
        The goal of this code is to determine where is the last complete
        log record in the log file and truncate the file at the
        end of that record.  It detects this by scanning the file and
        either reaching eof or else detecting an incomplete record.
        If it finds an incomplete record then the end of the preceding
        record is where it will truncate the file.

        The file is scanned by attempting to fread the length of a log
        record header.        If this fread does not read enough bytes, then
        we've reached an incomplete log record.  If it does read enough,
        then the buffer should contain a valid log record header and
        it is checked to determine the complete length of the record.
        Fseek is then called to advance to the end of the record.
        If the fseek fails then it indicates an incomplete record.

     *  NB:
        This is done here rather than in peek() since in the unix-file
        case, we only check the *last* partition opened, not each
        one read.
     *
     */
    make_log_name(last_partition, fname, smlevel_0::max_devname);
    DBGTHRD(<<" checking " << fname);

    // --- Start Modification ---
    // Use file descriptor (int) instead of FILE*
//    int fd = open(fname, O_RDONLY); // Open for reading
    RdmaSyscallResponse openResponse = rdmaOpenFile(fname, O_RDONLY, (S_IRWXU | S_IRWXG | S_IRWXO)); // mode is ignored
    int fd = (int) openResponse.status;
    if (fd < 0) {
      	// error
        smlevel_0::errlog->clog << fatal_prio
        	<< "Failed to open file " << fname << flushl;
        W_FATAL(eINTERNAL);
    }

    DBGTHRD(<<" opened " << fname << " fd " << fd << " pos " << pos);

    fileoff_t start_pos = pos;

#ifndef SM_LOG_UNIX_NO_SKIP_SEEK
    /* If the master checkpoint is in the current partition, seek
       to its position immediately, instead of scanning from the
       beginning of the log.   If the current partition doesn't have
       a checkpoint, must read entire paritition until the skip
       record is found. */

    const lsn_t &seek_lsn = _master_lsn;

    if (fd != -1 && seek_lsn.hi() == last_partition) { // Check fd != -1 instead of f
        start_pos = seek_lsn.lo();

        DBG(<<" starting position from checkpoint: " << start_pos);
//        // lseek/fseek not required because the read following this is stateless
//        RdmaSyscallResponse lseekResponse = rdmaLseekFile(fd, start_pos, SEEK_SET);
//        if (lseekResponse.status == -1 || lseekResponse.offset == (off_t)-1) {
//            smlevel_0::errlog->clog  << error_prio
//                << "log read: can't lseek to " << start_pos
//                 << " starting log scan at origin"
//                 << endl;
//            start_pos = pos; // Reset start_pos if seek fails
//        }
//        else
//            pos = start_pos; // Update pos after successful seek
    }

    pos = start_pos;
#endif
    // 'pos' now holds the offset for the upcoming read (either the checkpoint LSN offset or the partition start)
    DBG(<<" pos is now " << pos);

    if (fd != -1) { // Check fd != -1 instead of f
        allocaN<logrec_t::hdr_sz> buf;
        char* output_buf = static_cast<char*>(static_cast<void*>(buf));

        DBGTHRD(<<"read " << fname << " sz= " << logrec_t::hdr_sz);
        ssize_t n = rdmaWalRead(fd, pos, logrec_t::hdr_sz, static_cast<char*>(output_buf));

        // Loop condition checks if read was successful and reads a full header
        while (n == (ssize_t) logrec_t::hdr_sz)
        {
            // store current offset
            fileoff_t current_header_start_pos = pos;

            // advance offset till end of read (for next read)
            pos += n;

            DBG(<<" pos is now " << pos);
            logrec_t  *l = (logrec_t*) (void*) buf;

            if (l->type() == logrec_t::t_skip) {
    			DBGTHRD(<<"Found skip log record at offset " << current_header_start_pos); // pos is after header, n is hdr_sz
    			// We found a skip record. The valid log ends *before* this record.
    			// 'pos' is currently after the header. Set pos back to the start of the skip record for truncation.
   				pos -= n; // Revert pos back to the start of the skip header (pos was pos + n)
    			break; // Exit the WHILE loop to stop scanning
			}

			// --- Get and Check Record Length ---
			smsize_t len = l->length(); // Total length of the record
			DBGTHRD(<<"scanned log rec type=" << int(l->type()) << " length=" << len);


			// --- Handle Corrupt Header Length (Skip Behavior) vs. Valid Header Length ---
			// If length is less than header size, it's corruption. If >=, it's a valid length.
			if (len < logrec_t::hdr_sz) {
    			// Found a header claiming a length smaller than itself - indicates corruption.
    			// We treat this as garbage and attempt to skip over it
    			// by looking for the next header immediately after this corrupt header block.
    			smlevel_0::errlog->clog << error_prio
       				<< "Found log rec header with length < hdr_sz at offset "
                    << current_header_start_pos << ". Treating as garbage and attempting re-sync by skipping." << flushl;

    			// 'pos' is currently positioned immediately *after* the corrupt header block ((pos-n) + n).
    			// To implement the skip-over, the *next* read (at the end of the loop) should start from this position.
    			// We do NOT adjust 'pos' within this 'if' block. 'pos' is already correct for the skip.

    			// Execution will continue after this 'if' block.
    			// We skip the 'else' block below that processes a valid record.

			} else { // len >= logrec_t::hdr_sz (Valid header length)
    			// --- Logic for processing a VALID record ---
    			// This is the block that replaces the commented-out original 'else' logic.
    			// Original code asserted len >= logrec_t::hdr_sz here. It's guaranteed by the 'else'.
   				w_assert1(len >= logrec_t::hdr_sz);

   				DBGTHRD(<<"hdr_sz " << logrec_t::hdr_sz );
    			DBGTHRD(<<"len " << len );

    			// Calculate the absolute offset for lsn_ck.
    			// Original code used a relative seek: lseek(fd, seek_len, SEEK_CUR)
    			// where seek_len = len - (logrec_t::hdr_sz + sizeof(lsn_t))
   				// This relative seek was from the position *after* reading the header (pos - n + n = pos).
    			// The absolute offset for lsn_ck is (pos - n) + len - sizeof(lsn_t)
    			fileoff_t lsn_ck_offset = current_header_start_pos + len - sizeof(lsn_t);

    			DBGTHRD(<<"Calculated lsn_ck_offset: " << lsn_ck_offset);

    			// Declare lsn_ck buffer
    			lsn_t lsn_ck;

    			// Read the lsn_ck explicitly using rdmaWalRead at its calculated offset
    			ssize_t n_lsn_ck = rdmaWalRead(fd, lsn_ck_offset, sizeof(lsn_ck), reinterpret_cast<char*>(&lsn_ck));
    			DBGTHRD(<<"rdmaWalRead for lsn_ck return #bytes=" << n_lsn_ck ); // Debug after the read

    			// Check if lsn_ck read failed or was short (EOF/error)
    			if (n_lsn_ck != (ssize_t)sizeof(lsn_ck)) { // Check against sizeof(lsn_ck)
       				DBGTHRD(<<"Failed to read lsn_ck at offset " << lsn_ck_offset << ". Read " << n_lsn_ck << " bytes.");
        			w_rc_t e = RC(eOS); // Original code used eOS
        			if (n_lsn_ck < 0) { // Check for read error
             			smlevel_0::errlog->clog << fatal_prio
             				<< "ERROR: rdmaWalRead failed fetching lsn_ck at offset " << lsn_ck_offset << ". Return value: " << n_lsn_ck << "." << flushl;
             			W_FATAL(eINTERNAL); // Or handle error
        			} else { // n_lsn_ck == 0 -> EOF, or n_lsn_ck > 0 but short
             			// Reached EOF while trying to read lsn_ck, or got a partial read.
             			// Treat as end of valid log.
             			smlevel_0::errlog->clog << error_prio
             				<< "Reached EOF or got short read fetching lsn_ck at offset " << lsn_ck_offset
                            << ". Read " << n_lsn_ck << " bytes. Treating as end of valid log." << flushl;
        			}
        			// The valid log ends *before* this record. The truncation position is the start of this record's header.
        			pos = current_header_start_pos; // Set 'pos' back to the start of the current record's header
        			break; // Exit the WHILE loop
    			}
    			DBGTHRD(<<"lsn_ck = " << lsn_ck << " found at offset " << lsn_ck_offset); // Debug after successful read


    			// make sure log record's lsn matched its position in file
    			// The LSN stored in lsn_ck should match the offset where the *header* started (current_header_start_pos).
    			if ( (lsn_ck.lo() != current_header_start_pos) || // Check against the header's starting position
        			(lsn_ck.hi() != (uint4_t) last_partition ) ) // Assuming last_partition is correctly uint4_t
    			{
        			// LSN check failed - treat as incomplete record at the end of the log
        			smlevel_0::errlog->clog << error_prio
                        << "Found unexpected end of log -- probably due to a previous crash. LSN check failed at offset "
                        << current_header_start_pos << "."
            			<< " Expected LSN offset: " << current_header_start_pos << " Found LSN: " << lsn_ck << flushl;

        			// The valid end of the log is at the start of this record where the check failed.
        			pos = current_header_start_pos; // Set 'pos' back to the start of the header
        			break; // Exit the WHILE loop
   				}

    			// If LSN check passes, the current record is valid.
    			// Advance 'pos' to the start of the *next* log record header.
    			// This next header starts 'len' bytes after the current header's start.
    			pos = current_header_start_pos + len; // Set 'pos' to the offset for the next header read

			} // else (len >= hdr_sz)
            n = rdmaWalRead(fd, pos, logrec_t::hdr_sz, output_buf);
        } // while ((n = read(...)) > 0)

        if (n < 0) { // Check if the loop exited due to a read error
             w_rc_t e = RC(eOS);
             smlevel_0::errlog->clog << fatal_prio
             << "ERROR: read failed during log file scan." << flushl;
             W_FATAL(eINTERNAL); // Or handle error
        }


        rdmaCloseFile(fd); // Close the read file descriptor
        // --- End Modification ---


        {
            DBGTHRD(<<"explicit truncating " << fname << " to " << pos);
            // Use unlink or os_truncate
            RdmaSyscallResponse truncResponse = rdmaTruncateFile(fname, pos);
             if (truncResponse.status < 0) {
                  w_rc_t e = RC(eOS);
                  smlevel_0::errlog->clog  << fatal_prio
                      << "os_truncate(" << fname << ", " << pos << "):" << endl << e << endl;
                   W_COERCE(e);
             }

            //
            // but we can't just use truncate() --
            // we have to truncate to a size that's a mpl
            // of the page size. First append a skip record
            DBGTHRD(<<"explicit opening  " << fname );

            // --- Start Modification ---
            // Use file descriptor for append
            RdmaSyscallResponse openForAppendResponse = rdmaOpenFile(fname, O_WRONLY | O_APPEND, (S_IRWXU | S_IRWXG | S_IRWXO));
//            int fd_append = open(fname, O_WRONLY | O_APPEND); // Open in append mode
            int fd_append = (int) openForAppendResponse.status;
            if (fd_append == -1) { // Check fd_append != -1 instead of !f
                w_rc_t e = RC(fcOS);
                smlevel_0::errlog->clog  << fatal_prio
                    << "open(" << fname << ", O_WRONLY | O_APPEND):" << endl << e << endl;
                W_COERCE(e);
            }
            // --- End Modification ---

            skip_log *s = new skip_log; // deleted below
            s->set_lsn_ck( lsn_t(uint4_t(last_partition), sm_diskaddr_t(eof)) );

            DBGTHRD(<<"writing skip_log at offset " << eof << " with lsn "
                << s->get_lsn_ck() 
                << "and size " << s->length()
                );
#ifdef W_TRACE
			{
             	// Use rdmaLseekFile(SEEK_CUR) to get current position from the server.
             	// In append mode, this should return the current end of file (eof) after truncation.
            	RdmaSyscallResponse eofResponse = rdmaLseekFile(fd_append, 0, SEEK_CUR);
            	if (eofResponse.status >= 0) { // Check status for success
                 	fileoff_t current_pos_debug = eofResponse.offset;
                	DBGTHRD(<<"Position after open in append mode is " << current_pos_debug); // Should be equal to eof
            	} else {
                 	// Log error if rdmaLseekFile fails (probably not fatal for debug)
                	w_rc_t e = RC(eOS); // Use eOS
            	    smlevel_0::errlog->clog << error_prio << "rdmaLseekFile(SEEK_CUR) failed for debug after append open: " << endl << e << endl;
            	    // W_COERCE(e); // Decide if this should be fatal
            	}
        	}
#endif
            lsn_t skip_lsn_val = s->get_lsn_ck(); // Get the LSN value
        	lsn_t* skip_lsn_ptr = &skip_lsn_val; // Pass pointer to LSN value
            size_t skip_size = s->length();

            ssize_t write_skip_result = rdmaWalWrite(reinterpret_cast<const char*>(s), fd_append, reinterpret_cast<lsn_t_c*>(skip_lsn_ptr), eof, skip_size, true, true);

            // --- Start Modification ---
            // Check the return value. Full success is writing exactly skip_size bytes.
        	// Failure is -1 or potentially a short write (< skip_size, >= 0).
        	if (write_skip_result != (ssize_t)skip_size) {
            	w_rc_t e = RC(eOS); // Original code used eOS
            	if (write_skip_result < 0) { // Check for explicit error return
                 	smlevel_0::errlog->clog << fatal_prio <<
                    	"   rdmaWalWrite failed writing skip rec to log at offset " << eof << ". Return value: " << write_skip_result << "..." << flushl; // Added return value
                 	W_COERCE(e); // Handle fatal error
                 	// Consider returning error code or throwing exception
             	} else { // Short write occurred (0 <= result < skip_size)
                 	smlevel_0::errlog->clog << fatal_prio <<
                    	"   rdmaWalWrite short write for skip rec at offset " << eof << ". Wrote " << write_skip_result << " of " << skip_size << " bytes." << flushl; // Added details
                 	W_COERCE(e); // Treat short write as fatal error
             	}
        	} else {
             	DBGTHRD(<<"rdmaWalWrite successful for skip rec. Wrote " << write_skip_result << " bytes."); // Debug on success
        	}
            // --- End Modification ---

#ifdef W_TRACE
            {
             	// Use rdmaLseekFile(SEEK_CUR) to get current position after writing skip record.
             	// In append mode, this should return eof + s->length().
            	RdmaSyscallResponse eofResponse = rdmaLseekFile(fd_append, 0, SEEK_CUR);
             	if (eofResponse.status >= 0) { // Check status for success
                    fileoff_t current_pos_debug = eofResponse.offset;
                	DBGTHRD(<<"Position after skip write is " << current_pos_debug); // Should be eof + skip_size
            	} else {
                 	// Log error if rdmaLseekFile fails (probably not fatal for debug)
                 	w_rc_t e = RC(eOS); // Use eOS
                 	smlevel_0::errlog->clog << error_prio << "rdmaLseekFile(SEEK_CUR) failed for debug after skip write: " << endl << e << endl;
                 	// W_COERCE(e); // Decide if this should be fatal
            	}
            }
#endif
            // Calculate remaining padding needed for block alignment
            // Manually track position after skip write
            fileoff_t current_end_pos_after_skip = eof + skip_size;
            fileoff_t o = current_end_pos_after_skip % BLOCK_SIZE;
            DBGTHRD(<<"BLOCK_SIZE " << int(BLOCK_SIZE));
            if(o > 0) {
                o = BLOCK_SIZE - o;
                char *junk = new char[int(o)]; // delete[] at close scope
                if (!junk)
                    W_FATAL(fcOUTOFMEMORY); // Handle out of memory
#if ZERO_INIT
                fprintf(stderr, "Clearing before write %d %s\n", __LINE__
                        , __FILE__);
                memset(junk,'\0', int(o));
#endif
                
                DBGTHRD(<<"writing junk of length " << o << " at offset " << current_end_pos_after_skip);
#ifdef W_TRACE
            	{
                	// Use rdmaLseekFile(SEEK_CUR) to get current position before writing padding.
                	// In append mode, this should return current_end_pos_after_skip.
                	RdmaSyscallResponse eofResponse = rdmaLseekFile(fd_append, 0, SEEK_CUR);
                	if (eofResponse.status >= 0) { // Check status for success
                    	fileoff_t current_pos_debug = eofResponse.offset;
                    	DBGTHRD(<<"Position before junk write is " << current_pos_debug); // Should be current_end_pos_after_skip
                	} else {
                    	// Log error if rdmaLseekFile fails (probably not fatal for debug)
                    	w_rc_t e = RC(eOS); // Use eOS
                    	smlevel_0::errlog->clog << error_prio << "rdmaLseekFile(SEEK_CUR) failed for debug before junk write: " << endl << e << endl;
                    	// W_COERCE(e); // Decide if this should be fatal
                	}
            	}
#endif
            	// --- Replace local write with rdmaWalWrite for padding ---
            	// The write should start at current_end_pos_after_skip.
            	// For padding, lsn is probably null, start/end false?
            	size_t padding_size = (size_t)o; // Size of the padding
            	ssize_t write_junk_result = rdmaWalWrite(junk, fd_append, nullptr, current_end_pos_after_skip, padding_size, false, false); // Use offset, assuming nullptr, false, false

            	// Check the return value. Full success is writing exactly padding_size bytes.
            	if (write_junk_result != (ssize_t)padding_size) {
                	w_rc_t e = RC(eOS); // Original code used eOS
                 	if (write_junk_result < 0) { // Check for explicit error return
                    	smlevel_0::errlog->clog << fatal_prio <<
                    	"   rdmaWalWrite failed writing junk to log at offset " << current_end_pos_after_skip
                                << ". Return value: " << write_junk_result << "..." << flushl; // Added return value
                    	W_COERCE(e); // Handle fatal error
                    	// Consider returning error code or throwing exception
                	} else { // Short write occurred (0 <= result < padding_size)
                    	smlevel_0::errlog->clog << fatal_prio <<
                    	"   rdmaWalWrite short write for junk at offset " << current_end_pos_after_skip << ". Wrote "
                                << write_junk_result << " of " << padding_size << " bytes." << flushl; // Added details
                    	W_COERCE(e); // Treat short write as fatal error
                	}
            	} else {
                 	DBGTHRD(<<"rdmaWalWrite successful for junk. Wrote " << write_junk_result << " bytes."); // Debug on success
            	}

            	delete[] junk;
            	// o = 0; // Original line - likely just resetting padding length variable. Can remove.
        	}
        	delete s; // skip_log

        	// --- Check final position (equivalent to original ftell check) ---
        	// The final position should be block aligned. Get the actual final position from the server.
        	// Update the 'eof' variable that is used later in the constructor.
        	RdmaSyscallResponse finalEofResponse = rdmaLseekFile(fd_append, 0, SEEK_CUR);
        	if (finalEofResponse.status >= 0) { // Check status for success
            	eof = finalEofResponse.offset; // Update the 'eof' variable
             	DBGTHRD(<<"Final file size (eof) is now " << eof); // Updated debug message

            	if(((eof) % BLOCK_SIZE) != 0) {
                	w_rc_t e = RC(eOS); // Original code used eOS
                	smlevel_0::errlog->clog << fatal_prio <<
                    	"   rdmaLseekFile/rdmaWalWrite: final size not block aligned (size: " << eof << ")..." << flushl; // Updated message
                	W_COERCE(e); // Handle fatal error
                 	// Consider returning error code or throwing exception
            	}
             	// W_IGNORE(e); // Original ignored error, but we handled it above. Can remove.

        	} else {
             	// Log fatal error if rdmaLseekFile fails getting final position
             	w_rc_t e = RC(eOS); // Use eOS
             	smlevel_0::errlog->clog << fatal_prio << "rdmaLseekFile(SEEK_CUR) failed getting final pos: " << endl << e << endl;
             	W_COERCE(e); // Handle fatal error
             	// Consider returning error code or throwing exception
        	}

        	// --- Replace os_fsync with isRdmaFlushCompleted ---
        	// The original os_fsync ensures all data written to fd_append is durable.
        	// With the RDMA layer, the server signals flush completion via LSNs.
        	// We need to wait for the LSN corresponding to the end of the data we just wrote (the skip record and padding)
        	// to be flushed. The LSN of the skip record is set to the truncation point 'eof'.
        	// Assuming waiting for this LSN (which is 'eof') to be flushed is sufficient to guarantee
        	// the durability of the skip record and padding that were appended starting at 'eof'.

        	// Reconstruct the lsn_t associated with the end of the appended data (the skip record's LSN)
        	lsn_t skip_record_lsn_for_flush(uint4_t(last_partition), sm_diskaddr_t(eof)); // Use the final 'eof'

        	// Call the C-style wrapper function to check flush completion for this LSN
        	// isRdmaFlushCompleted takes lsn_t_c*, pass a reinterpret_cast pointer
        	int fsync_result = isRdmaFlushCompleted(reinterpret_cast<lsn_t_c*>(&skip_record_lsn_for_flush));

        	if (fsync_result < 0) { // Check for error return from isRdmaFlushCompleted (-1 indicates error)
            	w_rc_t e = RC(eOS); // Original code used eOS
            	smlevel_0::errlog->clog << fatal_prio <<
                	"   isRdmaFlushCompleted failed for LSN " << skip_record_lsn_for_flush << "..." << flushl; // Log the LSN
            	W_COERCE(e); // Handle fatal error
            	// Consider returning error code or throwing exception
        	} else {
             	DBGTHRD(<<"isRdmaFlushCompleted successful for LSN " << skip_record_lsn_for_flush); // Debug on success
        	}

#if W_DEBUG_LEVEL > 2
        	{
            	// Replace os_fstat with rdmaFstatCall
            	// os_fstat is likely a wrapper that takes int fd, already used fileno
            	w_rc_t e_stat; // Use a different variable name for the stat error
            	RdmaSyscallResponse statResponse = rdmaFstatCall(fd_append);
            	e_stat = MAKERC(statResponse.status == -1, eOS); // MAKERC check status field

            	if (e_stat.is_error()) {
                	smlevel_0::errlog->clog << fatal_prio
                    	    << " Cannot rdmaFstatCall fd " << fd_append
                        	<< ":" << endl << e_stat << endl << flushl;
                	W_COERCE(e_stat); // Handle fatal error
                 	// Consider returning error code or throwing exception
            	} else {
                	// Access the statbuf from the response union
                	DBGTHRD(<< "size of " << fname << " is " << statResponse.statbuf.st_size << " (from rdmaFstatCall)"); // Updated debug
            	}
        	}
#endif
        	// --- Replace local close with rdmaCloseFile ---
        	// close(fd_append); // Original close call
        	RdmaSyscallResponse closeResponse = rdmaCloseFile(fd_append);
        	if (closeResponse.status < 0) {
             	w_rc_t e = RC(eOS); // Use eOS
             	smlevel_0::errlog->clog << error_prio << "rdmaCloseFile failed for fd " << fd_append << endl << e << endl;
             	W_COERCE(e); // Decide if this should be fatal - likely not fatal but worth logging
        	} else {
             	DBGTHRD(<<"rdmaCloseFile successful for fd " << fd_append); // Debug on success
        	}

        } // if (f) -> now if (fd != -1)

    } else { // if (fd == -1) - File could not be opened for reading
         w_rc_t e = RC(eOS);
         smlevel_0::errlog->clog << fatal_prio
             << "ERROR: could not open log file \"" << fname << "\" for reading." << flushl;
         W_COERCE(e); // Or handle appropriately if file might legitimately not exist (though original code suggests it should exist if last_partition_exists)
    }
    } // End truncate at last complete log rec

    /*
     *  initialize current and durable lsn for
     *  the purpose of sanity checks in open*()
     *  and elsewhere
     */
    DBGTHRD( << "partition num = " << partition_num()
        <<" current_lsn " << curr_lsn()
        <<" durable_lsn " << durable_lsn());

    // The 'pos' variable at this point holds the file offset where the
    // valid log ends (start of the truncated record).
    lsn_t new_lsn(last_partition, eof);
    _curr_lsn = _durable_lsn = _flush_lsn = new_lsn;

    DBGTHRD( << "partition num = " << partition_num()
            <<" current_lsn " << curr_lsn()
            <<" durable_lsn " << durable_lsn());

    {
        /*
         *  create/open the "current" partition
         *  "current" could be new or existing
         *  Check its size and all the records in it
         *  by passing "true" for the last argument to open()
         */

        // Find out if there's a hint about the length of the 
        // partition (from the checkpoint).  This lsn serves as a
        // starting point from which to search for the skip_log record
        // in the file.  It's a performance thing...
        lsn_t lasthint;
        for(int q=0; q<listlength; q++) {
            if(lsnlist[q].hi() == last_partition) {
                lasthint = lsnlist[q];
            }
        }
        // _open_partition_for_append likely uses 'open' and returns a partition_t*
        // containing the file descriptor(s)
        partition_t *p = _open_partition_for_append(last_partition, lasthint,
                last_partition_exists, true);

        /* XXX error info lost */
        if(!p) {
            smlevel_0::errlog->clog << fatal_prio
            << "ERROR: could not open log file for partition "
            << last_partition << flushl;
            W_FATAL(eINTERNAL);
        }

        w_assert3(p->num() == last_partition);
        w_assert3(partition_num() == last_partition);
        w_assert3(partition_index() == p->index());

    }
    DBGTHRD( << "partition num = " << partition_num()
            <<" current_lsn " << curr_lsn()
            <<" durable_lsn " << durable_lsn());

    cs.exit();
    if(0){
        // Print various interesting info to the log:
        errlog->clog << info_prio 
            << "Log max_partition_size " << max_partition_size() << endl
            << "Log max_partition_size * PARTITION_COUNT " 
                    << max_partition_size() * PARTITION_COUNT << endl
            << "Log min_partition_size " << min_partition_size() << endl
            << "Log min_partition_size*PARTITION_COUNT " 
                    << min_partition_size() * PARTITION_COUNT << endl;

        errlog->clog << info_prio 
            << "Log BLOCK_SIZE (log write size) " << BLOCK_SIZE
            << endl
            << "Log segsize() (log buffer size) " << segsize()
            << endl
            << "Log segsize()/BLOCK_SIZE " << double(segsize())/double(BLOCK_SIZE)
            << endl;

        errlog->clog << info_prio 
            << "User-option smlevel_0::max_logsz " << max_logsz << endl
            << "Log _partition_data_size " << _partition_data_size 
            << endl
            << "Log _partition_data_size/segsize() " 
                << double(_partition_data_size)/double(segsize())
            << endl
            << "Log _partition_data_size/segsize()+BLOCK_SIZE " 
                << _partition_data_size + BLOCK_SIZE
            << endl;

        errlog->clog << info_prio 
            << "Log _start " << start_byte() << " end_byte() " << end_byte()
            << endl
            << "Log _curr_lsn " << _curr_lsn 
            << " _durable_lsn " << _durable_lsn
            << endl; 
        errlog->clog << info_prio 
            << "Curr epoch  base_lsn " << _cur_epoch.base_lsn
            << endl
            << "Curr epoch  base " << _cur_epoch.base
            << endl
            << "Curr epoch  start " << _cur_epoch.start
            << endl
            << "Curr epoch  end " << _cur_epoch.end
            << endl;
        errlog->clog << info_prio 
            << "Old epoch  base_lsn " << _old_epoch.base_lsn
            << endl
            << "Old epoch  base " << _old_epoch.base
            << endl
            << "Old epoch  start " << _old_epoch.start
            << endl
            << "Old epoch  end " << _old_epoch.end
            << endl;
    }
}



/* WARNING WARNING WARNING

   NEVER CHANGE THESE WHILE LOG INSERTS MIGHT BE IN PROGRESS!
 */
bool use_decoupled_memcpy = true;
bool enable_fastpath = false;
bool enable_mcs_expose = true;
bool use_combination_array = true;
bool use_expose_groups = true;
bool print_lsn_groups = false;
bool print_expose_groups = false;
bool print_working_set = false;

struct feature_set {
    char const* str;
    bool operator[](char c) const {
	return strchr(str, toupper(c)) || strchr(str, tolower(c));
    }
};
rc_t
log_m::set_log_features(char const* features) {
    feature_set fs = {features};
    if(fs['f'] && !fs['c'])
	return RC(eBADARGUMENT);
    if(fs['m'] && !fs['d'])
	return RC(eBADARGUMENT);
    if(fs['e'] && !fs['m'])
	return RC(eBADARGUMENT);
    if(fs['x'] && !fs['e'])
	return RC(eBADARGUMENT);
    if(fs['l'] && !fs['c'])
	return RC(eBADARGUMENT);
    enable_fastpath = fs['f'];
    use_combination_array = fs['c'];
    use_decoupled_memcpy = fs['d'];
    enable_mcs_expose = fs['m'];
    use_expose_groups = fs['e'];
    print_expose_groups = fs['x'];
    print_lsn_groups = fs['l'];
    print_working_set = fs['w'];
    return RCOK;
}

// caller responsible to delete the return value!
char const*
log_m::get_log_features() {
    char const features[] = {
	use_combination_array? 'c' : '-',
	enable_fastpath? 'f' : '-',
	print_lsn_groups? 'l' : '-',
	use_decoupled_memcpy? 'd' : '-',
	enable_mcs_expose? 'm' : '-',
	use_expose_groups? 'e' : '-',
	print_expose_groups? 'x' : '-',
	print_working_set? 'w' : '-',
	0
    };
    char* rval = new char[sizeof(features)];
    strcpy(rval, features);
    return rval;
}



log_core::~log_core() 
{
    if(THE_LOG != NULL)
    {
        partition_t        *p;
        for (uint i = 0; i < PARTITION_COUNT; i++) {
            p = _partition(i);
            p->close_for_read();
            p->close_for_append();
            DBG(<< " calling clear");
            p->clear();
        }
        delete [] _readbuf;
        delete _skip_log;
        w_assert1(_durable_lsn == _curr_lsn);
        delete [] _buf;

        DO_PTHREAD(pthread_mutex_destroy(&_wait_flush_lock));
        DO_PTHREAD(pthread_cond_destroy(&_wait_cond));
        DO_PTHREAD(pthread_cond_destroy(&_flush_cond));
        THE_LOG = NULL;
	for(int i=0; i < _active_slots; i++) {
	    long old_count = atomic_swap_ulong((unsigned long*) &_slots[i]->count, SLOT_UNUSED);
	    if(old_count != SLOT_AVAILABLE && old_count != SLOT_UNUSED) {
		fprintf(stderr, "old_count = %ld", old_count);
		w_assert1(old_count == SLOT_AVAILABLE || old_count == SLOT_UNUSED);
	    }
	}
	delete [] _slots;
	delete _slot_array;
    }
}

partition_t *
log_core::_partition(partition_index_t i) const
{
    return i<0 ? (partition_t *)0: (partition_t *) &_part[i];
}


void
log_core::destroy_file(partition_number_t n, bool pmsg)
{
    // Allocate buffer for filename and generate the name.
    char        *fname = new char[smlevel_0::max_devname];
    if (!fname)
        W_FATAL(fcOUTOFMEMORY); // Handle allocation failure
    w_auto_delete_array_t<char> ad_fname(fname); // Auto cleanup for filename buffer
    make_log_name(n, fname, smlevel_0::max_devname); // Generate filename

    // --- Start Modification: Replace unlink with rdmaUnlinkFile ---
    // The original code was: if (unlink(fname) == -1) { ... }
    // Use rdmaUnlinkFile to delete the file on the remote machine.
    // Need to determine the correct OpType for deleting a log partition file.
    // Assuming a constant like OP_TYPE_LOG_PARTITION_DELETE is defined.
    // If OpType is not relevant or a simpler overload exists, adjust the call.
    OpType opType = NORMAL;
    RdmaSyscallResponse unlinkResponse = rdmaUnlinkFile(fname, opType);

    // Check the status from the response. < 0 indicates an error.
    if (unlinkResponse.status < 0) { // Check status for error
    // --- End Modification: Replace unlink with rdmaUnlinkFile ---

        w_rc_t e = RC(smlevel_0::eOS); // Use smlevel_0::eOS for OS error

        // Original code logged the error using error_prio. Adapt log message.
        smlevel_0::errlog->clog  << error_prio
            << "ERROR: rdmaUnlinkFile failed for partition " << n << " (" << fname << "):"
            << " Status: " << unlinkResponse.status << "." << endl;

        // Original code printed a separate warning message if pmsg is true.
        if(pmsg) {
            smlevel_0::errlog->clog << error_prio
            << "warning : cannot free log file \""
            << fname << '\"' << flushl;
            // Original code printed 'e' here. You can print the w_rc_t.
            smlevel_0::errlog->clog << error_prio
            << "          " << e << flushl;
        }
        // Original code did NOT make this function fatal.
        // It just logged the error/warning and returned (void function).
        // The caller (partition_t::destroy) handles potential implications.
    }
    // If unlinkResponse.status >= 0, unlink was successful. Do nothing else.
}

/**\brief compute size of partition from given max-open-log-bytes size
 * \details
 * PARTITION_COUNT == smlevel_0::max_openlog is fixed.
 * SEGMENT_SIZE  is fixed.
 * BLOCK_SIZE  is fixed.
 * Only the partition size is determinable by the user; it's the
 * size of a partition file and PARTITION_COUNT*partition-size is
 * therefore the maximum amount of log space openable at one time.
 */
void log_core::_set_size(fileoff_t size) 
{
    /* The log consists of at most PARTITION_COUNT open files, 
     * each with space for some integer number of segments (log buffers) 
     * plus one extra block for writing skip records.
     *
     * Each segment is an integer number of blocks (BLOCK_SIZE), which
     * is the size of an I/O.  An I/O is padded, if necessary, to BLOCK_SIZE.
     */
    fileoff_t usable_psize = size/PARTITION_COUNT - BLOCK_SIZE;

    // partition must hold at least one buffer...
    if(usable_psize < _segsize)
	W_FATAL(eOUTOFLOGSPACE);

    // largest integral multiple of segsize() not greater than usable_psize:
    _partition_data_size = _floor(usable_psize, (segsize()));

    if(_partition_data_size == 0) 
    {
        // casts make it work for LP32
        fprintf(stderr, 
"log size is too small: size %ld usable_psize %ld, segsize() %lld, blocksize 0x%x\n",
                (long)size, (long) usable_psize, (long long) segsize(), BLOCK_SIZE);
        // casts make it work for LP32
        fprintf(stderr, "need at least %ld (%ld * 1024 = %lld) \n", 
                (long) _get_min_size(), 
                (long) _get_min_size()/1024,
                (long long) 1024 *(_get_min_size()/1024)
                );
        W_FATAL(eOUTOFLOGSPACE);
    }
    _partition_size = _partition_data_size + BLOCK_SIZE;
    /*
    fprintf(stderr, 
"size %ld usable_psize %ld segsize() %ld _part_data_size %ld _part_size %ld\n",
            size,
            usable_psize,
            segsize(),
            _partition_data_size,
            _partition_size
           );
    */
    // initial free space estimate... refined once log recovery is complete 
    // release_space(PARTITION_COUNT*_partition_data_size);
    release_space(recoverable_space(PARTITION_COUNT));
    if(!verify_chkpt_reservation() 
            || _space_rsvd_for_chkpt > _partition_data_size) {
        fprintf(stderr,
        "log partitions too small compared to buffer pool:\n"
        "    %lld bytes per partition available\n"
        "    %lld bytes needed for checkpointing dirty pages\n",
        (long long)_partition_data_size, (long long)_space_rsvd_for_chkpt);
        W_FATAL(eOUTOFLOGSPACE);
    }
}

void log_core::_acquire_buffer_space(insert_info* info, long recsize)
{
   INC_TSTAT(log_inserts);

   if (not use_combination_array) {
       w_assert2((unsigned long)(recsize) <= sizeof(logrec_t));
   }
   w_assert2(recsize > 0);


  /* Copy our data into the log buffer and update/create epochs. 
   * Re: Racing flush daemon over 
   * epochs, _start, _end, _curr_lsn, _durable_lsn :
   *
   * _start is set by  _prime at startup (no log flush daemon yet) 
   *                   and flush_daemon_work, not by insert
   * _end is set by  _prime at startup (no log flush daemon yet) 
   *                   and insert, not by log flush daemon
   * _old_epoch is  protected by _flush_lock
   * _cur_epoch is  protected by _flush_lock EXCEPT 
   *                when insert does not wrap, it sets _cur_epoch.end,
   *                which is only read by log flush daemon
   *                The _end is only set after the memcopy is done,
   *                so this should be safe.
   * _curr_lsn is set by  insert (and at startup)
   * _durable_lsn is set by flush daemon
   *
   * NOTE: _end, _start, epochs updated in 
   * _prime(), but that is called only for
   * opening a partition for append in startup/recovery case,
   * in which case there is no race to be had.
   *
   * It is also updated below.
   */

  /* 
   * Make sure there's actually space available in the
   * log buffer, 
   * accounting for the fact that flushes (by the daemon)
   * always work with full blocks. (they round down start of
   * flush to beginning of block and round up/pad end of flush to
   * end of block).
   *
   * If not, kick the flush daemon to make space.
   */
  while(*&_waiting_for_space || 
          end_byte() - start_byte() + recsize > segsize() - 2*BLOCK_SIZE) 
  {
      _insert_lock.release(&info->me);
      {
          CRITICAL_SECTION(cs, _wait_flush_lock);
          while(end_byte() - start_byte() + recsize > segsize() - 2*BLOCK_SIZE)
          {
              _waiting_for_space = true;
              // Use signal since the only thread that should be waiting 
              // on the _flush_cond is the log flush daemon.
              DO_PTHREAD(pthread_cond_signal(&_flush_cond));
              DO_PTHREAD(pthread_cond_wait(&_wait_cond, &_wait_flush_lock));
          }
      }
      _insert_lock.acquire(&info->me);
  }
  // Having ics now should mean that even if another insert snuck in here,
  // we're ok since we recheck the condition. However, we *could* starve here.
 

  /* _curr_lsn, _cur_epoch.end, and end_byte() are all strongly related.
   *
   * _curr_lsn is the lsn of first byte past the tail of the log.
   *    Tail of the log is the last byte of the last record inserted (does not
   *    include the skip_log record).  Inserted records go to _curr_lsn,
   *    _curr_lsn moves with records inserted.
   *
   * _cur_epoch.end and end_byte() are convenience variables to avoid doing
   * expensive operations like modulus and division on the _curr_lsn
   * (lsn of next record to be inserted):
   *
   * _cur_epoch.end is the position of the current lsn relative to the
   *    segsize() log buffer.  
   *    It is relative to the log buffer, and it wraps (modulo the
   *    segment size, which is the log buffer size). ("spill")
   *
   * end_byte()/_end is the non-wrapping version of _cur_epoch.end: 
   *     at log init time it is set to a value in the range [0, segsize()) 
   *     and is  incremented by every log insert for the lifetime of 
   *     the database.
   *     It is a byte-offset from the beginning of the log, considering that
   *     partitions are not "contiguous" in this sense.  Each partition has
   *     a lot of byte-offsets that aren't really in the log because
   *     we limit the size of a partition.
   *
   * _durable_lsn is the lsn of the first byte past the last 
   *     log record written to the file (not counting the skip log record).
   *
   * _cur_epoch.start and start_byte() are convenience variables to avoid doing
   * expensive operations like modulus and division on the _durable_lsn
   *
   * _cur_epoch.start is the position of the durable lsn relative to the
   *     segsize() log buffer.   Because the _cur_epoch.end wraps, start
   *     could become > end and this would create a mess.  For this
   *     reason, when the log buffer wraps, we create a new
   *     epoch. The old epoch represents the entire unflushed
   *     portion of the old log buffer (including a portion of the
   *     presently-inserted log record) and the new epoch represents
   *     the next segment (logically, log buffer), containing
   *     the wrapped portion of the presently-inserted log record.
   *
   *     If, however, by starting a new segment to handle the wrap, we
   *     would exceed the partition size, we create a new epoch and
   *     new segment to hold the entire log record -- log records do not
   *     span partitions.   So we make the old epoch represent
   *     the unflushed portion of the old log buffer (not including any
   *     part of this record) and the new epoch represents the first segment
   *     in the new partition, and contains the entire presently-inserted
   *     log record.  We write the inserted log record at the beginning
   *     of the log buffer.
   *
   * start_byte()/_start is the non-wrapping version of _cur_epoch.start:
   *     At log init time it is set to 0 
   *     and is bumped to match the durable lsn by every log flush.
   *     It is a byte-offset from the beginning of the log, considering that
   *     partitions are not "contiguous" in this sense.  Each partition has
   *     a lot of byte-offsets that aren't really in the log because
   *     we limit the size of a partition.
   *
   * start_byte() through end_byte() tell us the unflushed portion of the
   * log.
   */

  /* An epoch fits within a segment */
    w_assert2(_buf_epoch.end >= 0 && _buf_epoch.end <= segsize());

  /* end_byte() is the byte-offset-from-start-of-log-file 
   * version of _cur_epoch.end */
    w_assert2(_buf_epoch.end % segsize() == end_byte() % segsize());

  /* _curr_lsn is the lsn of the next-to-be-inserted log record, i.e., 
   * the next byte of the log to be written
   */
  /* _cur_epoch.end is the offset into the log buffer of the _curr_lsn */
    w_assert2(_buf_epoch.end % segsize() == _curr_lsn.lo() % segsize());
  /* _curr_epoch.end should never be > segsize at this point;
   * that would indicate a wraparound is in progress when we entered this 
   */
    w_assert2(end_byte() >= start_byte());

    // The following should be true since we waited on a condition 
    // variable while 
    // end_byte() - start_byte() + recsize > segsize() - 2*BLOCK_SIZE
    w_assert2(end_byte() - start_byte() <= segsize() - 2*BLOCK_SIZE);


    long end = _buf_epoch.end;
    long old_end = _buf_epoch.base + end;
    long new_end = end + recsize;
    // set spillsize to the portion of the new record that
    // wraps around to the beginning of the log buffer(segment)
    long spillsize = new_end - segsize();
    lsn_t curr_lsn = _curr_lsn;
    lsn_t next_lsn = _buf_epoch.base_lsn + new_end;
    long new_base = -1;
    long start_pos = end;

    if(spillsize <= 0) {
	// update epoch for next log insert
	_buf_epoch.end = new_end;
    }
    else if(next_lsn.lo() <= _partition_data_size) {
	// wrap within a partition
	_buf_epoch.base_lsn += _segsize;
	_buf_epoch.base += _segsize;
	_buf_epoch.start = 0;
	_buf_epoch.end = new_end = spillsize;
    }
    else {
	// new partition! need to update next_lsn/new_end to reflect this
        long leftovers = _partition_data_size - curr_lsn.lo();
        w_assert2(leftovers >= 0);
        if(leftovers && !reserve_space(leftovers)) {
            info->error = eOUTOFLOGSPACE;
	    if(use_decoupled_memcpy)
		_insert_lock.release(&info->me);
	    return;
	}
	
	curr_lsn = first_lsn(next_lsn.hi()+1);
	next_lsn = curr_lsn + recsize;
	new_base = _buf_epoch.base + _segsize;
	start_pos = 0;
	_buf_epoch = epoch(curr_lsn, new_base, 0, new_end=recsize);
    }
    
    // let the world know
    _curr_lsn = next_lsn;
    _end = _buf_epoch.base + new_end;

    if(enable_mcs_expose) {
	// join the memcpy-complete queue but don't spin yet
	info->me2._padding = 0;
	info->pred2 = _expose_lock.__unsafe_begin_acquire(&info->me2);
    }

    if(use_decoupled_memcpy)
	_insert_lock.release(&info->me);
    if(print_working_set)
	(*tls_histo)[_end - _start]++;
    
    info->lsn = curr_lsn; // where will we end up on disk?
    info->old_end = old_end; // lets us serialize with our predecessor after memcpy
    info->start_pos = start_pos; // != old_end when partitions wrap
    info->pos = start_pos + recsize; // coordinates groups of threads sharing a log allocation
    info->new_end = new_end; // eventually assigned to _cur_epoch 
    info->new_base = new_base; // positive if we started a new partition
    info->error = 0;
}

lsn_t log_core::_copy_to_buffer(logrec_t &rec, long pos, long recsize, insert_info* info)
{
    /*
      do the memcpy (or two)
    */
    lsn_t rlsn = info->lsn + pos;
    rec.set_lsn_ck(rlsn);

    // are we the ones that actually wrap? (do this *after* computing the lsn!)
    pos += info->start_pos;
    if(pos >= _segsize)
	pos -= _segsize;
    
    char const* data = (char const*) &rec;
    long spillsize = pos + recsize - _segsize;
    if(spillsize <= 0) {
	// normal insert
	memcpy(_buf+pos, data, recsize);
    }
    else {
        // spillsize > 0 so we are wrapping. 
        // The wrap is within a partition. 
        // next_lsn is still valid but not new_end
        //
        // Old epoch becomes valid for the flush daemon to
        // flush and "close".  It contains the first part of
        // this log record that we're trying to insert.
        // New epoch holds the rest of the log record that we're trying
        // to insert.
        //
        // spillsize is the portion that wraps around 
        // partsize is the portion that doesn't wrap.
        long partsize = recsize - spillsize;

        // Copy log record to buffer
        // memcpy : areas do not overlap
	memcpy(_buf+pos, data, partsize);
        memcpy(_buf, data+partsize, spillsize);
    }

    return rlsn;
}

static long const MAX_THREADS = 256;
long combination_stats[MAX_THREADS];
long expose_stats[1000*MAX_THREADS];


bool log_core::_wait_for_expose(insert_info* info, bool attempt_abort) {
    w_assert1(SLOT_FINISHED == info->vthis()->count);
    membar_producer();
    if(enable_mcs_expose) {
	if(attempt_abort && info->pred2 && _slot_array->indexof(info) % 32) {
	    unsigned long waiting = WAITING.hq._state;
	    membar_exit();
	    if(info->me2h._state == waiting && 
               waiting == atomic_cas_64(&info->me2h._state, waiting, ABORT_ME.hq._state)) 
            {
		//fprintf(stderr, "slot %d bailed from queue\n", info - _slot_array);
		return true; // abort succeeded
	    }
	}
	_expose_lock.__unsafe_end_acquire(&info->me2, info->pred2);
    }
    else {
	_spin_on_epoch(info->old_end);
    }
    return false;
}    

bool log_core::_update_epochs(insert_info* info, bool attempt_abort) {
    /* wait for our predecessor to catch up if we're ahead

       Even though the end pointer we're checking wraps regularly, we
       already have to limit each address in the buffer to one active
       writer or data corruption will result.
     */
    if( _wait_for_expose(info, attempt_abort))
	return true; // we escaped!

    //_print_expose_queue(&info->me2);
    
    /*
      now update the epoch(s)
    */
    long count = 0;
 do_update:
    ++count;
    w_assert1(*&_cur_epoch.vthis()->end + *&_cur_epoch.vthis()->base == info->old_end);
    if(info->new_base > 0) {
	// new partition! update epochs to reflect this

        // I just wrote part of the log record to the beginning of the
        // log buffer. How do I know that it didn't interfere with what
        // the flush daemon is writing? Because at the beginning of
        // this method, I waited until the log flush daemon ensured that
        // I had room to insert this entire record (with a fudge factor
        // of 2*BLOCK_SIZE)
        
        // update epochs
        CRITICAL_SECTION(cs, _flush_lock);
        w_assert3(_old_epoch.start == _old_epoch.end);
	_old_epoch = _cur_epoch;
	_cur_epoch = epoch(info->lsn, info->new_base, 0, info->new_end);
    }
    else if(info->pos > _segsize) {
	// wrapped buffer! update epochs
	CRITICAL_SECTION(cs, _flush_lock);
	w_assert3(_old_epoch.start == _old_epoch.end);
        _old_epoch = epoch(_cur_epoch.base_lsn, _cur_epoch.base, 
                    _cur_epoch.start, segsize());
        _cur_epoch.base_lsn += segsize();
        _cur_epoch.base += segsize();
        _cur_epoch.start = 0;
	_cur_epoch.end = info->new_end;
    }
    else {
	// normal update -- no need for a lock if we just increment its end
	w_assert1(_cur_epoch.start < info->new_end);
	_cur_epoch.end = info->new_end;
    }
    if(enable_mcs_expose) {
	/*
	  Four cases to consider
	  
	  1. Aborted
	  2. Aborting
	  3. Spinning (can't abort)
	  4. Busy
	 */
	assert(SLOT_FINISHED == info->vthis()->count);
	membar_exit();
	union {
	    mcs_lock::qnode* q;
	    insert_info* i;
	    long n;
	} next = { info->me2.vthis()->_next }, offset = {0};
	if(!next.q) {
	    if(&info->me2 != _expose_lock._tail || &info->me2 != atomic_cas_ptr(&_expose_lock._tail, &info->me2, NULL))
		next.q = _expose_lock.spin_on_next(&info->me2);
	}

	if(next.q) {
	    next.n -= (long) &offset.i->me2._next;
	    unsigned long value = atomic_cas_64(&next.i->me2h._state, WAITING.hq._state, 0);
	    if(value == ABORT_ME.hq._state) {
		// they aborted... up to us to do their dirty work
		w_assert1(SLOT_FINISHED == next.i->vthis()->count);
		w_assert1(next.i->pred2 == &info->me2);
		membar_producer();
		info->vthis()->count = SLOT_UNUSED;
		info = next.i;
		goto do_update;
	    }
	}
    }

#warning deal with valgrind checks
    /* if I get here I hit NULL or non-abort[ed|able] node
     */
    membar_producer();
    info->count = SLOT_UNUSED;
    if(print_expose_groups)
	atomic_inc_ulong((unsigned long*) &expose_stats[count]);
    return false;
}

typedef std::map<long,long> stat_map;
stat_map log_stats;
void print_log_stats() {
    if(print_lsn_groups) {
	fprintf(stderr, "Consolidation array group size distribution:\n");
	for(long i=0; i < (long)(sizeof(combination_stats)/sizeof(combination_stats[0])); i++) {
	    long count = combination_stats[i];
	    if(count) {
		fprintf(stderr, "	%ld %ld\n", i, count);
		combination_stats[i] = 0;
	    }
	}
    }

    if(print_expose_groups) {
	fprintf(stderr, "Exposure group size distribution:\n");
	for(long i=0; i < (long)(sizeof(expose_stats)/sizeof(expose_stats[0])); i++) {
	    long count = expose_stats[i];
	    if(count) {
		fprintf(stderr, "	%ld %ld\n", i, count);
		expose_stats[i] = 0;
	    }
	}
    }
    if(print_working_set) {
	histo::global_histo.print();
    }
}

static long const ONE = 1l<<32;

log_core::insert_info* log_core::_join_slot(long &idx, long &start, long size) {
    w_assert1(size > 0);
 probe_slot:
    idx %= _active_slots;
    insert_info* info = _slots[idx];
    
    long old_count = info->vthis()->count;
 join_slot:
    if(old_count < SLOT_AVAILABLE) {
	++idx;
	goto probe_slot;
    }

    // set to 'available' and add our size to the slot
    long new_count = old_count + size + ONE;
    long cur_count = atomic_cas_ulong((unsigned long *)&info->count, old_count, new_count);
    if(cur_count != old_count) {
	old_count = cur_count;
	goto join_slot;
    }
    start = old_count;
    return info;
}

void log_core::_allocate_slot(long idx) {
    _slots[idx] = _slot_array->allocate();
}


rc_t log_core::insert(logrec_t &rec, lsn_t* rlsn) {
    long size = rec.length();
    w_assert1((size_t)size <= sizeof(logrec_t));

    /* Copy our data into the buffer and update/create epochs. Note
       that, while we may race the flush daemon to update the epoch
       record, it will not touch the buffer until after we succeed so
       there is no race with memcpy(). If we do lose an epoch update
       race, it is only because the flush changed old_epoch.begin to
       equal old_epoch.end. The mutex ensures we don't race with any
       other inserts.
    */
    lsn_t rec_lsn;
    insert_info* info = 0;
    long pos = 0;
    bool acquired = false;
    if(enable_fastpath || !use_combination_array) {
	info = tls_info_array->allocate();
	if(use_combination_array) {
	    acquired = _insert_lock.attempt(&info->me);
	}
	else {
	    _insert_lock.acquire(&info->me);
	    acquired = true;
	}
	if(acquired) {
	    combination_stats[0]++;
	    info->count = SLOT_FINISHED - size;
	    pos = 0;
	    
	    // may release the lock
	    _acquire_buffer_space(info, size);
	    if(info->error) {
		// failed to acquire buffer space... abort
		_insert_lock.release(&info->me);
		return RC(info->error);
	    }
	}
	else {
	    // put it back.. we're going to consolidate
	    info->count = SLOT_UNUSED;
	}
    }
    
    if(!acquired) {
	// need to consolidate
	long idx =  (long)pthread_self();
	long old_count;
	info = _join_slot(idx, old_count, size);

	pos = old_count & (ONE-1);
	if(old_count == SLOT_AVAILABLE) {
	    /* First to arrive. Acquire the lock on behalf of the whole
	     * group, claim the first 'size' bytes, then make the rest
	     * visible to waiting threads.
	     */
	    _insert_lock.acquire(&info->me);

	    assert(info->vthis()->count > SLOT_AVAILABLE);

	    // swap out this slot and mark it busy
	    _allocate_slot(idx);

	    // negate the count to signal waiting threads and mark the slot busy
	    old_count = atomic_swap_ulong((unsigned long*) &info->count, SLOT_PENDING);
	    long group_size = old_count/ONE;
	    combination_stats[group_size]++;
	    old_count &= (ONE-1);

	    // grab space for everyone in one go (releases the lock)
	    _acquire_buffer_space(info, old_count);

	    // now let everyone else see it
	    membar_producer();
	    info->count = SLOT_FINISHED-old_count;
	}
	else {
	    /* Not first. Wait for the owner to tell us what's going on.
	     */
	    assert(old_count > SLOT_AVAILABLE);
	    old_count = _spin_on_count(&info->count, SLOT_FINISHED);
	}
    }

    // insert my value
    if(!info->error) 
	rec_lsn = _copy_to_buffer(rec, pos, size, info);

    // last one to leave cleans up
    long end_count = atomic_add_long_nv((unsigned long *)&info->count, size);
    w_assert3(end_count <= SLOT_FINISHED);
    if(end_count == SLOT_FINISHED) {
	if(!info->error)
	    _update_epochs(info, use_expose_groups && use_decoupled_memcpy);
	if(!use_decoupled_memcpy) 
	    _insert_lock.release(&info->me);
    }

    if(info->error)
	return RC(info->error);
    
    if(rlsn) *rlsn = rec_lsn;

    ADD_TSTAT(log_bytes_generated,size);
    return RCOK;
}

bool disable_wait_for_flush = false;

// Return when we know that the given lsn is durable. Wait for the
// log flush daemon to ensure that it's durable.
rc_t log_core::flush(lsn_t lsn, bool block)
{
    ASSERT_FITS_IN_POINTER(lsn_t);
    // else our reads to _durable_lsn would be unsafe

    // don't try to flush past end of log -- we might wait forever...
    lsn = std::min(lsn, (*&_curr_lsn)+ -1);
    
    // already durable?
    if(lsn >= *&_durable_lsn) {
	if (disable_wait_for_flush || !block) {
            *&_waiting_for_flush = true;
            DO_PTHREAD(pthread_cond_signal(&_flush_cond));
        }
        else {
	    CRITICAL_SECTION(cs, _wait_flush_lock);
	    while(lsn >= *&_durable_lsn) {
		*&_waiting_for_flush = true;
		// Use signal since the only thread that should be waiting 
		// on the _flush_cond is the log flush daemon.
		DO_PTHREAD(pthread_cond_signal(&_flush_cond));
		DO_PTHREAD(pthread_cond_wait(&_wait_cond, &_wait_flush_lock));
	    }
        }
    } else {
        INC_TSTAT(log_dup_sync_cnt);
    }
    return RCOK;
}

/**\brief Log-flush daemon driver.
 * \details
 * This method handles the wait/block of the daemon thread,
 * and when awake, calls its main-work method, flush_daemon_work.
 */
void log_core::flush_daemon() 
{
    /* Algorithm: attempt to flush non-durable portion of the buffer.
     * If we empty out the buffer, block until either enough
       bytes get written or a thread specifically requests a flush.
     */
    lsn_t last_completed_flush_lsn;
    bool success = false;
    while(1) {

        // wait for a kick. Kicks come at regular intervals from
        // inserts, but also at arbitrary times when threads request a
        // flush.
        {
	    if(disable_wait_for_flush)
		usleep(1000);
            CRITICAL_SECTION(cs, _wait_flush_lock);
            if(success && (*&_waiting_for_space || *&_waiting_for_flush)) {
                _waiting_for_flush = _waiting_for_space = false;
                DO_PTHREAD(pthread_cond_broadcast(&_wait_cond)); 
                // wake up anyone waiting on log flush
            }

            if(*&_shutting_down) {
                _shutting_down = false;
                break;
            }
        
            // sleep. We don't care if we get a spurious wakeup
            if(!success && !*&_waiting_for_space && !*&_waiting_for_flush) {
                // Use signal since the only thread that should be waiting 
                // on the _flush_cond is the log flush daemon.
                DO_PTHREAD(pthread_cond_wait(&_flush_cond, &_wait_flush_lock));
                INC_TSTAT(log_sync_cnt);
            }
        }

        // flush all records later than last_completed_flush_lsn
        // and return the resulting last durable lsn 
        lsn_t lsn = flush_daemon_work(last_completed_flush_lsn);

        // success=true if we wrote anything
        success = (lsn != last_completed_flush_lsn);
        last_completed_flush_lsn = lsn;
    }

    // make sure the buffer is completely empty before leaving...
    for(lsn_t lsn; 
        (lsn=flush_daemon_work(last_completed_flush_lsn)) != 
                last_completed_flush_lsn; 
        last_completed_flush_lsn=lsn) ;
}

/**\brief Flush unflushed-portion of log buffer.
 * @param[in] old_mark Durable lsn from last flush. Flush records later than this.
 * \details
 * This is the guts of the log daemon.
 *
 * Flush the log buffer of any log records later than \a old_mark. The
 * argument indicates what is already durable and these log records must
 * not be duplicated on the disk.
 *
 * Called by the log flush daemon.
 * Protection from duplicate flushing is handled by the fact that we have
 * only one log flush daemon.
 * \return Latest durable lsn resulting from this flush
 *
 */
lsn_t log_core::flush_daemon_work(lsn_t old_mark)
{
    lsn_t base_lsn_before, base_lsn_after;
    long base, start1, end1, start2, end2;
    {
        CRITICAL_SECTION(cs, _flush_lock);
        base_lsn_before = _old_epoch.base_lsn;
        base_lsn_after = _cur_epoch.base_lsn;
        base = _cur_epoch.base;

        // The old_epoch is valid (needs flushing) iff its end > start.
        // The old_epoch is valid id two cases, both when
        // insert wrapped thelog buffer
        // 1) by doing so reached the end of the partition,
        //     In this case, the old epoch might not be an entire
        //     even segment size
        // 2) still room in the partition
        //     In this case, the old epoch is exactly 1 segment in size.

        if(_old_epoch.start == _old_epoch.end) {
            // no wrap -- flush only the new
            start2 = _cur_epoch.start;
            end2 = _cur_epoch.end;

	    // false alarm?
	    if(start2 == end2)
		return old_mark;

            _cur_epoch.start = end2;

            start1 = start2; // fake start1 so the start_lsn calc below works
            end1 = start2;

            base_lsn_before = base_lsn_after;
        }
        else if(base_lsn_before.file() == base_lsn_after.file()) {
            // wrapped within partition -- flush both
            start2 = _cur_epoch.start;
            // race here with insert setting _curr_epoch.end, but
            // it won't matter. Since insert already did the memcpy,
            // we are safe and can flush the entire amount.
            end2 = _cur_epoch.end;
            _cur_epoch.start = end2;

            start1 = _old_epoch.start;
            end1 = _old_epoch.end;
            _old_epoch.start = end1;

            w_assert1(base_lsn_before + segsize() == base_lsn_after);
        }
        else {
            // new partition -- flushing only the old since the
            // two epochs target different files. Let the next
            // flush handle the new epoch.
            start2 = 0;
            end2 = 0; // don't fake end2 because end_lsn needs to see '0'

            start1 = _old_epoch.start;
            end1 = _old_epoch.end;

            // Mark the old epoch has no longer valid.
            _old_epoch.start = end1;

            w_assert1(base_lsn_before.file()+1 == base_lsn_after.file());
        }
    } // end critical section

    lsn_t start_lsn = base_lsn_before + start1;
    lsn_t end_lsn   = base_lsn_after + end2;
    long  new_start = base + end2;
    {
        // Avoid interference with compensations.
        CRITICAL_SECTION(cs, _comp_lock);
        _flush_lsn = end_lsn;
    }

    w_assert1(end_lsn == first_lsn(start_lsn.hi()+1)
          || end_lsn.lo() - start_lsn.lo() == (end1-start1) + (end2-start2));

    // start_lsn.file() determines partition # and whether _flushX
    // will open a new partition into which to flush.
    // That, in turn, is determined by whether the _old_epoch.base_lsn.file()
    // matches the _cur_epoch.base_lsn.file()
    _flushX(start_lsn, end_lsn, start1, end1, start2, end2);

    _durable_lsn = end_lsn;
    _start = new_start;

    return end_lsn;
}

// Find the log record at orig_lsn and turn it into a compensation
// back to undo_lsn
rc_t log_core::compensate(lsn_t orig_lsn, lsn_t undo_lsn) 
{
    // somewhere in the calling code, we didn't actually log anything.
    // so this would be a compensate to myself.  i.e. a no-op
    if(orig_lsn == undo_lsn)
        return RCOK;

    // FRJ: this assertion wasn't there originally, but I don't see
    // how the situation could possibly be correct
    w_assert1(orig_lsn <= _curr_lsn);
    
    // no need to grab a mutex if it's too late
    if(orig_lsn < _flush_lsn)
      return RC(eBADCOMPENSATION);
    
    CRITICAL_SECTION(cs, _comp_lock);
    // check again; did we just miss it?
    lsn_t flsn = _flush_lsn;
    if(orig_lsn < flsn)
      return RC(eBADCOMPENSATION);
    
    /* where does it live? the buffer is always aligned with a
       buffer-sized chunk of the partition, so all we need to do is
       take a modulus of the lsn to get its buffer position. It may be
       wrapped but we know it's valid because we're less than a buffer
       size from the current _flush_lsn -- nothing newer can be
       overwritten until we release the mutex.
     */
    long pos = orig_lsn.lo() % segsize();
    if(pos >= segsize() - logrec_t::hdr_sz) 
        return RC(eBADCOMPENSATION); // split record. forget it.

    // aligned?
    w_assert1((pos & 0x7) == 0);
    
    // grab the record and make sure it's valid
    logrec_t* s = (logrec_t*) &_buf[pos];

    // valid length?
    w_assert1((s->length() & 0x7) == 0);
    
    // split after the log header? don't mess with it
    if(pos + long(s->length()) > segsize())
        return RC(eBADCOMPENSATION);
    
    lsn_t lsn_ck = s->get_lsn_ck();
    if(lsn_ck != orig_lsn) {
        // this is a pretty rare occurrence, and I haven't been able
        // to figure out whether it's actually a bug
        fprintf(stderr, "\nlsn_ck = %d.%lld, orig_lsn = %d.%lld\n",
                lsn_ck.hi(), 
                (long long int)(lsn_ck.lo()), 
                orig_lsn.hi(), 
                (long long int)(orig_lsn.lo()));
cerr 
    << __LINE__ << " " __FILE__ << " "
    << "log rec is  " << *s << endl;
        return RC(eBADCOMPENSATION);
    }
    w_assert1(s->prev() == lsn_t::null || s->prev() >= undo_lsn);

    if(s->is_undoable_clr())
        return RC(eBADCOMPENSATION);

    // success!
    DBGTHRD(<<"COMPENSATING LOG RECORD " << undo_lsn << " : " << *s);
    s->set_clr(undo_lsn);
    return RCOK;
}

int
log_core::get_last_lsns(lsn_t *array)
{
    int j=0;
    for(int i=0; i < PARTITION_COUNT; i++) {
        const partition_t *p = this->_partition(i);
        if(p->num() > 0 && (p->last_skip_lsn().hi() == p->num())) {
            array[j++] = p->last_skip_lsn();
        }
    }
    return j;
}


std::deque<log_core::waiting_xct*> log_core::_log_space_waiters;

rc_t log_core::wait_for_space(fileoff_t &amt, timeout_in_ms timeout) 
{
    DBG(<<"log_core::wait_for_space " << amt);
    // if they're asking too much don't even bother
    if(amt > _partition_data_size)
        return RC(eOUTOFLOGSPACE);

    // wait for a signal or 100ms, whichever is longer...
    w_assert0(amt > 0);
    struct timespec when;
    if(timeout != WAIT_FOREVER)
    sthread_t::timeout_to_timespec(timeout, when);

    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    waiting_xct* wait = new waiting_xct(&amt, &cond);
    DO_PTHREAD(pthread_mutex_lock(&_space_lock));
    _waiting_for_space = true;
    _log_space_waiters.push_back(wait);
    while(amt) {
        /* First time through, someone could have freed up space
           before we acquired this mutex. 2+ times through, maybe our
           previous rounds got us enough that the normal log
           reservation can supply what we still need.
         */
        if(reserve_space(amt)) {
            amt = 0;

            // nullify our entry. Non-racy beause amt > 0 and we hold the mutex
            wait->needed = 0;

            // clean up in case it's pure false alarms
            while(_log_space_waiters.size() && ! _log_space_waiters.back()->needed) {
                delete _log_space_waiters.back();
                _log_space_waiters.pop_back();
            }
            break;
        }
        smlevel_1::chkpt->wakeup_and_take();
        if(timeout == WAIT_FOREVER) {
            fprintf(stderr, 
            "* - * - * tid %lld.%lld waiting forever for %lld bytes of log\n",
                (long long)(xct()->tid().get_hi()), 
                (long long)(xct()->tid().get_lo()), 
                (long long) amt);
            DO_PTHREAD(pthread_cond_wait(&cond, &_space_lock));
        } else {
            fprintf(stderr, 
                "* - * - * tid %lld.%lld waiting with timeout for %lld bytes of log\n",
                (long long)(xct()->tid().get_hi()), 
                (long long)(xct()->tid().get_lo()), 
                (long long) amt);
                int err = pthread_cond_timedwait(&cond, &_space_lock, &when);
                if(err == ETIMEDOUT) 
                break;
        }
    }
    fprintf(stderr, "* - * - * tid %lld.%lld done waiting (%lld bytes still needed)\n",
                (long long)(xct()->tid().get_hi()), 
        (long long)(xct()->tid().get_lo()), 
        (long long) amt);

    DO_PTHREAD(pthread_mutex_unlock(&_space_lock));
    return amt? RC(sthread_t::stTIMEOUT) : RCOK;
}

void log_core::release_space(fileoff_t amt) 
{
    DBG(<<"log_core::release_space " << amt);
    w_assert0(amt >= 0);
    /* NOTE: The use of _waiting_for_space is purposefully racy
       because we don't want to pay the cost of a mutex for every
       space release (which should happen every transaction
       commit...). Instead waiters use a timeout in case they fall
       through the cracks.

       Waiting transactions are served in FIFO order; those which time
       out set their need to -1 leave it for release_space to clean
       it up.
     */
    if(_waiting_for_space) {
        DO_PTHREAD(pthread_mutex_lock(&_space_lock));
        while(amt > 0 && _log_space_waiters.size()) {
            bool finished_one = false;
            waiting_xct* wx = _log_space_waiters.front();
            if( ! wx->needed) {
            finished_one = true;
            }
            else {
            fileoff_t can_give = std::min(amt, *wx->needed);
            *wx->needed -= can_give;
            amt -= can_give;
            if(! *wx->needed) {
                DO_PTHREAD(pthread_cond_signal(wx->cond));
                finished_one = true;
            }
            }
            
            if(finished_one) {
            delete wx;
            _log_space_waiters.pop_front();
            }
        }
        if(_log_space_waiters.empty())
            _waiting_for_space = false;
        
        DO_PTHREAD(pthread_mutex_unlock(&_space_lock));
    }
    
    atomic_add_long_delta(_space_available, amt); // use templated function
}

void 
log_core::activate_reservations() 
{
#if USE_LOG_RESERVATIONS
    /* With recovery complete we now activate log reservations.

       In fact, the activation should be as simple as setting the mode to
       t_forward_processing, but we also have to account for any space
       the log already occupies. We don't have to double-count
       anything because nothing will be undone should a crash occur at
       this point.
     */
    w_assert1(operating_mode == t_forward_processing);
	// FRJ: not true if any logging occurred during recovery
    // w_assert1(PARTITION_COUNT*_partition_data_size ==
    //       _space_available + _space_rsvd_for_chkpt);
    w_assert1(!_reservations_active);

    // knock off space used by full partitions
    long oldest_pnum = _min_chkpt_rec_lsn.hi();
    long newest_pnum = curr_lsn().hi();
    long full_partitions = newest_pnum - oldest_pnum; // can be zero
    _space_available -= recoverable_space(full_partitions);

    // and knock off the space used so far in the current partition
    _space_available -= curr_lsn().lo();
    _reservations_active = true;
#endif
}

rc_t                
log_core::file_was_archived(const char * /*file*/)
{
    // TODO: should check that this is the oldest, 
    // and that we indeed asked for it to be archived.
    _space_available += recoverable_space(1);
    return RCOK;
}
