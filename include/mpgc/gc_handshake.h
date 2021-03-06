/*
 *
 *  Multi Process Garbage Collector
 *  Copyright © 2016 Hewlett Packard Enterprise Development Company LP.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  As an exception, the copyright holders of this Library grant you permission
 *  to (i) compile an Application with the Library, and (ii) distribute the 
 *  Application containing code generated by the Library and added to the 
 *  Application during this compilation process under terms of your choice, 
 *  provided you also meet the terms and conditions of the Application license.
 *
 */

/*
 * gc_handshake.h
 *
 *  Created on: Jul 22, 2015
 *      Author: gidra
 */

#ifndef GC_GC_HANDSHAKE_H_
#define GC_GC_HANDSHAKE_H_

#include <atomic>
#include <vector>
#include <unordered_set>
#include <cstdio>
#include <random>

#include <execinfo.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#include "ruts/collections.h"
#include "ruts/managed.h"

#include "mpgc/mark_buffer.h"
#include "mpgc/gc_thread.h"
#include "mpgc/gc_skiplist_allocator.h"

namespace std {
  extern void cpu_relax();
}
namespace mpgc {
  extern volatile uint8_t request_gc_termination;
  namespace gc_handshake {
    extern void initialize1();
    extern void initialize2();

    //Enum to represent the phase in which GC is at any point.
    enum class Signum : char {
      sigSync1 = 0,
      sigSync2,
      sigAsync,
      sigDeferredAsync,
      sigSweep,
      sigDeferredSweep,
      // More actions may come here.
      sigInit
    };

    enum class Weak_signal : char {
      InBarrier,
      DoHandshake,
      Working
    };

    extern Signum *status_ptr;
    extern per_process_struct *process_struct;
    extern mark_bitmap *mbitmap;
    /*
     * We need three different life-time of data structures.
     * 1. Things which live as long as the process does, for
     * instance, pthread_t, status variable etc.
     * 2. Things which must not go away until the end of current
     * garbage collection cycle (even if the process dies), like
     * mark buffer, GC threads' dequeue etc.
     *    This structure must be accessible to GC threads of all
     *    the process so that in the event of some process dying,
     *    the GC thread of other process(es) can take over and
     *    cleanup after the current GC cycle finishes.
     *
     * 3. Things which must live forever, like bitmap.
     *
     */

    /*
     * This structure contains all those things which are mutator
     * thread local. Also, these things must live as long as the
     * process lives. Obviously, the GC thread can free structures
     * corresponding to a terminated thread, at the end of the current
     * GC cycle.
     */
    struct in_memory_thread_struct {
      enum class Alive : unsigned char {
        Dead = 0,
        Live
      };

      using on_stack_wp_set_type = std::unordered_set<const void*,
                                                      std::hash<const void*>,
                                                      std::equal_to<const void*>,
                                                      ruts::managed_space::allocator<const void*>>;

      on_stack_wp_set_type on_stack_wp_set;
      gc_allocator::localPoolType local_free_list;
      std::mt19937 rand;
      const pthread_t pthread;
      uint8_t * const stack_end;
      mutator_persist * const persist_data;
      mark_bitmap * const bitmap;
      std::atomic<gc_status> status_idx;
      std::atomic<Weak_signal> weak_signal;
      volatile Alive live;
      volatile bool mark_signal_disabled;
      volatile Signum mark_signal_requested;
      volatile bool sweep_signal_disabled;
      volatile bool sweep_signal_requested;
      volatile bool clear_local_allocator;

      static bool is_marked(in_memory_thread_struct *s) { return s->live == Alive::Dead; }
      void mark_dead() {
        /* We must disable the signals which has side-effects before
         * marking this structure dead.
         */
        mark_signal_disabled = true;
        sweep_signal_disabled = true;
        live = Alive::Dead;
      }

      bool marked_dead() {
        return live == Alive::Dead;
      }

      in_memory_thread_struct() :
          rand(pthread),
          pthread(pthread_self()),
          stack_end(compute_stack_addr(pthread)),
          persist_data(process_struct->mutator_persist_list().insert()),
          bitmap(mbitmap),
          status_idx(gc_status(Signum::sigInit)),
          weak_signal(Weak_signal::Working),
          live(Alive::Live),
          mark_signal_disabled(false),
          mark_signal_requested(Signum::sigInit),
          sweep_signal_disabled(false),
          sweep_signal_requested(false),
          clear_local_allocator(false)
      {}

      ~in_memory_thread_struct() {
        persist_data->mbuf.mark_dead();
      }

    private:
      /*
       * We statically compute the stack base when this struct
       * is first created. The possibility of the stack being
       * grown/shrink at runtime is not supported yet.
       */
      uint8_t *compute_stack_addr(pthread_t p) {
        void *stack_addr;
        std::size_t stack_size;
        pthread_attr_t attr;

        pthread_getattr_np(p, &attr);
        pthread_attr_getstack(&attr, &stack_addr, &stack_size);
        pthread_attr_destroy(&attr);
        return reinterpret_cast<uint8_t*>(stack_addr) + stack_size;
      }
    };

