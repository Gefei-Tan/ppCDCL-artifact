#ifndef EMP_OSTACK__H__
#define EMP_OSTACK__H__
#pragma once

#include "math.h"
#include <map>
#include <iostream>
#include "emp-dpf/emp-dpf.h"

#undef NDEBUG
#include <assert.h>

using Bit = Bit_T<GbWire>;
using Integer = Integer_T<GbWire>;
using namespace emp;
using namespace std;

/* Size of pointer for each level */
#define PTR_BIT 8

template<class T>
class PrivateStack {  
private:
  /* Guaranteed capacity that the stack can hold, actual capacity may be more */
  int min;
  
  /* Levels of stack */
  vector<vector<T>> levels;
  
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
    
    this->levels.resize(lv+1);
    this->levels[lv] = vector<T>(level_len(lv), default_v);
  }

  ~PrivateStack() {
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
    return this->levels[level][k];
  }
  
  void owrite(int level, int k, T const &val, Bit real) {
    this->levels[level][k] = this->levels[level][k].select(real, val);
  }
  
  Bit push(vector<T> const &vals, size_t level, Bit real) {
    int len = level_len(level);
    int size = len/5;
    
    // Move the pointer down one bucket when the push is real
    Integer tmp = this->ptrs[level] - Integer(PTR_BIT, 1, PUBLIC);
    Integer newval(PTR_BIT, 0, PUBLIC) ;
    newval = this->ptrs[level].select(real, tmp);
    Bit overflow = newval < Integer(PTR_BIT, 0, PUBLIC);

    // A push that would overflow is made a no-op
    real = real & !overflow;
    
    set_ptr(level, newval, !overflow);    
    
    for(int k = 0; k < 5; k++) {      // 5 buckets on each level
      Bit real2 = real & (this->ptrs[level] == Integer(PTR_BIT, k, PUBLIC));
      
      for(size_t j = 0; j < vals.size(); j++) { // bucket size doubles on each level: 1, 2, 4, 8, ...
        owrite(level, (k*size)+j, vals[j], real2);        
      }
    }
    
    // Count pushes at this level
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
        
    // Read the first bucket, then select the bucket the pointer refers to
    for(int k = 0; k < size; k++) {
      pvals -> push_back(oread(level, k));
    }
    
    for(int kk = 1; kk < 5; kk++) { // remaining buckets
      Bit eff = this->ptrs[level] == Integer(PTR_BIT, kk, PUBLIC);
            
      for(int k = 0; k < size; k++) {
        pvals->at(k) = pvals->at(k).select(eff, oread(level, (size*kk)+k));
      }
    }
    
    Integer tmp = this->ptrs[level] + Integer(PTR_BIT, 1, PUBLIC);
    set_ptr(level, tmp, real);
    
    this->pops[level] += 1;
    
    // Refill from the next level
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
  }
  
  T pop(Bit b, bool real=true) {
    vector<T> vals;
    
    Bit underflow = pop(0, &vals, Bit(real, PUBLIC));

    push(vals[0], !b);
    
    return vals[0].select(!b, default_v);
  }

  void set_ptr(int level, Integer ptr, Bit real) {
    this->ptrs[level] = this->ptrs[level].select(real, ptr);
    
    return;

  }



};

#endif //EMP_OSTACK__H__
