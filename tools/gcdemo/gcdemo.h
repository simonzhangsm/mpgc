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
 * gcdemo.h
 *
 *  Created on: May 31, 2016
 *      Author: uversky
 */

#ifndef GCDEMO_GCDEMO_H_
#define GCDEMO_GCDEMO_H_

#include "Graph.h"
#include "mpgc/gc.h"
#include <chrono>
#include <functional>
#include <random>

using UserGraph = mpgc::gc_vector<mpgc::gc_ptr<User>>;
using WrappedUserGraph = mpgc::gc_wrapped<UserGraph>;
using UserGraphPtr = mpgc::gc_ptr<WrappedUserGraph>;

using namespace std;

using AtomicULPtr = mpgc::gc_ptr<mpgc::gc_wrapped<std::atomic<unsigned long>>>;
using AtomicUIPtr = mpgc::gc_ptr<mpgc::gc_wrapped<std::atomic<unsigned int>>>;
using AtomicBoolPtr = mpgc::gc_ptr<mpgc::gc_wrapped<std::atomic<bool>>>;

string underline(const string s) {
  // The two escape sequences used below underline a character
  // and then clear the underlining.
  return "\33[4m" + s + "\33[0m";
}

inline
void printMemStats() {
  auto & ms = mpgc::memory_stats();
  cout << "Memory stats:" << endl
       << "  Bytes in heap:   " << to_string(ms.bytes_in_heap()) << endl
       << "    Bytes in use:  " << to_string(ms.bytes_in_use()) << endl
       << "    Bytes free:    " << to_string(ms.bytes_free()) << endl
       << "  GC cycle number: " << to_string(ms.cycle_number()) << endl
       << "  # processes:     " << to_string(ms.n_processes()) << endl
       << "  # objects:       " << to_string(ms.n_objects()) << endl
       << endl;
}

inline void displayProgressBarHeader() {
  // TODO: this is currently hardcoded for the defaults in advance
  //       (needs to be made more modular)
  cout << "Progress:  25%          50%          75%         100%\n" << flush;
}

// Display a progress bar for any event that is ongoing
// By default, have 50 ticks (for every 2% until 100%) divided every 25%
inline void
advanceProgressBar(const unsigned long current, const unsigned long max,
                   const bool isDebugMode = false,
                   const unsigned int numTicks = 50,
                   const unsigned int numDivisions = 4) {
  
  if (current >= max) return;

  const unsigned long tick     = max / numTicks;
  const unsigned long division = max / numDivisions;
  
  const bool isBeginning = (current == 0);
  const bool isEnd = (!isBeginning && current == max - 1);
  const bool isDivision = (!isEnd && current % division == 0);
  const bool isTick = (!isDivision && current % tick == 0);

  if (isDebugMode) {
    // In debug mode, simply print ticks and put memory stats in place of dividers
    if (isDivision) {
      cout << endl;
      printMemStats();
    }
    else if (isEnd)
      cout << endl << flush;
    else if (isTick)
      cout << "-" << flush;
  }
  else {
    // Print the progress bar as normal
    if (isBeginning)
      cout << "[" << flush;
    else if (isEnd)
      cout << "]" << endl << flush;
    else if (isDivision)
      cout << "|" << flush;
    else if (isTick)
      cout << "-" << flush;
  }

}

class RandomSeed {
  mt19937 generator;
  static unsigned int seed() {
    static mt19937 s(random_device{}());
    return s();
  }
public:
  RandomSeed() : generator(RandomSeed::seed()) {}
  unsigned int operator()() {
    return generator();
  }
};

extern thread_local RandomSeed random_seed;
// Base class for creating all the other generators
class RNG {
protected:
  mt19937 generator;
  RNG() : generator{random_seed()} {}
};

class UniformRNG : public RNG {
protected:
  uniform_int_distribution<unsigned long> uniDist;
public:
  function<unsigned long()> randElt;
  UniformRNG(unsigned long numElts)
    : uniDist(0, numElts-1),
      randElt{bind(uniDist, generator)} {}
  UniformRNG(unsigned long a, unsigned long b)
    : uniDist(a, b),
      randElt{bind(uniDist, generator)} {}
};

// RNG for determining number of tags and who to tag
class TagRNG : public UniformRNG {
private:
  // We want to keep tagging users until k = 1 trial fails
  //  with some probability p; negative_binomial_distribution
  //  provides exactly that functionality.
  negative_binomial_distribution<unsigned int> nbDistPost;
  negative_binomial_distribution<unsigned int> nbDistComment;
public:
  function<unsigned int()> numPostTags;
  function<unsigned int()> numCommentTags;

  // We assume roughly 3 tags are added for a post and 1 tag for a comment.
  // Unfortunately, the STL definition of a negative binomial distribution
  // is the inverse of what is found on Wikipedia - basically, the STL
  // definition is equivalent to the Wikipedia definition if p_STL = (1 - p_wiki).
  //
  // By the definition given in Wikipedia, we get that mu = p_{wiki}k/(1-p_{wiki}).
  // Subbing in, we get mu = (1 - p_STL)*k / p_STL.
  // With k = 1, this yields p_STL = 1/(mu + 1) .
  TagRNG(double muPost    = 3.0,
         double muComment = 1.0,
         unsigned long numUsers = 20e6)
  : UniformRNG(numUsers),
    nbDistPost(1, 1/(muPost+1)),
    nbDistComment(1, 1/(muComment+1)),
    numPostTags{bind(nbDistPost, generator)},
    numCommentTags{bind(nbDistComment, generator)} {}
};

// RNG for determining what action to take (post, comment)
class ActionRNG : public RNG
{
private:
  bernoulli_distribution bernDist;
public:
  function<bool()> isPost;

  // By default. assume that 60% of actions are new posts
  ActionRNG(double p = 0.6)
    : bernDist(p),
      isPost{bind(bernDist, generator)} {}
};

#endif /* GCDEMO_GCDEMO_H_ */
