#pragma once

#include "emp-sh2pc/emp-sh2pc.h"
#include "math.h"
#include <cstddef>
#include <iostream>
#include <map>
#include <vector>

#undef NDEBUG
#include <assert.h>

using emp::Bit;
using emp::Integer;
using emp::PUBLIC;
using std::cout;
using std::endl;
using std::map;
using std::size_t;
using std::vector;

/* Size of pointer for each level */
#define PTR_BIT 8

template<class T>
class PrivateStack {  
  // The capacity is set at initialization and cannot grow.

private:
  /* Guaranteed capacity that the stack can hold, actual capacity may be more */
  int min;
  
  /* Levels of stack */
  map<int, vector<T>*> levels;
  
  /* Pointers for each level */
  map<int, Integer> ptrs;
  
  /* Counters for pushes invoked for each level */
  map<int, int> pushes;
  
  /* Counters for pops invoked for each level */
  map<int, int> pops;

  T default_v;
  
public:
  PrivateStack() {}

  PrivateStack(int min, T default_v) {
    init(min, default_v);
  }

  void init(int min, T const &default_v) {

    this->default_v = default_v;
    this->min = min;
    int len = 0;
    int lv = 0;
    do {
      int llen = level_len(lv);
      init_level(lv, default_v);
      len += llen;
      lv++;
    } while(len < min);

  }
  
  void init_level(int lv, T const &default_v) {    
    this->ptrs[lv] = Integer(PTR_BIT, 5, PUBLIC);
    this->pushes[lv] = -3;
    this->pops[lv] = 0;
    
    vector<T> *vv = new vector<T>();
    for(int k = 0; k < level_len(lv); k++) {
      vv->push_back(default_v);
    }
    
    this->levels[lv] = vv;
  }

  /** For debugging only. */
  void dump_stack() {
    for (int lv = 0; lv < 5; lv++) {
      for (auto s : *this->levels[lv]) {
        cout << s.toString() << endl;
      }
    }
  }
  
  ~PrivateStack() {
    for (auto const & x : levels) {
      delete x.second;
    }
  }
  
  int level_len(int level) {
    return 5 * pow(2, level);
  }
  
  int level_base(int level) {
    int val = 0;
    
    for(int k = 0; k < level; k++) {
      val += level_len(k);
    }

    return val;
  }  
    
  T oread(int level, int k) {
    return this->levels[level]->at(k);
  }
  
  void owrite(int level, int k, T const &val, Bit real) {
    this->levels[level]->at(k) = this->levels[level]->at(k).select(real, val);
  }
  
  Bit push(vector<T> const &vals, size_t level, Bit real) {
    int len = level_len(level);
    int size = len/5;
    
    // Save new item at location
    Integer tmp = this->ptrs[level] - Integer(PTR_BIT, 1, PUBLIC);
    Integer newval(PTR_BIT, 0, PUBLIC) ;
    newval = this->ptrs[level].select(real, tmp);
    Bit overflow = newval < Integer(PTR_BIT, 0, PUBLIC);

    // If this push will overflow us, make it unreal
    real = real & !overflow;
    
    set_ptr(level, newval, !overflow);    
    
    for(int k = 0; k < 5; k++) {      // 5 buckets on each level
      Bit real2 = real & (this->ptrs[level] == Integer(PTR_BIT, k, PUBLIC));
      
      for(size_t j = 0; j < vals.size(); j++) { // one bucket here, size of 1, 2, 4, 8, etc...
        owrite(level, (k*size)+j, vals[j], real2);        
      }
    }
    
    // Increment action count
    this->pushes[level]++;
    
    if(this->pushes[level] >= 2) {
      this->pushes[level] = 0;      
                  
      if(level < this->ptrs.size()-1) {
        // shift to next level by two
        Integer midpoint(PTR_BIT, 2, PUBLIC);
        Bit real2 = this->ptrs[level] < midpoint; // index 0 or 1
        // Pack up the last two buckets into a single bucket for next level
        vector<T> bucket;

        for(int k = 0; k < 2; k++) {
          for(int j = 0; j < size; j++) {
            bucket.push_back(oread(level, (k+3)*size+j));
          }
        }

        Bit overflow2 = push(bucket, level+1, real2);      

        for(int kk = 0; kk < 2; kk++) { // shift by up to two buckets
          for(int k = 5-1; k >= 1; k--) {
            for(int j = 0; j < size; j++) {
              owrite(level, k*size+j, oread(level, (k-1)*size+j), real2);
            }
          }
        }
        
        tmp = this->ptrs[level] + midpoint;
        set_ptr(level, tmp, real2 & !overflow2);
      } else {
      }
    }   
    
    return overflow;
  }
  
