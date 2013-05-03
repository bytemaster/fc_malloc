#pragma once
#include "mmap_alloc.hpp"
#include <iostream>
#include <sstream>
#include <assert.h>
#include <string.h>
#include <vector>
#include <unordered_set>


#define CHECK_SIZE( x ) assert(((x) != 0) && !((x) & ((x) - 1)))
#define PAGE_SIZE (2*1024*1024)
#define LOG2(X) ((unsigned) (8*sizeof (unsigned long long) - __builtin_clzll((X)) - 1))
#define LZERO(X)  (__builtin_clzll((X)) )
#define NUM_BINS 32 // log2(PAGE_SIZE)

class block_header
{
  public: 
      block_header()
      :_prev_size(0),_size(-PAGE_SIZE),_flags(0)
      {
          //fprintf( stderr, "constructor... size: %d\n", _size );
          //memset( data(), 0, size() - 8 );
          assert( page_size() == PAGE_SIZE );
      }

      void* operator new (size_t s) { return malloc(PAGE_SIZE);/*mmap_alloc( PAGE_SIZE );*/ }
      void operator delete( void* p ) { free(p); /*mmap_free( p, PAGE_SIZE );*/ }

      void dump( const char* label )
      {
         fprintf( stderr, "%s ]  _prev_size: %d  _size: %d\n", label, _prev_size, _size);//, int(_flags) );
      }

      /** size of the block header including the header, data size is size()-8 */
      uint32_t      size()const { return abs(_size);                                            } 
      char*         data()      { return reinterpret_cast<char*>(((char*)this)+8);                       }

      block_header* next()const 
      { 
        return _size <= 0 ? nullptr : reinterpret_cast<block_header*>(((char*)this)+size());
      }

      block_header* prev()const      
      { 
        return _prev_size <= 0 ? nullptr : reinterpret_cast<block_header*>(((char*)this)-_prev_size); 
      }

      /** 
       *  creates a new block of size S at the end of this block.
       *
       *  @pre size is a power of 2
       *  @return a pointer to the new block, or null if no split was possible
       */ 
      block_header* split( uint32_t sz )
      {
         assert( sz >= 32 );
         assert( size() >= 32 );
         assert( sz <= (size() - 32) );
         assert( page_size() == PAGE_SIZE );
         assert( _size != 0xbad );
         CHECK_SIZE(sz);

         int32_t old_size      = _size;
         block_header* old_nxt = next(); 

         _size = size() - sz;
         assert( _size != 0 );
         block_header* nxt = next();
         assert( nxt != 0 );

         nxt->_prev_size   = _size;
         nxt->_size        = old_size < 0 ? -sz : sz;
         assert( _size != 0 );

         if( old_nxt ) old_nxt->_prev_size = nxt->_size;

         //memset( data(), 0, size()-8 );

         assert( size() + nxt->size() == uint32_t(abs(old_size)) );
         assert( nxt->next() == old_nxt );
         assert( nxt->prev() == this );
         assert( next() == nxt );
         assert( page_size() == PAGE_SIZE );
         assert( nxt->page_size() == PAGE_SIZE );
         assert( nxt != this );
         nxt->_flags = 0;
         return nxt;
      }

      /**
       *   @return the merged node, if any
       */
      block_header* merge_next()
      {
         assert( _size != 0xbad );
         block_header* cur_next = next();
         if( !cur_next ) return this;
         assert( cur_next->_size != 0xbad );
         assert( cur_next->size() > 0 );

       //  if( !cur_next->is_idle() ) return this;

         auto s = size();

         assert( _size > 0 );
         _size += cur_next->size();
         assert( _size != 0 );

         if( cur_next->_size > 0 ) 
         {
            block_header* new_next = next();
            new_next->_prev_size = size();
         }
         else
         {
            _size = -_size; // we are at the end.
            assert( _size != 0 );
         }
         assert( cur_next->_size = 0xbad );


        // memset( data(), 0, size()-8 );
         assert( size() > s );
         if( next() )
         {
          assert( size()/8 == next() - this );
          assert( next()->_prev_size == size() );
          assert( page_size() == PAGE_SIZE );
         }
         return this;
      }

      /**
       *   @return the merged node, or this.
       */
      block_header* merge_prev()
      {
         assert( page_size() == PAGE_SIZE );
         block_header* pre = prev();
         if( !pre ) return this;
         return prev()->merge_next();
      }

      block_header* head()
      {
         if( !prev() ) return this;
         return prev()->head();
      }
      block_header* tail()
      {
         if( !next() ) return this;
         return next()->tail();
      }

