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
      assert( pos == 0 );
      bit = 1;
    }
    bool get( uint32_t pos = 0)const { return bit; }
    uint64_t& get_bits(uint64_t ) { return bit; }

    bool clear( uint64_t pos  = 0)
    {
      assert( pos == 0 );
      return !(bit = 0);  
    }
    void clear_all() { clear(); }
    void set_all()   { set();   }

    uint64_t first_set_bit()const { return !bit; }
    uint64_t size()const          { return 1;    }


    struct iterator
    {
       public:
          uint64_t& get_bits()      { return _self->bit; }
          bool     end()const       { return _bit == 1;   }
          int64_t  bit()const       { return _bit; }
          void     set()            { _self->set(_bit); }
          bool     clear()          { return _self->clear(_bit); }
          bool     operator*()const { return _self->get(_bit); }

          iterator&  next_set_bit()
          {
              _bit = 1;
              return *this;
          }

          iterator( bit_index* s=nullptr, uint8_t b = 64 ):_self(s),_bit(b){}
       private:
          bit_index* _self;
          uint8_t     _bit;
    };

    iterator at( uint64_t p ) { return iterator(this, p); }

  private:
    uint64_t bit;
};

template<>
class bit_index<0> : public bit_index<1>{};

template<>
class bit_index<64>
{
    public:
      enum size_enum { index_size = 64 };
        bit_index(uint64_t s = 0):_bits(s){}

        /**
         *  option A: use conditional to check for 0 and return 64
         */
        uint64_t first_set_bit()const     { 
            return _bits == 0 ? 64 : LZERO(_bits); 
        }

        void dump( int depth )
        {
           for( int i = 0; i < depth; ++i )
              fprintf( stderr, "    " );
           fprintf( stderr, "%llx\n", _bits );
        }

        /**
         *  Option 2, compare + shift + lzr + compare + mult + or... this approach.. while
         *  the result of LZERO(0) is undefined, multiplying it by 0 is defined.
         *
         *  This code may be faster or slower depending upon this cache miss rate and
         *  the instruction level parallelism.  Benchmarks are required.
         */
        //uint64_t first_set_bit()const     { return (_bits == 0)<<6 | (LZERO(_bits) * (_bits!=0)); }
        bool     get( uint64_t pos )const { return _bits & (1ll<<(63-pos));   }
        void     set( uint64_t pos )    
        { 
            assert( pos < 64 );
            _bits |= (1ll<<(63-pos));       
        }
        bool     clear( uint64_t pos )  
        { 
//            fprintf( stderr, "bit_index<64>::clear %llu\n", pos );
            _bits &= ~(1ll<<(63-pos));      
            //fprintf( stderr, "bit_index<64> clear: %p   %llx\n", this, _bits );
            //fprintf( stderr, "bit_index<64>::clear %llu return %llu == 0\n", pos, _bits );
            return _bits == 0;
        }

        uint64_t size()const  { return 64;                          }
        uint64_t count()const { return __builtin_popcountll(_bits); }

        void set_all()   { _bits = -1; }
        void clear_all() { _bits = 0;  }

        uint64_t& get_bits( uint64_t bit )
        {
            assert( bit < 64 );
            return _bits;
        }

        struct iterator
        {
           public:
              uint64_t& get_bits()       { return _self->_bits; }
              bool     end()const       { return _bit == 64;   }
              int64_t  bit()const       { return _bit; }
              void     set()            { _self->set(_bit); }
              bool     clear()          { return _self->clear(_bit); }
              bool     operator*()const { return _self->get(_bit); }

              iterator&  next_set_bit()
              {
                  ++_bit;
                  if( end() ) return *this;
                  bit_index tmp( (_self->_bits << (_bit))>>(_bit) );  
                  _bit = tmp.first_set_bit();
                  return *this;
              }

              iterator( bit_index* s=nullptr, uint8_t b = 64 ):_self(s),_bit(b){}
           private:
              bit_index* _self;
              uint8_t     _bit;

        };

        iterator begin()      { return iterator(this,0); }
        iterator at(uint8_t i){ return iterator(this,i); }
        iterator end()        { return iterator(this,64); }
    protected:
        friend class iterator;
        uint64_t _bits;
};

/**
 *   A bit_index is a bitset optimized for searching for set bits.  The
 *   operations set and clear maintain higher-level indexes to optimize
 *   finding of set bits.
 *
 *   The fundamental size is 64 bit and the first set bit can be found
 *   with a single instruction. For indexes up-to 64*64 in size, the
 *   first set bit can be found with 2 clz + 1 compare + 1 mult + 1 add.
 *
 */
template<uint64_t Size>
class bit_index
{
    public:
      static_assert( Size >= 64, "smaller sizes not yet supported" );

      enum size_enum { 
         index_size        = Size,
         sub_index_size    = (Size+63) / 64,
         sub_index_count   = Size / sub_index_size 
      };
       static_assert( bit_index::sub_index_count > 0, "array with size 0 is too small" );
       static_assert( bit_index::sub_index_count <= 64, "array with size 64 is too big" );


