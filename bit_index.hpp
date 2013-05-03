#pragma once
#include <stdint.h>
#include <assert.h>
#include <stdio.h>

#define LZERO(X)  (__builtin_clzll((X)) )

template<uint64_t>
class bit_index;

template<uint64_t x>
struct log64;
template<>
struct log64<64> { enum { value = 1 }; };
template<>
struct log64<0> { enum { value = 0 }; };
template<uint64_t x>
struct log64 { enum { value = 1 + log64<x/64>::value }; };


template<uint64_t x>
struct pow64;

template<>
struct pow64<0> { enum ev{ value = 1 }; };

template<uint64_t x>
struct pow64 { enum ev{ value = pow64<x-1>::value*64ll }; };



template<>
class bit_index<1>
{
  public:
    enum size_enum { index_size = 1 };
    void set( uint64_t pos = 0)
    {
      fprintf( stderr, "set pos %lld\n", pos );
      assert( pos == 0 );
      bit = 1;
    }
    bool get( uint32_t pos = 0)const { return bit; }

    bool clear( uint64_t pos  = 0)
    {
      assert( pos == 0 );
      return !(bit = 0);  
    }
    void clear_all() { clear(); }
    void set_all()   { set();   }

    uint64_t first_set_bit()const { return !bit; }
    uint64_t size()const          { return 1;    }
  private:
    bool bit;
};

template<>
class bit_index<0> : public bit_index<1>{};

/**
 *  Size - number of bits in the index.
 *
 *  Provides O(log64) time search of a bit space for
 *  the first bit with a 1.
 */
template<>
class bit_index<64>
{
    public:
      enum size_enum { index_size = 64 };
        bit_index():_bits(0){}

        uint64_t first_set_bit()const     { return _bits ? 63-LZERO(_bits) : 64; }
        bool     get( uint64_t pos )const { return _bits & (1ll<<(pos));   }
        void     set( uint64_t pos )    
        { 
            assert( pos < 64 );
            fprintf( stderr, "b64 set %d\n", pos );
            _bits |= (1ll<<(pos));       
        }
        bool     clear( uint64_t pos )  
        { 
            _bits &= ~(1ll<<(pos));      
            return _bits == 0;
        }

        uint64_t size()const  { return 64;                          }
        uint64_t count()const { return __builtin_popcountll(_bits); }

        void set_all()   { _bits = -1; }
        void clear_all() { _bits = 0;  }
    protected:
        uint64_t _bits;
};

template<uint64_t Size>
class bit_index
{
    public:
      enum size_enum { 
        index_size  = Size
     };

        uint64_t first_set_bit()const
        {
            int base = _base_index.first_set_bit();
            fprintf( stderr, "base %d\n", base );
            if( base*64*64 >= Size ) return Size;

            return (base) * Size/64/64 + _this_index[base].first_set_bit();
        }
        bool get( uint64_t bit )const
        {
           return _this_index[bit/(Size/64/64)].get( bit%(Size/64) );
        }

        void set( uint64_t bit )
        {
           _base_index.set( bit/(Size/64/64) );
           return _this_index[bit/(Size/64/64)].set( (bit %(Size/64/64)) );
        }

        bool clear( uint64_t bit )
        {
           if( _this_index[bit/(Size/64/64)].clear( bit %(Size/64) ) )
              return _base_index.clear( bit/(Size/64/64) );
           return false;
        }

        void set_all()
        {
           _base_index.set_all();
           for( uint64_t i = 0; i < (Size/64)%64; ++i )
           {
             _this_index[i].set_all();
           }
        }

        void clear_all()
        {
           _base_index.clear_all();
           for( uint64_t i = 0; i < (Size/64)%64; ++i )
           {
             _this_index[i].clear_all();
           }
        }

        uint64_t count()const
        {
           uint64_t c = 0;
           for( uint64_t i = 0; i < (Size/64)%64; ++i )
           {
              c+=_this_index[i].count();
           }
           return 0;
        }
    protected:
        bit_index<(Size/64/64)>  _base_index;
        bit_index<(Size/64/64)>  _this_index[64];
};