      size_t        page_size()
      {
         auto t = tail();
         auto h = head();
         return ((char*)t-(char*)h) + t->size();
      }

      struct queue_state // the block is serving as a linked-list node
      {
          block_header*    qnext;
          block_header*    qprev;
          block_header**   head;
          block_header**   tail;
      };

      enum flag_enum 
      { 
        queued = 1, 
        idle   = 2,
        active = 4
      };

      bool         is_idle()const { return _flags & idle;  }
      bool         is_active()const { return _flags & active; }
      bool         is_queued()const { return _flags & queued;  }

      void         set_active( bool s )
      {
        if( s ) _flags |= active;
        else    _flags &= ~active;
      }
      void         set_queued( bool s ) 
      {
        if( s ) _flags |= queued;
        else    _flags &= ~queued;

        // anytime we change state it should be reset..
        if( is_queued() )
        {
          as_queue().qnext = nullptr;
          as_queue().qprev = nullptr;
        }
      }

      /** removes this node from any queue it is in */
      void dequeue()
      {
         block_header* pre = as_queue().qprev; 
         block_header* nxt = as_queue().qnext; 
         if( pre ) pre->as_queue().qnext = nxt;
         if( nxt ) nxt->as_queue().qprev = pre;
         set_queued(false);
      }

      void         set_idle( bool s ) 
      {
        if( s ) _flags |= idle;
        else    _flags &= ~idle;
        assert( is_idle() == s );
      }
      queue_state& as_queue()  
      { 
    //    assert( is_queued() );
        return *reinterpret_cast<queue_state*>(data()); 
      }

//  private:
      int32_t   _prev_size; // size of previous header.
      int32_t   _size:24; // offset to next, negitive indicates tail, 8 MB max, it could be neg
      int32_t   _flags:8; // offset to next, negitive indicates tail
};
static_assert( sizeof(block_header) == 8, "Compiler is not packing data" );

typedef block_header* block_header_ptr;

struct block_stack
{
    public:
      block_stack():_head(nullptr){}

      void push( block_header* h )
      {
         h->as_queue().qnext = _head;
         if( _head ) _head->as_queue().qprev = h;
         _head = h;
         //_head.push_back(h);
      }
      void push_all( block_header* h )
      {
         assert( h->is_queued() );
         assert( _head == nullptr );
         _head = h;
      }

      /*
      bool pop( block_header* h )
      {
         if( _head == nullptr ) return null;
         return _head.erase(h) != 0;
      }
      */

      /** returns all blocks */
      block_header* pop_all()
      {
        block_header* h = _head;
        _head = nullptr;
        return h;
      }

      block_header* pop()
      {
         if( _head )
         {
            auto tmp = _head;
            _head = _head->as_queue().qnext;
            if( _head )
            _head->as_queue().qprev = nullptr;
            return tmp;
         }
         return nullptr;
         /*
         if( _head.size() == 0 ) return nullptr;
         auto f = _head.begin();
         auto h = *f;
         _head.erase(f);
         return h;
         */
      }

      block_header* head(){ return _head; }

      //int size() { return int(_head.size()); }
    
    private:
      //std::unordered_set<block_header*> _head;
      block_header* _head;
};

/**
 *  Single threaded heap implementation, foundation
 *  for multi-threaded version;
 */
class fc_heap 
{
   public:
      block_header* alloc( size_t s );
      void          free( block_header* h );

      fc_heap()
      {
        memset(_bins, 0, sizeof(_bins) ); 
        _free_32_data = mmap_alloc( PAGE_SIZE );
        _free_64_data = mmap_alloc( PAGE_SIZE );

        _free_32_data_end = _free_32_data + PAGE_SIZE;
        _free_64_data_end = _free_64_data + PAGE_SIZE;

        _free_32_scan_end = &_free_32_state[PAGE_SIZE/32/64];
        _free_64_scan_end = &_free_64_state[PAGE_SIZE/64/64];

        _free_32_scan_pos = _free_32_state;
        _free_64_scan_pos = _free_64_state;

        memset( _free_32_state, 0xff, sizeof(_free_32_state ) );
        memset( _free_64_state, 0xff, sizeof(_free_64_state ) );
      }
      ~fc_heap()
      {
        mmap_free( _free_64_data, PAGE_SIZE );
        mmap_free( _free_32_data, PAGE_SIZE );
      }