  Bit pop(size_t level, vector<T> *pvals, Bit real) {    
    int len = level_len(level);
    int size = len/5;
    
    Bit underflow = this->ptrs[level] > Integer(PTR_BIT, 4, PUBLIC);
        
    // Pop the last value    
    for(int k = 0; k < size; k++) {
      pvals -> push_back(oread(level, k)); // Read in first bucket
    }
    
    for(int kk = 1; kk < 5; kk++) { // read all subsequent bucket
      Bit eff = this->ptrs[level] == Integer(PTR_BIT, kk, PUBLIC);
            
      for(int k = 0; k < size; k++) {
        
        pvals->at(k) = pvals->at(k).select(eff, oread(level, (size*kk)+k));
      }
    }
    
    Integer tmp = this->ptrs[level] + Integer(PTR_BIT, 1, PUBLIC);
    set_ptr(level, tmp, real);
    
    this->pops[level] += 1;
    
    // Unshift from prior level
    if(this->pops[level] >= 2) {
      this->pops[level] = 0;
           
      bool hasNextLevel = level < this->ptrs.size()-1;
      
      if(hasNextLevel) {
        Integer threshold(PTR_BIT, 3, PUBLIC); // at threshold, we still have 2 items
        Bit real2 = (this->ptrs[level] > threshold); // 1 or 0 items left
        
        
        vector<T> vlist;          
        Bit underflow2 = pop(level+1, &vlist, real2);        
        Bit mod = real2 & (!underflow2);
        
        for(int kk = 0; kk < 2; kk++) {
          for(int k = 0; k < 4; k++) {
            for(int j = 0; j < size; j++) {
              owrite(level, k*size+j, oread(level, (k+1)*size+j), mod);
            }
          }
        }

        for(size_t k = 0; k < vlist.size(); k++) {
          owrite(level, (size*3)+k, vlist[k], mod);
        }
        
        tmp = this->ptrs[level] - Integer(PTR_BIT, 2, PUBLIC);
        set_ptr(level, tmp, mod);
      } else {
      }
    }
            
    return underflow;
  }
    
  void push(T const &val, Bit real=Bit(1, ALICE)) {
    vector<T> v;
    v.push_back(val);
    Bit overflow = push(v, 0, real);

    if(overflow.reveal<bool>()) {
      // Note the full error can occur at one less than min elements, because the
      // next level can raise, so with capacity 15, item 15 will err
      throw "Stack full!!";
    }
  }
  
  T pop(Bit b, bool real=true) {
    vector<T> vals;
    
    Bit underflow = pop(0, &vals, Bit(real, PUBLIC));
    
    if((underflow & b).reveal<bool>()) {
      throw "Empty stack";
    }

    push(vals[0], !b);
    
    return vals[0].select(!b, default_v);
  }
  
  void set_ptr(int level, Integer ptr, Bit real) {
    this->ptrs[level] = this->ptrs[level].select(real, ptr);
    
    return;
    
    if(this->ptrs[level].template reveal<int>()<-2) {
      cout << "***** Pointer becomes negative for level " << level << endl;
    }
    
    cout << "((Level " << level <<
      " ptr set to " << this->ptrs[level].template reveal<int>() << "))" << endl;
  }


};