      void dump( int depth = 0 )
      {
           _base_index.dump( depth + 1 );
           for( int i = 0; i < 3; ++i )
             _sub_index[i].dump( depth + 2 );

/**
           for( int i = 0; i < depth; ++i )
              fprintf( stderr, "    " );
           fprintf( stderr, "%llx\n", _bits );
           */
      }

      
      uint64_t size()const  { return index_size; }
      uint64_t first_set_bit()const
      {
          uint64_t base = _base_index.first_set_bit();
          if( base >= sub_index_count ) 
          {
              return Size;
          }
          auto subidx = _sub_index[base].first_set_bit();
          return base * sub_index_size + subidx; //_sub_index[base].first_set_bit(); 
      }
      bool get( uint64_t bit )const
      {
         assert( bit < Size );
         int64_t sub_idx     = (bit/sub_index_size);
         int64_t sub_idx_bit = (bit%sub_index_size);
         return _sub_index[sub_idx].get(  sub_idx_bit );
      }
      
      void set( uint64_t bit )
      {
         assert( bit < Size );
         int64_t sub_idx     = (bit/sub_index_size);
         int64_t sub_idx_bit = (bit%sub_index_size);
         _base_index.set(sub_idx);
         return _sub_index[sub_idx].set( sub_idx_bit );
      }
      
      bool clear( uint64_t bit )
      {
         assert( bit < Size );
         int64_t sub_idx     = (bit/sub_index_size);
         int64_t sub_idx_bit = (bit%sub_index_size);
         if( _sub_index[sub_idx].clear( sub_idx_bit ) )
            return _base_index.clear(sub_idx);
         return false;
      }
      
      void set_all()
      {
         _base_index.set_all();
         for( uint64_t i = 0; i < sub_index_count; ++i )
         {
           _sub_index[i].set_all();
         }
      }
      
      void clear_all()
      {
         _base_index.clear_all();
         for( uint64_t i = 0; i < sub_index_count; ++i )
         {
           _sub_index[i].clear_all();
         }
      }
      
      uint64_t count()const
      {
         uint64_t c = 0;
         for( uint64_t i = 0; i < sub_index_count; ++i )
         {
            c+=_sub_index[i].count();
         }
         return 0;
      }

      /**
       *  Returns the in64_t that contains bit
       */
      uint64_t& get_bits( uint64_t bit )
      {
         int64_t sub_idx      = (bit/sub_index_size);
         int64_t sub_idx_bit  = (bit%sub_index_size);
         return _sub_index[sub_idx].get_bits( sub_idx_bit );
      }


      struct iterator
      {
         public:
            uint64_t&  get_bits()         { return sub_itr.get_bits(); }
            bool       operator*()const   { return *sub_itr;           }
            bool       end()const { return sub_idx >= sub_index_count; }
            int64_t    bit()const { return pos; }
            void       set() 
            { 
                bit_idx->_base_index.set(sub_idx); 
                sub_itr.set();
            }
            bool       clear() 
            { 
                if( sub_itr.clear() )
                {
                  return bit_idx->_base_index.clear(sub_idx); 
                }
                return false;
            }

            /**
             *  Find the next bit after this one that is set..
             */
            iterator&  next_set_bit()
            {
                if( end() ) return *this;
                sub_itr.next_set_bit();
                if( sub_itr.end() )
                {
                   sub_idx = bit_idx->_base_index.at(sub_idx).next_set_bit().bit();
                   if( end() )
                   {
                      pos = Size;
                      return *this;
                   }
                   auto fb = bit_idx->_sub_index[sub_idx].first_set_bit();
                   sub_itr = bit_idx->_sub_index[sub_idx].at(fb);
                }
                pos = sub_idx * sub_index_size + sub_itr.bit();
                return *this;
            }

            /**
             *  Move to the next bit.
             */
            iterator&  operator++()
            { 
               assert( !end() );
               ++pos;
               ++sub_itr;
               if( sub_itr.end() )
               {
                  ++sub_idx;
                  if( !end() )
                  {
                     sub_itr = bit_idx->_sub_index[sub_idx].begin();
                  }
                  else pos = Size;
               }
               return *this;
            }
            iterator& operator++(int) { return this->operator++(); }
            iterator operator+(uint64_t delta) { return iterator( bit_idx, pos + delta ); }


            iterator( bit_index* self=nullptr, int64_t bit=Size)
            :bit_idx(self),pos(bit),sub_idx((bit/64)%64)
            {
               sub_itr = bit_idx->_sub_index[sub_idx].at(bit%sub_index_size);
            }
            iterator& operator=(const iterator& i )
            {
               bit_idx = i.bit_idx;
               pos = i.pos;
               sub_idx = i.sub_idx;
               sub_itr = i.sub_itr;
               return *this;
            }
         private:
            friend class bit_index;
            bit_index*                          bit_idx;
            int64_t                             pos;
            int8_t                              sub_idx;
            typename bit_index<sub_index_size>::iterator sub_itr;
      };

      iterator begin()            { return iterator( this, 0 );    }
      iterator end()              { return iterator( this, Size ); }
      iterator at(int64_t p)      { return iterator( this, p );    }
    protected:
      friend class iterator;
      bit_index<64>              _base_index;
      bit_index<sub_index_size>  _sub_index[sub_index_count];
};





