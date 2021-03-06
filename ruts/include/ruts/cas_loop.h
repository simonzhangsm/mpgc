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
 * cas_loop.h
 *
 *  Created on: Sep 1, 2014
 *      Author: evank
 */

#ifndef CAS_LOOP_H_
#define CAS_LOOP_H_

#include <atomic>
#include <type_traits>
#include <algorithm>
#include <iterator>

namespace ruts {
  template <typename T>
  struct cas_loop_return_value {
    bool succeeded = false;
    T prior_value;
    T new_value;
    operator bool() const { return succeeded; }
    cas_loop_return_value() = default;
    template <typename U>
    cas_loop_return_value(const cas_loop_return_value<U>(other))
    : succeeded { other.succeeded},
      prior_value { other.prior_value},
      new_value { other.new_value }
      {
        //empty
      }
      T resulting_value() const {
        return succeeded ? new_value : prior_value;
      }
  private:
      const T &_cpv() { return prior_value; }
      cas_loop_return_value _ret(bool s) {
        succeeded = s;
        return this;
      }
      template <typename Update>
      T _update(Update update_fn) {
        new_value = update_fn(prior_value);
        return new_value;
      }
      template <typename Continue, typename Update>
      bool try_once(std::atomic<T> &a, Continue continue_fn, Update update_fn) {
        succeeded = continue_fn(_cpv());
        if (!succeeded) {
          return true;
        }
        succeeded = a.compare_exchange_strong(prior_value, _update(update_fn));
        return succeeded;
      }
      template <typename Continue, typename Update>
      bool first_try(std::atomic<T> &a, Continue continue_fn, Update update_fn) {
        prior_value = a;
        return try_once(a, continue_fn, update_fn);
      }

      template <typename Continue, typename Update>
      void try_more(std::atomic<T> &a, Continue continue_fn, Update update_fn) {
        while (!try_once(a, continue_fn, update_fn)) {
          // empty
        }
      }
  public:
      // Shouldn't be public, but I can't get the friendship working right.

      template <typename Continue, typename Update>
      cas_loop_return_value loop(std::atomic<T> &a, Continue continue_fn, Update update_fn) {
        if (!first_try(a, continue_fn, update_fn)) {
          try_more(a, continue_fn, update_fn);
        }
        return *this;
      }

      template <typename Continue, typename Update>
      cas_loop_return_value once(std::atomic<T> &a, Continue continue_fn, Update update_fn) {
        first_try(a, continue_fn, update_fn);
        return *this;
      }

      template <typename U, typename V,
      typename = std::enable_if_t<std::is_convertible<U,T>() && std::is_convertible<V,T>()>
      >
      cas_loop_return_value change(std::atomic<T> &a, const U &from, const V &to) {
        prior_value = from;
        new_value = to;
        succeeded = a.compare_exchange_strong(prior_value, new_value);
        return *this;
      }

  };


  template <typename T, typename Continue, typename Update>
  inline cas_loop_return_value<T> try_cas(std::atomic<T> &a, Continue continue_fn, Update update_fn) {
    cas_loop_return_value<T> clrv;
    return clrv.once(a, continue_fn, update_fn);
  }

  template <typename T, typename U, typename V,
  typename = std::enable_if_t<std::is_convertible<U,T>() && std::is_convertible<V,T>()>
  >
  inline cas_loop_return_value<T> try_change_value(std::atomic<T> &a, const U &from, const V &to) {
    cas_loop_return_value<T> clrv;
    return clrv.change(a, from, to);
  }


  template <typename T, typename Continue, typename Update>
  inline cas_loop_return_value<T> try_cas_loop(std::atomic<T> &a, Continue continue_fn, Update update_fn) {
    cas_loop_return_value<T> clrv;
    return clrv.loop(a, continue_fn, update_fn);
  }

  template <typename T, typename Continue, typename Update>
  inline cas_loop_return_value<T> try_cas_loop(std::atomic<T> &a, long max_tries, Continue continue_fn, Update update_fn) {
    return try_cas_loop(a, [&](const T &curr) { return max_tries-- > 0 && continue_fn(curr);}, update_fn);
  }

  // QUESTION:  If the second param is, say, int, will this one match or the one with Continue?
  template <typename T, typename Update>
  inline cas_loop_return_value<T> try_cas_loop(std::atomic<T> &a, long max_tries, Update update_fn) {
    return try_cas_loop(a, max_tries, [](const T &){return true;}, update_fn);
  }

  template <typename T, typename Update>
  inline cas_loop_return_value<T> cas_loop(std::atomic<T> &a, Update update_fn) {
    return try_cas_loop(a, [](const T &){return true;}, update_fn);
  }

  template <typename T>
  inline cas_loop_return_value<T> increment_to_at_least(std::atomic<T> &a, const T &to) {
    return try_cas_loop(a, 
			[&](const T &old) { return old < to; },
			[&](const T &old) { return to; });
  }
  
  template <typename T, typename Iter, typename Fn>
  inline void process_onceish(std::atomic<T> &a, const Iter &from, const Iter &to, const Fn &fn, std::input_iterator_tag) {
    T next = a;
    T current = 0;
    std::for_each(from, to, [&](auto &arg) {
      if (current++ == next) {
        fn(arg);
        next = increment_to_at_least(a, next+1).resulting_value();
      }
    });
  }

  template <typename T, typename Iter, typename Fn>
  inline void process_onceish(std::atomic<T> &a, const Iter &from, const Iter &to, const Fn &fn, std::random_access_iterator_tag) {
    T next = a;
    for (Iter p = from+next; p < to; p = from+next) {
      fn(*p);
      next = try_change_value(a, next, next+1).resulting_value();
    }
  }

  template <typename T, typename Iter, typename Fn>
  inline void process_onceish(std::atomic<T> &a, Iter &&from, Iter &&to, const Fn &fn) {
    process_onceish(a, std::forward<Iter>(from), std::forward<Iter>(to), fn,
                    typename std::iterator_traits<Iter>::iterator_category());
  }
}



#endif /* CAS_LOOP_H_ */