 //  private:
      char* alloc32()
      {
         uint32_t c = 0;
         while( 0 == *_free_32_scan_pos )
         {
            ++_free_32_scan_pos;
            if( _free_32_scan_pos == _free_32_scan_end )
            {
                _free_32_scan_pos = _free_32_state;
            }
            if( ++c == sizeof(_free_32_state)/sizeof(int64_t) )
            {
              return alloc64();
            }
         }
         int bit = LZERO(*_free_32_scan_pos);
         int offset = (_free_32_scan_pos - _free_32_state)*64;

         *_free_32_scan_pos ^= (1ll<<(63-bit)); // flip the bit
        // fprintf( stderr, "alloc offset: %d bit %d  pos %d\n", offset,bit,(offset+bit) );

         return _free_32_data + (offset+bit)*32;
      }

      char* alloc64()
      {
         uint32_t c = 0;
         while( 0 == *_free_64_scan_pos )
         {
            ++_free_64_scan_pos;
            if( _free_64_scan_pos == _free_64_scan_end )
            {
                _free_64_scan_pos = _free_64_state;
            }
            if( ++c == sizeof(_free_64_state)/sizeof(int64_t) )
            {
              return nullptr;
            }
         }
         int bit = LZERO(*_free_64_scan_pos);
         int offset = (_free_64_scan_pos - _free_64_state)*64;

         *_free_64_scan_pos ^= (1ll<<(63-bit)); // flip the bit

         return _free_64_data + (offset+bit)*64;
      }

      bool free32( char* p )
      {
         if( p >= _free_32_data &&
              _free_32_data_end > p )
         {
            uint32_t offset = (p - _free_32_data)/32;
            uint32_t bit = offset & (64-1);
            uint32_t idx = offset/64;
            
            _free_32_state[idx] ^= (1ll<<((63-bit))); 
            return true;
         }
         return false;
      }
      bool free64( char* p )
      {
         if( p >= _free_64_data &&
              _free_64_data_end > p )
         {
          uint32_t offset = (p - _free_64_data)/64;
          uint32_t bit = offset & (64-1);
          uint32_t idx = offset/64;

          _free_64_state[idx] ^= (1ll<<((63-bit))); 
          return true;
         }
         return false;
      }

      char*                       _free_32_data;
      char*                       _free_64_data;
      char*                       _free_32_data_end;
      char*                       _free_64_data_end;
      uint64_t*                   _free_32_scan_pos;
      uint64_t*                   _free_64_scan_pos;
      uint64_t*                   _free_32_scan_end;
      uint64_t*                   _free_64_scan_end;
      uint64_t                    _free_32_state[PAGE_SIZE/32/64];
      uint64_t                    _free_64_state[PAGE_SIZE/64/64];
      block_stack _bins[NUM_BINS]; // anything less than 1024 bytes
};


/**
 *  Return a block of size s or greater
 *  @pre size >= 32
 *  @pre size is power of 2
 */
block_header* fc_heap::alloc( size_t s )
{
   assert( s >= 32 );
   CHECK_SIZE( s ); // make sure it is a power of 2
   uint32_t min_bin = LOG2(s); // find the min bin for it.
   while( min_bin < 32 )
   {
      block_header* h = _bins[min_bin].pop();
      if( h )
      {
          assert( h->_size != 0 );
          assert( h->_size != 0xbad );
          assert( h->is_queued() );
          h->set_queued(false);
          if( h->size() - 32 < s  )
          {
            h->set_active(true);
            return h;
          }
          block_header* tail = h->split(s); 
          assert( h->_size != 0 );

          h->set_active(true);
          this->free(h);

          tail->set_active(true);
          return tail;
      }
      ++min_bin;
   }
   // mmap a new page
   block_header* h = new block_header();
   block_header* t = h->split(s);

   h->set_active(true);
   free(h);

   t->set_active(true);
   return t;
}

void fc_heap::free( block_header* h )
{
    assert( h != nullptr );
    assert( h->is_active() );
    assert( h->_size != 0 );
    assert( h->size() < PAGE_SIZE );

    auto pre = h->prev();
    auto nxt = h->next();

    if( nxt && !nxt->is_active() && nxt->is_queued() )
    {
        auto nxt_bin = LOG2(nxt->size());
        if( _bins[nxt_bin].head() == nxt )
        {
          _bins[nxt_bin].pop();
          nxt->set_queued(false);
        }
        else
        {
          nxt->dequeue();
        }
        h = h->merge_next();
    }

    if( pre && !pre->is_active() && pre->is_queued() )
    {
        auto pre_bin = LOG2(pre->size());
        if( _bins[pre_bin].head() == pre )
        {
          _bins[pre_bin].pop();
          pre->set_queued(false);
        }
        else
        {
          pre->dequeue();
        }
        h = pre->merge_next();
    }

    if( h->size() == PAGE_SIZE )
    {
      delete h;
      return;
    }

    h->set_active(false);
    h->set_queued(true );
    auto hbin = LOG2(h->size());
    _bins[hbin].push(h);
}