    typedef ruts::sequential_lazy_delete_collection<in_memory_thread_struct, std::allocator<in_memory_thread_struct>> in_memory_thread_struct_list_type;
    extern in_memory_thread_struct_list_type thread_struct_list;

    /*
     * We create a thread-local pointer to create the above defined
     * thread-local struct. This way we can keep around the struct
     * beyond the life of thread, and yet create it for every thread.
     */
    struct thread_struct_handle {
      in_memory_thread_struct *handle;

      thread_struct_handle();

      ~thread_struct_handle() { handle->mark_dead(); }
    };

    extern thread_local thread_struct_handle thread_struct_handles;

    //Useful for debugging
    struct dump_offsets {
    private:
      using T = offset_ptr<const gc_allocated>;

      constexpr static std::size_t buf_count = 6;
      constexpr static std::size_t buf_size = 1 << 28;

      std::size_t i[buf_count];
      T *buf[buf_count];
      volatile uint8_t index = 0;

    public:
      dump_offsets() {
        for (std::size_t j = 0; j < buf_count; j++) {
          buf[j] = (T*) malloc(sizeof(T) * buf_size);
          i[j] = 0;
        }
      }

      ~dump_offsets() {
        for (std::size_t j = 0; j < buf_count; j++) {
          delete buf[j];
        }
      }

      void open_dump_file() {
        index = (index + 2) % buf_count;
        i[index] = i[index + 1] = 0;
      }

     void dump_offset(const offset_ptr<const gc_allocated> &p) {
        buf[index][i[index]++] = p;
        assert(i[index] <= buf_size);
      }

      void dump_offset_alloc(const offset_ptr<const gc_allocated> &p) {
        buf[index + 1][i[index + 1]++] = p;
        assert(i[index + 1] <= buf_size);
      }
    };

    //Useful for debugging
    class backtrace_struct {
      void* buffer[4096];
      char fname[64];
      int count;
      int fd;
     public:
      backtrace_struct()  : count(-1), fd(-1) {
        std::snprintf(fname, 64, "0x%lx", thread_struct_handles.handle->pthread);
        fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
      }

      ~backtrace_struct() {
         close(fd);
      }

      void backtrace_to_file() {
        lseek(fd, 0, SEEK_SET);
        count = backtrace(buffer, 4096);
        backtrace_symbols_fd(buffer, count, fd);
      }
    };

    /*
     * It is critical that the stack is scanned in a software signal to ensure
     * that the registers are captured too. Therefore, when we defer the async
     * signal, and indeed get a signal while it is deferred, we send another
     * signal to ourself, which basically does exactly what async signal does.
     */
    inline void do_deferred_async_signal(in_memory_thread_struct &thread_struct) {
      pthread_sigqueue(thread_struct.pthread, SIGRTMIN, {.sival_int = static_cast<char>(Signum::sigDeferredAsync)});
      while (thread_struct.status_idx.load().status() != Signum::sigAsync) {
        pthread_yield();
      }
    }

    inline void do_deferred_sweep_signal(in_memory_thread_struct &thread_struct) {
      pthread_sigqueue(thread_struct.pthread, SIGRTMIN, {.sival_int = static_cast<char>(Signum::sigDeferredSweep)});
      while (thread_struct.status_idx.load().status() != Signum::sigSweep) {
        pthread_yield();
      }
    }

    inline void post_handshake(Signum sig, bool doWeakCheck) {
      sigval_t sigval;
      sigval.sival_int = static_cast<char>(sig);
      /* The following fence is required because any process struct's
       * status change must get to the memory before the following
       * head is accessed.
       */
      std::atomic_thread_fence(std::memory_order_seq_cst);
      in_memory_thread_struct *h = thread_struct_list.head();
      while (h) {
        if (!h->marked_dead()) {
          if (doWeakCheck) {
            Weak_signal expected_weak_signal = Weak_signal::InBarrier;
            h->weak_signal.compare_exchange_strong(expected_weak_signal, Weak_signal::DoHandshake);
          }
          //send signal
          pthread_sigqueue(h->pthread, SIGRTMIN, sigval);
        }
        h = thread_struct_list.next(h);
      }
    }

    inline void wait_handshake(Signum sig, bool doWeakCheck) {
      in_memory_thread_struct *h = thread_struct_list.head();
      while (h) {
        // We must replace this busy loop with some other means of waiting.
        // This is very CPU wasting mechanism.
        while (!h->marked_dead() && (h->status_idx.load().status() != sig ||
               (doWeakCheck && h->weak_signal == gc_handshake::Weak_signal::DoHandshake))) {
          std::cpu_relax();
          if (request_gc_termination) {
            return;
          }
        }
        h = thread_struct_list.next(h);
      }
    }

    inline void handshake(Signum sig, bool doWeakCheck = false) {
      post_handshake(sig, doWeakCheck);
      wait_handshake(sig, doWeakCheck);
    }

  }
}

#endif /* GC_GC_HANDSHAKE_H_ */
