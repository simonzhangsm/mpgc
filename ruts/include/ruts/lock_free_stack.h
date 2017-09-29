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
 * lock_free_queue.h
 *
 *  Created on: Aug 7, 2016
 *      Author: gidra
 */

#ifndef LF_STACK_H_
#define LF_STACK_H_

#include<cstddef>
#include<atomic>
#include<memory>
#include<cassert>

#include "versioned_ptr.h"
/*
 * This class provides a lock-free unbounded stack implemented
 * using a singly linked-list.
 */

namespace ruts {
  template <typename T, typename A = std::allocator<T>>
  class lf_stack {
   public:
     template<typename U>
     using pointer
     = std::conditional_t<std::is_const<U>::value,
                          typename std::allocator_traits<A>
                          ::template rebind_alloc<std::remove_const_t<U>>
                          ::const_pointer,
                          typename std::allocator_traits<A>
                          ::template rebind_alloc<std::remove_const_t<U>>
                          ::pointer>;
   private:
     struct entry {
       T value;
       pointer<entry> next;
     };
     using entry_allocator_type = typename std::allocator_traits<A>::template rebind_alloc<entry>;
     using versioned_head_t = versioned<pointer<entry>>;

     typename versioned_head_t::atomic_pointer _head;
     entry_allocator_type alloc;

  public:
     lf_stack() : _head(nullptr), alloc() {
     }

     ~lf_stack() {
       clear();
     }

     constexpr pointer<T> head() const {
       pointer<entry> p = _head;
       return p != nullptr ? &p->value : nullptr;
     }

     constexpr pointer<T> next(pointer<T> p) const {
       pointer<entry> e = reinterpret_cast<entry*>(static_cast<uint8_t*>(static_cast<void*>(p)) -
                                                   offsetof(entry, value));
       e = e->next;
       return e != nullptr ? &e->value : nullptr;
     }

     constexpr bool empty() const { return _head == nullptr;}

     void clear() {
       versioned_head_t temp = _head.contents();
       while (temp != nullptr) {
         auto ret = _head.inc_and_change(temp, temp->next);
         if (ret.succeeded) {
           temp->value.~T();
           pointer<entry> n = temp->next;
           alloc.deallocate(temp, 1);
           temp = n;
         } else {
           temp = ret.prior_value;
         }
       }
     }

     template <typename ...Args>
     void static construct(void *p, Args&&... args) {
       new (p) T(std::forward<Args>(args)...);
       static_cast<entry*>(p)->next = nullptr;
     }

     template <typename ...Args>
     pointer<T> allocate(Args&&... args) {
       pointer<entry> e = alloc.allocate(1);
       construct(static_cast<void*>(e), std::forward<Args>(args)...);
       return &(e->value);
     }
     //NOTE:We must ensure that we convert pointer<T> to void* using static_cast and not reinterpret_cast. This is
     //because typecast operator may be overloaded (like in offset_ptr) in some cases.
     void push(pointer<T> p) {
       pointer<entry> e = reinterpret_cast<entry*>(static_cast<uint8_t*>(static_cast<void*>(p)) -
                                                   offsetof(entry, value));
       _head.update([&e](versioned_head_t h) {
                      e->next = h;
                      h.inc_and_set(e);
                      return h;
                    });
     }

     void push(pointer<T> begin, pointer<T> end) {
       pointer<entry> b = reinterpret_cast<entry*>(static_cast<uint8_t*>(static_cast<void*>(begin)) -
                                                   offsetof(entry, value));
       pointer<entry> e = reinterpret_cast<entry*>(static_cast<uint8_t*>(static_cast<void*>(end)) -
                                                   offsetof(entry, value));
       _head.update([&b, &e](versioned_head_t h) {
                     e->next = h;
                     h.inc_and_set(b);
                     return h;
                    });
     }

     void pop(pointer<T> &ret) {
       _head.try_update([&](const versioned_head_t& h) {
                          if (h == nullptr) {
                            ret = nullptr;
                            return false;
                          } else {
                            return true;
                          }
                        }, 
                        [&](versioned_head_t h) {
                          ret = &(h->value);
                          h.inc_and_set(h->next);
                          return h;
                        });
     }

     void deallocate(pointer<T> e) {
       alloc.deallocate(reinterpret_cast<entry*>(static_cast<uint8_t*>(static_cast<void*>(e)) - offsetof(entry, value)), 1);
     }
  };
}



#endif /* LF_STACK_H_ */