class thread_heap;

class garbage_thread
{
   public:
      static garbage_thread& get();
      uint64_t               avail( int bin );
      int64_t                claim( int bin, int64_t num );
      block_header*          get_claim( int bin, int64_t pos );

   protected:
      void   register_thread_heap( thread_heap* h );

      friend class thread_heap;
      static void run();
};


class thread_heap
{
  public:
    static thread_heap& get();

    block_header* allocate( size_t s )
    {
       if( s >= PAGE_SIZE )
       {
          // TODO: allocate special mmap region...
       }

       uint32_t min_bin = LOG2(s); // find the min bin for it.
       while( min_bin < NUM_BINS )
       {
          block_header* h = cache_alloc(min_bin, s);
          if( h ) return h;

          garbage_thread& gc = garbage_thread::get();
          if( auto av = gc.avail( min_bin ) )
          {
             int64_t claim_num = std::min<int64_t>(4,av);
             int64_t claim = gc.claim( min_bin, claim_num );
             int64_t end = claim + claim_num;
             while( claim < end )
             {
                block_header* h = gc.get_claim(min_bin,claim);
                if( h )
                {
                   cache(h);
                }
                ++claim;
             }
             h = cache_alloc(min_bin, s);
             if( h ) return h; // else... we actually didn't get our claim
          }
          ++min_bin;
       }
       block_header* h = new block_header();
       h->set_active(true);
       if( s <= PAGE_SIZE - 32 )
       {
          block_header* t = h->split(s);
          t->set_active(true);
          cache( h );
          return t;
       }
       return h;
    }

    block_header* cache_alloc( int bin, size_t s )
    {
       block_header* c = pop_cache(bin);
       if( c && (c->size() - 32) > s )
       {
           block_header* t = c->split(s);
           c->set_active(true);
           if( !cache( c ) )
           {
             this->free(c);
           }
           t->set_active(true);
           return t;
       }
       return nullptr;
    }

    bool          cache( block_header* h )
    {
       uint32_t b = LOG2( h->size() );
       if( _cache_size[b] < 4 ) 
       {
         h->set_queued(true);
         _cache[b].push(h);
         _cache_size[b]++;
         return true;
       }
       return false;
    }

    block_header* pop_cache( int bin )
    {
        block_header* h = _cache[bin].pop();
        if( h ) 
        { 
          _cache_size[bin]--; 
          h->set_queued(false);
          return h;
        }
        return nullptr;
    }

    void free( block_header* h )
    {
       h->set_queued(true);
       _gc_on_deck.push( h );
       if( !_gc_at_bat.head() )
         _gc_at_bat.push_all( _gc_on_deck.pop_all() );
    }
  private:
    thread_heap();

    friend garbage_thread;
    block_stack _gc_at_bat; // waiting for gc to empty
    block_stack _gc_on_deck; // caching until gc pickups at bat
    block_stack _cache[NUM_BINS];
    int16_t     _cache_size[NUM_BINS];

};












static fc_heap static_heap;

void* fc_malloc( size_t s )
{
  if( s <= 64 ) 
  {
    if( s <= 32 )
        return static_heap.alloc32();
    else
        return static_heap.alloc64();
  }
  // round up to nearest power of 2 > 32
  s += 8; // room for header.
  if( s < 32 ) s = 32; // min size
  s = (1<<(LOG2(s-1)+1)); // round up to nearest power of 2
  if( s < 24 ) s = 24;

  block_header* h = static_heap.alloc( s );
  assert( h->is_active() );
//  h->set_idle(false); 
//  assert( h->page_size() == PAGE_SIZE );
  return h->data();
}
void fc_free( void* f )
{
  if( static_heap.free32((char*)f) || static_heap.free64((char*)f) ) return; 
  block_header* bh = (block_header*)(((char*)f)-8);
 // fprintf( stderr, "fc_free(block: %p)\n", bh );
//  assert( bh->is_active() );
  //assert( bh->page_size() == PAGE_SIZE );
  static_heap.free(bh);
}


